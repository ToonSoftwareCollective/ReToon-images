// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-bus: the Toon's own MQTT broker for scripts and the web interface.
 *
 *   toon-bus put NAME          key=value lines on stdin as retoon/NAME
 *                              (retained JSON with "time"); none: cleared
 *   toon-bus send TOPIC TEXT   one message, not retained (a command)
 *   toon-bus get NAME [KEY]    retoon/NAME as JSON, or one value of it
 *   toon-bus dump              every retoon/NAME as "NAME/key=value" lines
 *   toon-bus serve [SOCKET]    the bus for the web interface's browsers,
 *                              behind lighttpd's /ws (serve.c)
 *
 * Exit status 1 when the broker or the value is not there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "toonbus.h"

int serve_main(const char *path);

struct get {
	const char *name, *key;
	int found;
};

static void get_one(const char *name, const char *json, void *ud)
{
	struct get *g = ud;
	char v[4096];

	if (strcmp(name, g->name))
		return;
	if (!g->key) {
		puts(json);
		g->found = 1;
	} else if (!bus_json_value(json, g->key, v, sizeof(v))) {
		puts(v);
		g->found = 1;
	}
}

/* every key of a flat object as NAME/key=value */
static void dump_one(const char *name, const char *json, void *ud)
{
	static char kv[16384];
	char *line, *save;

	(void)ud;
	bus_json_kv(json, kv, sizeof(kv));
	for (line = strtok_r(kv, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		printf("%s/%s\n", name, line);
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "put")) {
		static char kv[16384];
		size_t n = fread(kv, 1, sizeof(kv) - 1, stdin);

		kv[n] = 0;
		return bus_put_once(argv[2], kv) ? 1 : 0;
	}
	if (argc == 4 && !strcmp(argv[1], "send"))
		return bus_send_once(argv[2], argv[3], 0) ? 1 : 0;
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "get")) {
		struct get g = { argv[2], argc == 4 ? argv[3] : NULL, 0 };

		return bus_snapshot(get_one, &g) < 0 || !g.found;
	}
	if (argc == 2 && !strcmp(argv[1], "dump"))
		return bus_snapshot(dump_one, NULL) < 0;
	if ((argc == 2 || argc == 3) && !strcmp(argv[1], "serve"))
		return serve_main(argc == 3 ? argv[2] : NULL);
	fprintf(stderr, "usage: toon-bus put NAME | send TOPIC TEXT | get NAME [KEY] | dump | serve [SOCKET]\n");
	return 2;
}
