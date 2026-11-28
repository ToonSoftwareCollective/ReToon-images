// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: the entity table and the Home Assistant websocket link.
 *
 * Only the entities the dashboard uses are subscribed (subscribe_entities),
 * so a large installation costs nothing here. The access token and the
 * resolved address come from "/usr/bin/toon-ha token", run in the background.
 * Works over ws:// and wss://; a certificate is only checked when asked for
 * (verify=1 in ha.conf), since most local installations sign their own.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include "ui.h"

/* ---- entities ---------------------------------------------------------- */

struct bind {
	struct entity *e;
	void (*cb)(void *);
	void *user;
	struct bind *next;
};
static struct entity *entities;
static struct bind *binds;
static char extra[16][96];	/* wanted for the thermostat, not for a card */
static int nextra;
static char (*app_ids)[96];	/* wanted by apps (ent_want_apps) */
static int napp_ids;

/* what is wanted besides the cards: the thermostat's and the apps' entities */
static void want_extras(void)
{
	for (int i = 0; i < nextra; i++)
		ent_get(extra[i])->wanted = true;
	for (int i = 0; i < napp_ids; i++)
		ent_get(app_ids[i])->wanted = true;
}

struct entity *ent_get(const char *id)
{
	struct entity *e;

	for (e = entities; e; e = e->next)
		if (!strcmp(e->id, id))
			return e;
	e = calloc(1, sizeof(*e));
	snprintf(e->id, sizeof(e->id), "%s", id);
	e->attr = cJSON_CreateObject();
	e->next = entities;
	entities = e;
	return e;
}

struct entity *ent_find(const char *id)
{
	struct entity *e;

	for (e = entities; e; e = e->next)
		if (!strcmp(e->id, id))
			return e;
	return NULL;
}

void ent_set(struct entity *e, const char *state)
{
	if (e->state && state && !strcmp(e->state, state))
		return;
	free(e->state);
	e->state = state ? strdup(state) : NULL;
	e->dirty = true;
}

const char *ent_str(struct entity *e, const char *attr)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(e->attr, attr);

	return cJSON_IsString(v) ? v->valuestring : NULL;
}

double ent_num(struct entity *e, const char *attr, double def)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(e->attr, attr);

	return cJSON_IsNumber(v) ? v->valuedouble : def;
}

void ent_bind(const char *id, void (*cb)(void *), void *user)
{
	struct bind *b = calloc(1, sizeof(*b));

	b->e = ent_get(id);
	b->e->wanted = true;
	b->cb = cb;
	b->user = user;
	b->next = binds;
	binds = b;
}

void ent_unbind_all(void)
{
	struct entity *e;

	while (binds) {
		struct bind *b = binds;

		binds = b->next;
		free(b);
	}
	for (e = entities; e; e = e->next)
		e->wanted = false;
	want_extras();
}

void ent_want(const char *const *ids, int n)
{
	struct bind *b;
	struct entity *e;

	for (e = entities; e; e = e->next)
		e->wanted = false;
	for (b = binds; b; b = b->next)
		b->e->wanted = true;
	nextra = n > 16 ? 16 : n;
	for (int i = 0; i < nextra; i++)
		snprintf(extra[i], sizeof(extra[i]), "%s", ids[i]);
	want_extras();
}

bool ent_want_apps(const char *const *ids, int n)
{
	bool more = false;

	for (int i = 0; i < n; i++) {
		struct entity *e = ent_find(ids[i]);

		if (!e || !e->wanted)
			more = true;
	}
	free(app_ids);
	napp_ids = n > 0 ? n : 0;
	app_ids = napp_ids ? calloc(napp_ids, sizeof(*app_ids)) : NULL;
	for (int i = 0; i < napp_ids; i++)
		snprintf(app_ids[i], sizeof(app_ids[i]), "%s", ids[i]);
	want_extras();
	return more;
}

void ent_flush(void)
{
	struct entity *e;
	struct bind *b;

	for (b = binds; b; b = b->next)
		if (b->e->dirty)
			b->cb(b->user);
	for (e = entities; e; e = e->next) {
		if (e->dirty)
			e->gen++;
		e->dirty = false;
	}
}

/* ---- websocket --------------------------------------------------------- */

enum { H_IDLE, H_TOKEN, H_CONNECT, H_TLS, H_UPGRADE, H_AUTH, H_READY };
static int st = H_IDLE, fd = -1;
static uint32_t t_retry, t_rx, t_ping;
static char status[160] = "Not configured";
static char addr[64], host[128], token[4096];
static bool tls, verify;
static SSL_CTX *ctx;
static SSL *ssl;
static int port = 8123, next_id = 1;
static unsigned char *rx, *msg;
static size_t rx_len, rx_cap, msg_len;

/* questions waiting for their "result" (ha_request) */
struct pending {
	int id;
	ha_cb cb;
	void *user;
	struct pending *next;
};
static struct pending *pending;
static char err[200];

const char *ha_error(void) { return err; }

static void pending_fail(void)
{
	snprintf(err, sizeof(err), "the connection to Home Assistant was lost");
	while (pending) {
		struct pending *p = pending;

		pending = p->next;
		p->cb(NULL, p->user);
		free(p);
	}
}

bool ha_ready(void) { return st == H_READY; }

/* Home Assistant's web address for a request of our own (a camera picture):
   the URL start ("https://name:8123"), for curl's CURLOPT_RESOLVE the
   "name:port:address" that keeps the name for TLS, and whether to check the
   certificate. False when not connected. */
bool ha_http(char *base, size_t bn, char *resolve, size_t rn, bool *check)
{
	const char *name = host[0] ? host : addr;

	if (st != H_READY)
		return false;
	snprintf(base, bn, "%s://%s:%d", tls ? "https" : "http", name, port);
	snprintf(resolve, rn, "%s:%d:%s", name, port, addr);
	*check = verify;
	return true;
}
const char *ha_status(void) { return status; }

static void say(const char *s)
{
	snprintf(status, sizeof(status), "%s", s);
	set_status("ha", s);
}

static void drop(const char *why, int retry_s)
{
	if (ssl)
		SSL_free(ssl);
	ssl = NULL;
	if (fd >= 0)
		close(fd);
	fd = -1;
	st = H_IDLE;
	rx_len = msg_len = 0;
	t_retry = now_ms() + retry_s * 1000;
	if (why)
		say(why);
	pending_fail();
}

static int send_all(const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n;
		bool again;

		if (ssl) {
			n = SSL_write(ssl, p, len);
			again = n <= 0 && (SSL_get_error(ssl, n) == SSL_ERROR_WANT_WRITE ||
					   SSL_get_error(ssl, n) == SSL_ERROR_WANT_READ);
		} else {
			n = send(fd, p, len, MSG_NOSIGNAL);
			again = n < 0 && errno == EAGAIN;
		}
		if (n > 0) {
			p += n;
			len -= n;
		} else if (again) {
			struct pollfd pf = { .fd = fd, .events = POLLOUT | (ssl ? POLLIN : 0) };

			if (poll(&pf, 1, 2000) <= 0)
				return -1;
		} else if (ssl || errno != EINTR) {
			return -1;
		}
	}
	return 0;
}

static int ws_send(int opcode, const char *data, size_t len)
{
	unsigned char *f = malloc(len + 14);
	size_t h = 2;
	uint32_t mask = lv_rand(1, 0x7fffffff);
	int ret;

	f[0] = 0x80 | opcode;
	if (len < 126) {
		f[1] = 0x80 | len;
	} else if (len < 65536) {
		f[1] = 0x80 | 126;
		f[2] = len >> 8;
		f[3] = len;
		h = 4;
	} else {
		f[1] = 0x80 | 127;
		memset(f + 2, 0, 4);
		f[6] = len >> 24;
		f[7] = len >> 16;
		f[8] = len >> 8;
		f[9] = len;
		h = 10;
	}
	memcpy(f + h, &mask, 4);
	for (size_t i = 0; i < len; i++)
		f[h + 4 + i] = data[i] ^ ((unsigned char *)&mask)[i & 3];
	ret = send_all(f, h + 4 + len);
	free(f);
	return ret;
}

static void send_json(cJSON *o)
{
	char *s = cJSON_PrintUnformatted(o);

	if (s && ws_send(1, s, strlen(s)))
		drop("Connection lost", 5);
	free(s);
	cJSON_Delete(o);
}

void ha_call(const char *domain, const char *service, const char *entity, cJSON *data)
{
	cJSON *o, *t;

	if (entity && is_local(entity)) {
		local_call(domain, service, entity, data);
		cJSON_Delete(data);
		return;
	}
	if (entity && is_mqtt(entity)) {
		mqtt_call(domain, service, entity, data);
		cJSON_Delete(data);
		return;
	}
	if (st != H_READY) {
		cJSON_Delete(data);
		return;
	}
	o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "id", next_id++);
	cJSON_AddStringToObject(o, "type", "call_service");
	cJSON_AddStringToObject(o, "domain", domain);
	cJSON_AddStringToObject(o, "service", service);
	if (data)
		cJSON_AddItemToObject(o, "service_data", data);
	if (entity) {
		t = cJSON_AddObjectToObject(o, "target");
		cJSON_AddStringToObject(t, "entity_id", entity);
	}
	send_json(o);
}

bool ha_request(cJSON *o, ha_cb cb, void *user)
{
	struct pending *p;
	int id = next_id++;

	if (st != H_READY) {
		cJSON_Delete(o);
		return false;
	}
	cJSON_AddNumberToObject(o, "id", id);
	send_json(o);
	if (st != H_READY)	/* the link went down while sending */
		return false;
	p = calloc(1, sizeof(*p));
	p->id = id;
	p->cb = cb;
	p->user = user;
	p->next = pending;
	pending = p;
	return true;
}

/*
 * Home Assistant's display precision of each entity ("Display precision" in
 * its entity settings, or the integration's suggested one), from its entity
 * registry as its own frontend gets it: {"sensor.x": 3, ...}. Asked when
 * connected and every ten minutes after.
 */
static cJSON *precision;
static uint32_t t_prec;

static void on_registry(cJSON *res, void *user)
{
	cJSON *list = cJSON_GetObjectItemCaseSensitive(res, "entities"), *it, *p;

	(void)user;
	if (!cJSON_IsArray(list))
		return;
	p = cJSON_CreateObject();
	cJSON_ArrayForEach(it, list) {
		cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "ei");
		cJSON *dp = cJSON_GetObjectItemCaseSensitive(it, "dp");

		if (cJSON_IsString(id) && cJSON_IsNumber(dp) && dp->valueint >= 0 && dp->valueint <= 9 &&
		    !cJSON_GetObjectItemCaseSensitive(p, id->valuestring))
			cJSON_AddNumberToObject(p, id->valuestring, dp->valueint);
	}
	/* shown again where it changed */
	for (struct entity *e = entities; e; e = e->next) {
		cJSON *a = cJSON_GetObjectItemCaseSensitive(precision, e->id);
		cJSON *b = cJSON_GetObjectItemCaseSensitive(p, e->id);

		if ((a ? a->valueint : -1) != (b ? b->valueint : -1))
			e->dirty = true;
	}
	cJSON_Delete(precision);
	precision = p;
}

static void ask_registry(void)
{
	cJSON *o = cJSON_CreateObject();

	cJSON_AddStringToObject(o, "type", "config/entity_registry/list_for_display");
	t_prec = now_ms();
	ha_request(o, on_registry, NULL);
}

/* the decimals Home Assistant shows for this entity; -1: none set */
int ent_precision(struct entity *e)
{
	cJSON *v = e ? cJSON_GetObjectItemCaseSensitive(precision, e->id) : NULL;

	return cJSON_IsNumber(v) ? v->valueint : -1;
}

static void on_result(cJSON *m)
{
	cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");

	for (struct pending **pp = &pending; *pp; pp = &(*pp)->next) {
		struct pending *p = *pp;
		cJSON *msg;

		if (!cJSON_IsNumber(id) || p->id != id->valueint)
			continue;
		msg = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "error"), "message");
		snprintf(err, sizeof(err), "%s", cJSON_IsString(msg) ? msg->valuestring : "Home Assistant refused it");
		*pp = p->next;
		p->cb(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "success")) ?
		      cJSON_GetObjectItemCaseSensitive(m, "result") : NULL, p->user);
		free(p);
		return;
	}
}

static void subscribe(void)
{
	cJSON *o = cJSON_CreateObject(), *ids;
	struct entity *e;
	int n = 0;

	cJSON_AddNumberToObject(o, "id", next_id++);
	cJSON_AddStringToObject(o, "type", "subscribe_entities");
	ids = cJSON_AddArrayToObject(o, "entity_ids");
	for (e = entities; e; e = e->next)
		if (e->wanted && !is_local(e->id) && !is_mqtt(e->id)) {
			cJSON_AddItemToArray(ids, cJSON_CreateString(e->id));
			n++;
		}
	if (n)
		send_json(o);
	else
		cJSON_Delete(o);
}

/* one entity in the compressed state format: {"s": state, "a": {attributes}} */
static void apply_state(struct entity *e, cJSON *v, bool replace)
{
	cJSON *s = cJSON_GetObjectItemCaseSensitive(v, "s");
	cJSON *a = cJSON_GetObjectItemCaseSensitive(v, "a"), *i;

	if (cJSON_IsString(s))
		ent_set(e, s->valuestring);
	if (!cJSON_IsObject(a))
		return;
	if (replace) {
		cJSON_Delete(e->attr);
		e->attr = cJSON_Duplicate(a, 1);
	} else {
		cJSON_ArrayForEach(i, a) {
			cJSON_DeleteItemFromObjectCaseSensitive(e->attr, i->string);
			cJSON_AddItemToObject(e->attr, i->string, cJSON_Duplicate(i, 1));
		}
	}
	e->dirty = true;
}

static void on_event(cJSON *ev)
{
	cJSON *a = cJSON_GetObjectItemCaseSensitive(ev, "a");
	cJSON *c = cJSON_GetObjectItemCaseSensitive(ev, "c");
	cJSON *r = cJSON_GetObjectItemCaseSensitive(ev, "r"), *i, *k;

	cJSON_ArrayForEach(i, a)
		apply_state(ent_get(i->string), i, true);
	cJSON_ArrayForEach(i, c) {
		struct entity *e = ent_get(i->string);
		cJSON *plus = cJSON_GetObjectItemCaseSensitive(i, "+");
		cJSON *minus = cJSON_GetObjectItemCaseSensitive(i, "-");

		if (plus)
			apply_state(e, plus, false);
		cJSON_ArrayForEach(k, cJSON_GetObjectItemCaseSensitive(minus, "a"))
			if (cJSON_IsString(k)) {
				cJSON_DeleteItemFromObjectCaseSensitive(e->attr, k->valuestring);
				e->dirty = true;
			}
	}
	cJSON_ArrayForEach(i, r)
		if (cJSON_IsString(i))
			ent_set(ent_get(i->valuestring), "unavailable");
	ent_flush();
}

static void on_message(const char *text, size_t len)
{
	cJSON *m = cJSON_ParseWithLength(text, len), *t;
	const char *type;

	if (!m)
		return;
	t = cJSON_GetObjectItemCaseSensitive(m, "type");
	type = cJSON_IsString(t) ? t->valuestring : "";
	if (!strcmp(type, "auth_required")) {
		cJSON *o = cJSON_CreateObject();

		cJSON_AddStringToObject(o, "type", "auth");
		cJSON_AddStringToObject(o, "access_token", token);
		send_json(o);
	} else if (!strcmp(type, "auth_ok")) {
		st = H_READY;
		say("Connected");
		subscribe();
		ask_registry();
	} else if (!strcmp(type, "auth_invalid")) {
		drop("Home Assistant refused the login", 60);
	} else if (!strcmp(type, "event")) {
		on_event(cJSON_GetObjectItemCaseSensitive(m, "event"));
	} else if (!strcmp(type, "result")) {
		on_result(m);
	}
	cJSON_Delete(m);
}

/* Take complete frames out of rx; returns -1 when the link must be dropped. */
static int parse_frames(void)
{
	for (;;) {
		size_t h = 2, len;
		int op;

		if (rx_len < 2)
			return 0;
		len = rx[1] & 0x7f;
		if (len == 126) {
			if (rx_len < 4)
				return 0;
			len = rx[2] << 8 | rx[3];
			h = 4;
		} else if (len == 127) {
			if (rx_len < 10)
				return 0;
			len = (size_t)rx[6] << 24 | rx[7] << 16 | rx[8] << 8 | rx[9];
			h = 10;
		}
		if (rx[1] & 0x80)
			return -1;	/* a server never masks */
		if (rx_len < h + len)
			return 0;
		op = rx[0] & 0x0f;
		if (op == 8)
			return -1;
		if (op == 9) {
			ws_send(10, (char *)rx + h, len);
		} else if (op <= 2) {
			msg = realloc(msg, msg_len + len + 1);
			memcpy(msg + msg_len, rx + h, len);
			msg_len += len;
			if (rx[0] & 0x80) {
				on_message((char *)msg, msg_len);
				msg_len = 0;
				if (fd < 0)
					return 0;
			}
		}
		memmove(rx, rx + h + len, rx_len - h - len);
		rx_len -= h + len;
	}
}

static void on_token(int status_code, char *out, void *user)
{
	char *line, *save;
	struct sockaddr_in sa = { .sin_family = AF_INET };
	char err[120] = "";

	(void)user;
	if (st != H_TOKEN)
		return;
	addr[0] = host[0] = token[0] = 0;
	port = 8123;
	tls = verify = false;
	for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(line, "addr=", 5))
			snprintf(addr, sizeof(addr), "%s", line + 5);
		else if (!strncmp(line, "port=", 5))
			port = atoi(line + 5);
		else if (!strncmp(line, "host=", 5))
			snprintf(host, sizeof(host), "%s", line + 5);
		else if (!strncmp(line, "tls=", 4))
			tls = atoi(line + 4);
		else if (!strncmp(line, "verify=", 7))
			verify = atoi(line + 7);
		else if (!strncmp(line, "token=", 6))
			snprintf(token, sizeof(token), "%s", line + 6);
		else if (!strncmp(line, "error=", 6))
			snprintf(err, sizeof(err), "%s", line + 6);
	}
	if (status_code || !addr[0] || !token[0]) {
		drop(err[0] ? err : "Not configured", err[0] ? 30 : 10);
		return;
	}
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
		drop("Bad Home Assistant address", 30);
		return;
	}
	{	/* the web interface says where the Toon itself tries to go */
		char where[80];

		snprintf(where, sizeof(where), "%s:%d", addr, port);
		set_status("ha_addr", where);
	}
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0 || (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) && errno != EINPROGRESS)) {
		drop("Home Assistant is not reachable", 20);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));
	st = H_CONNECT;
	t_rx = now_ms();
	say("Connecting");
}

void ha_restart(void)
{
	drop("Connecting", 0);
}

void ha_init(void)
{
	t_retry = now_ms();
}

void ha_poll(void)
{
	static char *token_cmd[] = { "/usr/bin/toon-ha", "token", NULL };
	uint32_t now = now_ms();
	struct pollfd pf;

	if (st == H_IDLE) {
		if ((int32_t)(now - t_retry) >= 0 && !job_run(token_cmd, on_token, NULL))
			st = H_TOKEN;
		return;
	}
	if (st == H_TOKEN)
		return;
	if (st == H_READY && (int32_t)(now - t_prec) > 600000)
		ask_registry();		/* a precision changed in Home Assistant */

	if (st == H_CONNECT) {
		int err = 0;
		socklen_t l = sizeof(err);

		pf = (struct pollfd){ .fd = fd, .events = POLLOUT };
		if (poll(&pf, 1, 0) <= 0) {
			if (now - t_rx > 8000)
				drop("Home Assistant is not reachable", 20);
			return;
		}
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
		if (err) {
			drop("Home Assistant is not reachable", 20);
			return;
		}
		st = H_TLS;
		if (tls) {
			if (!ctx)
				ctx = SSL_CTX_new(TLS_client_method());
			SSL_CTX_set_default_verify_paths(ctx);
			ssl = SSL_new(ctx);
			SSL_set_verify(ssl, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
			SSL_set_fd(ssl, fd);
			if (strspn(host, "0123456789.") != strlen(host))
				SSL_set_tlsext_host_name(ssl, host);
			if (verify)
				SSL_set1_host(ssl, host);
		}
	}

	if (st == H_TLS) {
		char req[512];

		if (ssl) {
			int r = SSL_connect(ssl), e = r == 1 ? 0 : SSL_get_error(ssl, r);

			if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
				if (now - t_rx > 10000)
					drop("Home Assistant is not reachable", 20);
				return;
			}
			if (e) {
				drop(verify && SSL_get_verify_result(ssl) != X509_V_OK ?
				     "The certificate of Home Assistant is not trusted" :
				     "No secure connection to Home Assistant", 60);
				return;
			}
		}
		snprintf(req, sizeof(req),
			 "GET /api/websocket HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
			 "Connection: Upgrade\r\nSec-WebSocket-Key: dG9vbi11aS13cy1rZXkxNg==\r\n"
			 "Sec-WebSocket-Version: 13\r\n\r\n", host[0] ? host : addr, port);
		if (send_all(req, strlen(req))) {
			drop("Home Assistant is not reachable", 20);
			return;
		}
		st = H_UPGRADE;
		t_rx = now;
	}

	for (;;) {
		ssize_t n;

		if (rx_cap - rx_len < 16384) {
			rx_cap += 65536;
			rx = realloc(rx, rx_cap);
		}
		if (ssl) {
			n = SSL_read(ssl, rx + rx_len, rx_cap - rx_len);
			if (n <= 0 && SSL_get_error(ssl, n) == SSL_ERROR_WANT_READ)
				break;
		} else {
			n = recv(fd, rx + rx_len, rx_cap - rx_len, 0);
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				break;
		}
		if (n <= 0) {
			drop("Connection lost", 5);
			return;
		}
		rx_len += n;
		t_rx = now;
		if (st == H_UPGRADE) {
			unsigned char *end = memmem(rx, rx_len, "\r\n\r\n", 4);

			if (!end)
				continue;
			if (rx_len < 12 || memcmp(rx + 9, "101", 3)) {
				drop("That address is not Home Assistant", 60);
				return;
			}
			rx_len -= end + 4 - rx;
			memmove(rx, end + 4, rx_len);
			st = H_AUTH;
		}
		if (parse_frames()) {
			drop("Connection lost", 5);
			return;
		}
		if (fd < 0)
			return;
	}

	if (st == H_READY && now - t_ping > 30000) {
		cJSON *o = cJSON_CreateObject();

		t_ping = now;
		cJSON_AddNumberToObject(o, "id", next_id++);
		cJSON_AddStringToObject(o, "type", "ping");
		send_json(o);
	}
	if (fd >= 0 && now - t_rx > 75000)
		drop("Connection lost", 2);
}
