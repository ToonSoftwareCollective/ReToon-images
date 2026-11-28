// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: values straight from an MQTT broker as "mqtt.*" entities, so
 * pages can show and switch things without Home Assistant in between.
 *
 * Two sources:
 *  - Home Assistant's discovery topics (<prefix>/<component>/.../config),
 *    which zigbee2mqtt, Tasmota, ESPHome and many others publish. Everything
 *    found there is listed in the web interface.
 *  - topics the user described in mqtt-topics.json on p4.
 *
 * Only the state topics of entities that are on a page are subscribed. Value
 * templates are not run: the part "value_json.a.b" is taken out of them and
 * used as the place of the value in the JSON message, which covers what
 * devices publish in practice.
 *
 * The broker settings are those of toon-mqttd (mqtt.conf). The connection
 * lives in its own thread; what arrives is handled in the main loop.
 */
#include <ctype.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mosquitto.h>
#include "ui.h"

#define MQTT_CONF	CONFIG_DIR "/mqtt.conf"
#define TOPICS_FILE	CONFIG_DIR "/mqtt-topics.json"
#define LIST_FILE	"/run/toon/mqtt-entities.json"

struct mq {
	char id[96], comp[24];
	char *name, *unit, *dclass, *icon;
	char *state_t, *cmd_t, *path;		/* path: keys separated by \n, NULL = whole message */
	char *pl_on, *pl_off, *st_on, *st_off, *pl_press;
	char *bri_state_t, *bri_cmd_t, *bri_path;
	int bri_scale;
	bool json, manual, subscribed;
	struct mq *next;
};
static struct mq *list;
static int nlist;

/* one broker connection; a new one is made when the settings change */
struct conn {
	struct mosquitto *m;
	char host[128], user[128], pass[256];
	int port;
	bool local;		/* the Toon's own broker: no broker was set */
	volatile bool quit;
};
static struct conn *cur;

struct item {
	struct conn *from;
	int kind;				/* 0 message, 1 connected, 2 disconnected */
	char *topic, *payload;
	size_t len;
	struct item *next;
};
static struct item *q_head, **q_tail = &q_head;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;

static char prefix[64] = "homeassistant", own_base[80], own_idbase[80];
static bool online, list_dirty;
static uint32_t t_list, t_check;
static time_t mt_conf, mt_topics;

static time_t mtime(const char *path)
{
	struct stat sb;

	return stat(path, &sb) ? 0 : sb.st_mtime;
}

/* ---- the connection thread ---------------------------------------------- */

static void push(struct conn *c, int kind, const char *topic, const void *payload, size_t len)
{
	struct item *i = calloc(1, sizeof(*i));

	i->from = c;
	i->kind = kind;
	if (topic) {
		i->topic = strdup(topic);
		i->payload = malloc(len + 1);
		memcpy(i->payload, payload, len);
		i->payload[len] = 0;
		i->len = len;
	}
	pthread_mutex_lock(&q_lock);
	*q_tail = i;
	q_tail = &i->next;
	pthread_mutex_unlock(&q_lock);
}

static void cb_connect(struct mosquitto *m, void *arg, int rc)
{
	push(arg, rc ? 2 : 1, NULL, NULL, 0);
}

static void cb_disconnect(struct mosquitto *m, void *arg, int rc)
{
	push(arg, 2, NULL, NULL, 0);
}

static void cb_message(struct mosquitto *m, void *arg, const struct mosquitto_message *msg)
{
	if (msg->payloadlen <= 256 * 1024)
		push(arg, 0, msg->topic, msg->payload, msg->payloadlen);
}

static void *run(void *arg)
{
	struct conn *c = arg;

	while (!c->quit) {
		if (!mosquitto_connect(c->m, c->host, c->port, 60))
			while (!c->quit && !mosquitto_loop(c->m, 500, 1))
				;
		for (int i = 0; i < 50 && !c->quit; i++)
			usleep(100000);
	}
	mosquitto_disconnect(c->m);
	mosquitto_destroy(c->m);
	c->m = NULL;		/* the struct itself stays: queued items may still point at it */
	return NULL;
}

static void conn_start(void)
{
	char buf[64], id[96] = "toon-ui-";
	struct conn *c;
	pthread_t th;

	if (cur)
		cur->quit = true;	/* its thread cleans up */
	cur = NULL;
	online = false;
	for (struct mq *e = list; e; e = e->next)
		e->subscribed = false;
	c = calloc(1, sizeof(*c));
	if (file_value(MQTT_CONF, "host", c->host, sizeof(c->host)) || !c->host[0]) {
		/* no broker set: the Toon's own, for devices that use that one */
		snprintf(c->host, sizeof(c->host), "127.0.0.1");
		c->port = 1883;
		c->local = true;
		snprintf(prefix, sizeof(prefix), "homeassistant");
	} else {
		c->port = file_value(MQTT_CONF, "port", buf, sizeof(buf)) ? 1883 : atoi(buf);
		if (c->port <= 0)
			c->port = 1883;
		file_value(MQTT_CONF, "username", c->user, sizeof(c->user));
		file_value(MQTT_CONF, "password", c->pass, sizeof(c->pass));
		if (file_value(MQTT_CONF, "discovery_prefix", prefix, sizeof(prefix)) || !prefix[0])
			snprintf(prefix, sizeof(prefix), "homeassistant");
	}
	gethostname(id + 8, sizeof(id) - 9);
	c->m = mosquitto_new(id, true, c);
	if (!c->m) {
		free(c);
		return;
	}
	mosquitto_threaded_set(c->m, true);
	if (c->user[0])
		mosquitto_username_pw_set(c->m, c->user, c->pass[0] ? c->pass : NULL);
	mosquitto_connect_callback_set(c->m, cb_connect);
	mosquitto_disconnect_callback_set(c->m, cb_disconnect);
	mosquitto_message_callback_set(c->m, cb_message);
	set_status("mqtt", c->local ? "Not configured" : "Connecting");
	cur = c;
	if (pthread_create(&th, NULL, run, c)) {
		cur = NULL;
		return;
	}
	pthread_detach(th);
}

/* ---- entities ------------------------------------------------------------- */

static struct mq *mq_find(const char *id)
{
	struct mq *e;

	for (e = list; e; e = e->next)
		if (!strcmp(e->id, id))
			return e;
	return NULL;
}

int mqtt_state(void)
{
	return !cur || (cur->local && !nlist) ? 0 : online ? 2 : 1;
}

/* the text for the web Status page and the info screen */
static const char *status_now(void)
{
	static char buf[64];

	if (!cur)
		return "Not configured";
	if (cur->local)		/* only worth a word when devices use it */
		snprintf(buf, sizeof(buf), !online ? "Not configured" :
			 nlist ? "Own broker, %d values found" : "Not configured", nlist);
	else if (!online)
		snprintf(buf, sizeof(buf), "Broker not reachable");
	else
		snprintf(buf, sizeof(buf), "Connected, %d values found", nlist);
	return buf;
}

const char *mqtt_status(void)
{
	return status_now();
}

const char *mqtt_domain(const char *id)
{
	struct mq *e = mq_find(id);

	return e ? e->comp : "sensor";
}

static void set(char **dst, const char *src)
{
	free(*dst);
	*dst = src && src[0] ? strdup(src) : NULL;
}

static void mq_clear(struct mq *e)
{
	char **f[] = { &e->name, &e->unit, &e->dclass, &e->icon, &e->state_t, &e->cmd_t, &e->path,
		       &e->pl_on, &e->pl_off, &e->st_on, &e->st_off, &e->pl_press,
		       &e->bri_state_t, &e->bri_cmd_t, &e->bri_path };

	for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
		set(f[i], NULL);
}

static void mq_remove(const char *id)
{
	struct mq **pp = &list, *e;

	for (; (e = *pp); pp = &e->next)
		if (!strcmp(e->id, id)) {
			if (e->subscribed && cur && online && e->state_t)
				mosquitto_unsubscribe(cur->m, NULL, e->state_t);
			*pp = e->next;
			mq_clear(e);
			free(e);
			nlist--;
			list_dirty = true;
			return;
		}
}

static struct mq *mq_new(const char *id)
{
	struct mq *e = mq_find(id);

	if (e) {
		bool sub = e->subscribed;

		mq_clear(e);
		e->subscribed = sub;
		return e;
	}
	e = calloc(1, sizeof(*e));
	snprintf(e->id, sizeof(e->id), "%s", id);
	e->next = list;
	list = e;
	nlist++;
	return e;
}

static void put_attr(struct entity *e, const char *key, const char *val)
{
	cJSON_DeleteItemFromObjectCaseSensitive(e->attr, key);
	if (val)
		cJSON_AddStringToObject(e->attr, key, val);
}

/* An entity that is on a page: tell the cards about it and listen to it. */
static void mq_attach(struct mq *m)
{
	struct entity *e = ent_find(m->id);

	if (!e || !e->wanted)
		return;
	put_attr(e, "friendly_name", m->name);
	put_attr(e, "unit_of_measurement", m->unit);
	put_attr(e, "device_class", m->dclass);
	put_attr(e, "icon", m->icon);
	e->dirty = true;
	if (!m->state_t && !e->state)
		ent_set(e, !strcmp(m->comp, "button") ? "unknown" : "off");
	if (cur && online && !m->subscribed) {
		if (m->state_t)
			mosquitto_subscribe(cur->m, NULL, m->state_t, 0);
		if (m->bri_state_t && (!m->state_t || strcmp(m->bri_state_t, m->state_t)))
			mosquitto_subscribe(cur->m, NULL, m->bri_state_t, 0);
		m->subscribed = true;
	}
}

/* after a new dashboard: drop what is no longer shown, add what is new */
void mqtt_restart(void)
{
	struct mq *m;

	for (m = list; m; m = m->next) {
		struct entity *e = ent_find(m->id);

		if (m->subscribed && !(e && e->wanted)) {
			if (cur && online && m->state_t)
				mosquitto_unsubscribe(cur->m, NULL, m->state_t);
			m->subscribed = false;
		}
		mq_attach(m);
	}
	ent_flush();
}

/* ---- templates and values ------------------------------------------------- */

/* "{{ value_json.a['b'][0] | float }}" -> "a\nb\n0"; NULL when it uses the whole message */
static char *tpl_path(const char *tpl)
{
	const char *p = tpl ? strstr(tpl, "value_json") : NULL;
	char out[256];
	size_t n = 0;

	if (!p)
		return NULL;
	p += 10;
	while (n < sizeof(out) - 2) {
		const char *s;

		if (*p == '.') {
			s = ++p;
			while (isalnum((unsigned char)*p) || *p == '_')
				p++;
		} else if (*p == '[') {
			char quote = p[1];

			if (quote == '\'' || quote == '"') {
				s = p += 2;
				while (*p && *p != quote)
					p++;
			} else {
				s = ++p;
				while (isdigit((unsigned char)*p))
					p++;
			}
		} else {
			break;
		}
		if (p == s || n + (p - s) + 2 > sizeof(out))
			break;
		if (n)
			out[n++] = '\n';
		memcpy(out + n, s, p - s);
		n += p - s;
		if (*p == '\'' || *p == '"')
			p++;
		if (*p == ']')
			p++;
	}
	out[n] = 0;
	return n ? strdup(out) : NULL;
}

/* the value at path in a message; -1 when it is not there */
static int value_at(const char *payload, const char *path, char *out, size_t len)
{
	cJSON *doc, *v;
	char keys[256], *k, *save;
	int ret = -1;

	if (!path) {
		while (isspace((unsigned char)*payload))
			payload++;
		snprintf(out, len, "%s", payload);
		while (out[0] && isspace((unsigned char)out[strlen(out) - 1]))
			out[strlen(out) - 1] = 0;
		return 0;
	}
	v = doc = cJSON_Parse(payload);
	snprintf(keys, sizeof(keys), "%s", path);
	for (k = strtok_r(keys, "\n", &save); k && v; k = strtok_r(NULL, "\n", &save))
		v = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, atoi(k)) :
				       cJSON_GetObjectItemCaseSensitive(v, k);
	if (v && !cJSON_IsNull(v)) {
		if (cJSON_IsString(v)) {
			snprintf(out, len, "%s", v->valuestring);
		} else {
			char *s = cJSON_PrintUnformatted(v);

			snprintf(out, len, "%s", s ? s : "");
			free(s);
		}
		ret = 0;
	}
	cJSON_Delete(doc);
	return ret;
}

static bool on_off(struct mq *m)
{
	return !strcmp(m->comp, "switch") || !strcmp(m->comp, "light") ||
	       !strcmp(m->comp, "binary_sensor") || !strcmp(m->comp, "fan") ||
	       !strcmp(m->comp, "siren");
}

static void on_state(struct mq *m, const char *topic, const char *payload)
{
	struct entity *e = ent_find(m->id);
	char val[512];

	if (!e || !e->wanted)
		return;
	if (m->bri_state_t && !strcmp(topic, m->bri_state_t) &&
	    !value_at(payload, m->bri_path, val, sizeof(val)) && m->bri_scale > 0) {
		double b = atof(val) * 255 / m->bri_scale;

		cJSON_DeleteItemFromObjectCaseSensitive(e->attr, "brightness");
		cJSON_AddNumberToObject(e->attr, "brightness", LV_CLAMP(0, b, 255));
		e->dirty = true;
	}
	if (!m->state_t || strcmp(topic, m->state_t) || value_at(payload, m->path, val, sizeof(val)))
		return;
	if (on_off(m)) {
		if (!strcasecmp(val, m->st_on ? m->st_on : "ON"))
			ent_set(e, "on");
		else if (!strcasecmp(val, m->st_off ? m->st_off : "OFF"))
			ent_set(e, "off");
	} else {
		ent_set(e, val);
	}
}

/* ---- discovery -------------------------------------------------------------- */

static cJSON *cfg_get(cJSON *cfg, const char *name, const char *abbr)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, name);

	return v ? v : cJSON_GetObjectItemCaseSensitive(cfg, abbr);
}

/* text of a setting; numbers and booleans as HA would compare them */
static const char *cfg_str(cJSON *cfg, const char *name, const char *abbr, char *buf, size_t len)
{
	cJSON *v = cfg_get(cfg, name, abbr);

	if (cJSON_IsString(v))
		return v->valuestring;
	if (cJSON_IsBool(v))
		return cJSON_IsTrue(v) ? "true" : "false";
	if (cJSON_IsNumber(v)) {
		snprintf(buf, len, "%g", v->valuedouble);
		return buf;
	}
	return NULL;
}

/* a topic setting, with the "~" short form filled in */
static void cfg_topic(char **dst, cJSON *cfg, cJSON *top, const char *name, const char *abbr)
{
	cJSON *v = cfg_get(cfg, name, abbr), *b = cJSON_GetObjectItemCaseSensitive(cfg, "~");
	char out[512];
	const char *t, *base;
	size_t n;

	if (!b)
		b = cJSON_GetObjectItemCaseSensitive(top, "~");
	base = cJSON_IsString(b) ? b->valuestring : "";
	set(dst, NULL);
	if (!cJSON_IsString(v) || !v->valuestring[0])
		return;
	t = v->valuestring;
	n = strlen(t);
	if (t[0] == '~')
		snprintf(out, sizeof(out), "%s%s", base, t + 1);
	else if (t[n - 1] == '~')
		snprintf(out, sizeof(out), "%.*s%s", (int)n - 1, t, base);
	else
		snprintf(out, sizeof(out), "%s", t);
	set(dst, out);
}

static void make_id(char *out, size_t len, const char *a, const char *b)
{
	size_t n;

	snprintf(out, len, "mqtt.%s%s%s", a, a[0] && b[0] ? "_" : "", b);
	for (n = 5; out[n]; n++)
		out[n] = isalnum((unsigned char)out[n]) ? tolower((unsigned char)out[n]) : '_';
}

static void add_discovered(const char *id, const char *comp, cJSON *cfg, cJSON *top)
{
	cJSON *dev = cfg_get(top, "device", "dev"), *v;
	const char *dname = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(dev, "name")) ?
			    cJSON_GetObjectItemCaseSensitive(dev, "name")->valuestring : "";
	const char *name, *s;
	char buf[64], full[200];
	struct mq *m;
	size_t own = strlen(own_base), ownid = strlen(own_idbase);

	m = mq_new(id);
	m->manual = false;
	snprintf(m->comp, sizeof(m->comp), "%s", comp);
	cfg_topic(&m->state_t, cfg, top, "state_topic", "stat_t");
	cfg_topic(&m->cmd_t, cfg, top, "command_topic", "cmd_t");
	if ((m->state_t && !strncmp(m->state_t, own_base, own)) ||
	    (m->cmd_t && !strncmp(m->cmd_t, own_base, own)) ||
	    (ownid && m->state_t && !strncmp(m->state_t, own_idbase, ownid)) ||
	    (ownid && m->cmd_t && !strncmp(m->cmd_t, own_idbase, ownid)) || (!m->state_t && !m->cmd_t)) {
		mq_remove(id);		/* this Toon's own values are toon.* already */
		return;
	}
	name = cfg_str(cfg, "name", "name", buf, sizeof(buf));
	if (name && dname[0] && strncmp(name, dname, strlen(dname)))
		snprintf(full, sizeof(full), "%s %s", dname, name);
	else
		snprintf(full, sizeof(full), "%s", name ? name : dname[0] ? dname : id + 5);
	set(&m->name, full);
	set(&m->unit, cfg_str(cfg, "unit_of_measurement", "unit_of_meas", buf, sizeof(buf)));
	set(&m->dclass, cfg_str(cfg, "device_class", "dev_cla", buf, sizeof(buf)));
	set(&m->icon, cfg_str(cfg, "icon", "ic", buf, sizeof(buf)));
	s = cfg_str(cfg, "value_template", "val_tpl", buf, sizeof(buf));
	if (!s)
		s = cfg_str(cfg, "state_value_template", "stat_val_tpl", buf, sizeof(buf));
	m->path = tpl_path(s);
	set(&m->pl_on, cfg_str(cfg, "payload_on", "pl_on", buf, sizeof(buf)));
	set(&m->pl_off, cfg_str(cfg, "payload_off", "pl_off", buf, sizeof(buf)));
	s = cfg_str(cfg, "state_on", "stat_on", buf, sizeof(buf));
	set(&m->st_on, s ? s : m->pl_on);
	s = cfg_str(cfg, "state_off", "stat_off", buf, sizeof(buf));
	set(&m->st_off, s ? s : m->pl_off);
	set(&m->pl_press, cfg_str(cfg, "payload_press", "pl_prs", buf, sizeof(buf)));

	m->json = false;
	m->bri_scale = 255;
	if (!strcmp(comp, "light")) {
		s = cfg_str(cfg, "schema", "schema", buf, sizeof(buf));
		m->json = s && !strcmp(s, "json");
		v = cfg_get(cfg, "brightness_scale", "bri_scl");
		if (cJSON_IsNumber(v) && v->valuedouble > 0)
			m->bri_scale = v->valueint;
		if (m->json) {
			set(&m->path, "state");
			set(&m->st_on, "ON");
			set(&m->st_off, "OFF");
			set(&m->bri_state_t, m->state_t);
			set(&m->bri_path, "brightness");
		} else {
			cfg_topic(&m->bri_state_t, cfg, top, "brightness_state_topic", "bri_stat_t");
			cfg_topic(&m->bri_cmd_t, cfg, top, "brightness_command_topic", "bri_cmd_t");
			m->bri_path = tpl_path(cfg_str(cfg, "brightness_value_template", "bri_val_tpl",
						       buf, sizeof(buf)));
		}
	}
	list_dirty = true;
	mq_attach(m);
}

/* <prefix>/<component>/[<node>/]<object>/config */
static void on_config(const char *rest, const char *payload, size_t len)
{
	char parts[4][96] = { "", "", "", "" }, id[96];
	const char *node = "", *obj;
	cJSON *cfg, *cmps, *c;
	int n = 0;

	while (n < 4 && *rest) {
		size_t l = strcspn(rest, "/");

		snprintf(parts[n++], sizeof(parts[0]), "%.*s", (int)l, rest);
		rest += l + (rest[l] == '/');
	}
	if (*rest || n < 3 || strcmp(parts[n - 1], "config"))
		return;
	obj = parts[n - 2];
	if (n == 4)
		node = parts[1];
	make_id(id, sizeof(id), node, obj);

	cfg = len ? cJSON_ParseWithLength(payload, len) : NULL;
	if (strcmp(parts[0], "device")) {
		if (cJSON_IsObject(cfg))
			add_discovered(id, parts[0], cfg, cfg);
		else if (!len)
			mq_remove(id);		/* an empty message takes it away */
	} else {
		/* one message for a whole device: its parts are under "cmps" */
		cmps = cfg_get(cfg, "components", "cmps");
		cJSON_ArrayForEach(c, cmps) {
			cJSON *p = cfg_get(c, "platform", "p");
			char sub[96];

			snprintf(sub, sizeof(sub), "%s_%s", obj, c->string);
			make_id(id, sizeof(id), node, sub);
			if (cJSON_IsString(p))
				add_discovered(id, p->valuestring, c, cfg);
		}
	}
	cJSON_Delete(cfg);
}

/* ---- the user's own topics ------------------------------------------------- */

static void load_topics(void)
{
	struct mq *m, *next;
	char *buf = NULL, id[96];
	cJSON *doc, *t;
	FILE *f;
	long n;

	mt_topics = mtime(TOPICS_FILE);
	for (m = list; m; m = next) {
		next = m->next;
		if (m->manual)
			mq_remove(m->id);
	}
	list_dirty = true;
	f = fopen(TOPICS_FILE, "r");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = calloc(1, n + 1);
	if (fread(buf, 1, n, f) != (size_t)n)
		buf[0] = 0;
	fclose(f);
	doc = cJSON_Parse(buf);
	free(buf);
	cJSON_ArrayForEach(t, doc) {
		const char *tid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "id"));
		const char *topic = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "topic"));
		const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "kind"));
		const char *field = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "field"));
		char path[256];
#define TXT(key) cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, key))

		if (!tid || !tid[0] || !topic || !topic[0])
			continue;
		make_id(id, sizeof(id), "", !strncmp(tid, "mqtt.", 5) ? tid + 5 : tid);
		m = mq_new(id);
		m->manual = true;
		snprintf(m->comp, sizeof(m->comp), "%s", kind && !strcmp(kind, "switch") ? "switch" :
			 kind && !strcmp(kind, "binary_sensor") ? "binary_sensor" : "sensor");
		set(&m->state_t, topic);
		set(&m->name, TXT("name") && TXT("name")[0] ? TXT("name") : id + 5);
		set(&m->unit, TXT("unit"));
		set(&m->icon, TXT("icon"));
		set(&m->dclass, TXT("device_class"));
		set(&m->cmd_t, TXT("command_topic"));
		set(&m->pl_on, TXT("payload_on"));
		set(&m->pl_off, TXT("payload_off"));
		set(&m->st_on, m->pl_on);
		set(&m->st_off, m->pl_off);
		if (field && field[0]) {
			snprintf(path, sizeof(path), "%s", field);
			for (char *p = path; *p; p++)
				if (*p == '.')
					*p = '\n';
			set(&m->path, path);
		}
		mq_attach(m);
#undef TXT
	}
	cJSON_Delete(doc);
	ent_flush();
}

/* ---- commands from the cards ------------------------------------------------ */

static void pub(const char *topic, const char *payload)
{
	if (cur && online && topic)
		mosquitto_publish(cur->m, NULL, topic, strlen(payload), payload, 0, false);
}

void mqtt_call(const char *domain, const char *service, const char *entity, cJSON *data)
{
	struct mq *m = mq_find(entity);
	struct entity *e = ent_find(entity);
	cJSON *bri = cJSON_GetObjectItemCaseSensitive(data, "brightness_pct");
	bool on;
	char buf[96];

	if (!m || !e || !cur || !online)
		return;
	if (!strcmp(service, "press")) {
		pub(m->cmd_t, m->pl_press ? m->pl_press : "PRESS");
		return;
	}
	if (!strcmp(service, "toggle"))
		on = !(e->state && !strcmp(e->state, "on"));
	else if (!strcmp(service, "turn_on"))
		on = true;
	else if (!strcmp(service, "turn_off"))
		on = false;
	else
		return;
	if (!m->cmd_t || !strcmp(m->comp, "binary_sensor") || !strcmp(m->comp, "sensor"))
		return;
	if (m->json) {
		if (on && cJSON_IsNumber(bri))
			snprintf(buf, sizeof(buf), "{\"state\":\"ON\",\"brightness\":%d}",
				 (int)(bri->valuedouble * m->bri_scale / 100 + 0.5));
		else
			snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", on ? "ON" : "OFF");
		pub(m->cmd_t, buf);
	} else {
		if (on && cJSON_IsNumber(bri) && m->bri_cmd_t) {
			snprintf(buf, sizeof(buf), "%d", (int)(bri->valuedouble * m->bri_scale / 100 + 0.5));
			pub(m->bri_cmd_t, buf);
		}
		pub(m->cmd_t, on ? (m->pl_on ? m->pl_on : "ON") : (m->pl_off ? m->pl_off : "OFF"));
	}
	if (!m->state_t) {		/* nothing will tell us: assume it worked */
		ent_set(e, on ? "on" : "off");
		ent_flush();
	}
}

/* ---- main loop ---------------------------------------------------------------- */

/* the list the web interface offers when a card is made */
static void write_list(void)
{
	cJSON *arr = cJSON_CreateArray();
	struct mq *m;
	char *s;
	FILE *f;

	for (m = list; m; m = m->next) {
		cJSON *o = cJSON_CreateObject();

		cJSON_AddStringToObject(o, "id", m->id);
		cJSON_AddStringToObject(o, "name", m->name ? m->name : m->id);
		cJSON_AddStringToObject(o, "kind", m->comp);
		cJSON_AddStringToObject(o, "topic", m->state_t ? m->state_t : m->cmd_t ? m->cmd_t : "");
		if (m->manual)
			cJSON_AddTrueToObject(o, "manual");
		cJSON_AddItemToArray(arr, o);
	}
	s = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	f = fopen(LIST_FILE ".tmp", "w");
	if (f && s) {
		fputs(s, f);
		fclose(f);
		rename(LIST_FILE ".tmp", LIST_FILE);
	} else if (f) {
		fclose(f);
	}
	free(s);
}

static void on_connected(void)
{
	char topic[128];
	struct mq *m;

	online = true;
	snprintf(topic, sizeof(topic), "%s/+/+/config", prefix);
	mosquitto_subscribe(cur->m, NULL, topic, 0);
	snprintf(topic, sizeof(topic), "%s/+/+/+/config", prefix);
	mosquitto_subscribe(cur->m, NULL, topic, 0);
	for (m = list; m; m = m->next) {
		m->subscribed = false;
		mq_attach(m);
	}
}

void mqtt_poll(void)
{
	struct item *i, *next;
	uint32_t now = now_ms();
	size_t plen = strlen(prefix);
	char buf[96];
	bool changed = false;

	pthread_mutex_lock(&q_lock);
	i = q_head;
	q_head = NULL;
	q_tail = &q_head;
	pthread_mutex_unlock(&q_lock);
	for (; i; i = next) {
		next = i->next;
		if (i->from != cur) {
			/* from a connection that was replaced */
		} else if (i->kind == 1) {
			on_connected();
			changed = true;
		} else if (i->kind == 2) {
			online = false;
			changed = true;
			/* what came over the broker is not current any more: after
			   a reconnect a value only counts again once it is sent again
			   (a retained one comes at once), not the one from before */
			for (struct mq *m = list; m; m = m->next) {
				struct entity *e = m->state_t ? ent_find(m->id) : NULL;

				if (e && e->state)
					ent_set(e, "unavailable");
			}
		} else if (!strncmp(i->topic, prefix, plen) && i->topic[plen] == '/' &&
			   strlen(i->topic) > 7 &&
			   !strcmp(i->topic + strlen(i->topic) - 7, "/config")) {
			on_config(i->topic + plen + 1, i->payload, i->len);
		} else {
			for (struct mq *m = list; m; m = m->next)
				if (m->subscribed && ((m->state_t && !strcmp(m->state_t, i->topic)) ||
				    (m->bri_state_t && !strcmp(m->bri_state_t, i->topic))))
					on_state(m, i->topic, i->payload);
			changed = true;
		}
		free(i->topic);
		free(i->payload);
		free(i);
	}
	if (changed)
		ent_flush();

	if (list_dirty) {
		list_dirty = false;
		t_list = now | 1;
	}
	if (t_list && now - t_list > 1500) {
		t_list = 0;
		write_list();
		changed = true;
	}
	if (changed && cur)
		set_status("mqtt", status_now());

	if (now - t_check < 2000)
		return;
	t_check = now;
	if (mtime(MQTT_CONF) != mt_conf) {
		mt_conf = mtime(MQTT_CONF);
		conn_start();
	}
	if (mtime(TOPICS_FILE) != mt_topics)
		load_topics();
}

void mqtt_init(void)
{
	char host[64] = "";

	/* the topics toon-mqttd publishes this Toon under: its name (without
	 * "toon-"), and those of its id, which it used before 1.26 */
	gethostname(host, sizeof(host) - 1);
	snprintf(own_base, sizeof(own_base), "toon/%s/", !strncmp(host, "toon-", 5) ? host + 5 : host);
	if (!file_value("/run/toon/id", "id", host, sizeof(host)) && host[0])
		snprintf(own_idbase, sizeof(own_idbase), "toon/%s/", host);
	mosquitto_lib_init();
	mt_conf = mtime(MQTT_CONF);
	load_topics();
	write_list();
	conn_start();
}
