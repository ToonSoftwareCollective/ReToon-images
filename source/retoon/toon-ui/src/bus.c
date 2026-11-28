// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: the connection to the Toon's own MQTT broker (mosquitto on
 * 127.0.0.1, see toon-hub's S41toon-broker), for the apps' retoon.publish
 * and retoon.subscribe. It is made the first time an app uses one of them.
 *
 * The connection lives in its own thread, like the one in mqtt.c; what
 * arrives is queued and handed out in the main loop by appbus_poll().
 */
#include <mosquitto.h>
#include <pthread.h>
#include <unistd.h>
#include "ui.h"

#define BUS_HOST	"127.0.0.1"
#define BUS_PORT	1883
#define MAX_PAYLOAD	(256 * 1024)

struct sub {
	char *pattern;
	int refs;
	struct sub *next;
};

struct item {
	int kind;			/* 0 message, 1 connected */
	char *topic, *payload;
	size_t len;
	bool retained;
	struct item *next;
};

static struct mosquitto *m;
static struct sub *subs;
static struct item *q_head, **q_tail = &q_head;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool online;

static void push(int kind, const struct mosquitto_message *msg)
{
	struct item *i = calloc(1, sizeof(*i));

	i->kind = kind;
	if (msg) {
		i->topic = strdup(msg->topic);
		i->payload = malloc(msg->payloadlen + 1);
		memcpy(i->payload, msg->payload, msg->payloadlen);
		i->payload[msg->payloadlen] = 0;
		i->len = msg->payloadlen;
		i->retained = msg->retain;
	}
	pthread_mutex_lock(&q_lock);
	*q_tail = i;
	q_tail = &i->next;
	pthread_mutex_unlock(&q_lock);
}

static void cb_connect(struct mosquitto *mo, void *arg, int rc)
{
	if (!rc) {
		online = true;
		push(1, NULL);
	}
}

static void cb_disconnect(struct mosquitto *mo, void *arg, int rc)
{
	online = false;
}

static void cb_message(struct mosquitto *mo, void *arg, const struct mosquitto_message *msg)
{
	if (msg->payloadlen <= MAX_PAYLOAD)
		push(0, msg);
}

static void *run(void *arg)
{
	for (;;) {
		if (!mosquitto_connect(m, BUS_HOST, BUS_PORT, 60))
			while (!mosquitto_loop(m, 1000, 1))
				;
		online = false;
		sleep(2);
	}
	return NULL;
}

/* the connection, made on first use; false if it cannot be made */
static bool start(void)
{
	static bool tried;
	char id[96] = "toon-ui-apps-";
	pthread_t th;

	if (m)
		return true;
	if (tried)
		return false;
	tried = true;
	mosquitto_lib_init();
	gethostname(id + 13, sizeof(id) - 14);
	m = mosquitto_new(id, true, NULL);
	if (!m)
		return false;
	mosquitto_threaded_set(m, true);
	mosquitto_connect_callback_set(m, cb_connect);
	mosquitto_disconnect_callback_set(m, cb_disconnect);
	mosquitto_message_callback_set(m, cb_message);
	if (pthread_create(&th, NULL, run, NULL)) {
		mosquitto_destroy(m);
		m = NULL;
		return false;
	}
	pthread_detach(th);
	return true;
}

bool appbus_publish(const char *topic, const void *payload, size_t len, bool retain)
{
	if (!start() || !online)
		return false;
	return mosquitto_publish(m, NULL, topic, (int)len, payload, 0, retain) == MOSQ_ERR_SUCCESS;
}

void appbus_subscribe(const char *pattern)
{
	struct sub *s;

	if (!start())
		return;
	for (s = subs; s; s = s->next)
		if (!strcmp(s->pattern, pattern)) {
			/* asked again: the broker sends the kept messages again, so a
			   second app gets the current state too */
			s->refs++;
			if (online)
				mosquitto_subscribe(m, NULL, pattern, 0);
			return;
		}
	s = calloc(1, sizeof(*s));
	s->pattern = strdup(pattern);
	s->refs = 1;
	s->next = subs;
	subs = s;
	if (online)
		mosquitto_subscribe(m, NULL, pattern, 0);
}

void appbus_unsubscribe(const char *pattern)
{
	for (struct sub **p = &subs; *p; p = &(*p)->next) {
		struct sub *s = *p;

		if (strcmp(s->pattern, pattern))
			continue;
		if (--s->refs > 0)
			return;
		*p = s->next;
		if (m && online)
			mosquitto_unsubscribe(m, NULL, s->pattern);
		free(s->pattern);
		free(s);
		return;
	}
}

bool appbus_online(void)
{
	return online;
}

/* what arrived, to cb; in the main loop */
void appbus_poll(appbus_cb cb)
{
	struct item *i, *next;

	if (!m)
		return;
	pthread_mutex_lock(&q_lock);
	i = q_head;
	q_head = NULL;
	q_tail = &q_head;
	pthread_mutex_unlock(&q_lock);
	for (; i; i = next) {
		next = i->next;
		if (i->kind == 1) {
			/* (again) connected: the broker forgot the subscriptions */
			for (struct sub *s = subs; s; s = s->next)
				mosquitto_subscribe(m, NULL, s->pattern, 0);
		} else {
			cb(i->topic, i->payload, i->len, i->retained);
		}
		free(i->topic);
		free(i->payload);
		free(i);
	}
}
