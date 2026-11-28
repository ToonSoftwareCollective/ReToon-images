// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-bus serve: the bus for the web interface's browsers.
 *
 * lighttpd takes the websocket on /ws (with the web login) and passes each
 * connection on to the socket here as plain text (mod_wstunnel). One JSON
 * object per line, both ways; frames may split or join lines, so only the
 * newline counts.
 *
 * The browser's first line is {"auth":"<token>"}: a token the web API made
 * for it a moment before (GET /ws-token, a file in TOKEN_DIR), used once.
 * Then it gets every kept retoon/<name> there is and {"ready":true}, and
 * after that each message as it comes:
 *   {"topic":"retoon/climate","data":{...}}    (data null: taken away)
 * A browser sends {"topic":"retoon/boiler/set","payload":"target 21"}; only
 * the commands in allowed() go on, a thermostat command as "@web ...".
 */
#define _GNU_SOURCE	/* accept4 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "toonbus.h"

#define SOCK_PATH	"/run/toon/bus-ws.sock"
#define TOKEN_DIR	"/run/toon/ws-tokens"
#define TOKEN_MAX_AGE	60	/* s from GET /ws-token to its use */
#define AUTH_WAIT	10	/* s for the first line */
#define MAX_CLIENTS	8
#define MAX_LINE	16384	/* from a browser */
#define MAX_OUT		(512 * 1024)	/* not yet taken by lighttpd: then it goes */
#define MAX_PAYLOAD	4096

struct client {
	int fd, authed;
	time_t since;
	char in[MAX_LINE];
	size_t inlen;
	char *out;
	size_t outlen, outcap;
};

struct kept {
	char *topic, *line;	/* the line as sent to a browser */
	struct kept *next;
};

static struct client clients[MAX_CLIENTS];
static struct kept *kept;
static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* ---- lines --------------------------------------------------------------- */

static void json_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = *s;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

/* a payload that is a JSON object goes as it is; anything else as text */
static int is_object(const char *p)
{
	size_t n;

	while (isspace((unsigned char)*p))
		p++;
	n = strlen(p);
	while (n && isspace((unsigned char)p[n - 1]))
		n--;
	return n >= 2 && p[0] == '{' && p[n - 1] == '}' && !memchr(p, '\n', n);
}

static char *message_line(const char *topic, const char *payload)
{
	char *line = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&line, &len);

	if (!f)
		return NULL;
	fputs("{\"topic\":", f);
	json_string(f, topic);
	fputs(",\"data\":", f);
	if (!*payload)
		fputs("null", f);
	else if (is_object(payload))
		fputs(payload, f);
	else
		json_string(f, payload);
	fputs("}\n", f);
	fclose(f);
	return line;
}

/* ---- browsers ------------------------------------------------------------ */

static void drop(struct client *c)
{
	close(c->fd);
	free(c->out);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

static void queue(struct client *c, const char *s)
{
	size_t n = strlen(s);

	if (c->fd < 0)
		return;
	if (c->outlen + n > MAX_OUT) {	/* it does not keep up */
		drop(c);
		return;
	}
	if (c->outlen + n > c->outcap) {
		size_t cap = (c->outlen + n) * 2;
		char *o = realloc(c->out, cap);

		if (!o) {
			drop(c);
			return;
		}
		c->out = o;
		c->outcap = cap;
	}
	memcpy(c->out + c->outlen, s, n);
	c->outlen += n;
}

static void flush(struct client *c)
{
	ssize_t w;

	if (c->fd < 0 || !c->outlen)
		return;
	w = write(c->fd, c->out, c->outlen);
	if (w < 0) {
		if (errno != EAGAIN && errno != EINTR)
			drop(c);
		return;
	}
	memmove(c->out, c->out + w, c->outlen - w);
	c->outlen -= w;
}

static void to_all(const char *line)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].authed)
			queue(&clients[i], line);
}

/* a token from the web API: 32 hex characters, a file made just now */
static int token_ok(const char *t)
{
	char path[sizeof(TOKEN_DIR) + 72];
	struct stat st;
	size_t i;
	int ok;

	for (i = 0; t[i]; i++)
		if (!isxdigit((unsigned char)t[i]))
			return 0;
	if (i != 32)
		return 0;
	snprintf(path, sizeof(path), TOKEN_DIR "/%s", t);
	if (stat(path, &st))
		return 0;
	ok = S_ISREG(st.st_mode) && time(NULL) - st.st_mtime <= TOKEN_MAX_AGE &&
		time(NULL) >= st.st_mtime;
	unlink(path);		/* once */
	return ok;
}

/* what a browser may send */
static int allowed(const char *topic)
{
	return !strcmp(topic, BUS_BASE "/boiler/set") || !strcmp(topic, BUS_BASE "/display/set") ||
		!strcmp(topic, BUS_BASE "/notify/set") || !strcmp(topic, BUS_BASE "/ui/set");
}

static void reply_error(struct client *c, const char *what, const char *topic)
{
	char *line = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&line, &len);

	if (!f)
		return;
	fputs("{\"error\":", f);
	json_string(f, what);
	if (topic) {
		fputs(",\"topic\":", f);
		json_string(f, topic);
	}
	fputs("}\n", f);
	fclose(f);
	queue(c, line);
	free(line);
}

static void handle_line(struct client *c, char *line)
{
	static char topic[128], payload[MAX_PAYLOAD], cmd[MAX_PAYLOAD + 8];
	char token[64];

	if (!*line)
		return;
	if (!c->authed) {
		if (bus_json_value(line, "auth", token, sizeof(token)) || !token_ok(token)) {
			reply_error(c, "auth", NULL);
			flush(c);
			drop(c);
			return;
		}
		c->authed = 1;
		for (struct kept *k = kept; k; k = k->next)
			queue(c, k->line);
		queue(c, "{\"ready\":true}\n");
		return;
	}
	if (bus_json_value(line, "topic", topic, sizeof(topic)) ||
	    bus_json_value(line, "payload", payload, sizeof(payload))) {
		reply_error(c, "bad message", NULL);
		return;
	}
	if (!allowed(topic)) {
		reply_error(c, "not allowed", topic);
		return;
	}
	for (char *p = payload; *p; p++)
		if ((unsigned char)*p < 0x20 && *p != '\n' && *p != '\t')
			*p = ' ';
	if (!strcmp(topic, BUS_BASE "/boiler/set")) {
		/* who it came from is ours to say */
		if (payload[0] == '@') {
			reply_error(c, "not allowed", topic);
			return;
		}
		snprintf(cmd, sizeof(cmd), "@web %s", payload);
	} else {
		snprintf(cmd, sizeof(cmd), "%s", payload);
	}
	if (bus_send(topic, cmd))
		reply_error(c, "no bus", topic);
}

static void read_client(struct client *c)
{
	ssize_t r = read(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen);
	char *start, *nl;

	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
		drop(c);
		return;
	}
	if (r < 0)
		return;
	c->inlen += r;
	c->in[c->inlen] = 0;
	start = c->in;
	while (c->fd >= 0 && (nl = strchr(start, '\n'))) {
		*nl = 0;
		if (nl > start && nl[-1] == '\r')
			nl[-1] = 0;
		handle_line(c, start);
		start = nl + 1;
	}
	if (c->fd < 0)
		return;
	c->inlen -= start - c->in;
	memmove(c->in, start, c->inlen);
	if (c->inlen >= sizeof(c->in) - 1)	/* a line too long */
		drop(c);
}

/* ---- the bus ------------------------------------------------------------- */

static void on_bus(const char *topic, const char *payload)
{
	struct kept **pk, *k;
	char *line = message_line(topic, payload);
	const char *name = topic + sizeof(BUS_BASE);

	if (!line)
		return;
	/* a state (retoon/<name>) is kept for browsers that come later */
	if (!strncmp(topic, BUS_BASE "/", sizeof(BUS_BASE)) && !strchr(name, '/')) {
		for (pk = &kept; *pk && strcmp((*pk)->topic, topic); pk = &(*pk)->next)
			;
		k = *pk;
		if (!*payload) {
			if (k) {
				*pk = k->next;
				free(k->topic);
				free(k->line);
				free(k);
			}
		} else if (k) {
			free(k->line);
			k->line = strdup(line);
		} else if ((k = calloc(1, sizeof(*k)))) {
			k->topic = strdup(topic);
			k->line = strdup(line);
			k->next = kept;
			kept = k;
		}
	}
	to_all(line);
	free(line);
}

/* ---- main ---------------------------------------------------------------- */

static int listen_on(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	unlink(path);
	umask(077);		/* for lighttpd (root) only */
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 8)) {
		close(fd);
		return -1;
	}
	return fd;
}

int serve_main(const char *path)
{
	struct pollfd pfd[MAX_CLIENTS + 2];
	int lfd, i;

	if (!path)
		path = SOCK_PATH;
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	for (i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	mkdir(TOKEN_DIR, 0700);
	lfd = listen_on(path);
	if (lfd < 0) {
		perror(path);
		return 1;
	}
	if (bus_open("web")) {
		fprintf(stderr, "toon-bus serve: no bus\n");
		return 1;
	}
	bus_on(BUS_BASE "/+", on_bus);
	bus_on(BUS_BASE "/notify/event", on_bus);

	while (!stop) {
		int n = 0;
		time_t now = time(NULL);

		pfd[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = bus_fd(), .events = POLLIN };
		for (i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &clients[i];

			if (c->fd >= 0 && !c->authed && now - c->since > AUTH_WAIT)
				drop(c);
			pfd[n++] = (struct pollfd){ .fd = c->fd,
				.events = c->fd < 0 ? 0 : POLLIN | (c->outlen ? POLLOUT : 0) };
		}
		if (poll(pfd, n, 1000) < 0) {
			if (errno != EINTR)
				break;
			continue;
		}
		if (pfd[1].revents)
			bus_poll();
		for (i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &clients[i];
			short re = pfd[2 + i].revents;

			if (c->fd < 0 || pfd[2 + i].fd != c->fd)
				continue;
			if (re & POLLIN)
				read_client(c);
			else if (re & (POLLHUP | POLLERR))
				drop(c);
		}
		if (pfd[0].revents & POLLIN) {
			int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

			if (fd >= 0) {
				for (i = 0; i < MAX_CLIENTS && clients[i].fd >= 0; i++)
					;
				if (i == MAX_CLIENTS) {
					static const char full[] = "{\"error\":\"too many\"}\n";

					(void)!write(fd, full, sizeof(full) - 1);
					close(fd);
				} else {
					clients[i].fd = fd;
					clients[i].since = now;
				}
			}
		}
		for (i = 0; i < MAX_CLIENTS; i++)
			flush(&clients[i]);
	}
	for (i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0)
			drop(&clients[i]);
	close(lfd);
	unlink(path);
	bus_close();
	return 0;
}
