/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * toonbus: how the ReToon programs talk to each other, over the Toon's own
 * MQTT broker (mosquitto on 127.0.0.1:1883, S41toon-broker).
 *
 * State: each program publishes what it knows as one retained JSON object on
 * retoon/<name> (retoon/boiler, retoon/voc, ...). bus_put() takes the same
 * "key=value" lines the programs used to write to /run/toon/<name>, turns
 * them into JSON and adds "time" (seconds since 1970): when the program
 * last gave it to bus_put(). It is sent when it changes, and when the
 * program puts the same again BUS_REFRESH_S after the last send. So a
 * program that measures puts its state every cycle, and a reader can tell
 * from an old time that it stopped (or hangs). After a reconnect the last
 * state is sent again with its own time.
 *
 * Reading: bus_watch(name) keeps the newest retoon/<name> in memory;
 * bus_value() and bus_age() read from there, never waiting for the broker.
 *
 * Commands: bus_on(topic, fn) calls fn(topic, payload) for every message on
 * that topic (wildcards allowed), from bus_poll() in the program's own
 * thread. bus_fd() becomes readable when something is waiting, for poll().
 *
 * Everything keeps working while the broker is away: puts are kept and
 * sent when it is back, watches and commands are subscribed again.
 */
#ifndef TOONBUS_H
#define TOONBUS_H

#include <stddef.h>

#define BUS_BASE	"retoon"
#define BUS_REFRESH_S	30

/* connect as "retoon-<client>"; 0 or -1. Call once, at the start. */
int bus_open(const char *client);
void bus_close(void);

/* "key=value\n..." as retained JSON on retoon/<name>; kv NULL or "" takes
   the kept message away (the program stops or has nothing to say). */
void bus_put(const char *name, const char *kv);

/* keep the newest retoon/<name> in memory */
void bus_watch(const char *name);
/* a value from it as text (numbers as written); 0, or -1 when not there */
int bus_value(const char *name, const char *key, char *out, size_t len);
/* seconds since its "time"; -1 when nothing (with a time) came yet */
long bus_age(const char *name);
/* the same as "key=value" lines to read like a file; NULL when nothing */
FILE *bus_fopen(const char *name);
/* the whole object as JSON text, copied to out; 0 or -1 */
int bus_json(const char *name, char *out, size_t len);

/* commands and events */
typedef void (*bus_fn)(const char *topic, const char *payload);
void bus_on(const char *topic, bus_fn fn);
int bus_send(const char *topic, const char *payload);	/* not retained */
int bus_fd(void);
void bus_poll(void);

int bus_connected(void);

/* For short-lived programs (the web interface's CGI, scripts): connect,
   take every retained retoon/<name> there is, disconnect. fn(name, json)
   for each; returns how many, or -1 without a broker. No bus_open needed. */
typedef void (*bus_snap_fn)(const char *name, const char *json, void *ud);
int bus_snapshot(bus_snap_fn fn, void *ud);
/* one message from a short-lived program, without bus_open; retained or not */
int bus_send_once(const char *topic, const char *payload, int retain);

/* bus_put for a short-lived program (a script, through toon-bus put) */
int bus_put_once(const char *name, const char *kv);

/* a value out of a flat JSON object as text; 0, or -1 */
int bus_json_value(const char *json, const char *key, char *out, size_t len);
/* a flat JSON object back to "key=value" lines (objects and lists left
   out, line breaks in values become spaces); returns the length */
size_t bus_json_kv(const char *json, char *out, size_t len);

#endif
