// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ha: the small jobs around the Home Assistant connection, for toon-ui
 * and the web interface. Speaks http and https, ws and wss.
 *
 *   toon-ha token                  addr=, port=, host=, tls=, verify=, token=
 *                                  (or error=) for toon-ui
 *   toon-ha login URL CODE CLIENT  finish the web login: trade the one-time
 *                                  code and make a long-lived access token
 *                                  named after this Toon; prints token=, user=
 *                                  (HA_VERIFY=1 checks the certificate)
 *   toon-ha entities               [{"id","name","state"[,"options","hvac_modes",
 *                                  "preset_modes","fan_modes","swing_modes"]}...]
 *                                  of everything
 *   toon-ha ws JSON                one websocket command, prints the reply
 *   toon-ha ws JSON SECONDS        same, then prints the events that follow
 *                                  (subscriptions) for SECONDS seconds
 *
 * Settings are read from ha.conf on p4:
 *   url=http://homeassistant.local:8123   (or https://)
 *   token=<long-lived access token>
 *   verify=1                              check the https certificate
 *   refresh_token=, client_id=            (older logins)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#define HA_CONF "/mnt/data/toon-os/config/ha.conf"

static char host[128], addr[64], token[4096];
static int port = 8123;
static bool tls, verify;
static int fd = -1;
static SSL *ssl;

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("error=");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	exit(1);
}

static int conf(const char *key, char *out, size_t len)
{
	char line[8192];
	size_t klen = strlen(key);
	FILE *f = fopen(HA_CONF, "r");
	int found = -1;

	out[0] = 0;
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			line[strcspn(line, "\r\n")] = 0;
			snprintf(out, len, "%s", line + klen + 1);
			found = 0;
		}
	fclose(f);
	return found;
}

/* "http://name:8123/" -> host, port, tls; the name is looked up (DNS or mDNS) */
static void set_url(const char *url)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
	const char *p = url;
	char *c;

	tls = false;
	if (!strncmp(p, "https://", 8)) {
		tls = true;
		p += 8;
	} else if (!strncmp(p, "http://", 7)) {
		p += 7;
	}
	snprintf(host, sizeof(host), "%s", p);
	host[strcspn(host, "/")] = 0;
	port = tls ? 443 : 8123;
	c = strrchr(host, ':');
	if (c) {
		*c = 0;
		port = atoi(c + 1);
	}
	if (!host[0] || port <= 0 || port > 65535)
		die("That is not a Home Assistant address");
	if (getaddrinfo(host, NULL, &hints, &res))
		die("Cannot find %s on the network", host);
	inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, addr, sizeof(addr));
	freeaddrinfo(res);
}

/* ---- one connection ----------------------------------------------------- */

static void net_open(void)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	struct timeval tv = { .tv_sec = 10 };

	inet_pton(AF_INET, addr, &sa.sin_addr);
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
		die("The Toon cannot reach Home Assistant at %s:%d", addr, port);
	if (tls) {
		SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());

		if (verify) {
			SSL_CTX_set_default_verify_paths(ctx);
			SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
		}
		ssl = SSL_new(ctx);
		SSL_set_fd(ssl, fd);
		if (strspn(host, "0123456789.") != strlen(host))
			SSL_set_tlsext_host_name(ssl, host);
		if (verify)
			SSL_set1_host(ssl, host);
		if (SSL_connect(ssl) != 1)
			die(verify && SSL_get_verify_result(ssl) != X509_V_OK ?
			    "The certificate of Home Assistant is not trusted" :
			    "No secure connection to Home Assistant (is it really https?)");
	}
}

static void net_close(void)
{
	if (ssl) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
	}
	ssl = NULL;
	if (fd >= 0)
		close(fd);
	fd = -1;
}

static void net_write(const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = ssl ? SSL_write(ssl, p, len) : send(fd, p, len, MSG_NOSIGNAL);

		if (n <= 0)
			die("Connection to Home Assistant lost");
		p += n;
		len -= n;
	}
}

static ssize_t net_read(void *buf, size_t len)
{
	return ssl ? SSL_read(ssl, buf, len) : recv(fd, buf, len, 0);
}

static void read_exact(void *buf, size_t len)
{
	char *p = buf;

	while (len) {
		ssize_t n = net_read(p, len);

		if (n <= 0)
			die("Home Assistant did not answer");
		p += n;
		len -= n;
	}
}

/* ---- http ---------------------------------------------------------------- */

/* POST a form; returns the body of the answer (to be freed) */
static char *http_post(const char *path, const char *form)
{
	char head[1024], *buf = NULL, *body, *out;
	size_t len = 0, cap = 0;
	ssize_t n;

	net_open();
	snprintf(head, sizeof(head),
		 "POST %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
		 "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n",
		 path, host, port, strlen(form));
	net_write(head, strlen(head));
	net_write(form, strlen(form));
	for (;;) {
		if (cap - len < 4096) {
			cap += 16384;
			buf = realloc(buf, cap);
		}
		n = net_read(buf + len, cap - len - 1);
		if (n <= 0)
			break;
		len += n;
	}
	net_close();
	if (!buf)
		die("Home Assistant did not answer");
	buf[len] = 0;
	body = strstr(buf, "\r\n\r\n");
	if (!body)
		die("Home Assistant did not answer");
	*body = 0;
	body += 4;
	if (strcasestr(buf, "transfer-encoding: chunked")) {
		char *w = out = malloc(len + 1), *p = body;

		for (;;) {
			size_t chunk = strtoul(p, &p, 16);

			p = strstr(p, "\r\n");
			if (!chunk || !p || p + 2 + chunk > buf + len)
				break;
			memcpy(w, p + 2, chunk);
			w += chunk;
			p += 2 + chunk + 2;
		}
		*w = 0;
	} else {
		out = strdup(body);
	}
	free(buf);
	return out;
}

/* ---- websocket ----------------------------------------------------------- */

static void ws_send(const char *text)
{
	size_t len = strlen(text), h = 2;
	unsigned char *f = malloc(len + 14);
	static const unsigned char mask[4] = { 0x37, 0xfa, 0x21, 0x3d };

	f[0] = 0x81;
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
	memcpy(f + h, mask, 4);
	for (size_t i = 0; i < len; i++)
		f[h + 4 + i] = text[i] ^ mask[i & 3];
	net_write(f, h + 4 + len);
	free(f);
}

/* the next text message, parsed; NULL when the other side closed */
static cJSON *ws_recv(void)
{
	char *msg = NULL;
	size_t mlen = 0;

	for (;;) {
		unsigned char h[10];
		size_t len;
		int op;

		read_exact(h, 2);
		op = h[0] & 0x0f;
		len = h[1] & 0x7f;
		if (len == 126) {
			read_exact(h + 2, 2);
			len = h[2] << 8 | h[3];
		} else if (len == 127) {
			read_exact(h + 2, 8);
			len = (size_t)h[6] << 24 | h[7] << 16 | h[8] << 8 | h[9];
		}
		if (len > 64u << 20)
			die("Home Assistant sent too much");
		msg = realloc(msg, mlen + len + 1);
		read_exact(msg + mlen, len);
		if (op == 8) {
			free(msg);
			return NULL;
		}
		if (op > 2)
			continue;	/* ping, pong */
		mlen += len;
		if (h[0] & 0x80) {
			cJSON *j;

			msg[mlen] = 0;
			j = cJSON_ParseWithLength(msg, mlen);
			free(msg);
			if (!j)
				die("Home Assistant sent something unreadable");
			return j;
		}
	}
}

static const char *jstr(cJSON *o, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	return cJSON_IsString(v) ? v->valuestring : "";
}

static void ws_open(void)
{
	char req[512], buf[2048];
	size_t len = 0;
	cJSON *m, *o;
	char *s;

	net_open();
	snprintf(req, sizeof(req),
		 "GET /api/websocket HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
		 "Connection: Upgrade\r\nSec-WebSocket-Key: dG9vbi11aS13cy1rZXkxNg==\r\n"
		 "Sec-WebSocket-Version: 13\r\n\r\n", host, port);
	net_write(req, strlen(req));
	/* the answer header, one byte at a time so no frame is swallowed */
	while (len < sizeof(buf) - 1 && (len < 4 || memcmp(buf + len - 4, "\r\n\r\n", 4)))
		read_exact(buf + len++, 1);
	if (len < 12 || memcmp(buf + 9, "101", 3))
		die("That address is not Home Assistant");
	m = ws_recv();			/* auth_required */
	cJSON_Delete(m);
	o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "type", "auth");
	cJSON_AddStringToObject(o, "access_token", token);
	s = cJSON_PrintUnformatted(o);
	ws_send(s);
	free(s);
	cJSON_Delete(o);
	m = ws_recv();
	if (!m || strcmp(jstr(m, "type"), "auth_ok"))
		die("Home Assistant refused the login");
	cJSON_Delete(m);
}

/* send one command (takes cmd), wait for its result message */
static cJSON *ws_call(cJSON *cmd)
{
	static int id;
	cJSON *m;
	char *s;

	cJSON_AddNumberToObject(cmd, "id", ++id);
	s = cJSON_PrintUnformatted(cmd);
	ws_send(s);
	free(s);
	cJSON_Delete(cmd);
	while ((m = ws_recv())) {
		cJSON *mid = cJSON_GetObjectItemCaseSensitive(m, "id");

		if (cJSON_IsNumber(mid) && mid->valueint == id && !strcmp(jstr(m, "type"), "result"))
			return m;
		cJSON_Delete(m);
	}
	die("Home Assistant closed the connection");
	return NULL;
}

static cJSON *cmd(const char *type)
{
	cJSON *o = cJSON_CreateObject();

	cJSON_AddStringToObject(o, "type", type);
	return o;
}

/* ---- the jobs ------------------------------------------------------------ */

/* the access token of an older login that kept a refresh token */
static void refresh_login(void)
{
	char refresh[1024], client[256], form[2048], *reply;
	cJSON *j;

	conf("refresh_token", refresh, sizeof(refresh));
	conf("client_id", client, sizeof(client));
	if (!refresh[0])
		die("Not logged in to Home Assistant");
	snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s",
		 refresh, client);
	reply = http_post("/auth/token", form);
	j = cJSON_Parse(reply);
	snprintf(token, sizeof(token), "%s", jstr(j, "access_token"));
	if (!token[0])
		die("Home Assistant did not accept the login");
}

static void load_conf(void)
{
	char url[256], v[8];

	if (conf("url", url, sizeof(url)) || !url[0])
		exit(1);		/* not set up: no message */
	conf("verify", v, sizeof(v));
	verify = v[0] == '1';
	set_url(url);
	conf("token", token, sizeof(token));
	if (!token[0])
		refresh_login();
}

static int do_login(const char *url, const char *code, const char *client)
{
	char form[2048], name[96] = "Toon ", *reply;
	cJSON *j, *r, *list, *t, *o;
	const char *mine = NULL;

	gethostname(name + 5, sizeof(name) - 6);
	set_url(url);
	snprintf(form, sizeof(form), "grant_type=authorization_code&code=%s&client_id=%s", code, client);
	reply = http_post("/auth/token", form);
	j = cJSON_Parse(reply);
	snprintf(token, sizeof(token), "%s", jstr(j, "access_token"));
	if (!token[0])
		die("Home Assistant did not accept the login");

	ws_open();
	/* a token of this name from an earlier login has to go first */
	list = ws_call(cmd("auth/refresh_tokens"));
	cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(list, "result")) {
		if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "is_current")))
			mine = jstr(t, "id");
		if (!strcmp(jstr(t, "type"), "long_lived_access_token") &&
		    !strcmp(jstr(t, "client_name"), name)) {
			o = cmd("auth/delete_refresh_token");
			cJSON_AddStringToObject(o, "refresh_token_id", jstr(t, "id"));
			cJSON_Delete(ws_call(o));
		}
	}
	o = cmd("auth/long_lived_access_token");
	cJSON_AddStringToObject(o, "client_name", name);
	cJSON_AddNumberToObject(o, "lifespan", 3650);
	r = ws_call(o);
	if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "success")) || !jstr(r, "result")[0])
		die("Home Assistant did not give an access token");
	printf("token=%s\n", jstr(r, "result"));
	r = ws_call(cmd("auth/current_user"));
	printf("user=%s\n", jstr(cJSON_GetObjectItemCaseSensitive(r, "result"), "name"));
	fflush(stdout);
	/* the login itself is not needed any more; the answer may never come */
	if (mine && mine[0]) {
		char *s;

		o = cmd("auth/delete_refresh_token");
		cJSON_AddStringToObject(o, "refresh_token_id", mine);
		cJSON_AddNumberToObject(o, "id", 1000);
		s = cJSON_PrintUnformatted(o);
		ws_send(s);
		usleep(300000);
	}
	return 0;
}

static int do_entities(void)
{
	cJSON *r, *s, *out = cJSON_CreateArray();
	char *txt;

	load_conf();
	ws_open();
	r = ws_call(cmd("get_states"));
	cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(r, "result")) {
		cJSON *a = cJSON_GetObjectItemCaseSensitive(s, "attributes"), *e = cJSON_CreateObject();
		const char *id = jstr(s, "entity_id"), *name = jstr(a, "friendly_name");
		char state[160];

		snprintf(state, sizeof(state), "%s%s%s", jstr(s, "state"),
			 jstr(a, "unit_of_measurement")[0] ? " " : "", jstr(a, "unit_of_measurement"));
		cJSON_AddStringToObject(e, "id", id);
		cJSON_AddStringToObject(e, "name", name[0] ? name : id);
		cJSON_AddStringToObject(e, "state", state);
		/* for the editor's option buttons: what can be chosen */
		for (const char **k = (const char *[]){ "options", "hvac_modes", "preset_modes", "fan_modes",
						      "swing_modes", NULL }; *k; k++) {
			cJSON *l = cJSON_GetObjectItemCaseSensitive(a, *k);

			if (cJSON_IsArray(l) && cJSON_GetArraySize(l))
				cJSON_AddItemToObject(e, *k, cJSON_Duplicate(l, 1));
		}
		cJSON_AddItemToArray(out, e);
	}
	txt = cJSON_PrintUnformatted(out);
	puts(txt);
	return 0;
}

int main(int argc, char **argv)
{
	const char *job = argc > 1 ? argv[1] : "";

	verify = getenv("HA_VERIFY") && atoi(getenv("HA_VERIFY"));
	if (!strcmp(job, "token")) {
		load_conf();
		printf("addr=%s\nport=%d\nhost=%s\ntls=%d\nverify=%d\ntoken=%s\n",
		       addr, port, host, tls, verify, token);
		return 0;
	}
	if (!strcmp(job, "login") && argc == 5)
		return do_login(argv[2], argv[3], argv[4]);
	if (!strcmp(job, "entities"))
		return do_entities();
	if (!strcmp(job, "ws") && (argc == 3 || argc == 4)) {
		cJSON *c = cJSON_Parse(argv[2]), *r;
		char *txt;

		if (!cJSON_IsObject(c))
			die("Bad command");
		load_conf();
		ws_open();
		r = ws_call(c);
		txt = cJSON_PrintUnformatted(r);
		puts(txt);
		if (argc == 4) {	/* keep printing what follows (subscriptions) */
			alarm(atoi(argv[3]));
			while ((r = ws_recv())) {
				txt = cJSON_PrintUnformatted(r);
				puts(txt);
				fflush(stdout);
				cJSON_Delete(r);
			}
		}
		return 0;
	}
	fprintf(stderr, "usage: toon-ha token | login URL CODE CLIENT | entities | ws JSON\n");
	return 2;
}
