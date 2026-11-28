// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toonbus: the ReToon programs on the Toon's own broker. See toonbus.h.
 *
 * The connection runs in a thread of its own (net_loop), which connects,
 * reconnects every second while the broker is away and runs libmosquitto's
 * loop. (mosquitto_loop_start gives up when the very first connect fails,
 * as it does while the broker is still starting.) What the callbacks touch
 * is behind one mutex.
 * Commands are queued and a byte goes into a pipe, so the program handles
 * them in its own thread from bus_poll().
 *
 * A state's "time" is when its program last gave it to bus_put(), not when
 * it was sent: a program that stops putting (or hangs) shows as old, even
 * though a reconnect sends its last state again.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mosquitto.h>
#include "toonbus.h"

#define HOST	"127.0.0.1"
#define PORT	1883
#define MAX_JSON 16384	/* one state as JSON */

struct put {
	char name[48], *json;	/* json without "time"; NULL = cleared */
	time_t made;		/* the last bus_put() of it: its "time" */
	time_t sent;
	struct put *next;
};
struct watch {
	char name[48], *json;
	struct watch *next;
};
struct on {
	char *topic;
	bus_fn fn;
	struct on *next;
};
struct item {
	char *topic, *payload;
	struct item *next;
};

static struct mosquitto *m;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct put *puts_;
static struct watch *watches;
static struct on *ons;
static struct item *q_head, *q_tail;
static int pipe_fd[2] = { -1, -1 }, connected;
static pthread_t net;
static volatile int running;

/* ---- key=value lines to JSON --------------------------------------------- */

static int is_number(const char *v)
{
	const char *p = v;

	if (*p == '-')
		p++;
	if (!isdigit((unsigned char)*p))
		return 0;
	if (*p == '0' && isdigit((unsigned char)p[1]))
		return 0;	/* "007" stays text */
	while (isdigit((unsigned char)*p))
		p++;
	if (*p == '.') {
		if (!isdigit((unsigned char)*++p))
			return 0;
		while (isdigit((unsigned char)*p))
			p++;
	}
	return !*p;
}

static void json_str(char *out, size_t len, size_t *n, const char *v, const char *end)
{
	if (*n < len)
		out[(*n)++] = '"';
	for (; v < end && *v && *n + 7 < len; v++) {
		unsigned char c = *v;

		if (c == '"' || c == '\\') {
			out[(*n)++] = '\\';
			out[(*n)++] = c;
		} else if (c < 0x20) {
			*n += snprintf(out + *n, len - *n, "\\u%04x", c);
		} else {
			out[(*n)++] = c;
		}
	}
	if (*n < len)
		out[(*n)++] = '"';
}

/* "{...}" without the closing brace, so "time" can follow; NULL if empty */
static char *kv_json(const char *kv)
{
	char *out = malloc(MAX_JSON), *val;
	size_t n = 0;
	int first = 1;
	const char *line = kv;

	if (!out)
		return NULL;
	out[n++] = '{';
	while (line && *line && n + 16 < MAX_JSON) {
		const char *nl = strchr(line, '\n'), *end = nl ? nl : line + strlen(line);
		const char *eq = memchr(line, '=', end - line), *k;
		size_t vl;

		if (eq && eq > line) {
			for (k = line; k < eq; k++)
				if (!isalnum((unsigned char)*k) && *k != '_' && *k != '-')
					break;
			if (k == eq && strncmp(line, "time=", 5)) {
				size_t was = n;

				vl = end - eq - 1;
				if (vl && eq[vl] == '\r')	/* a line from a script may end in \r\n */
					vl--;
				val = strndup(eq + 1, vl);
				if (!val)
					break;
				if (!first)
					out[n++] = ',';
				json_str(out, MAX_JSON - 64, &n, line, eq);
				out[n++] = ':';
				/* numbers and true/false as themselves */
				if (is_number(val) || !strcmp(val, "true") || !strcmp(val, "false"))
					n += snprintf(out + n, MAX_JSON - 64 - n, "%s", val);
				else
					json_str(out, MAX_JSON - 64, &n, val, val + vl);
				free(val);
				if (n >= MAX_JSON - 64)
					n = was;	/* does not fit: left out whole, never cut */
				else
					first = 0;
			}
		}
		line = nl ? nl + 1 : NULL;
	}
	out[n] = 0;
	return out;
}

/* ---- a value out of a flat JSON object ----------------------------------- */

static int json_value(const char *json, const char *key, char *out, size_t len)
{
	size_t kl = strlen(key);
	const char *p = json;

	while ((p = strchr(p, '"'))) {
		const char *k = p + 1, *e = k;

		while (*e && *e != '"')
			e += *e == '\\' && e[1] ? 2 : 1;
		if (!*e)
			return -1;
		p = e + 1;
		while (isspace((unsigned char)*p))
			p++;
		if (*p != ':')
			continue;	/* a value, not a key */
		p++;
		while (isspace((unsigned char)*p))
			p++;
		if ((size_t)(e - k) == kl && !memcmp(k, key, kl)) {
			size_t n = 0;

			if (*p == '"') {
				for (p++; *p && *p != '"' && n + 1 < len; p++) {
					if (*p == '\\' && p[1] == 'u' && isxdigit((unsigned char)p[2]) &&
					    isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4]) &&
					    isxdigit((unsigned char)p[5])) {
						char hex[5] = { p[2], p[3], p[4], p[5], 0 };
						long c = strtol(hex, NULL, 16);

						/* what json_str writes (control characters);
						   anything else outside ASCII as '?' */
						out[n++] = c > 0 && c < 0x80 ? (char)c : '?';
						p += 5;
					} else if (*p == '\\' && p[1]) {
						p++;
						out[n++] = *p == 'n' ? '\n' : *p == 't' ? '\t' :
							   *p == 'r' ? '\r' : *p == 'b' ? '\b' : *p == 'f' ? '\f' : *p;
					} else {
						out[n++] = *p;
					}
				}
			} else {
				while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p) && n + 1 < len)
					out[n++] = *p++;
			}
			if (len)
				out[n] = 0;
			return 0;
		}
		/* skip this value */
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				if (*p == '\\' && p[1])
					p++;
			if (*p)
				p++;
		} else if (*p == '{' || *p == '[') {
			int depth = 0;

			for (; *p; p++) {
				if (*p == '"') {
					for (p++; *p && *p != '"'; p++)
						if (*p == '\\' && p[1])
							p++;
				} else if (*p == '{' || *p == '[') {
					depth++;
				} else if ((*p == '}' || *p == ']') && !--depth) {
					p++;
					break;
				}
				if (!*p)
					break;
			}
		}
	}
	return -1;
}

/* ---- sending ------------------------------------------------------------- */

static void topic_of(char *t, size_t len, const char *name)
{
	snprintf(t, len, BUS_BASE "/%s", name);
}

/* with mu held */
static void send_put(struct put *p)
{
	char topic[96], *msg;
	size_t n;

	topic_of(topic, sizeof(topic), p->name);
	if (!connected)
		return;
	p->sent = time(NULL);
	if (!p->json) {
		mosquitto_publish(m, NULL, topic, 0, "", 0, true);
		return;
	}
	n = strlen(p->json);
	msg = malloc(n + 40);
	if (!msg)
		return;
	memcpy(msg, p->json, n);
	snprintf(msg + n, 40, "%s\"time\":%lld}", n > 1 ? "," : "", (long long)p->made);
	mosquitto_publish(m, NULL, topic, (int)strlen(msg), msg, 0, true);
	free(msg);
}

void bus_put(const char *name, const char *kv)
{
	struct put *p;
	char *json = kv && *kv ? kv_json(kv) : NULL;

	pthread_mutex_lock(&mu);
	for (p = puts_; p && strcmp(p->name, name); p = p->next)
		;
	if (!p) {
		p = calloc(1, sizeof(*p));
		if (!p) {
			pthread_mutex_unlock(&mu);
			free(json);
			return;
		}
		snprintf(p->name, sizeof(p->name), "%s", name);
		p->next = puts_;
		puts_ = p;
	} else if (!p->json && !json) {
		pthread_mutex_unlock(&mu);	/* cleared already */
		return;
	}
	p->made = time(NULL);
	/* the same as before: only now and then, to keep its time current */
	if (p->json && json && !strcmp(p->json, json) && p->made - p->sent < BUS_REFRESH_S) {
		pthread_mutex_unlock(&mu);
		free(json);
		return;
	}
	free(p->json);
	p->json = json;
	send_put(p);
	pthread_mutex_unlock(&mu);
}

int bus_send(const char *topic, const char *payload)
{
	int r;

	pthread_mutex_lock(&mu);
	r = connected ? mosquitto_publish(m, NULL, topic, (int)strlen(payload), payload, 0, false) : -1;
	pthread_mutex_unlock(&mu);
	return r ? -1 : 0;
}

/* ---- receiving ----------------------------------------------------------- */

static void on_connect(struct mosquitto *mm, void *ud, int rc)
{
	struct put *p;
	struct watch *w;
	struct on *o;
	char topic[96];

	(void)ud;
	if (rc)
		return;
	pthread_mutex_lock(&mu);
	connected = 1;
	for (w = watches; w; w = w->next) {
		topic_of(topic, sizeof(topic), w->name);
		mosquitto_subscribe(mm, NULL, topic, 0);
	}
	for (o = ons; o; o = o->next)
		mosquitto_subscribe(mm, NULL, o->topic, 0);
	for (p = puts_; p; p = p->next)
		if (p->json)
			send_put(p);
	pthread_mutex_unlock(&mu);
}

static void on_disconnect(struct mosquitto *mm, void *ud, int rc)
{
	(void)mm; (void)ud; (void)rc;
	pthread_mutex_lock(&mu);
	connected = 0;
	pthread_mutex_unlock(&mu);
}

static void on_message(struct mosquitto *mm, void *ud, const struct mosquitto_message *msg)
{
	struct watch *w;
	struct on *o;
	const char *pl = msg->payloadlen ? msg->payload : "";
	char topic[96];
	bool match;

	(void)mm; (void)ud;
	pthread_mutex_lock(&mu);
	for (w = watches; w; w = w->next) {
		topic_of(topic, sizeof(topic), w->name);
		if (!strcmp(topic, msg->topic)) {
			free(w->json);
			w->json = msg->payloadlen ? strndup(pl, msg->payloadlen) : NULL;
		}
	}
	for (o = ons; o; o = o->next) {
		if (mosquitto_topic_matches_sub(o->topic, msg->topic, &match) || !match)
			continue;
		if (msg->retain && strstr(o->topic, "/set"))
			continue;	/* an old command kept by the broker: not again */
		struct item *it = calloc(1, sizeof(*it));

		if (!it)
			break;
		it->topic = strdup(msg->topic);
		it->payload = strndup(pl, msg->payloadlen);
		if (q_tail)
			q_tail->next = it;
		else
			q_head = it;
		q_tail = it;
		/* a full pipe already wakes the reader */
		(void)!write(pipe_fd[1], "", 1);
		break;		/* one queue entry per message */
	}
	pthread_mutex_unlock(&mu);
}

void bus_watch(const char *name)
{
	struct watch *w;
	char topic[96];

	pthread_mutex_lock(&mu);
	for (w = watches; w && strcmp(w->name, name); w = w->next)
		;
	if (!w && (w = calloc(1, sizeof(*w)))) {
		snprintf(w->name, sizeof(w->name), "%s", name);
		w->next = watches;
		watches = w;
		topic_of(topic, sizeof(topic), name);
		if (connected)
			mosquitto_subscribe(m, NULL, topic, 0);
	}
	pthread_mutex_unlock(&mu);
}

int bus_value(const char *name, const char *key, char *out, size_t len)
{
	struct watch *w;
	int r = -1;

	pthread_mutex_lock(&mu);
	for (w = watches; w && strcmp(w->name, name); w = w->next)
		;
	if (w && w->json)
		r = json_value(w->json, key, out, len);
	pthread_mutex_unlock(&mu);
	return r;
}

long bus_age(const char *name)
{
	char t[24];

	if (bus_value(name, "time", t, sizeof(t)))
		return -1;
	long age = (long)(time(NULL) - atoll(t));
	return age < 0 ? 0 : age;
}

FILE *bus_fopen(const char *name)
{
	struct watch *w;
	char *kv = NULL;
	FILE *f = NULL;

	pthread_mutex_lock(&mu);
	for (w = watches; w && strcmp(w->name, name); w = w->next)
		;
	if (w && w->json) {
		size_t len = strlen(w->json) + 64;

		kv = malloc(len);
		if (kv)
			bus_json_kv(w->json, kv, len);
	}
	pthread_mutex_unlock(&mu);
	if (!kv)
		return NULL;
	/* fmemopen's own buffer goes with the fclose */
	f = fmemopen(NULL, strlen(kv) + 1, "w+");
	if (f) {
		fputs(kv, f);
		rewind(f);
	}
	free(kv);
	return f;
}

int bus_json(const char *name, char *out, size_t len)
{
	struct watch *w;
	int r = -1;

	pthread_mutex_lock(&mu);
	for (w = watches; w && strcmp(w->name, name); w = w->next)
		;
	if (w && w->json && len) {
		snprintf(out, len, "%s", w->json);
		r = 0;
	}
	pthread_mutex_unlock(&mu);
	return r;
}

void bus_on(const char *topic, bus_fn fn)
{
	struct on *o = calloc(1, sizeof(*o));

	if (!o)
		return;
	o->topic = strdup(topic);
	o->fn = fn;
	pthread_mutex_lock(&mu);
	o->next = ons;
	ons = o;
	if (connected)
		mosquitto_subscribe(m, NULL, topic, 0);
	pthread_mutex_unlock(&mu);
}

int bus_fd(void)
{
	return pipe_fd[0];
}

void bus_poll(void)
{
	char drain[64];

	while (read(pipe_fd[0], drain, sizeof(drain)) > 0)
		;
	for (;;) {
		struct item *it;
		struct on *o;
		bus_fn fn = NULL;
		bool match;

		pthread_mutex_lock(&mu);
		it = q_head;
		if (it) {
			q_head = it->next;
			if (!q_head)
				q_tail = NULL;
			for (o = ons; o; o = o->next)
				if (!mosquitto_topic_matches_sub(o->topic, it->topic, &match) && match) {
					fn = o->fn;
					break;
				}
		}
		pthread_mutex_unlock(&mu);
		if (!it)
			return;
		if (fn)
			fn(it->topic, it->payload);
		free(it->topic);
		free(it->payload);
		free(it);
	}
}

int bus_connected(void)
{
	int c;

	pthread_mutex_lock(&mu);
	c = connected;
	pthread_mutex_unlock(&mu);
	return c;
}

/* ---- short-lived programs ------------------------------------------------ */

int bus_json_value(const char *json, const char *key, char *out, size_t len)
{
	return json_value(json, key, out, len);
}

size_t bus_json_kv(const char *json, char *out, size_t len)
{
	const char *p = json;
	char key[128], v[4096];	/* the sensor options are one long value */
	size_t n = 0;

	if (len)
		out[0] = 0;
	while (p && (p = strchr(p, '"'))) {
		size_t k = 0;

		for (p++; *p && *p != '"'; p++) {
			if (*p == '\\' && p[1])
				p++;
			if (k + 1 < sizeof(key))
				key[k++] = *p;
		}
		key[k] = 0;
		if (!*p++)
			break;
		while (isspace((unsigned char)*p))
			p++;
		if (*p != ':')
			continue;	/* that was a value */
		p++;
		while (isspace((unsigned char)*p))
			p++;
		if (*p != '{' && *p != '[' && !json_value(json, key, v, sizeof(v))) {
			char *c;

			for (c = v; *c; c++)
				if (*c == '\n' || *c == '\r')
					*c = ' ';
			if (n + strlen(key) + strlen(v) + 3 < len)
				n += snprintf(out + n, len - n, "%s=%s\n", key, v);
		}
		/* past this value */
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				if (*p == '\\' && p[1])
					p++;
			if (*p)
				p++;
		} else if (*p == '{' || *p == '[') {
			int depth = 0;

			for (; *p; p++) {
				if (*p == '"') {
					for (p++; *p && *p != '"'; p++)
						if (*p == '\\' && p[1])
							p++;
				} else if (*p == '{' || *p == '[') {
					depth++;
				} else if ((*p == '}' || *p == ']') && !--depth) {
					p++;
					break;
				}
				if (!*p)
					break;
			}
		}
	}
	return n;
}

struct snap {
	bus_snap_fn fn;
	void *ud;
	char marker[64];
	int n, done;
};

static void snap_message(struct mosquitto *mm, void *ud, const struct mosquitto_message *msg)
{
	struct snap *s = ud;
	const char *name = msg->topic + sizeof(BUS_BASE);	/* after "retoon/" */
	char *json;

	(void)mm;
	if (!strcmp(msg->topic, s->marker)) {
		s->done = 1;
		return;
	}
	if (!msg->retain || !msg->payloadlen || strncmp(msg->topic, BUS_BASE "/", sizeof(BUS_BASE)) ||
	    strchr(name, '/'))
		return;
	json = strndup(msg->payload, msg->payloadlen);
	if (!json)
		return;
	s->fn(name, json, s->ud);
	s->n++;
	free(json);
}

static struct mosquitto *short_connect(void *ud)
{
	struct mosquitto *mm;
	int i;

	mosquitto_lib_init();
	mm = mosquitto_new(NULL, true, ud);
	if (!mm)
		return NULL;
	/* at boot the broker may still be starting: a few seconds at most */
	for (i = 0; mosquitto_connect(mm, HOST, PORT, 10) != MOSQ_ERR_SUCCESS; i++) {
		if (i == 10) {
			mosquitto_destroy(mm);
			return NULL;
		}
		usleep(300000);
	}
	return mm;
}

/* The broker sends the retained messages of a subscription right after
   it, before anything published later: so when our own marker (published
   after subscribing) comes back, all of them are here. */
int bus_snapshot(bus_snap_fn fn, void *ud)
{
	struct snap s = { .fn = fn, .ud = ud };
	struct mosquitto *mm = short_connect(&s);
	struct timespec t0, t;

	if (!mm)
		return -1;
	snprintf(s.marker, sizeof(s.marker), BUS_BASE "/_snapshot/%d", (int)getpid());
	mosquitto_message_callback_set(mm, snap_message);
	mosquitto_subscribe(mm, NULL, BUS_BASE "/+", 0);
	mosquitto_subscribe(mm, NULL, s.marker, 0);
	mosquitto_publish(mm, NULL, s.marker, 1, "1", 0, false);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	do {
		if (mosquitto_loop(mm, 100, 1) != MOSQ_ERR_SUCCESS)
			break;
		clock_gettime(CLOCK_MONOTONIC, &t);
	} while (!s.done && (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < 1500);
	mosquitto_disconnect(mm);
	mosquitto_destroy(mm);
	return s.n;
}

static void sent_once(struct mosquitto *mm, void *ud, int mid)
{
	(void)mm; (void)mid;
	*(int *)ud = 1;
}

int bus_send_once(const char *topic, const char *payload, int retain)
{
	int done = 0, i;
	struct mosquitto *mm = short_connect(&done);

	if (!mm)
		return -1;
	mosquitto_publish_callback_set(mm, sent_once);
	/* QoS 1: the broker's PUBACK says it has it, before we disconnect */
	if (mosquitto_publish(mm, NULL, topic, (int)strlen(payload), payload, 1, retain)) {
		mosquitto_destroy(mm);
		return -1;
	}
	for (i = 0; i < 20 && !done; i++)
		if (mosquitto_loop(mm, 100, 1) != MOSQ_ERR_SUCCESS)
			break;
	mosquitto_disconnect(mm);
	mosquitto_destroy(mm);
	return done ? 0 : -1;
}

int bus_put_once(const char *name, const char *kv)
{
	char topic[96], *json = kv && *kv ? kv_json(kv) : NULL, *msg;
	size_t n;
	int r;

	topic_of(topic, sizeof(topic), name);
	if (!json)
		return bus_send_once(topic, "", 1);
	n = strlen(json);
	msg = malloc(n + 40);
	if (!msg) {
		free(json);
		return -1;
	}
	memcpy(msg, json, n);
	snprintf(msg + n, 40, "%s\"time\":%lld}", n > 1 ? "," : "", (long long)time(NULL));
	r = bus_send_once(topic, msg, 1);
	free(msg);
	free(json);
	return r;
}

/* ---- start and stop ------------------------------------------------------ */

static void *net_loop(void *arg)
{
	int up = 0;

	(void)arg;
	while (running) {
		if (!up) {
			if (mosquitto_connect(m, HOST, PORT, 30) == MOSQ_ERR_SUCCESS) {
				up = 1;
			} else {
				sleep(1);
				continue;
			}
		}
		if (mosquitto_loop(m, 1000, 1) != MOSQ_ERR_SUCCESS) {
			pthread_mutex_lock(&mu);
			connected = 0;
			pthread_mutex_unlock(&mu);
			mosquitto_disconnect(m);
			up = 0;
			sleep(1);
		}
	}
	return NULL;
}

int bus_open(const char *client)
{
	char id[64];

	if (m)
		return 0;
	if (pipe(pipe_fd))
		return -1;
	fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK);
	fcntl(pipe_fd[1], F_SETFL, O_NONBLOCK);
	fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC);
	mosquitto_lib_init();
	snprintf(id, sizeof(id), "retoon-%s", client);
	m = mosquitto_new(id, true, NULL);
	if (!m)
		return -1;
	mosquitto_connect_callback_set(m, on_connect);
	mosquitto_disconnect_callback_set(m, on_disconnect);
	mosquitto_message_callback_set(m, on_message);
	mosquitto_threaded_set(m, true);
	running = 1;
	/* the broker may still be starting: net_loop keeps trying */
	pthread_create(&net, NULL, net_loop, NULL);
	return 0;
}

void bus_close(void)
{
	if (!m)
		return;
	/* the network thread first, so only this thread uses the connection;
	   then what is still queued (a cleared state) and the goodbye */
	running = 0;
	pthread_join(net, NULL);
	if (connected) {
		mosquitto_loop(m, 100, 1);
		mosquitto_disconnect(m);
		mosquitto_loop(m, 100, 1);
	}
	mosquitto_destroy(m);
	m = NULL;
}
