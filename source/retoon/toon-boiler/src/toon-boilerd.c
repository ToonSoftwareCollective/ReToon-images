// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-boilerd: the Toon 2's link to the Quby boiler module (UART1, 4800 8E1).
 *
 * Frames are 7 bytes in both directions:
 *
 *   [CMD][b1][b2][b3][b4][b5][CRC]     CRC-8, poly 0x21, init 0xff
 *
 * CMD is an ASCII letter with bit 7 set towards the module and clear in its
 * reply; the module echoes each frame before answering.
 *
 *   'K' 45 .. 01 / 00   use OpenTherm / use the on/off contact
 *   'K' 4f .. 01 / 00   on/off contact closed (heat) / open
 *   'K' 53 01 id fl 00  what the module itself exchanges with the boiler
 *   'B' 50              is an OpenTherm slave answering?
 *   'B' 56              module version; the one odd reply: 'V' 'A' 'C' 00 <fw>
 *   'M' 00 00 id 00 00  read the module's copy of an OpenTherm data id;
 *                       reply 'M' 00 type id hi lo
 *   'M' 02 01 id hi lo  write a data id
 *
 * Thermostat: with "hvac heat" the daemon holds the room at the target
 * temperature. What that target is (presets, week program, pre-heating,
 * frost protection) is decided in thermostat.c. The room temperature is the
 * average of the sensors chosen in the web interface (retoon/control, kept
 * by toon-ui) or else the Toon's own (retoon/climate, from toon-sensord):
 *   - OpenTherm: heat is requested when the flow temperature the control
 *     rule asks for is above 30 degrees; on/off: in pulses of a 20-minute
 *     period, as long as that flow asks for (thermostat.c). Either way at
 *     least three minutes pass between releasing and requesting again
 *   - on/off mode: the contact follows the request ('K' 4f)
 *   - OpenTherm mode: the heating bit in the master status (id 0) and a flow
 *     setpoint (id 1, see th_flow). The module
 *     drops the OpenTherm session after about 90 seconds without these two
 *     writes, so they are repeated every minute, also when idle (heating
 *     off, hot water enabled, setpoint 6 degrees: the idle values)
 *   - OpenTherm mode: every minute also the room setpoint (id 16) and the
 *     room temperature (id 24), otherwise the module keeps old values
 *   - no heat without a fresh room temperature, and none after the daemon
 *     stops
 * A new unit starts with the thermostat off; after that it comes back the
 * way it was left.
 *
 * OpenTherm is looked for at start and when asked to. Once it has worked on
 * this Toon (OT_FLAG, kept on p4 until someone chooses the on/off contact)
 * the boiler connection is OpenTherm and stays that: only a person switches
 * to the on/off contact. If the device stops answering, the module's
 * OpenTherm side is switched on again once (and every OT_REARM_S after
 * that), the keep-alive goes on, and after OT_ERROR_MISSES checks the state
 * says it does not answer (ot=0), until it does again. A heat pump goes into
 * a fault when its OpenTherm line goes quiet, so the line is never dropped.
 * Changes of the message go to the system log, with their time.
 *
 * Everything goes over the Toon's own MQTT broker (libtoonbus). State:
 * retoon/boiler and retoon/opentherm (retained JSON). Commands: one line
 * (or several) on retoon/boiler/set: "mode ot", "mode onoff", "hvac heat",
 * "hvac off", "target 20.5", "preset comfort|home|sleep|away",
 * "program resume", "max_flow 45" (0: the boiler's own), "hot_water 60". Without the broker there is no room temperature, so
 * no heat (see above).
 *
 * Instead of the module on its cable, the boiler can also be reached through
 * an ESP with an OpenTherm adapter running ESPHome (ESP_CONF: "host=",
 * "key=", optionally "port="; ToonSoftwareCollective/ReToon-esphome). The
 * module's frames are then answered from the ESP's entities (esp_transact):
 * a read of a data id gives the matching reading, writing id 1 sets its flow
 * setpoint, id 0 its heating, hot water and cooling switches, id 7 its
 * cooling level, ids 16/24 the room setpoint and temperature, id 56 its hot
 * water target. Everything else here stays the same, also the keep-alive
 * every minute, which the ESP needs: without it for five minutes it stops
 * asking for heat by itself. Cooling and the hot water setpoint need package
 * v2 on the ESP; v2 writes id 56 to the boiler only when it changes (boilers
 * keep it in memory that wears out). There is no on/off contact.
 *
 *   toon-boilerd [-v] [-d tty] [-c esp.conf]   -v: log every frame to stderr,
 *                                              -c: ESP settings (default ESP_CONF)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <termios.h>
#include <toonbus.h>
#include <time.h>
#include <unistd.h>
#include <toonbus.h>
#include "esphome.h"
#include "thermostat.h"

#define OT_FLAG		"/mnt/data/toon-os/config/boiler-opentherm"	/* it worked here */
#define HW_FLAG		"/mnt/data/toon-os/config/boiler-hot-water"	/* for the home page */
#define ESP_CONF	"/mnt/data/toon-os/config/boiler-esphome"	/* host=, key=, port= */
#define ESP_RETRY_S	15	/* connecting takes up to a few seconds: not too often */
#define ESP_BOOT_RETRY_S 3	/* ... but in the first two minutes after a start,
				   when WiFi may come up just after us */
#define ESP_BOOT_S	120
#define ESP_FRESH_S	120	/* a boiler reading this recent: OpenTherm answers */
#define CMD_TOPIC	"retoon/boiler/set"

#define FRAME		7
#define REPLY_MS	400	/* the module answers within about 150 ms */
#define KREPLY_MS	120	/* 'K' frames and writes are only echoed */
#define DETECT_MS	5000	/* time the module needs to find an OT slave */
#define CHECK_S		30	/* how often the link and OT presence are checked */
#define RETRY_S		5	/* re-probe cadence while the module is missing */
#define STARTUP_WAIT_MS	10000	/* let the module finish powering up at boot */
#define OT_ERROR_MISSES	2	/* failed checks in a row before "not answering" */
#define KEEPALIVE_S	60	/* master status + control setpoint interval */
#define OT_REARM_S	600	/* switching OpenTherm on again while it is silent */

#define MSG_NO_NEWS	0x0a	/* in a reply: nothing new since the last read */
#define SLAVE_DHW	0x01	/* id 3 high byte: the boiler has hot water */
#define SLAVE_COOL	0x04	/* ... and can cool */
#define COOL_STATUS	0x0600	/* id 0: cooling on, hot water enabled */
#define IDLE_STATUS	0x0200	/* id 0: heating off, hot water enabled */
#define HEAT_STATUS	0x0300	/* id 0: heating on, hot water enabled */
#define IDLE_SETPOINT	6.0	/* id 1 while not heating */

#define CLIMATE_BUS	"climate"	/* retoon/climate: the Toon's own sensor */
#define CONTROL_BUS	"control"	/* average of the chosen sensors (toon-ui) */
#define CONTROL_MAX_AGE	180
#define OUTSIDE_BUS	"outside"	/* the chosen entity, from toon-ui */
#define OUTSIDE_MAX_AGE	300
#define CLIMATE_MAX_AGE	60	/* seconds; older than this is no reading */
#define CONTROL_S	10
#define HYSTERESIS	0.2
#define MIN_OFF_S	180	/* pause before asking for heat again */
#define FLOW_MIN	30.0	/* OpenTherm flow setpoint range */
#define MAX_LOWEST	20.0	/* the lowest boiler maximum (id 57) taken as real:
				   low-temperature heat pumps and floor heating */
#define FLOW_MAX	80.0

enum { MODE_UNKNOWN, MODE_ONOFF, MODE_OT, MODE_DETECT };

static const char *const mode_name[] = { "unknown", "onoff", "opentherm", "detecting" };

/*
 * The table of data ids and how the module handles each of them. It runs
 * its own OpenTherm round and writes only store a value for it. Flags:
 * 0x01 read every round, 0x02 write every round, 0x08 read with data, 0x10
 * read once, 0x20 write once. A read flag goes before a write one. Id 56 is
 * read, and written only when it changes (send_hot_water).
 */
static const uint8_t mastermode[][2] = {
	{ 0, 0x08 }, { 1, 0x02 }, { 2, 0x02 }, { 3, 0x01 }, { 15, 0x01 },
	{ 17, 0x01 }, { 25, 0x01 }, { 5, 0x01 }, { 6, 0x01 }, { 9, 0x01 },
	{ 16, 0x02 }, { 18, 0x01 }, { 20, 0x03 }, { 21, 0x03 }, { 22, 0x03 },
	{ 24, 0x02 }, { 26, 0x01 }, { 27, 0x01 }, { 28, 0x01 }, { 35, 0x01 },
	{ 48, 0x01 }, { 56, 0x01 }, { 57, 0x01 }, { 70, 0x00 }, { 71, 0x00 },
	{ 72, 0x00 }, { 73, 0x00 }, { 74, 0x10 }, { 75, 0x10 }, { 76, 0x10 },
	{ 77, 0x00 }, { 87, 0x00 }, { 113, 0x01 }, { 114, 0x01 }, { 115, 0x01 },
	{ 116, 0x01 }, { 117, 0x01 }, { 118, 0x01 }, { 119, 0x01 }, { 120, 0x01 },
	{ 121, 0x01 }, { 122, 0x01 }, { 123, 0x01 }, { 125, 0x10 }, { 127, 0x10 },
	{ 202, 0x00 }, { 206, 0x00 }, { 39, 0x00 }, { 40, 0x00 }, { 92, 0x00 },
	{ 208, 0x00 }, { 93, 0x00 }, { 94, 0x00 },
	/* more values many boilers have, for the screen and Home Assistant
	   (in the module's slow ring; a boiler without them answers unknown
	   data id) */
	{ 19, 0x01 }, { 29, 0x01 }, { 30, 0x01 }, { 31, 0x01 }, { 32, 0x01 },
	{ 33, 0x01 }, { 34, 0x01 }, { 36, 0x01 },
};

/* Data ids published in the state file: what the module reads from the
   boiler (mastermode, read flags) and what is written to it */
static const uint8_t poll_ids[] = {
	0, 1, 3, 5, 16, 17, 18, 24, 25, 26, 27, 28, 48, 56, 57, 116, 120,
	9, 15, 35, 113, 114, 115, 117, 118, 119, 121, 122, 123, 125, 127,
	19, 29, 30, 31, 32, 33, 34, 36,
};

/*
 * The boiler's values that have no value of their own elsewhere, for the
 * screen and Home Assistant: retoon/opentherm, one key each,
 * "<key>=<value>|<unit>|<device class>|<state class>|<name>", only for what
 * the boiler answered (READ-ACK). Ids with their own entities (boiler and
 * return water, pressure, modulation, setpoints, status, faults, outside)
 * are not repeated here.
 */
#define OPENTHERM_BUS	"opentherm"
enum { F88, U16, S16, HB_U8, LB_U8, HB_S8, LB_S8 };
static const struct ot_value {
	uint8_t id, kind;
	const char *key, *unit, *dclass, *sclass, *name;
} ot_values[] = {
	{ 9, F88, "remote_setpoint", "\302\260C", "temperature", "measurement", "Remote room setpoint" },
	{ 15, HB_U8, "max_capacity", "kW", "power", "measurement", "Maximum power" },
	{ 15, LB_U8, "min_modulation", "%", "", "measurement", "Minimum modulation" },
	{ 19, F88, "hot_water_flow", "L/min", "volume_flow_rate", "measurement", "Hot water flow" },
	{ 29, F88, "solar_storage_temperature", "\302\260C", "temperature", "measurement", "Solar storage temperature" },
	{ 30, F88, "solar_collector_temperature", "\302\260C", "temperature", "measurement", "Solar collector temperature" },
	{ 31, F88, "boiler_water_2", "\302\260C", "temperature", "measurement", "Flow temperature circuit 2" },
	{ 32, F88, "hot_water_temperature_2", "\302\260C", "temperature", "measurement", "Hot water temperature 2" },
	{ 33, S16, "exhaust_temperature", "\302\260C", "temperature", "measurement", "Exhaust temperature" },
	{ 34, F88, "heat_exchanger_temperature", "\302\260C", "temperature", "measurement", "Heat exchanger temperature" },
	{ 36, F88, "flame_current", "\302\265A", "", "measurement", "Flame current" },
	{ 26, F88, "hot_water_temperature", "\302\260C", "temperature", "measurement", "Hot water temperature" },
	{ 35, HB_U8, "fan_setpoint", "Hz", "frequency", "measurement", "Fan speed setpoint" },
	{ 35, LB_U8, "fan_speed", "Hz", "frequency", "measurement", "Fan speed" },
	{ 48, HB_S8, "hot_water_max", "\302\260C", "temperature", "", "Hot water setpoint upper limit" },
	{ 48, LB_S8, "hot_water_min", "\302\260C", "temperature", "", "Hot water setpoint lower limit" },
	{ 113, U16, "burner_starts_failed", "", "", "total_increasing", "Unsuccessful burner starts" },
	{ 114, U16, "flame_signal_low", "", "", "total_increasing", "Flame signal too low" },
	{ 115, U16, "oem_diagnostic_code", "", "", "", "OEM diagnostic code" },
	{ 116, U16, "burner_starts", "", "", "total_increasing", "Burner starts" },
	{ 117, U16, "heating_pump_starts", "", "", "total_increasing", "Heating pump starts" },
	{ 118, U16, "hot_water_pump_starts", "", "", "total_increasing", "Hot water pump starts" },
	{ 119, U16, "hot_water_burner_starts", "", "", "total_increasing", "Hot water burner starts" },
	{ 120, U16, "burner_hours", "h", "duration", "total_increasing", "Burner hours" },
	{ 121, U16, "heating_pump_hours", "h", "duration", "total_increasing", "Heating pump hours" },
	{ 122, U16, "hot_water_pump_hours", "h", "duration", "total_increasing", "Hot water pump hours" },
	{ 123, U16, "hot_water_burner_hours", "h", "duration", "total_increasing", "Hot water burner hours" },
	{ 125, F88, "opentherm_version", "", "", "", "OpenTherm version (installation)" },
	{ 127, HB_U8, "product_type", "", "", "", "Installation product type" },
	{ 127, LB_U8, "product_version", "", "", "", "Installation product version" },
};

#define N_POLL	(sizeof(poll_ids) / sizeof(poll_ids[0]))

struct value {
	int valid;
	uint8_t type;
	uint16_t val;
};

static int tty = -1, verbose;
/* the OpenTherm module on an ESP (ESPHome) instead of the cable */
static int esp_mode, esp_port = 6053, esp_status;
static char esp_host[128], esp_key[64], esp_err[160];
static int mode = MODE_UNKNOWN, link_ok, ot_present;
static int ot_known;		/* OpenTherm worked on this Toon: it stays OpenTherm */
static int hw_known;		/* ... and the boiler has hot water */
static int ot_misses;		/* OpenTherm checks in a row without an answer */
static struct value values[N_POLL];
static char version[16];
static char message[120];

/* thermostat */
static int heating, cooling, room_ok, room_own;
static double cool_sent;	/* the level last written to id 7 */
static int cool_flag;		/* ... and the module was told to send it */
static int outside_ok;
static double outside;
static const char *outside_source = "";
static double hw_sent;		/* the hot water setpoint last dealt with */
static int hw_writing;		/* ... and the module is still busy with it */
static double room;
static long long last_release = -MIN_OFF_S * 1000LL;
static volatile sig_atomic_t stop;

static int ot_raw(int id, int *val);
static int slave_flags(void);

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static uint8_t crc8(const uint8_t *p, int n)
{
	uint8_t c = 0xff;
	int i;

	while (n--) {
		c ^= *p++;
		for (i = 0; i < 8; i++)
			c = c & 0x80 ? (c << 1) ^ 0x21 : c << 1;
	}
	return c;
}

static void log_frame(const char *dir, const uint8_t *f)
{
	logmsg("%s %02x %02x %02x %02x %02x %02x %02x", dir, f[0], f[1], f[2],
	       f[3], f[4], f[5], f[6]);
}

/* Read up to n bytes before the deadline; returns the number read. */
static int read_until(uint8_t *buf, int n, long long deadline)
{
	int got = 0;

	while (got < n) {
		struct pollfd pfd = { .fd = tty, .events = POLLIN };
		long long left = deadline - now_ms();
		int r;

		if (left <= 0 || poll(&pfd, 1, left) <= 0)
			break;
		r = read(tty, buf + got, n - got);
		if (r <= 0)
			break;
		got += r;
	}
	return got;
}

/*
 * Send one frame and collect the answer. The receive stream is scanned for a
 * valid frame with the same letter and bit 7 clear, which skips the echo and
 * resynchronises after noise. Returns 0 with the reply in rx, 1 if only the
 * echo came back (normal for 'K' and for writes), -1 if nothing did.
 */
static int esp_transact(char cmd, uint8_t b1, uint8_t b3, uint16_t val, uint8_t *rx);

static int transact(char cmd, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4,
		    uint8_t b5, uint8_t *rx)
{
	uint8_t tx[FRAME] = { cmd | 0x80, b1, b2, b3, b4, b5 }, buf[64];
	long long deadline;
	int len = 0, echo = 0, quiet = cmd == 'M' && !b1 && !verbose, i;
	int unanswered = cmd == 'K' || (cmd == 'M' && b1);

	if (esp_mode)
		return esp_transact(cmd, b1, b3, b4 << 8 | b5, rx);
	tx[6] = crc8(tx, 6);
	tcflush(tty, TCIFLUSH);
	if (write(tty, tx, FRAME) != FRAME)
		return -1;
	if (!quiet)
		log_frame("tx", tx);

	/* 14 bytes at 4800 baud take about 30 ms on the wire */
	deadline = now_ms() + (unanswered ? KREPLY_MS : REPLY_MS);
	while (len < (int)sizeof(buf)) {
		int r = read_until(buf + len, 1, deadline);

		if (r <= 0)
			break;
		len += r;
		for (i = 0; i + FRAME <= len; i++) {
			if (crc8(buf + i, 6) != buf[i + 6])
				continue;
			if (!memcmp(buf + i, tx, FRAME)) {
				echo = 1;
			} else if (buf[i] == (uint8_t)cmd) {
				memcpy(rx, buf + i, FRAME);
				if (!quiet)
					log_frame("rx", rx);
				return 0;
			}
		}
	}
	if (!quiet && !(echo && unanswered))
		logmsg("rx %s", echo ? "echo only" : "nothing");
	return echo ? 1 : -1;
}

/*
 * The version reply has its own layout and no CRC, so it is read as a raw
 * byte stream. The whole stream is scanned (not just what follows a clean
 * 7-byte echo), because the first frame after opening the port can come back
 * garbled or short.
 */
static int esp_open(void);

static int get_version(void)
{
	static const uint8_t tx[FRAME] = { 0xc2, 0x56, 0, 0, 0, 0, 0xdb };
	uint8_t buf[64];
	long long deadline;
	int len = 0, i;

	if (esp_mode)
		return esp_open();

	tcflush(tty, TCIFLUSH);
	if (write(tty, tx, FRAME) != FRAME)
		return -1;
	deadline = now_ms() + REPLY_MS;
	while (len < (int)sizeof(buf)) {
		int r = read_until(buf + len, 1, deadline);

		if (r <= 0)
			break;
		len += r;
		/* the echo's 0x56 is followed by 00, so it cannot match */
		for (i = 0; i + 5 <= len; i++)
			if (buf[i] == 'V' && buf[i + 1] >= 'A' && buf[i + 1] <= 'Z') {
				snprintf(version, sizeof(version), "%c%c %d",
					 buf[i + 1], buf[i + 2], buf[i + 4]);
				logmsg("module firmware %s", version);
				return 0;
			}
	}
	return -1;
}

static void write_opentherm(void)
{
	FILE *f;
	char *out = NULL;
	size_t out_len = 0;
	unsigned int i, j;

	/* the boiler's values only with OpenTherm; an ESP module's own sensor
	   (below) also without, as it needs no boiler */
	if (mode != MODE_OT && !esp_mode) {
		bus_put(OPENTHERM_BUS, NULL);
		return;
	}
	f = open_memstream(&out, &out_len);
	if (!f)
		return;
	for (i = 0; mode == MODE_OT && i < sizeof(ot_values) / sizeof(ot_values[0]); i++) {
		const struct ot_value *o = &ot_values[i];
		char v[16];

		for (j = 0; j < N_POLL && poll_ids[j] != o->id; j++)
			;
		/* only what the boiler answered itself: READ-ACK */
		if (j == N_POLL || !values[j].valid || values[j].type != 4)
			continue;
		/* id 9, 0: the boiler sets no room temperature (no override) */
		if (o->id == 9 && !values[j].val)
			continue;
		switch (o->kind) {
		case F88: snprintf(v, sizeof(v), "%.2f", (int16_t)values[j].val / 256.0); break;
		case U16: snprintf(v, sizeof(v), "%u", values[j].val); break;
		case S16: snprintf(v, sizeof(v), "%d", (int16_t)values[j].val); break;
		case HB_U8: snprintf(v, sizeof(v), "%u", values[j].val >> 8); break;
		case LB_U8: snprintf(v, sizeof(v), "%u", values[j].val & 0xff); break;
		case HB_S8: snprintf(v, sizeof(v), "%d", (int8_t)(values[j].val >> 8)); break;
		default: snprintf(v, sizeof(v), "%d", (int8_t)values[j].val); break;
		}
		fprintf(f, "%s=%s|%s|%s|%s|%s\n", o->key, v, o->unit, o->dclass, o->sclass, o->name);
	}
	/* an ESP module's own 1-wire sensor (ReToon-esphome, temperature.yaml):
	   usually next to the boiler, so no room temperature here; passed on
	   for Home Assistant, where it can be chosen like any sensor */
	if (esp_mode && esp_connected()) {
		double t;

		if (!esp_state("module_temperature", &t, NULL))
			fprintf(f, "module_temperature=%.2f|\302\260C|temperature|measurement|OpenTherm module temperature\n", t);
	}
	fclose(f);
	bus_put(OPENTHERM_BUS, out);
	free(out);
}

static void write_state(void)
{
	char *out = NULL;
	size_t out_len = 0;
	FILE *f = open_memstream(&out, &out_len);
	unsigned int i;

	if (!f)
		return;
	fprintf(f, "link=%s\nmode=%s\not=%d\n", link_ok ? "ok" : "down",
		mode_name[mode], ot_present);
	fprintf(f, "hvac=%s\ntarget=%.1f\nheat=%d\ncool=%d\n", th.hvac ? "heat" : "off",
		th.target, heating, cooling);
	fprintf(f, "external=%d\n", th.external && (!link_ok || mode == MODE_ONOFF ||
					     (mode == MODE_OT && slave_flags() >= 0 &&
					      !(slave_flags() & SLAVE_COOL))));
	if (outside_ok)
		fprintf(f, "outside=%.1f\noutside_source=%s\n", outside, outside_source);
	if (room_ok)
		fprintf(f, "room=%.2f\nroom_source=%s\n", room, room_own ? "toon" : "sensors");
	th_state(f);
	fprintf(f, "version=%s\n", version);
	fprintf(f, "transport=%s\n", esp_mode ? "esphome" : "module");
	if (esp_mode) {
		fprintf(f, "esp_host=%s\n", esp_host);
		if (esp_connected()) {
			double t;

			fprintf(f, "esp_name=%s\nesp_version=%s\nesp_project_version=%s\n",
				esp_name(), esp_version(), esp_project_version());
			/* its 1-wire sensor, for the web page */
			if (!esp_state("module_temperature", &t, NULL))
				fprintf(f, "esp_temperature=%.1f\n", t);
		}
	}
	fprintf(f, "message=%s\n", message);
	fprintf(f, "ot_known=%d\n", ot_known);
	if (mode == MODE_OT && slave_flags() >= 0)
		fprintf(f, "ot_dhw=%d\not_cool=%d\n", !!(slave_flags() & SLAVE_DHW),
			!!(slave_flags() & SLAVE_COOL));
	if (mode == MODE_OT && slave_flags() >= 0 && (slave_flags() & SLAVE_DHW)) {
		int v;

		if (th_hot_water() > 0)
			fprintf(f, "hw_wanted=%.0f\n", th_hot_water());
		if (!ot_raw(56, &v))
			fprintf(f, "hw_setpoint=%.0f\n", (int16_t)v / 256.0);
		if (!ot_raw(26, &v))
			fprintf(f, "hw_temp=%.0f\n", (int16_t)v / 256.0);
		if (!ot_raw(0, &v))
			fprintf(f, "hw_active=%d\n", !!(v & 0x04));
		if (!ot_raw(48, &v) && (int8_t)(v >> 8) > (int8_t)v)
			fprintf(f, "hw_min=%d\nhw_max=%d\n", (int8_t)v, (int8_t)(v >> 8));
	}
	/* id<number>=<OpenTherm reply type> <16-bit value, hex> */
	for (i = 0; i < N_POLL; i++)
		if (values[i].valid)
			fprintf(f, "id%d=%d %04x\n", poll_ids[i], values[i].type,
				values[i].val);
	fclose(f);
	bus_put("boiler", out);
	free(out);
	write_opentherm();
}

static void set_message(const char *fmt, ...)
{
	va_list ap;

	char old[sizeof(message)];

	snprintf(old, sizeof(old), "%s", message);
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	logmsg("%s", message);
	if (strcmp(old, message))
		syslog(LOG_INFO, "%s", message);
}

static void clear_values(void)
{
	memset(values, 0, sizeof(values));
	hw_sent = 0;
	hw_writing = 0;
	cool_flag = 0;
}

/* 1 = an OpenTherm slave answers the module, 0 = not, -1 = no reply. */
static int check_ot(void)
{
	uint8_t rx[FRAME];

	if (transact('B', 0x50, 0, 0, 0, 0, rx))
		return -1;
	return rx[5] != 0;
}

/* Value of a polled data id as f8.8, or fallback if the boiler has none. */
/* the last reply for an id, when the boiler knows it */
static int ot_raw(int id, int *val)
{
	unsigned int i;

	for (i = 0; i < N_POLL; i++)
		if (poll_ids[i] == id && values[i].valid && values[i].type != 6 &&
		    values[i].type != 7) {
			*val = values[i].val;
			return 0;
		}
	return -1;
}

/* the high byte of id 3, -1 = not known (yet) */
static int slave_flags(void)
{
	int v;

	return ot_raw(3, &v) ? -1 : (v >> 8) & 0xff;
}

static double ot_value(int id, double fallback)
{
	int v;

	return ot_raw(id, &v) ? fallback : (int16_t)v / 256.0;
}

/* write a temperature to an OpenTherm data id */
static void ot_write(uint8_t id, double value)
{
	uint8_t rx[FRAME];
	int v = value * 256;

	transact('M', 2, 1, id, (v >> 8) & 0xff, v & 0xff, rx);
}

/*
 * The hot water setpoint, within the boiler's own limits (id 48: upper in the
 * high byte, lower in the low one). Only when it changes, written once:
 * boilers can keep this setting in memory that wears out.
 */
static void send_hot_water(void)
{
	uint8_t rx[FRAME];
	double want = th_hot_water(), set = ot_value(56, -1);
	int limits;

	/* through an ESP only from package v2, which writes it once */
	if (esp_mode && !esp_has_entity("hot_water_target"))
		return;

	if (hw_writing) {
		/* the module clears the flag when the boiler has answered */
		if (!transact('K', 0x53, 0, 56, 0, 0, rx) && !(rx[4] & 0x20)) {
			transact('K', 0x53, 1, 56, 1, 0, rx);
			hw_writing = 0;
		}
		return;
	}
	if (want <= 0 || set < 0 || slave_flags() < 0 || !(slave_flags() & SLAVE_DHW))
		return;
	if (!ot_raw(48, &limits) && (int8_t)(limits >> 8) > (int8_t)limits) {
		if (want > (int8_t)(limits >> 8))
			want = (int8_t)(limits >> 8);
		if (want < (int8_t)limits)
			want = (int8_t)limits;
	}
	if (want == hw_sent)
		return;
	/* after a start, it may be right already; later the reply can be old */
	if (hw_sent || fabs(want - set) >= 0.5) {
		ot_write(56, want);
		if (!esp_mode) {
			transact('K', 0x53, 1, 56, 0x20, 0, rx);
			hw_writing = 1;
		}
		logmsg("hot water setpoint %.0f", want);
	}
	hw_sent = want;
}

/*
 * Tell the module what is wanted now. In OpenTherm mode this is also what
 * keeps its session with the boiler alive.
 */
/* the boiler said over OpenTherm that it can cool */
static int boiler_cools(void)
{
	int flags = slave_flags();

	return mode == MODE_OT && flags >= 0 && (flags & SLAVE_COOL);
}

static void send_demand(void)
{
	uint8_t rx[FRAME];
	double flow = IDLE_SETPOINT, max, level;
	/* cooling for an external device only: the boiler is just not asked
	   for heat */
	int status = cooling && boiler_cools() ? COOL_STATUS : IDLE_STATUS;

	if (mode == MODE_ONOFF) {
		transact('K', 0x4f, 0, 0, 0, heating, rx);
		return;
	}
	if (mode != MODE_OT)
		return;
	if (heating) {
		/* id 57 is the boiler's own maximum heating setpoint */
		max = ot_value(57, FLOW_MAX);
		if (max < MAX_LOWEST || max > FLOW_MAX)
			max = FLOW_MAX;
		flow = th_flow(room, max);
		status = HEAT_STATUS;
	}
	/* tap water not kept warm: the hot water switch off */
	if (!th_hot_water_keep_warm())
		status &= ~0x0200;
	ot_write(1, flow);
	transact('M', 2, 1, 0, status >> 8, status & 0xff, rx);
	/* the cooling control signal: a degree above the target is everything */
	if ((th.cool_ok && boiler_cools()) || cool_sent) {
		level = !cooling ? 0 : 100 * (room - th.eff_target + HYSTERESIS);
		if (cooling)
			level = level < 10 ? 10 : level > 100 ? 100 : level;
		/* the module only stores a value; flag 2 makes it send this id */
		if (!cool_flag) {
			transact('K', 0x53, 1, 7, 2, 0, rx);
			cool_flag = 1;
		}
		ot_write(7, level);
		cool_sent = level;
	}
	/* for what is on the other end, a heat pump may follow these */
	if (room_ok) {
		ot_write(16, th.eff_target);
		ot_write(24, room);
	}
	send_hot_water();
}

/* "temperature" from retoon/<name> when it is not older than max_age seconds */
static int read_temperature(const char *name, int max_age, double *out)
{
	char val[32], *end;
	long age = bus_age(name);
	double v;

	if (age < 0 || age > max_age || bus_value(name, "temperature", val, sizeof(val)))
		return 0;
	v = strtod(val, &end);
	/* "nan" or nonsense is no reading: no heat on it */
	if (end == val || !isfinite(v) || v <= -30 || v >= 60)
		return 0;
	*out = v;
	return 1;
}

/* the chosen sensors when they are there, the Toon's own otherwise */
static void read_room(void)
{
	static int was_own = -1;
	char sensors[96] = "";
	static char was_sensors[96];

	room_own = 0;
	room_ok = read_temperature(CONTROL_BUS, CONTROL_MAX_AGE, &room);
	if (!room_ok) {
		room_ok = read_temperature(CLIMATE_BUS, CLIMATE_MAX_AGE, &room);
		room_own = 1;
	}
	/* which sensors: toon-ui's code for the set in its average (with the
	   own sensor's correction), or the own sensor's correction alone */
	if (bus_value(room_own ? CLIMATE_BUS : CONTROL_BUS, room_own ? "temp_offset" : "set",
		      sensors, sizeof(sensors)))
		sensors[0] = 0;
	/* from another sensor (or another set of them, or a new correction)
	   now: what the thermostat was measuring about the house starts again */
	if (room_ok && (room_own != was_own || strcmp(sensors, was_sensors))) {
		if (was_own >= 0)
			th_sensor_changed();
		was_own = room_own;
		snprintf(was_sensors, sizeof(was_sensors), "%s", sensors);
	}
}

/* OpenTherm's own (id 27) when it is a real value, or else Home Assistant's */
static void read_outside(void)
{
	int v;

	outside_source = "opentherm";
	outside_ok = 0;
	if (!ot_raw(27, &v) && (int16_t)v / 256.0 > -40 && (int16_t)v / 256.0 < 60) {
		outside = (int16_t)v / 256.0;
		outside_ok = 1;
		return;
	}
	outside_source = "entity";
	outside_ok = read_temperature(OUTSIDE_BUS, OUTSIDE_MAX_AGE, &outside);
}

/* The thermostat decision; returns non-zero if the request changed. */
static int control(void)
{
	int want = heating, want_cool = cooling, changed = 0;
	int flags = slave_flags();
	int boiler_cool = mode != MODE_OT ? 0 : flags < 0 ? -1 : !!(flags & SLAVE_COOL);
	/* the setting for an external device: without a boiler module heat
	   and cool are decided as for an on/off boiler, only the requests go
	   out (state file, Home Assistant). Next to a boiler that can not
	   cool (on/off, or OpenTherm without cooling) the cooling goes to the
	   external device: the boiler is then just not asked for heat. */
	int ext = !link_ok && th.external;
	int ext_cool = link_ok && th.external && !boiler_cool &&
		       (mode == MODE_ONOFF || mode == MODE_OT);
	int can_cool = ext || ext_cool ? 1 : boiler_cool;
	int ok = link_ok || ext;

	double max = ot_value(57, FLOW_MAX);

	/* the same check as when the setpoint is sent: what the boiler
	   really allows (a floor heating boiler may say 35) */
	if (max < MAX_LOWEST || max > FLOW_MAX)
		max = FLOW_MAX;
	read_room();
	read_outside();
	/* the boiler's own flow reading, for learning with the water it really
	   delivered (a district heating set may not give one) */
	th.flow_meas = mode == MODE_OT ? ot_value(25, -1) : -1;
	/* for the measurement log: what else the boiler says about itself */
	th.ret_meas = mode == MODE_OT ? ot_value(28, -1) : -1;
	th.modulation = mode == MODE_OT ? ot_value(17, -1) : -1;
	{
		int v;

		th.flame = mode == MODE_OT && !ot_raw(0, &v) ? !!(v & 0x08) : -1;
	}
	th.can_heat = ok;
	th_tick(room_ok, room, heating, mode == MODE_OT, max, can_cool, outside_ok, outside);
	if (th.cooling) {
		want = 0;
		if (!th.hvac || !room_ok || !ok)
			want_cool = 0;
		else if (room >= th.eff_target + HYSTERESIS)
			want_cool = 1;
		else if (room <= th.eff_target - HYSTERESIS)
			want_cool = 0;
	} else {
		want_cool = 0;
		/* with cooling chosen, not before the boiler said it can */
		if (!th.active || !room_ok || !ok || (th.cool_wanted && can_cool < 0))
			want = 0;
		else if (th.by_flow || th.pulse)
			want = th.want;	/* the flow temperature asked for decides */
		/* the room band: what the radiators still hold counts (once learned) */
		else if (heating ? th.lag_stop : th.lag_nostart)
			want = 0;
		else if (room <= th.eff_target - HYSTERESIS)
			want = 1;
		else if (room >= th.eff_target + HYSTERESIS)
			want = 0;
	}

	if (heating && !want) {
		heating = 0;
		changed = 1;
	}
	if (cooling && !want_cool) {
		cooling = 0;
		changed = 1;
	}
	if (changed)
		last_release = now_ms();
	if (now_ms() - last_release >= MIN_OFF_S * 1000LL) {
		if (want && !heating) {
			heating = 1;
			changed = 1;
		}
		if (want_cool && !cooling) {
			cooling = 1;
			changed = 1;
		}
	}
	if (!changed)
		return 0;
	logmsg("%s (room %.2f, target %.1f)", heating ? "heat on" : cooling ? "cool on" : "idle",
	       room, th.eff_target);
	send_demand();
	return 1;
}

static void load_mastermode(void)
{
	uint8_t rx[FRAME];
	unsigned int i;

	for (i = 0; i < sizeof(mastermode) / sizeof(mastermode[0]) && !stop; i++)
		transact('K', 0x53, 1, mastermode[i][0], mastermode[i][1], 0, rx);
}

static void use_onoff(void)
{
	uint8_t rx[FRAME];

	transact('K', 0x45, 0, 0, 0, 0, rx);
	mode = MODE_ONOFF;
	ot_present = 0;
	ot_misses = 0;
	clear_values();
	send_demand();
}

static void set_flag(const char *path, int on)
{
	FILE *f;

	if (system("mount -o remount,rw /mnt/data 2>/dev/null"))
		return;
	if (on) {
		f = fopen(path, "w");
		if (f)
			fclose(f);
	} else {
		unlink(path);
	}
	/* not while toon-update is writing its download there */
	if (access("/run/toon/update.lock", F_OK) &&
	    system("sync; mount -o remount,ro /mnt/data 2>/dev/null"))
		return;
}

/* Remember on p4 that OpenTherm works here (or, chosen away, that it does
   not matter): only written when it changes. */
static void remember_ot(int known)
{
	if (known == ot_known)
		return;
	ot_known = known;
	set_flag(OT_FLAG, known);
	if (!known) {
		hw_known = 0;
		set_flag(HW_FLAG, 0);
	}
}

/* what the boiler says it has, once it has said so (id 3) */
static void remember_hot_water(void)
{
	int flags = slave_flags();

	if (flags >= 0 && !!(flags & SLAVE_DHW) != hw_known) {
		hw_known = !!(flags & SLAVE_DHW);
		set_flag(HW_FLAG, hw_known);
	}
}

/*
 * Let the module look for an OpenTherm device: 1 found, 0 not, -1 no answer.
 * On a Toon where OpenTherm worked before it stays OpenTherm either way (the
 * device may still be starting, as after a power cut); otherwise nothing
 * found means the on/off contact.
 */
static int detect_ot(void)
{
	uint8_t rx[FRAME];
	long long until;
	int r;

	mode = MODE_DETECT;
	write_state();
	transact('K', 0x45, 0, 0, 0, 1, rx);
	until = now_ms() + (esp_mode ? 0 : DETECT_MS);
	while (!stop && now_ms() < until)
		usleep(100000);
	r = check_ot();
	ot_misses = r == 1 ? 0 : OT_ERROR_MISSES;
	/* an ESP has no on/off contact: there it stays OpenTherm and keeps
	   trying, so a boiler connected later is found by itself */
	if (r == 1 || ot_known || esp_mode) {
		mode = MODE_OT;
		ot_present = r == 1;
		send_demand();
		/* that flag is about the module on the cable */
		if (r == 1 && !esp_mode)
			remember_ot(1);
	} else {
		use_onoff();
	}
	return r;
}

static void set_mode(int want)
{
	int r;

	if (want == MODE_ONOFF && esp_mode) {
		set_message("an OpenTherm module on an ESP has no on/off contact");
		write_state();
		return;
	}
	if (want == MODE_ONOFF) {
		/* chosen: no looking for OpenTherm again */
		remember_ot(0);
		use_onoff();
		set_message("on/off contact selected");
		write_state();
		return;
	}

	set_message("looking for an OpenTherm device");
	r = detect_ot();
	if (r == 1)
		set_message("OpenTherm device found");
	else if (ot_known || esp_mode)
		set_message("OpenTherm device not answering; OpenTherm stays on, the Toon keeps trying");
	else
		set_message(r ? "module did not answer" :
			    "no OpenTherm device found, back to on/off");
	write_state();
}

/*
 * Start with on/off first, load the table, then look for an
 * OpenTherm device once. After this OpenTherm is only tried again on request.
 */
static void startup(int wait)
{
	uint8_t rx[FRAME];
	long long until = now_ms() + (wait ? STARTUP_WAIT_MS : 0);

	/*
	 * At power-on the module may still be starting, so at boot keep asking
	 * for a while before reporting it missing; later retries try once and
	 * rely on RETRY_S.
	 */
	link_ok = 0;
	if (wait) {
		set_message("waiting for the boiler module");
		write_state();
	}
	for (;;) {
		link_ok = !get_version() || check_ot() >= 0;
		if (link_ok || stop || now_ms() >= until || esp_mode)
			break;
		usleep(300000);
	}
	if (!link_ok) {
		if (esp_mode)
			set_message("OpenTherm module (ESPHome) not reachable: %s", esp_err);
		else if (wait)
			set_message("no boiler module");
		write_state();
		return;
	}
	link_ok = 1;
	if (esp_mode) {
		/* no contact, no table: OpenTherm at once */
		set_message("OpenTherm module (ESPHome) connected: %s", esp_name());
		set_mode(MODE_OT);
		return;
	}
	transact('B', 0x4a, 0, 0, 0, 0, rx);	/* clears "it restarted" */
	use_onoff();
	load_mastermode();
	set_mode(MODE_OT);
}


/* ---- the OpenTherm module on an ESP (ESPHome) ------------------------ */

/* which ESP entity holds which OpenTherm data id (ToonSoftwareCollective/
   ReToon-esphome, opentherm.yaml: the entity names in snake case) */
enum { E_F88, E_S16, E_U16, E_HI, E_LO, E_HI_S8, E_LO_S8 };
static const struct esp_map {
	uint8_t id, kind;
	const char *entity;
} esp_map[] = {
	{ 1, E_F88, "flow_setpoint" },
	{ 16, E_F88, "room_setpoint" },
	{ 17, E_F88, "modulation" },
	{ 18, E_F88, "water_pressure" },
	{ 19, E_F88, "hot_water_flow" },
	{ 24, E_F88, "room_temperature" },
	{ 25, E_F88, "boiler_water" },
	{ 26, E_F88, "hot_water_temperature" },
	{ 27, E_F88, "outside_temperature" },
	{ 28, E_F88, "return_water" },
	{ 33, E_S16, "exhaust_temperature" },
	{ 35, E_HI, "fan_speed_setpoint" },
	{ 35, E_LO, "fan_speed" },
	{ 36, E_F88, "flame_current" },
	{ 48, E_HI_S8, "hot_water_setpoint_upper_limit" },
	{ 48, E_LO_S8, "hot_water_setpoint_lower_limit" },
	{ 56, E_F88, "hot_water_setpoint" },
	{ 57, E_F88, "maximum_flow_setpoint" },
	{ 116, E_U16, "burner_starts" },
	{ 117, E_U16, "heating_pump_starts" },
	{ 118, E_U16, "hot_water_pump_starts" },
	{ 119, E_U16, "hot_water_burner_starts" },
	{ 120, E_U16, "burner_hours" },
	{ 121, E_U16, "heating_pump_hours" },
	{ 122, E_U16, "hot_water_pump_hours" },
	{ 123, E_U16, "hot_water_burner_hours" },
	{ 125, E_F88, "boiler_opentherm_version" },
	{ 127, E_HI, "boiler_product_type" },
	{ 127, E_LO, "boiler_product_version" },
};

/* status bits (ids 0, 3, 5) from binary sensors */
static const struct esp_bit {
	uint8_t id, bit;
	const char *entity;
} esp_bits[] = {
	{ 0, 0, "fault" }, { 0, 1, "heating_active" }, { 0, 2, "hot_water_active" },
	{ 0, 3, "flame" }, { 0, 4, "cooling_active" }, { 0, 6, "diagnostic" },
	{ 3, 8, "has_hot_water" }, { 3, 9, "on_off_control_type" }, { 3, 10, "can_cool" },
	{ 5, 8, "service_request" }, { 5, 9, "lockout_reset_possible" },
	{ 5, 10, "low_water_pressure" }, { 5, 11, "flame_fault" },
	{ 5, 12, "air_pressure_fault" }, { 5, 13, "water_over_temperature" },
};

/* the ESP's last address, for when its name is not found (esp_set_fallback):
   "host=<the name it was for>", "addr=<IPv4>"; written when it changes */
#define ESP_ADDR_FILE	"/mnt/data/toon-os/config/boiler-esphome.addr"
static char esp_saved_addr[INET_ADDRSTRLEN];

static void esp_load_addr(void)
{
	FILE *f = fopen(ESP_ADDR_FILE, "r");
	char line[200], host[128] = "", addr[INET_ADDRSTRLEN] = "";

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "host=", 5))
			snprintf(host, sizeof(host), "%.127s", line + 5);
		else if (!strncmp(line, "addr=", 5))
			snprintf(addr, sizeof(addr), "%.15s", line + 5);
	}
	fclose(f);
	/* only for the name it was found for */
	if (!strcmp(host, esp_host)) {
		snprintf(esp_saved_addr, sizeof(esp_saved_addr), "%s", addr);
		esp_set_fallback(addr);
	}
}

static void esp_save_addr(void)
{
	const char *addr = esp_address();
	FILE *f;

	if (!addr[0] || !strcmp(addr, esp_saved_addr))
		return;
	if (system("mount -o remount,rw /mnt/data 2>/dev/null"))
		return;
	f = fopen(ESP_ADDR_FILE ".new", "w");
	if (f) {
		fprintf(f, "host=%s\naddr=%s\n", esp_host, addr);
		if (!fclose(f) && !rename(ESP_ADDR_FILE ".new", ESP_ADDR_FILE)) {
			snprintf(esp_saved_addr, sizeof(esp_saved_addr), "%s", addr);
			esp_set_fallback(addr);
		}
	}
	/* not while toon-update is writing its download there */
	if (access("/run/toon/update.lock", F_OK))
		(void)!system("sync; mount -o remount,ro /mnt/data 2>/dev/null");
}

static void esp_load_conf(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[200];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "host=", 5))
			snprintf(esp_host, sizeof(esp_host), "%.127s", line + 5);
		else if (!strncmp(line, "key=", 4))
			snprintf(esp_key, sizeof(esp_key), "%.63s", line + 4);
		else if (!strncmp(line, "port=", 5) && atoi(line + 5) > 0)
			esp_port = atoi(line + 5);
	}
	fclose(f);
	esp_mode = esp_host[0] && esp_key[0];
	if (esp_mode)
		esp_load_addr();
}

/* the newest of the boiler's readings is recent: OpenTherm answers */
static int esp_boiler_answers(void)
{
	static const char *const ids[] = { "boiler_water", "return_water", "modulation" };
	unsigned int i;

	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		double v;
		long long age;

		if (!esp_state(ids[i], &v, &age) && age < ESP_FRESH_S * 1000LL)
			return 1;
	}
	return 0;
}

/* connect when not connected: 0 = up (version filled in), -1 = not */
static int esp_open(void)
{
	long long until;

	if (esp_connected())
		return 0;
	if (esp_connect(esp_host, esp_port, esp_key, esp_err, sizeof(esp_err)))
		return -1;
	if (strcmp(esp_project(), "retoon.opentherm")) {
		snprintf(esp_err, sizeof(esp_err), "%s is no ReToon OpenTherm module (project \"%s\")",
			 esp_name(), esp_project());
		esp_close();
		return -1;
	}
	esp_err[0] = 0;
	esp_save_addr();
	snprintf(version, sizeof(version), "ESPHome %.7s", esp_version());
	logmsg("OpenTherm module %s: ESPHome %s, %s %s", esp_name(), esp_version(),
	       esp_project(), esp_project_version());
	/* the states come right after the subscription: wait a moment for
	   them, so the first check already sees the boiler */
	until = now_ms() + 2000;
	while (now_ms() < until && !esp_boiler_answers()) {
		struct pollfd p = { .fd = esp_fd(), .events = POLLIN };

		if (poll(&p, 1, 100) > 0 && esp_poll())
			return -1;
	}
	return 0;
}

/* a reading as the module would give it: 0 with the reply in rx */
static int esp_read(uint8_t id, uint8_t *rx)
{
	unsigned int i;
	int found = 0, known = 0;
	uint16_t v = 0;
	double d;

	rx[3] = id;
	for (i = 0; i < sizeof(esp_bits) / sizeof(esp_bits[0]); i++) {
		if (esp_bits[i].id != id)
			continue;
		found = 1;
		if (esp_state(esp_bits[i].entity, &d, NULL))
			continue;
		known = 1;
		if (d)
			v |= 1 << esp_bits[i].bit;
	}
	/* id 0's high byte is the master status: what was sent last */
	if (id == 0)
		v |= esp_status & 0xff00;
	for (i = 0; i < sizeof(esp_map) / sizeof(esp_map[0]); i++) {
		int x;

		if (esp_map[i].id != id || !esp_has_entity(esp_map[i].entity))
			continue;
		found = 1;
		if (esp_state(esp_map[i].entity, &d, NULL))
			continue;
		known = 1;
		x = lround(esp_map[i].kind == E_F88 ? d * 256 : d);
		switch (esp_map[i].kind) {
		case E_F88: case E_S16: case E_U16: v = x; break;
		case E_HI: case E_HI_S8: v = (v & 0x00ff) | (x & 0xff) << 8; break;
		default: v = (v & 0xff00) | (x & 0xff); break;
		}
	}
	/* 4 READ-ACK, 7 UNKNOWN-DATAID; not read yet: nothing new */
	rx[2] = !found ? 7 : known ? 4 : MSG_NO_NEWS;
	rx[4] = v >> 8;
	rx[5] = v & 0xff;
	return 0;
}

static void esp_write(uint8_t id, uint16_t val)
{
	switch (id) {
	case 0:
		esp_status = val;
		esp_set_switch("heating", !!(val & 0x0100));
		esp_set_switch("hot_water", !!(val & 0x0200));
		if (esp_has_entity("cooling"))		/* from package v2 */
			esp_set_switch("cooling", !!(val & 0x0400));
		break;
	case 1:
		/* also the keep-alive the ESP waits for */
		esp_set_number("flow_setpoint", round((int16_t)val / 25.6) / 10);
		break;
	case 7:
		if (esp_has_entity("cooling_level"))
			esp_set_number("cooling_level", round((int16_t)val / 256.0));
		break;
	/* to a tenth, the entities' step: f8.8 would show as 20.95703 °C
	   wherever the ESP is seen (Home Assistant) */
	case 16:
		esp_set_number("room_setpoint", round((int16_t)val / 25.6) / 10);
		break;
	case 24:
		esp_set_number("room_temperature", round((int16_t)val / 25.6) / 10);
		break;
	case 56:
		/* the ESP writes it to the boiler once, and reads it after */
		esp_set_number("hot_water_target", (int16_t)val / 256.0);
		break;
	}
}

/* the module's frames, answered from the ESP (see the top) */
static int esp_transact(char cmd, uint8_t b1, uint8_t b3, uint16_t val, uint8_t *rx)
{
	memset(rx, 0, FRAME);
	rx[0] = cmd;
	if (!esp_connected())
		return -1;
	switch (cmd) {
	case 'B':
		rx[1] = b1;
		if (b1 == 0x50)
			rx[5] = esp_boiler_answers();
		return 0;
	case 'M':
		if (!b1)
			return esp_read(b3, rx);
		esp_write(b3, val);
		return 1;
	}
	/* 'K': the module's own settings; only echoed */
	return 1;
}

static void esp_lost(void)
{
	link_ok = 0;
	mode = MODE_UNKNOWN;
	ot_present = 0;
	clear_values();
	set_message("OpenTherm module (ESPHome) connection lost");
	write_state();
}

static void handle_command(char *line)
{
	char *src = NULL;
	int done;

	line[strcspn(line, "\r\n")] = 0;
	/* "@screen target 21.5": who sent it, for the change log */
	if (line[0] == '@' && strchr(line, ' ')) {
		src = line + 1;
		line = strchr(line, ' ');
		*line++ = 0;
	}
	logmsg("command: %s%s%s", line, src ? " from " : "", src ? src : "");
	th_source(src);
	done = th_command(line);
	th_source(NULL);
	if (done)
		return;
	if (!link_ok)
		return;
	if (!strcmp(line, "mode ot"))
		set_mode(MODE_OT);
	else if (!strcmp(line, "mode onoff"))
		set_mode(MODE_ONOFF);
}

static int open_tty(const char *dev)
{
	struct termios tio;
	int fd = open(dev, O_RDWR | O_NOCTTY);

	if (fd < 0)
		return -1;
	memset(&tio, 0, sizeof(tio));
	/* 8 data bits, even parity; the module ignores frames without it */
	tio.c_cflag = CS8 | PARENB | CREAD | CLOCAL;
	tio.c_iflag = INPCK | IGNPAR;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	cfsetispeed(&tio, B4800);
	cfsetospeed(&tio, B4800);
	tcsetattr(fd, TCSANOW, &tio);
	return fd;
}

/* commands from retoon/boiler/set, handled in the main loop's thread */
static int cmd_seen;

static void on_cmd(const char *topic, const char *payload)
{
	char buf[512], *cmd, *save;

	(void)topic;
	snprintf(buf, sizeof(buf), "%s", payload);
	/* several commands may come in one message; one without "@<who> " is
	   from an app or another client of the broker */
	for (cmd = strtok_r(buf, "\n", &save); cmd; cmd = strtok_r(NULL, "\n", &save)) {
		char line[160];
		size_t i;

		for (i = 0; cmd[i]; i++)	/* printable text only */
			if ((unsigned char)cmd[i] < 0x20 || (unsigned char)cmd[i] > 0x7e)
				break;
		if (cmd[i] && cmd[i] != '\r')
			continue;
		snprintf(line, sizeof(line), "%s%s", cmd[0] == '@' ? "" : "@app ", cmd);
		handle_command(line);
	}
	cmd_seen = 1;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttymxc0", *esp_conf = ESP_CONF;
	long long next_check = 0, started = now_ms();
	unsigned int next_id = 0;
	long long next_idle = 0, next_control = 0;
	int opt, misses = 0;

	while ((opt = getopt(argc, argv, "vd:c:")) != -1) {
		if (opt == 'v')
			verbose = 1;
		else if (opt == 'd')
			dev = optarg;
		else if (opt == 'c')
			esp_conf = optarg;
		else
			return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	mkdir("/run/toon", 0755);
	esp_load_conf(esp_conf);
	if (!esp_mode) {
		tty = open_tty(dev);
		if (tty < 0) {
			perror(dev);
			return 1;
		}
	}
	bus_open("boilerd");
	bus_watch(CLIMATE_BUS);
	bus_watch(CONTROL_BUS);
	bus_watch(OUTSIDE_BUS);
	bus_on(CMD_TOPIC, on_cmd);

	openlog("toon-boilerd", 0, LOG_DAEMON);
	ot_known = !access(OT_FLAG, F_OK);
	hw_known = !access(HW_FLAG, F_OK);
	th_init();
	startup(1);
	next_check = now_ms() + CHECK_S * 1000;

	int wait = 190;

	while (!stop) {
		struct pollfd pfd[2] = {
			{ .fd = bus_fd(), .events = POLLIN },
			{ .fd = esp_fd(), .events = POLLIN },
		};
		uint8_t rx[FRAME];
		int n;

		/* the module is paced at about five frames a second; just after
		   a command, not: what it changed is shown at once */
		n = poll(pfd, esp_mode && esp_fd() >= 0 ? 2 : 1, wait);
		wait = 190;
		/* the ESP: its states come in several times a second, so they do
		   not end this round (that would starve the rest) */
		if (esp_mode && esp_connected() &&
		    ((n > 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) && esp_poll()) ||
		     esp_tick())) {
			esp_lost();
			next_check = now_ms() + ESP_RETRY_S * 1000;	/* the ESP restarts */
		}
		if (n > 0 && (pfd[0].revents & POLLIN)) {
			bus_poll();
			if (cmd_seen) {
				cmd_seen = 0;
				next_check = now_ms() + CHECK_S * 1000;
				next_control = 0;
				wait = 0;
			}
			continue;
		}

		if (now_ms() >= next_control) {
			next_control = now_ms() + CONTROL_S * 1000;
			if (control())
				next_idle = now_ms() + KEEPALIVE_S * 1000;
			write_state();
		}

		if (!link_ok) {
			if (now_ms() >= next_check) {
				startup(0);
				next_check = now_ms() + (!esp_mode ? RETRY_S :
					now_ms() - started < ESP_BOOT_S * 1000 ? ESP_BOOT_RETRY_S : ESP_RETRY_S) * 1000;
			}
			continue;
		}

		if (now_ms() >= next_check) {
			int r = check_ot();

			/* a restarted module has forgotten its flags and its mode */
			if (!transact('B', 0x4a, 0, 0, 0, 0, rx) && rx[1] == 0x4a && rx[5]) {
				set_message("boiler module restarted");
				clear_values();
				startup(0);
				next_check = now_ms() + CHECK_S * 1000;
				continue;
			}

			next_check = now_ms() + CHECK_S * 1000;
			if (r < 0) {
				if (esp_mode) {
					esp_lost();
				} else if (++misses >= 3) {
					link_ok = 0;
					mode = MODE_UNKNOWN;
					clear_values();
					set_message("boiler module stopped answering");
				}
			} else {
				misses = 0;
				if (mode == MODE_OT && r) {
					if (ot_misses) {
						set_message("OpenTherm device answering again");
						next_idle = 0;	/* the demand again, at once */
					}
					ot_present = 1;
					ot_misses = 0;
				} else if (mode == MODE_OT) {
					/* never back to the on/off contact by itself: a
					   heat pump faults when the line goes quiet */
					ot_misses++;
					if (ot_misses == 1 ||
					    (ot_misses > OT_ERROR_MISSES &&
					     (ot_misses - OT_ERROR_MISSES) % (OT_REARM_S / CHECK_S) == 0)) {
						/* the module's OpenTherm side on again; the
						   demand follows when it has looked */
						transact('K', 0x45, 0, 0, 0, 1, rx);
						next_idle = now_ms() + DETECT_MS;
					}
					if (ot_misses == 1)
						set_message("OpenTherm device not answering, connecting again");
					if (ot_misses == OT_ERROR_MISSES) {
						ot_present = 0;
						clear_values();
						set_message("OpenTherm device not answering; OpenTherm stays on, the Toon keeps trying");
					}
				}
			}
			write_state();
			continue;
		}

		if (now_ms() >= next_idle) {
			send_demand();
			next_idle = now_ms() + KEEPALIVE_S * 1000;
			continue;
		}

		if (mode != MODE_OT)
			continue;

		if (!transact('M', 0, 0, poll_ids[next_id], 0, 0, rx) &&
		    rx[3] == poll_ids[next_id]) {
			/* faster than its round: keep what the boiler last said */
			if (rx[2] != MSG_NO_NEWS) {
				values[next_id].valid = 1;
				values[next_id].type = rx[2];
			}
			values[next_id].val = rx[4] << 8 | rx[5];
		}
		if (++next_id >= N_POLL) {
			next_id = 0;
			remember_hot_water();
			write_state();
		}
	}

	/* leave the boiler without a heat request */
	heating = 0;
	cooling = 0;
	send_demand();
	bus_put("boiler", NULL);
	bus_put(OPENTHERM_BUS, NULL);
	bus_close();
	return 0;
}
