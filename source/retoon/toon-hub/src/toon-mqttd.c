// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-mqttd: the Toon on MQTT, for the Toon's own broker and for Home
 * Assistant.
 *
 * The Toon's own broker (mosquitto on 127.0.0.1, S41toon-broker) gets, always:
 *
 *   retoon/display         {"on":true,"brightness":80}, retained
 *   retoon/status          "online"/"offline" (last will)
 *
 * and takes retoon/display/set: "ON", "OFF" or a brightness 0-100.
 *
 * The other programs publish their own state there (retoon/climate,
 * retoon/boiler, ..., libtoonbus) and take their own commands
 * (retoon/boiler/set, retoon/ui/set, retoon/notify/set). This program keeps
 * the newest of each state in memory for Home Assistant, sends Home
 * Assistant's commands on as such messages, and passes taps on a screen
 * message (retoon/notify/event, from toon-ui) on to Home Assistant.
 *
 * Home Assistant, when a broker is set (mqtt.conf), gets the Toon as a
 * device through MQTT discovery, so nothing has to be configured there:
 *
 *   sensors        room temperature, humidity, light, air-quality index,
 *                  the OpenTherm values of the boiler
 *   binary sensors boiler module link, heat request, flame, boiler fault
 *   climate        the thermostat (off/heat, target temperature)
 *   select         boiler connection: OpenTherm or on/off contact
 *   light          the display backlight
 *   notify         a message on the screen (banner or popup, toon-ui's
 *                  notify.c): plain text, or JSON with title, level,
 *                  buttons, sound... on <base>/notify/set
 *   event          a tap on such a message: event_type "button" (with the
 *                  button's label) or "dismissed", with its tag and title
 *
 * Topics are <base>/<key> with base "toon/<hostname without toon->", so
 * "toon/1a2b3c" for a Toon with its standard name and "toon/kitchen" for
 * one called "toon-kitchen"; commands arrive on <base>/<key>/set, and for a
 * renamed Toon also on toon/<id>/<key>/set as before 1.26. <base>/status
 * is "online"/"offline" (last will). Home Assistant's ids (node) stay those
 * of the Toon's own id, so renaming it changes no entity.
 *
 * Broker settings come from a file that is not part of the OS image:
 *
 *   host=192.168.1.10
 *   port=1883                       (optional)
 *   username=...                    (optional)
 *   password=...                    (optional)
 *   discovery_prefix=homeassistant  (optional)
 *
 * Both connections have their own thread (libmosquitto's loop); the main
 * thread waits for changed states and sends.
 *
 *   toon-mqttd [-c config] [-v]
 */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <mosquitto.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <toonbus.h>

#define CONFIG_FILE	"/mnt/data/toon-os/config/mqtt.conf"
/* the other programs' states on the Toon's own broker (retoon/<name>),
   read here as if they were "key=value" files: see snap_open() */
#define STATE		"retoon:"
#define BOILER_FILE	STATE "boiler"
#define SENSOR_FILE	STATE "sensor-choice"	/* options=a|b|c, use=b (toon-ui) */
#define UI_FILE		STATE "ui"		/* pages=a|b|c, view=1 (toon-ui) */
#define METER_FILE	STATE "meter"		/* meter adapters, from toon-zwaved */
#define CLIMATE_FILE	STATE "climate"
#define VOC_FILE	STATE "voc"
#define LANG_FILE	"/mnt/data/toon-os/config/language"
#define LOCAL_HOST	"127.0.0.1"	/* the Toon's own broker */
#define LOCAL_PORT	1883
#define LOCAL_BASE	"retoon"

#define PUBLISH_S	5	/* the values without a file (backlight) are read this often */
#define REFRESH_S	300	/* everything is sent again this often */
#define RETRY_S		15
#define NONE		"None"	/* Home Assistant shows this as "unknown" */

enum { C_SENSOR, C_BINARY, C_OTHER };

/*
 * One published value. "extra" is the entity-specific part of the discovery
 * message; state and availability topics, ids and the device are added to it.
 */
struct entity {
	const char *key, *component, *name, *extra;
	char last[96];
};

#define DIAG	"\"entity_category\":\"diagnostic\","
#define TEMP	"\"device_class\":\"temperature\",\"unit_of_measurement\":\"\302\260C\"," \
		"\"state_class\":\"measurement\""
#define ONOFF	"\"payload_on\":\"1\",\"payload_off\":\"0\""

static struct entity entities[] = {
	{ "temperature", "sensor", "Temperature", TEMP },	/* the thermostat's */
	{ "internal_temperature", "sensor", "Internal temperature", TEMP },
	{ "outside_temperature", "sensor", "Outside temperature", TEMP },
	{ "humidity", "sensor", "Humidity",
	  "\"device_class\":\"humidity\",\"unit_of_measurement\":\"%\","
	  "\"state_class\":\"measurement\"" },
	{ "illuminance", "sensor", "Light",
	  "\"device_class\":\"illuminance\",\"unit_of_measurement\":\"lx\","
	  "\"state_class\":\"measurement\"" },
	{ "voc_index", "sensor", "VOC below clean air",	/* % lower gas-sensor resistance */
	  "\"icon\":\"mdi:air-filter\",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\"" },
	{ "air_quality", "sensor", "Air quality",
	  "\"icon\":\"mdi:air-filter\",\"json_attributes_topic\":\"%s/air_quality/attributes\"" },

	{ "boiler_water", "sensor", "Flow temperature", TEMP },
	{ "return_water", "sensor", "Return water", TEMP },
	{ "flow_setpoint", "sensor", "Flow setpoint", TEMP },
	{ "hot_water_setpoint", "sensor", "Hot water setpoint", TEMP },
	{ "max_heating_setpoint", "sensor", "Maximum heating setpoint", DIAG TEMP },
	{ "modulation", "sensor", "Modulation",
	  "\"icon\":\"mdi:fire\",\"unit_of_measurement\":\"%\","
	  "\"state_class\":\"measurement\"" },
	{ "water_pressure", "sensor", "Water pressure",
	  "\"device_class\":\"pressure\",\"unit_of_measurement\":\"bar\","
	  "\"state_class\":\"measurement\"" },
	{ "fault_code", "sensor", "Fault code", DIAG "\"icon\":\"mdi:alert-circle\"" },
	{ "boiler_message", "sensor", "Connection status",
	  DIAG "\"icon\":\"mdi:message-text\"" },

	/* the link to the heating installation: the boiler module on its cable
	   or an ESP over WiFi (connectivity: "Installation: Connected") */
	{ "boiler_module", "binary_sensor", "Installation",
	  DIAG "\"device_class\":\"connectivity\"," ONOFF },
	/* the boiler or heat pump answers OpenTherm (unknown on the contact) */
	{ "opentherm", "binary_sensor", "OpenTherm",
	  DIAG "\"device_class\":\"connectivity\"," ONOFF },
	{ "heat_request", "binary_sensor", "Heat request",
	  "\"device_class\":\"heat\"," ONOFF },
	{ "cool_request", "binary_sensor", "Cool request",
	  "\"device_class\":\"cold\"," ONOFF },
	{ "flame", "binary_sensor", "Flame", "\"icon\":\"mdi:fire\"," ONOFF },
	{ "heating_active", "binary_sensor", "Heating active", "\"icon\":\"mdi:radiator\"," ONOFF },
	{ "hot_water_active", "binary_sensor", "Hot water active", "\"icon\":\"mdi:water-boiler\"," ONOFF },
	{ "cooling_active", "binary_sensor", "Cooling active", "\"icon\":\"mdi:snowflake\"," ONOFF },
	{ "boiler_fault", "binary_sensor", "Installation fault",
	  "\"device_class\":\"problem\"," ONOFF },

	{ "boiler_connection", "select", "Connection",
	  "\"icon\":\"mdi:transit-connection-variant\","
	  "\"options\":[\"OpenTherm\",\"On/off\"],"
	  "\"command_topic\":\"%s/boiler_connection/set\"" },
	/* the maximum boiler water temperature in force (OpenTherm); setting it
	   makes it a fixed one, as on the Boiler page (a heating curve from
	   Home Assistant) */
	{ "max_flow", "number", "Maximum flow temperature",
	  "\"icon\":\"mdi:thermometer-chevron-up\",\"device_class\":\"temperature\","
	  "\"unit_of_measurement\":\"\302\260C\",\"min\":20,\"max\":80,\"step\":1,"
	  "\"mode\":\"box\",\"command_topic\":\"%s/max_flow/set\"" },
	/* the hot water temperature the Toon asks for (OpenTherm, a boiler
	   with hot water); setting it makes it a fixed one, without the hot
	   water week program (by the outside temperature, for example) */
	{ "hot_water", "number", "Hot water temperature",
	  "\"icon\":\"mdi:water-boiler\",\"device_class\":\"temperature\","
	  "\"unit_of_measurement\":\"\302\260C\",\"min\":30,\"max\":80,\"step\":1,"
	  "\"mode\":\"box\",\"command_topic\":\"%s/hot_water/set\"" },
	/* keep the tap water warm (OpenTherm, a combi boiler): off clears the
	   hot water switch in the master status */
	{ "hot_water_keep_warm", "switch", "Keep hot water warm",
	  "\"icon\":\"mdi:water-thermometer\"," ONOFF ",\"command_topic\":\"%s/hot_water_keep_warm/set\"" },
	{ "preset_lock", "switch", "Thermostat lock",
	  "\"icon\":\"mdi:lock\"," ONOFF ",\"command_topic\":\"%s/preset_lock/set\"" },
	/* its options come from toon-ui (SENSOR_FILE): sensor_discovery() */
	{ "thermostat_sensor", "select", "Thermostat sensor", NULL },
	/* the page on the screen; its options come from toon-ui: page_discovery() */
	{ "page", "select", "Page", NULL },

	/* the extra topics of these are filled in by publish_discovery() */
	{ "thermostat/mode", "climate", "Thermostat", NULL },
	{ "display", "light", "Display", NULL },
	{ "notify", "notify", "Notification", NULL },
	{ "notify/action", "event", "Notification button", NULL },

	/* published only, part of the two entities above */
	{ "thermostat/target", NULL },
	{ "thermostat/action", NULL },
	{ "thermostat/preset", NULL },
	{ "thermostat/current", NULL },
	{ "display/brightness", NULL },
};

#define N_ENTITIES	(sizeof(entities) / sizeof(entities[0]))

/* The Toon 1 has only a temperature sensor: no humidity, light or air quality. */
static int toon1;

static int absent(const char *key)
{
	return toon1 && (!strcmp(key, "humidity") || !strcmp(key, "illuminance") ||
			 !strcmp(key, "voc_index") || !strcmp(key, "air_quality"));
}

static int is_toon1(void)
{
	char model[64] = "";
	FILE *f = fopen("/proc/device-tree/model", "r");

	if (f) {
		if (!fgets(model, sizeof(model), f))
			model[0] = 0;
		fclose(f);
	}
	return strstr(model, "Toon 1") != NULL;
}

/* one broker connection, run by its own thread */
struct link {
	struct mosquitto *m;
	char host[128], user[128], pass[128];
	int port, retry_s;
	volatile int connected, republish, quit;
	pthread_t thread;
};
static struct link ext;		/* Home Assistant's broker (mqtt.conf) */
static struct link loc;		/* the Toon's own broker */

static char base[64], idbase[64], node[64], devname[64], prefix[64] = "homeassistant";
static int verbose;
static volatile sig_atomic_t stop;
static pthread_mutex_t cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static int wake_pipe[2] = { -1, -1 };	/* a command was passed on: look again soon */
/* the kept states came in after the start: until then the states would read
   as missing, and Home Assistant would get "None" for a moment */
#define READY_TOPIC	LOCAL_BASE "/_mqttd/ready"
#define READY_MAX_S	5
static volatile int loc_ready;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---- the states --------------------------------------------------------- */

/*
 * The newest retoon/<name> of each program, kept by the local connection's
 * thread (on_message). Read as "key=value" text, so the code below reads a
 * state as it read the files those programs used to write.
 */
/* only these: anything else on retoon/+ (an app's, a device's) is not ours */
static struct state {
	const char *name;
	char *json;
} states[] = {
	{ "boiler" }, { "climate" }, { "voc" }, { "opentherm" }, { "meter" }, { "sensor-choice" },
	{ "ui" },
};
#define N_STATES	(int)(sizeof(states) / sizeof(states[0]))
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

static void state_keep(const char *name, const char *payload, int len)
{
	int i;

	pthread_mutex_lock(&state_lock);
	for (i = 0; i < N_STATES; i++)
		if (!strcmp(states[i].name, name)) {
			free(states[i].json);
			states[i].json = len ? strndup(payload, len) : NULL;
		}
	pthread_mutex_unlock(&state_lock);
}

/* a state as key=value text, or a file's text; NULL when there is none */
static char *read_text(const char *path)
{
	char *text = NULL;

	if (!strncmp(path, STATE, strlen(STATE))) {
		const char *name = path + strlen(STATE);
		int i;

		pthread_mutex_lock(&state_lock);
		for (i = 0; i < N_STATES; i++)
			if (!strcmp(states[i].name, name) && states[i].json) {
				size_t len = strlen(states[i].json) + 64;

				text = malloc(len);
				if (text)
					bus_json_kv(states[i].json, text, len);
			}
		pthread_mutex_unlock(&state_lock);
	} else {
		FILE *f = fopen(path, "r");
		char buf[4096];
		size_t n;

		if (f) {
			n = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
			text = malloc(n + 1);
			if (text) {
				memcpy(text, buf, n);
				text[n] = 0;
			}
		}
	}
	return text;
}

/* text to read like a file; the copy goes with the fclose */
static FILE *text_file(const char *text)
{
	FILE *f = fmemopen(NULL, strlen(text) + 1, "w+");

	if (f) {
		fputs(text, f);
		rewind(f);
	}
	return f;
}

/*
 * While one round of values is made, each state is read once and the values
 * come from that copy: some thirty values out of the same few states was the
 * larger part of this program's work on a Toon 1.
 */
static struct snap {
	const char *path;
	char *text;		/* NULL: no such state */
} snaps[12];
static int snapping, nsnaps;

static void snap_begin(void)
{
	snapping = 1;
}

static void snap_end(void)
{
	for (int i = 0; i < nsnaps; i++)
		free(snaps[i].text);
	nsnaps = 0;
	snapping = 0;
}

static FILE *snap_open(const char *path)
{
	struct snap *sn = NULL;
	FILE *f;
	char *text;
	int i;

	if (snapping) {
		for (i = 0; i < nsnaps; i++)
			if (!strcmp(snaps[i].path, path))
				sn = &snaps[i];
		if (!sn && nsnaps < (int)(sizeof(snaps) / sizeof(snaps[0]))) {
			sn = &snaps[nsnaps++];
			sn->path = path;
			sn->text = read_text(path);
		}
		if (sn)
			return sn->text ? text_file(sn->text) : NULL;
	}
	text = read_text(path);
	if (!text)
		return NULL;
	f = text_file(text);
	free(text);
	return f;
}

/* Value of "key=" in a state or a key=value file; returns 0 if found. */
static int file_value(const char *path, const char *key, char *out, size_t len)
{
	char line[1100];	/* the sensor options and pages are one long line */
	size_t klen = strlen(key);
	FILE *f = snap_open(path);
	int found = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			line[strcspn(line, "\n")] = 0;
			snprintf(out, len, "%s", line + klen + 1);
			found = 0;
		}
	fclose(f);
	return found;
}

/* An OpenTherm data id from toon-boilerd: 0 if the boiler has a value. */
static int ot_raw(int id, int *val)
{
	char key[16], buf[32];
	unsigned int type, v;

	snprintf(key, sizeof(key), "id%d", id);
	if (file_value(BOILER_FILE, key, buf, sizeof(buf)) ||
	    sscanf(buf, "%u %x", &type, &v) != 2 || type == 6 || type == 7)
		return -1;
	*val = v;
	return 0;
}

static void ot_f88(int id, const char *fmt, char *out, size_t len)
{
	int v;

	if (ot_raw(id, &v))
		snprintf(out, len, NONE);
	else
		snprintf(out, len, fmt, (int16_t)v / 256.0);
}

static char bl_dir[300];
static long bl_max = 100;
static int bl_last = 80;	/* brightness to return to when switched on */

static void find_backlight(void)
{
	struct dirent *de;
	DIR *d = opendir("/sys/class/backlight");
	char path[400], buf[32];
	FILE *f;

	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(bl_dir, sizeof(bl_dir), "/sys/class/backlight/%.200s", de->d_name);
		snprintf(path, sizeof(path), "%s/max_brightness", bl_dir);
		f = fopen(path, "r");
		if (f) {
			if (fgets(buf, sizeof(buf), f) && atol(buf) > 0)
				bl_max = atol(buf);
			fclose(f);
		}
		break;
	}
	closedir(d);
}

static int get_brightness(void)
{
	char path[400], buf[32];
	FILE *f;
	int v = -1;

	if (!bl_dir[0])
		return -1;
	snprintf(path, sizeof(path), "%s/brightness", bl_dir);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fgets(buf, sizeof(buf), f))
		v = atol(buf) * 100 / bl_max;
	fclose(f);
	return v;
}

static void set_brightness(int percent)
{
	char path[400];
	FILE *f;

	if (!bl_dir[0])
		return;
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	if (percent)
		bl_last = percent;
	snprintf(path, sizeof(path), "%s/brightness", bl_dir);
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%ld\n", percent * bl_max / 100);
	fclose(f);
}

/* Current value of an entity as the text that goes on its topic. */
static void entity_value(const char *key, char *out, size_t len)
{
	char buf[96];
	int v, ot;

	snprintf(out, len, NONE);
	ot = !file_value(BOILER_FILE, "mode", buf, sizeof(buf)) && !strcmp(buf, "opentherm");

	if (!strcmp(key, "temperature") || !strcmp(key, "thermostat/current")) {
		/* what the thermostat works with: the chosen sensors' average,
		   or the Toon's own sensor */
		if (!file_value(BOILER_FILE, "room", buf, sizeof(buf)) ||
		    !file_value(CLIMATE_FILE, "temperature", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "outside_temperature")) {
		/* what the thermostat works with: OpenTherm's, or the sensor chosen */
		if (!file_value(BOILER_FILE, "outside", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "internal_temperature")) {
		if (!file_value(CLIMATE_FILE, "temperature", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "humidity") || !strcmp(key, "illuminance")) {
		if (!file_value(CLIMATE_FILE, key, buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "air_quality")) {
		/* the word toon-m4d learns from this Toon's history, in the
		   language of the Toon (Dutch unless config/language says en) */
		static const char *const w[][3] = { { "good", "Good", "Goed" }, { "fair", "Fair", "Redelijk" },
			{ "moderate", "Moderate", "Matig" }, { "poor", "Poor", "Slecht" } };
		char lang[8] = "";

		file_value(LANG_FILE, "lang", lang, sizeof(lang));
		/* the first minutes after a start: the word from before it, so a
		   restart leaves no gap in Home Assistant's history */
		if (!file_value(VOC_FILE, "state", buf, sizeof(buf)) && !strcmp(buf, "warmup") &&
		    file_value(VOC_FILE, "quality_last", buf, sizeof(buf)))
			snprintf(out, len, "%s", strcmp(lang, "en") ? "Aan het meten…" : "Measuring…");
		else if (!file_value(VOC_FILE, "quality", buf, sizeof(buf)) ||
			 !file_value(VOC_FILE, "quality_last", buf, sizeof(buf)))
			for (int i = 0; i < 4; i++)
				if (!strcmp(buf, w[i][0]))
					snprintf(out, len, "%s", w[i][strcmp(lang, "en") ? 2 : 1]);
	} else if (!strcmp(key, "voc_index")) {
		/* no number while the sensor is still heating up */
		if (!file_value(VOC_FILE, "state", buf, sizeof(buf)) &&
		    strcmp(buf, "warmup") &&
		    !file_value(VOC_FILE, "index", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "boiler_water")) {
		ot_f88(25, "%.1f", out, len);
	} else if (!strcmp(key, "return_water")) {
		ot_f88(28, "%.1f", out, len);
	} else if (!strcmp(key, "flow_setpoint")) {
		ot_f88(1, "%.1f", out, len);
	} else if (!strcmp(key, "hot_water_setpoint")) {
		ot_f88(56, "%.0f", out, len);
	} else if (!strcmp(key, "max_heating_setpoint")) {
		ot_f88(57, "%.0f", out, len);
	} else if (!strcmp(key, "max_flow")) {
		/* the user's maximum now, else the boiler's own */
		if (!file_value(BOILER_FILE, "max_flow_now", buf, sizeof(buf)))
			snprintf(out, len, "%.0f", atof(buf));
		else
			ot_f88(57, "%.0f", out, len);
	} else if (!strcmp(key, "hot_water")) {
		/* what the Toon asks for, else what the boiler has */
		if (!file_value(BOILER_FILE, "hw_wanted", buf, sizeof(buf)))
			snprintf(out, len, "%.0f", atof(buf));
		else
			ot_f88(56, "%.0f", out, len);
	} else if (!strcmp(key, "modulation")) {
		ot_f88(17, "%.0f", out, len);
	} else if (!strcmp(key, "water_pressure")) {
		ot_f88(18, "%.1f", out, len);
	} else if (!strcmp(key, "fault_code")) {
		/* only while the boiler says fault (id 0 bit 0): many boilers
		   send 255 when there is none */
		if (!ot_raw(0, &v) && !(v & 0x01))
			snprintf(out, len, "none");
		else if (!ot_raw(5, &v))
			snprintf(out, len, "%d", v & 0xff);
	} else if (!strcmp(key, "boiler_message")) {
		if (!file_value(BOILER_FILE, "message", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "boiler_module")) {
		snprintf(out, len, "%d",
			 !file_value(BOILER_FILE, "link", buf, sizeof(buf)) && !strcmp(buf, "ok"));
	} else if (!strcmp(key, "opentherm")) {
		if (ot && !file_value(BOILER_FILE, "ot", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf[0] == '1' ? "1" : "0");
	} else if (!strcmp(key, "heat_request")) {
		if (!file_value(BOILER_FILE, "heat", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "cool_request")) {
		if (!file_value(BOILER_FILE, "cool", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "flame")) {
		/* id 0, low byte: the boiler's status flags */
		if (ot && !ot_raw(0, &v))
			snprintf(out, len, "%d", !!(v & 8));
	} else if (!strcmp(key, "heating_active") || !strcmp(key, "hot_water_active") ||
		   !strcmp(key, "cooling_active")) {
		/* the same flags: central heating 2, hot water 4, cooling 16 */
		if (ot && !ot_raw(0, &v))
			snprintf(out, len, "%d", !!(v & (key[0] == 'h' && key[1] == 'e' ? 2 :
							  key[0] == 'h' ? 4 : 16)));
	} else if (!strcmp(key, "boiler_fault")) {
		if (ot && !ot_raw(0, &v))
			snprintf(out, len, "%d", v & 1);
	} else if (!strcmp(key, "boiler_connection")) {
		if (!file_value(BOILER_FILE, "mode", buf, sizeof(buf))) {
			if (!strcmp(buf, "onoff"))
				snprintf(out, len, "On/off");
			else if (ot || !strcmp(buf, "detecting"))
				snprintf(out, len, "OpenTherm");
		}
	} else if (!strcmp(key, "hot_water_keep_warm")) {
		if (!file_value(BOILER_FILE, "hw_keep_warm", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf[0] == '0' ? "0" : "1");
	} else if (!strcmp(key, "preset_lock")) {
		if (!file_value(BOILER_FILE, "hold", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf[0] == '2' ? "1" : "0");
	} else if (!strcmp(key, "thermostat_sensor")) {
		if (!file_value(SENSOR_FILE, "use", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "page")) {
		char pages[1000], *p, *s;
		int view, i;

		if (!file_value(UI_FILE, "view", buf, sizeof(buf)) &&
		    !file_value(UI_FILE, "pages", pages, sizeof(pages)) && pages[0]) {
			view = atoi(buf);
			for (s = pages, i = 0; s; s = p, i++) {
				p = strchr(s, '|');
				if (p)
					*p++ = 0;
				if (i == view)
					snprintf(out, len, "%s", s);
			}
		}
	} else if (!strcmp(key, "thermostat/mode")) {
		/* Home Assistant's "auto": the device follows its own schedule */
		char prog[8] = "";

		file_value(BOILER_FILE, "program_enabled", prog, sizeof(prog));
		if (!file_value(BOILER_FILE, "hvac", buf, sizeof(buf)))
			snprintf(out, len, "%s", strcmp(buf, "heat") ? buf :
				 prog[0] == '1' ? "auto" : "heat");
	} else if (!strcmp(key, "thermostat/target")) {
		if (!file_value(BOILER_FILE, "target", buf, sizeof(buf)))
			snprintf(out, len, "%s", buf);
	} else if (!strcmp(key, "thermostat/preset")) {
		if (!file_value(BOILER_FILE, "preset", buf, sizeof(buf)))
			snprintf(out, len, "%s", strcmp(buf, "manual") ? buf : "none");
	} else if (!strcmp(key, "thermostat/action")) {
		char heat[8], cool[8] = "";

		file_value(BOILER_FILE, "cool", cool, sizeof(cool));
		if (!file_value(BOILER_FILE, "hvac", buf, sizeof(buf)) &&
		    !file_value(BOILER_FILE, "heat", heat, sizeof(heat)))
			snprintf(out, len, "%s", strcmp(buf, "heat") ? "off" :
				 heat[0] == '1' ? "heating" : cool[0] == '1' ? "cooling" : "idle");
	} else if (!strcmp(key, "display")) {
		v = get_brightness();
		if (v >= 0)
			snprintf(out, len, v ? "ON" : "OFF");
	} else if (!strcmp(key, "display/brightness")) {
		v = get_brightness();
		if (v > 0)
			snprintf(out, len, "%d", v);
		else if (!v)
			snprintf(out, len, "%d", bl_last);
	}
}

/* ---- MQTT --------------------------------------------------------------- */

static void publish(struct link *l, const char *topic, const char *payload, int retain)
{
	if (!l->m || !l->connected)
		return;
	logmsg("> %s %s", topic, payload);
	mosquitto_publish(l->m, NULL, topic, strlen(payload), payload, 0, retain);
}

/*
 * The sensor the thermostat works with, as a select: "Average" (of all the
 * sensors chosen in the web interface), "Toon" (its own) or one of the
 * chosen ones. The options change with the web interface's choice: toon-ui
 * writes them to SENSOR_FILE, and a new list is announced again.
 */
static char sensor_opts[1000];	/* the options announced */

static void sensor_discovery(void)
{
	char topic[200], msg[2200], opts[1000], list[1400], dev[300], *p, *s;
	const char *suffix = strrchr(base, '/') + 1;
	size_t n = 0;

	snprintf(dev, sizeof(dev),
		 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s%s\",\"manufacturer\":\"Quby\",\"model\":\"%s\"}",
		 node, devname[0] ? "" : "Toon ", devname[0] ? devname : suffix, toon1 ? "Toon 1" : "Toon 2");

	snprintf(topic, sizeof(topic), "%s/select/%s/thermostat_sensor/config", prefix, node);
	if (file_value(SENSOR_FILE, "options", opts, sizeof(opts)) || !opts[0]) {
		publish(&ext, topic, "", 1);	/* no sensors chosen: nothing to choose */
		sensor_opts[0] = 0;
		return;
	}
	snprintf(sensor_opts, sizeof(sensor_opts), "%s", opts);
	for (s = opts; s; s = p) {
		p = strchr(s, '|');
		if (p)
			*p++ = 0;
		if (strchr(s, '"') || strchr(s, '\\'))
			continue;
		n += snprintf(list + n, sizeof(list) - n, "%s\"%s\"", n ? "," : "", s);
		if (n >= sizeof(list))
			return;
	}
	snprintf(msg, sizeof(msg),
		 "{\"name\":\"Thermostat sensor\",\"unique_id\":\"%s_thermostat_sensor\","
		 "\"object_id\":\"%s_thermostat_sensor\",\"availability_topic\":\"%s/status\","
		 "\"icon\":\"mdi:thermometer-lines\",\"options\":[%s],"
		 "\"command_topic\":\"%s/thermostat_sensor/set\",\"state_topic\":\"%s/thermostat_sensor\",%s}",
		 node, node, base, list, base, base, dev);
	publish(&ext, topic, msg, 1);
}

/*
 * The page on the screen, as a select: an option per page, named as on the
 * screen (toon-ui's retoon/ui "pages"). Choosing one shows that page, as
 * "view N" on retoon/ui/set. A new list of pages is announced again.
 */
static char page_opts[1000];	/* the pages announced */

static void page_discovery(void)
{
	char topic[200], msg[2400], opts[1000], list[1400], dev[300], *p, *s;
	const char *suffix = strrchr(base, '/') + 1;
	size_t n = 0;

	snprintf(dev, sizeof(dev),
		 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s%s\",\"manufacturer\":\"Quby\",\"model\":\"%s\"}",
		 node, devname[0] ? "" : "Toon ", devname[0] ? devname : suffix, toon1 ? "Toon 1" : "Toon 2");

	snprintf(topic, sizeof(topic), "%s/select/%s/page/config", prefix, node);
	if (file_value(UI_FILE, "pages", opts, sizeof(opts)) || !opts[0]) {
		page_opts[0] = 0;	/* toon-ui not there yet: the next round */
		return;
	}
	snprintf(page_opts, sizeof(page_opts), "%s", opts);
	for (s = opts; s; s = p) {
		p = strchr(s, '|');
		if (p)
			*p++ = 0;
		/* toon-ui leaves out the characters that would break the JSON */
		n += snprintf(list + n, sizeof(list) - n, "%s\"%s\"", n ? "," : "", s);
		if (n >= sizeof(list))
			return;
	}
	snprintf(msg, sizeof(msg),
		 "{\"name\":\"Page\",\"unique_id\":\"%s_page\","
		 "\"object_id\":\"%s_page\",\"availability_topic\":\"%s/status\","
		 "\"icon\":\"mdi:view-dashboard\",\"options\":[%s],"
		 "\"command_topic\":\"%s/page/set\",\"state_topic\":\"%s/page\",%s}",
		 node, node, base, list, base, base, dev);
	publish(&ext, topic, msg, 1);
}

static void publish_discovery(void)
{
	static const char device[] =
		"\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s%s\","
		"\"manufacturer\":\"Quby\",\"model\":\"%s\"}";
	char topic[200], msg[2200], extra[1100], dev[300];
	const char *suffix = strrchr(base, '/') + 1;
	unsigned int i;

	snprintf(dev, sizeof(dev), device, node, devname[0] ? "" : "Toon ",
		 devname[0] ? devname : suffix, toon1 ? "Toon 1" : "Toon 2");
	for (i = 0; i < N_ENTITIES; i++) {
		struct entity *e = &entities[i];
		char object[64], *p;

		if (!e->component)
			continue;
		snprintf(object, sizeof(object), "%s", e->key);
		for (p = object; *p; p++)
			if (*p == '/')
				*p = '_';

		if (!strcmp(e->component, "climate")) {
			snprintf(extra, sizeof(extra),
				 "\"modes\":[\"off\",\"heat\",\"auto\"],"
				 "\"mode_state_topic\":\"%s/thermostat/mode\","
				 "\"mode_command_topic\":\"%s/thermostat/mode/set\","
				 "\"temperature_state_topic\":\"%s/thermostat/target\","
				 "\"temperature_command_topic\":\"%s/thermostat/target/set\","
				 "\"current_temperature_topic\":\"%s/thermostat/current\","
				 "\"action_topic\":\"%s/thermostat/action\","
				 "\"preset_modes\":[\"comfort\",\"home\",\"sleep\",\"away\"],"
				 "\"preset_mode_state_topic\":\"%s/thermostat/preset\","
				 "\"preset_mode_command_topic\":\"%s/thermostat/preset/set\","
				 "\"temperature_unit\":\"C\",\"min_temp\":6,\"max_temp\":30,"
				 "\"temp_step\":0.5",
				 base, base, base, base, base, base, base, base);
		} else if (!strcmp(e->component, "light")) {
			snprintf(extra, sizeof(extra),
				 "\"icon\":\"mdi:tablet\","
				 "\"state_topic\":\"%s/display\","
				 "\"command_topic\":\"%s/display/set\","
				 "\"brightness_state_topic\":\"%s/display/brightness\","
				 "\"brightness_command_topic\":\"%s/display/brightness/set\","
				 "\"brightness_scale\":100,\"on_command_type\":\"brightness\"",
				 base, base, base, base);
		} else if (!strcmp(e->component, "notify")) {
			snprintf(extra, sizeof(extra),
				 "\"icon\":\"mdi:message-alert\","
				 "\"command_topic\":\"%s/notify/set\"", base);
		} else if (!strcmp(e->component, "event")) {
			snprintf(extra, sizeof(extra),
				 "\"icon\":\"mdi:gesture-tap-button\","
				 "\"event_types\":[\"button\",\"dismissed\"],"
				 "\"state_topic\":\"%s/notify/action\"", base);
		} else if (!strcmp(e->key, "thermostat_sensor")) {
			sensor_discovery();
			continue;
		} else if (!strcmp(e->key, "page")) {
			page_discovery();
			continue;
		} else {
			/* the select carries one %s for its command topic */
			if (strstr(e->extra, "%s"))
				snprintf(extra, sizeof(extra), e->extra, base);
			else
				snprintf(extra, sizeof(extra), "%s", e->extra);
			snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
				 ",\"state_topic\":\"%s/%s\"", base, e->key);
		}

		snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", prefix, e->component,
			 node, object);
		if (absent(e->key)) {	/* also takes away an earlier announcement */
			publish(&ext, topic, "", 1);
			continue;
		}
		snprintf(msg, sizeof(msg),
			 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"object_id\":\"%s_%s\","
			 "\"availability_topic\":\"%s/status\",%s,%s}",
			 e->name, node, object, node, object, base, extra, dev);
		publish(&ext, topic, msg, 1);
	}
}

/* trend and learning of the air-quality word, as attributes of its sensor:
   trend worse/better/steady over the last 15 minutes, learning while the
   Toon has less than a day of its own history */
static void air_attributes(int all)
{
	static char last[160];
	char msg[160], topic[160], trend[8] = "0", learning[8] = "0", steps[32] = "", hours[16] = "0";

	if (absent("air_quality") || file_value(VOC_FILE, "quality", msg, sizeof(msg)))
		return;
	file_value(VOC_FILE, "quality_trend", trend, sizeof(trend));
	file_value(VOC_FILE, "quality_learning", learning, sizeof(learning));
	file_value(VOC_FILE, "quality_steps", steps, sizeof(steps));
	file_value(VOC_FILE, "quality_learned_h", hours, sizeof(hours));
	snprintf(msg, sizeof(msg), "{\"trend\":\"%s\",\"learning\":%s,\"steps_percent\":\"%s\",\"learned_hours\":%d}",
		 atoi(trend) > 0 ? "worse" : atoi(trend) < 0 ? "better" : "steady",
		 atoi(learning) ? "true" : "false", steps, atoi(hours));
	if (!all && !strcmp(msg, last))
		return;
	snprintf(last, sizeof(last), "%s", msg);
	snprintf(topic, sizeof(topic), "%s/air_quality/attributes", base);
	publish(&ext, topic, msg, 0);
}

/*
 * The readings of meter adapters (toon-zwaved): one sensor each for what is
 * in retoon/meter, "n<node>_<what>=<value>". Announced when they come,
 * taken away when they go (the adapter was turned off).
 */
#define MAX_METER	64
static struct meter_entity {
	char key[48], last[32];
	int announced;
} meters[MAX_METER];
static int nmeters, meter_nodes;

static void meter_name(const char *what, char *out, size_t len)
{
	static const char *const names[][2] = {
		{ "gas", "Gas" }, { "gas_flow", "Gas flow" },
		{ "pulse_import", "Electricity import (meter sensor)" },
		{ "pulse_export", "Electricity export (meter sensor)" },
		{ "import_t1", "Electricity import tariff 1" }, { "import_t2", "Electricity import tariff 2" },
		{ "export_t1", "Electricity export tariff 1" }, { "export_t2", "Electricity export tariff 2" },
		{ "heat", "Heat" }, { "water", "Water" }, { "water_flow", "Water flow" },
	};
	char b[32];
	size_t n;

	snprintf(b, sizeof(b), "%s", what);
	n = strlen(b);
	if (n > 6 && !strcmp(b + n - 6, "_power"))
		b[n - 6] = 0;
	for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(names[i][0], b)) {
			snprintf(out, len, "%s%s", names[i][1], strcmp(b, what) ? " power" : "");
			return;
		}
	snprintf(out, len, "Meter %s", what);
}

static void meter_discovery(struct meter_entity *e, int remove)
{
	char topic[320], msg[1400], name[96], object[64], dev[300], cls[200];
	const char *what = strchr(e->key, '_') + 1;
	size_t n = strlen(what);
	const char *suffix = strrchr(base, '/') + 1;

	snprintf(object, sizeof(object), "meter_%s", e->key);
	snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", prefix, node, object);
	if (remove) {
		publish(&ext, topic, "", 1);
		e->announced = 0;
		return;
	}
	meter_name(what, name, sizeof(name));
	if (meter_nodes > 1)	/* more than one adapter: which one */
		snprintf(name + strlen(name), sizeof(name) - strlen(name), " (node %d)", atoi(e->key + 1));
	if (n > 6 && !strcmp(what + n - 6, "_power"))
		snprintf(cls, sizeof(cls), "\"device_class\":\"power\",\"unit_of_measurement\":\"kW\",\"state_class\":\"measurement\"");
	else if (n > 5 && !strcmp(what + n - 5, "_flow"))
		snprintf(cls, sizeof(cls), "\"device_class\":\"volume_flow_rate\",\"unit_of_measurement\":\"m\302\263/h\",\"state_class\":\"measurement\"");
	else if (!strcmp(what, "gas"))
		snprintf(cls, sizeof(cls), "\"device_class\":\"gas\",\"unit_of_measurement\":\"m\302\263\",\"state_class\":\"total_increasing\"");
	else if (!strcmp(what, "water"))
		snprintf(cls, sizeof(cls), "\"device_class\":\"water\",\"unit_of_measurement\":\"m\302\263\",\"state_class\":\"total_increasing\"");
	else if (!strncmp(what, "heat", 4) || strstr(what, "_s"))
		snprintf(cls, sizeof(cls), "\"icon\":\"mdi:meter-electric\",\"state_class\":\"measurement\"");
	else
		snprintf(cls, sizeof(cls), "\"device_class\":\"energy\",\"unit_of_measurement\":\"kWh\",\"state_class\":\"total_increasing\"");
	snprintf(dev, sizeof(dev),
		 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s%s\",\"manufacturer\":\"Quby\",\"model\":\"%s\"}",
		 node, devname[0] ? "" : "Toon ", devname[0] ? devname : suffix, toon1 ? "Toon 1" : "Toon 2");
	snprintf(msg, sizeof(msg),
		 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"object_id\":\"%s_%s\","
		 "\"availability_topic\":\"%s/status\",%s,\"state_topic\":\"%s/meter/%s\",%s}",
		 name, node, object, node, object, base, cls, base, e->key, dev);
	publish(&ext, topic, msg, 1);
	e->announced = 1;
}

/*
 * The boiler's other OpenTherm values, from toon-boilerd: retoon/opentherm,
 * "<key>=<value>|<unit>|<device class>|<state class>|<name>". Announced when
 * they first come; one the boiler stops answering stays, as unknown (a
 * boiler often has a value invalid for a while).
 */
#define OPENTHERM_FILE	STATE "opentherm"
#define MAX_OT		48	/* toon-boilerd sends 31 at most now */
static struct ot_entity {
	char key[40], last[24], unit[8], dclass[24], sclass[24], name[64];
	int announced;
} ots[MAX_OT];
static int nots;

static void ot_discovery(struct ot_entity *e)
{
	char topic[320], msg[1400], object[64], dev[300], cls[200] = "";
	const char *suffix = strrchr(base, '/') + 1;
	size_t n = 0;

	snprintf(object, sizeof(object), "%s", e->key);
	snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", prefix, node, object);
	if (e->dclass[0])
		n += snprintf(cls + n, sizeof(cls) - n, "\"device_class\":\"%s\",", e->dclass);
	if (e->unit[0])
		n += snprintf(cls + n, sizeof(cls) - n, "\"unit_of_measurement\":\"%s\",", e->unit);
	if (e->sclass[0])
		n += snprintf(cls + n, sizeof(cls) - n, "\"state_class\":\"%s\",", e->sclass);
	else if (!e->dclass[0])	/* version, product, diagnostic code */
		n += snprintf(cls + n, sizeof(cls) - n, DIAG);
	snprintf(dev, sizeof(dev),
		 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s%s\",\"manufacturer\":\"Quby\",\"model\":\"%s\"}",
		 node, devname[0] ? "" : "Toon ", devname[0] ? devname : suffix, toon1 ? "Toon 1" : "Toon 2");
	snprintf(msg, sizeof(msg),
		 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"object_id\":\"%s_%s\","
		 "\"availability_topic\":\"%s/status\",%s\"state_topic\":\"%s/opentherm/%s\",%s}",
		 e->name, node, object, node, object, base, cls, base, e->key, dev);
	publish(&ext, topic, msg, 1);
	e->announced = 1;
}

static void ot_publish(int all)
{
	char line[192], topic[160], seen[MAX_OT] = { 0 };
	FILE *f = snap_open(OPENTHERM_FILE);

	while (f && fgets(line, sizeof(line), f)) {
		char *val = strchr(line, '='), *p[4];
		int i, k;

		if (!val)
			continue;
		*val++ = 0;
		val[strcspn(val, "\n")] = 0;
		for (k = 0, p[0] = val; k < 4 && (p[k] = strchr(k ? p[k - 1] : val, '|')); k++)
			*p[k]++ = 0;
		if (k < 4)
			continue;
		for (i = 0; i < nots && strcmp(ots[i].key, line); i++)
			;
		if (i == nots) {
			if (nots == MAX_OT)
				continue;
			memset(&ots[nots], 0, sizeof(ots[0]));
			snprintf(ots[nots].key, sizeof(ots[0].key), "%.39s", line);
			snprintf(ots[nots].unit, sizeof(ots[0].unit), "%.7s", p[0]);
			snprintf(ots[nots].dclass, sizeof(ots[0].dclass), "%.23s", p[1]);
			snprintf(ots[nots].sclass, sizeof(ots[0].sclass), "%.23s", p[2]);
			snprintf(ots[nots].name, sizeof(ots[0].name), "%.63s", p[3]);
			nots++;
		}
		seen[i] = 1;
		if (!ots[i].announced || all)
			ot_discovery(&ots[i]);
		if (!all && !strcmp(ots[i].last, val))
			continue;
		snprintf(ots[i].last, sizeof(ots[i].last), "%.23s", val);
		snprintf(topic, sizeof(topic), "%.63s/opentherm/%.39s", base, ots[i].key);
		publish(&ext, topic, val, 0);
	}
	if (f)
		fclose(f);
	for (int i = 0; i < nots; i++)
		if (!seen[i] && (all || strcmp(ots[i].last, NONE))) {
			snprintf(ots[i].last, sizeof(ots[i].last), NONE);
			snprintf(topic, sizeof(topic), "%.63s/opentherm/%.39s", base, ots[i].key);
			publish(&ext, topic, NONE, 0);
		}
}

static void meter_publish(int all)
{
	char line[96], topic[160], seen[MAX_METER] = { 0 };
	int nodes[8], nn = 0;
	FILE *f = snap_open(METER_FILE);

	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *nl;
		int i, node_id;

		if (line[0] != 'n' || !eq || !strchr(line, '_'))
			continue;
		*eq = 0;
		if ((nl = strchr(eq + 1, '\n')))
			*nl = 0;
		node_id = atoi(line + 1);
		for (i = 0; i < nn && nodes[i] != node_id; i++)
			;
		if (i == nn && nn < 8)
			nodes[nn++] = node_id;
		for (i = 0; i < nmeters && strcmp(meters[i].key, line); i++)
			;
		if (i == nmeters) {
			if (nmeters == MAX_METER)
				continue;
			snprintf(meters[nmeters].key, sizeof(meters[0].key), "%.47s", line);
			meters[nmeters].last[0] = 0;
			meters[nmeters++].announced = 0;
		}
		seen[i] = 1;
		if (!all && meters[i].announced && !strcmp(meters[i].last, eq + 1))
			continue;
		snprintf(meters[i].last, sizeof(meters[i].last), "%.31s", eq + 1);
		snprintf(topic, sizeof(topic), "%s/meter/%.47s", base, line);
		publish(&ext, topic, eq + 1, 0);
	}
	if (f)
		fclose(f);
	/* names say which adapter once there is more than one: announce again */
	if (nn != meter_nodes) {
		meter_nodes = nn;
		for (int i = 0; i < nmeters; i++)
			meters[i].announced = 0;
	}
	for (int i = 0; i < nmeters; ) {
		if (!seen[i]) {
			meter_discovery(&meters[i], 1);
			meters[i] = meters[--nmeters];
			seen[i] = seen[nmeters];
			continue;
		}
		if (!meters[i].announced || all)
			meter_discovery(&meters[i], 0);
		i++;
	}
}

static void publish_states(int all)
{
	char topic[160], value[96];
	unsigned int i;

	snap_begin();
	for (i = 0; i < N_ENTITIES; i++) {
		struct entity *e = &entities[i];

		/* no state of their own: commands in, events out */
		if (absent(e->key) || (e->component && (!strcmp(e->component, "notify") ||
							!strcmp(e->component, "event"))))
			continue;
		entity_value(e->key, value, sizeof(value));
		if (!all && !strcmp(value, e->last))
			continue;
		snprintf(e->last, sizeof(e->last), "%s", value);
		snprintf(topic, sizeof(topic), "%s/%s", base, e->key);
		publish(&ext, topic, value, 0);
	}
	air_attributes(all);
	meter_publish(all);
	ot_publish(all);
	{	/* other sensors chosen in the web interface: other options */
		char opts[1000] = "";

		file_value(SENSOR_FILE, "options", opts, sizeof(opts));
		if (strcmp(opts, sensor_opts))
			sensor_discovery();
		opts[0] = 0;
		file_value(UI_FILE, "pages", opts, sizeof(opts));
		if (strcmp(opts, page_opts))
			page_discovery();
	}
	snap_end();
}

/* who the next boiler command comes from, for the thermostat's change log:
   Home Assistant, or "app" for the Toon's own bus (apps) */
static const char *const boiler_src = "ha";

static void boiler_command(const char *fmt, ...)
{
	char line[96];
	va_list ap;
	int n;

	n = snprintf(line, sizeof(line), "@%s ", boiler_src);
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	publish(&loc, LOCAL_BASE "/boiler/set", line, 0);
}

/* A message for the screen: on to toon-ui (retoon/notify/set) as it is,
   plain text or JSON */
static void notify(const char *p, int len)
{
	char *msg = strndup(p, len);

	if (!msg)
		return;
	publish(&loc, LOCAL_BASE "/notify/set", msg, 0);
	free(msg);
}

/* ---- the Toon's own broker ------------------------------------------------ */

static void display_send(int force)
{
	static char last[64];
	char msg[64];
	int v = get_brightness();

	if (v < 0)
		return;
	snprintf(msg, sizeof(msg), "{\"on\":%s,\"brightness\":%d}", v ? "true" : "false",
		 v ? v : bl_last);
	if (!force && !strcmp(msg, last))
		return;
	snprintf(last, sizeof(last), "%s", msg);
	publish(&loc, LOCAL_BASE "/display", msg, 1);
}

/* ---- commands ------------------------------------------------------------- */

/* "ON", "OFF" or a brightness */
static void display_command(const char *val)
{
	if (!strcmp(val, "OFF"))
		set_brightness(0);
	else if (!strcmp(val, "ON")) {
		if (get_brightness() <= 0)
			set_brightness(bl_last);
	} else if (isdigit((unsigned char)val[0]))
		set_brightness(atoi(val));
}

static void local_command(const char *key, const char *val)
{
	if (!strcmp(key, "display/set"))
		display_command(val);
}

static void ha_command(const char *key, const struct mosquitto_message *msg, const char *val)
{
	if (!strcmp(key, "notify/set")) {
		notify(msg->payload, msg->payloadlen);
	} else if (!strcmp(key, "thermostat/mode/set")) {
		/* auto follows the week program, heat holds the temperature (the
		   week program's only switch in Home Assistant) */
		if (!strcmp(val, "off")) {
			boiler_command("hvac off\n");
		} else if (!strcmp(val, "heat") || !strcmp(val, "auto")) {
			boiler_command("hvac heat\n");
			boiler_command("program %s\n", val[0] == 'a' ? "on" : "off");
		}
	} else if (!strcmp(key, "thermostat/target/set")) {
		boiler_command("target %.1f\n", atof(val));
	} else if (!strcmp(key, "thermostat/preset/set")) {
		if (strcmp(val, "none"))
			boiler_command("preset %s\n", val);
	} else if (!strcmp(key, "max_flow/set")) {
		if (isdigit((unsigned char)val[0]))
			boiler_command("max_flow %.0f\n", atof(val));
	} else if (!strcmp(key, "hot_water/set")) {
		if (isdigit((unsigned char)val[0]))
			boiler_command("hot_water %.0f\n", atof(val));
	} else if (!strcmp(key, "hot_water_keep_warm/set")) {
		if (!strcmp(val, "1") || !strcmp(val, "0"))
			boiler_command("hot_water_keep_warm %s\n", val[0] == '1' ? "on" : "off");
	} else if (!strcmp(key, "preset_lock/set")) {
		boiler_command(!strcmp(val, "1") ? "hold lock\n" : "program resume\n");
	} else if (!strcmp(key, "thermostat_sensor/set")) {
		const char *c;

		for (c = val; *c && !strchr(" \"\\|", *c) && *c > 0x20 && *c < 0x7f; c++)
			;
		if (!*c && *val)
			boiler_command("sensor %s\n", val);
	} else if (!strcmp(key, "page/set")) {
		char pages[1000], *p, *s, line[16];
		int i;

		if (!file_value(UI_FILE, "pages", pages, sizeof(pages)))
			for (s = pages, i = 0; s; s = p, i++) {
				p = strchr(s, '|');
				if (p)
					*p++ = 0;
				if (!strcmp(s, val)) {
					snprintf(line, sizeof(line), "view %d", i);
					publish(&loc, LOCAL_BASE "/ui/set", line, 0);
					break;
				}
			}
	} else if (!strcmp(key, "boiler_connection/set")) {
		if (!strcmp(val, "OpenTherm"))
			boiler_command("mode ot\n");
		else if (!strcmp(val, "On/off"))
			boiler_command("mode onoff\n");
	} else if (!strcmp(key, "display/set")) {
		if (!strcmp(val, "OFF"))
			set_brightness(0);
		else if (get_brightness() <= 0)
			set_brightness(bl_last);
	} else if (!strcmp(key, "display/brightness/set")) {
		set_brightness(atoi(val));
	}
}

/* the states that change what Home Assistant is sent at once */
static int ha_file(const char *name)
{
	return !strcmp(name, "climate") || !strcmp(name, "voc") || !strcmp(name, "boiler") ||
	       !strcmp(name, "ui");
}

/* runs in a connection's thread */
static void on_message(struct mosquitto *m, void *arg, const struct mosquitto_message *msg)
{
	struct link *l = arg;
	const char *b = l == &ext ? base : LOCAL_BASE;
	size_t blen = strlen(b);
	char val[64];

	(void)m;
	/* the broker sends the kept messages of a subscription before anything
	   published after it: our own message back means they are all here */
	if (l == &loc && !strcmp(msg->topic, READY_TOPIC)) {
		loc_ready = 1;
		if (write(wake_pipe[1], "", 1) < 0)
			return;
		return;
	}
	/* the other programs' states, and a tap on a screen message */
	if (l == &loc && !strncmp(msg->topic, LOCAL_BASE "/", sizeof(LOCAL_BASE))) {
		const char *name = msg->topic + sizeof(LOCAL_BASE);

		if (!strchr(name, '/') && name[0] != '_') {
			state_keep(name, msg->payload, msg->payloadlen);
			if (ha_file(name) && write(wake_pipe[1], "", 1) < 0)
				return;
			return;
		}
		if (!strcmp(name, "notify/event") && !msg->retain && msg->payloadlen) {
			char *ev = strndup(msg->payload, msg->payloadlen), topic[160];

			if (ev) {
				snprintf(topic, sizeof(topic), "%s/notify/action", base);
				publish(&ext, topic, ev, 0);
				free(ev);
			}
			return;
		}
	}
	/* a renamed Toon also takes commands on the topics of its id */
	if (l == &ext && idbase[0] && !strncmp(msg->topic, idbase, strlen(idbase)) &&
	    msg->topic[strlen(idbase)] == '/') {
		b = idbase;
		blen = strlen(b);
	}
	/* a kept command would be played back at every connect: never act on one */
	if (msg->retain || !msg->payloadlen || strncmp(msg->topic, b, blen) || msg->topic[blen] != '/')
		return;
	snprintf(val, sizeof(val), "%.*s", msg->payloadlen, (const char *)msg->payload);
	val[strcspn(val, "\r\n")] = 0;
	logmsg("< %s %.60s", msg->topic, val);
	pthread_mutex_lock(&cmd_lock);
	if (l == &ext)
		ha_command(msg->topic + blen + 1, msg, val);
	else
		local_command(msg->topic + blen + 1, val);
	pthread_mutex_unlock(&cmd_lock);
	/* report the result soon, once the daemon has taken it */
	if (write(wake_pipe[1], "", 1) < 0)
		return;
}

static void on_connect(struct mosquitto *m, void *arg, int rc)
{
	struct link *l = arg;
	char topic[160];

	if (rc) {
		fprintf(stderr, "%s refused the connection: %s\n", l->host,
			mosquitto_connack_string(rc));
		return;
	}
	l->connected = 1;
	if (l == &loc) {
		mosquitto_subscribe(m, NULL, LOCAL_BASE "/+", 0);
		mosquitto_subscribe(m, NULL, LOCAL_BASE "/display/set", 0);
		mosquitto_subscribe(m, NULL, LOCAL_BASE "/notify/event", 0);
		mosquitto_subscribe(m, NULL, READY_TOPIC, 0);
		mosquitto_publish(m, NULL, READY_TOPIC, 1, "1", 0, false);
		publish(l, LOCAL_BASE "/status", "online", 1);
	} else {
		/*
		 * Earlier versions asked the broker to keep the last thermostat
		 * command, to get it back after a restart. The thermostat remembers
		 * its own state now, and a command played back would count as a new
		 * choice by hand: take the kept ones away.
		 */
		snprintf(topic, sizeof(topic), "%s/thermostat/mode/set", base);
		mosquitto_publish(m, NULL, topic, 0, NULL, 0, true);
		snprintf(topic, sizeof(topic), "%s/thermostat/target/set", base);
		mosquitto_publish(m, NULL, topic, 0, NULL, 0, true);
		snprintf(topic, sizeof(topic), "%s/+/set", base);
		mosquitto_subscribe(m, NULL, topic, 0);
		snprintf(topic, sizeof(topic), "%s/+/+/set", base);
		mosquitto_subscribe(m, NULL, topic, 0);
		if (idbase[0]) {
			/* the commands of the id's topics; its old kept status goes */
			snprintf(topic, sizeof(topic), "%s/+/set", idbase);
			mosquitto_subscribe(m, NULL, topic, 0);
			snprintf(topic, sizeof(topic), "%s/+/+/set", idbase);
			mosquitto_subscribe(m, NULL, topic, 0);
			snprintf(topic, sizeof(topic), "%s/status", idbase);
			mosquitto_publish(m, NULL, topic, 0, NULL, 0, true);
		}
		publish_discovery();
		snprintf(topic, sizeof(topic), "%s/status", base);
		publish(l, topic, "online", 1);
	}
	l->republish = 1;
	if (write(wake_pipe[1], "", 1) < 0)
		return;
}

static void on_disconnect(struct mosquitto *m, void *arg, int rc)
{
	struct link *l = arg;

	(void)m;
	(void)rc;
	l->connected = 0;
}

/* a connection's thread: connect, run, and try again after a while */
static void *link_run(void *arg)
{
	struct link *l = arg;

	while (!l->quit) {
		if (!mosquitto_connect(l->m, l->host, l->port, 60))
			/* a publish from the main thread or link_stop() wakes it */
			while (!l->quit && !mosquitto_loop(l->m, 5000, 1))
				;
		l->connected = 0;
		for (int i = 0; i < l->retry_s * 10 && !l->quit; i++)
			usleep(100000);
	}
	mosquitto_disconnect(l->m);
	return NULL;
}

static int link_start(struct link *l, const char *id, const char *will)
{
	l->m = mosquitto_new(id, true, l);
	if (!l->m)
		return -1;
	mosquitto_threaded_set(l->m, true);
	if (l->user[0])
		mosquitto_username_pw_set(l->m, l->user, l->pass[0] ? l->pass : NULL);
	mosquitto_will_set(l->m, will, 7, "offline", 0, true);
	mosquitto_connect_callback_set(l->m, on_connect);
	mosquitto_disconnect_callback_set(l->m, on_disconnect);
	mosquitto_message_callback_set(l->m, on_message);
	if (pthread_create(&l->thread, NULL, link_run, l)) {
		mosquitto_destroy(l->m);
		l->m = NULL;
		return -1;
	}
	return 0;
}

static void link_stop(struct link *l, const char *will)
{
	if (!l->m)
		return;
	if (l->connected) {
		publish(l, will, "offline", 1);
		usleep(300000);		/* the thread sends it */
	}
	l->quit = 1;
	mosquitto_disconnect(l->m);	/* ends the thread's loop now */
	pthread_join(l->thread, NULL);
	mosquitto_destroy(l->m);
	l->m = NULL;
}

static int read_config(const char *path)
{
	char buf[128];

	if (file_value(path, "host", ext.host, sizeof(ext.host)) || !ext.host[0])
		return -1;
	ext.port = 1883;
	if (!file_value(path, "port", buf, sizeof(buf)) && atoi(buf) > 0)
		ext.port = atoi(buf);
	file_value(path, "username", ext.user, sizeof(ext.user));
	file_value(path, "password", ext.pass, sizeof(ext.pass));
	file_value(path, "discovery_prefix", prefix, sizeof(prefix));
	return 0;
}

int main(int argc, char **argv)
{
	const char *config = CONFIG_FILE;
	char hostname[64] = "toon", own[64], topic[160], ext_will[160], *p;
	time_t next_refresh = 0, next_publish = 0, next_conf = 0, started = time(NULL);
	int opt, ha_dirty = 1;

	toon1 = is_toon1();
	while ((opt = getopt(argc, argv, "vc:")) != -1) {
		if (opt == 'v')
			verbose = 1;
		else if (opt == 'c')
			config = optarg;
		else
			return 1;
	}

	/*
	 * hostname "toon-1a2b3c" gives node "toon_1a2b3c", base "toon/1a2b3c".
	 * A Toon that was given another name ("toon-kitchen") keeps the node of
	 * its own id (/run/toon/id), so Home Assistant sees the same device and
	 * entities, but its topics follow the name: base "toon/kitchen".
	 */
	gethostname(hostname, sizeof(hostname) - 1);
	snprintf(own, sizeof(own), "%s", hostname);
	if (!file_value("/run/toon/id", "id", topic, sizeof(topic)) && topic[0]) {
		char def[64];

		snprintf(def, sizeof(def), "toon-%.50s", topic);
		if (strcmp(def, hostname)) {
			snprintf(devname, sizeof(devname), "%s", hostname);
			snprintf(idbase, sizeof(idbase), "toon/%.50s", topic);
		}
		snprintf(hostname, sizeof(hostname), "%s", def);
	}
	snprintf(node, sizeof(node), "%s", hostname);
	for (p = node; *p; p++)
		if (*p == '-' || *p == '.')
			*p = '_';
	snprintf(base, sizeof(base), "toon/%.58s", !strncmp(own, "toon-", 5) ? own + 5 : own);
	snprintf(ext_will, sizeof(ext_will), "%s/status", base);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	find_backlight();
	if (pipe(wake_pipe))
		return 1;
	fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);

	mosquitto_lib_init();
	snprintf(loc.host, sizeof(loc.host), LOCAL_HOST);
	loc.port = LOCAL_PORT;
	loc.retry_s = 2;
	if (link_start(&loc, "toon-mqttd", LOCAL_BASE "/status"))
		return 1;

	while (!stop) {
		/* a changed state or a command wakes this loop */
		struct pollfd pfd = { .fd = wake_pipe[0], .events = POLLIN };
		time_t now;

		if (poll(&pfd, 1, 1000) < 0 && !stop)
			usleep(100000);
		if (pfd.revents) {
			char buf[64];

			while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
				;
			ha_dirty = 1;
			next_publish = 0;
		}
		now = time(NULL);

		/* the Toon's own broker */
		if (loc.republish) {
			loc.republish = 0;
			display_send(1);
		}

		/* Home Assistant's broker: the file may be put in place while running */
		if (!ext.m && now >= next_conf) {
			next_conf = now + RETRY_S;
			ext.retry_s = RETRY_S;
			if (!read_config(config))
				link_start(&ext, node, ext_will);
		}
		/* a round on a change of a file, and every PUBLISH_S without one
		   (the backlight has no file) */
		if (!loc_ready && now - started < READY_MAX_S)
			continue;
		if (now >= next_publish || ext.republish || ha_dirty) {
			int all = now >= next_refresh || ext.republish;

			display_send(0);
			if (ext.connected) {
				ext.republish = 0;
				publish_states(all);
				if (all)
					next_refresh = now + REFRESH_S;
			}
			ha_dirty = 0;
			next_publish = now + PUBLISH_S;
		}
	}

	link_stop(&ext, ext_will);
	link_stop(&loc, LOCAL_BASE "/status");
	mosquitto_lib_cleanup();
	return 0;
}
