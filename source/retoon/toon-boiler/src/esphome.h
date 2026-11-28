// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A small ESPHome native API client: an encrypted connection (Noise,
 * NNpsk0 with the device's API key) to one ESPHome device, its entities
 * and their states, and switch and number commands. Used by toon-boilerd
 * for an OpenTherm module on an ESP (ToonSoftwareCollective/ReToon-esphome).
 */
#ifndef ESPHOME_H
#define ESPHOME_H

#include <stddef.h>

/* connect, log in and subscribe to states; 0 = up, -1 = error (in err) */
int esp_connect(const char *host, int port, const char *key_b64, char *err, size_t errlen);
void esp_close(void);
int esp_connected(void);
/* the socket to poll for input, -1 when not connected */
int esp_fd(void);
/* handle what arrived (call when esp_fd is readable) and keep the
   connection alive (call regularly); -1 = connection lost */
int esp_poll(void);
int esp_tick(void);

/* the last state of an entity by object id: 0 = known, -1 = not (yet);
   *age_ms: how long ago it came */
int esp_state(const char *object_id, double *value, long long *age_ms);
int esp_has_entity(const char *object_id);
int esp_set_switch(const char *object_id, int on);
int esp_set_number(const char *object_id, double value);

/* the address of the last connection ("" before one), and one to try
   when the name is not found: right after a start, mDNS can fail for a
   while when the network is already there (the key makes sure only that
   ESP can answer on it) */
const char *esp_address(void);
void esp_set_fallback(const char *ip);

/* what the device said about itself */
const char *esp_name(void);
const char *esp_version(void);		/* ESPHome version */
const char *esp_project(void);		/* project name */
const char *esp_project_version(void);

#endif
