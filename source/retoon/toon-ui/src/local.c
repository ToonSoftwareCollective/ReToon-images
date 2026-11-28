// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: the unit's own measurements and controls as "toon.*" entities, so
 * the same cards show them and they keep working without Home Assistant.
 * Sources, on the Toon's own MQTT broker: retoon/climate (toon-sensord),
 * retoon/voc (toon-vocd), retoon/boiler and retoon/opentherm (toon-boilerd),
 * retoon/meter (meter adapters, read by toon-zwaved); commands go to
 * retoon/boiler/set. For toon-boilerd this keeps retoon/control (the chosen
 * room sensors) and retoon/outside, and retoon/sensor-choice for toon-mqttd.
 */
#include <math.h>
#include <dirent.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui.h"

#define CLIMATE	STATE "climate"
#define VOC	STATE "voc"
#define BOILER	STATE "boiler"
#define BOILER_CMD "retoon/boiler/set"
#define METER	STATE "meter"
#define OPENTHERM STATE "opentherm"
#define THERMOSTAT_CONF CONFIG_DIR "/thermostat.json"

static const struct {
	const char *id, *name, *unit, *dclass, *icon;
} defs[] = {
	{ "toon.temperature", "Room temperature", "°C", "temperature", "mdi:thermometer" },
	{ "toon.internal_temperature", "Internal temperature", "°C", "temperature", "mdi:thermometer" },
	{ "toon.outside_temperature", "Outside temperature", "°C", "temperature", "mdi:sun-thermometer-outline" },
	{ "toon.humidity", "Humidity", "%", "humidity", "mdi:water-percent" },
	{ "toon.illuminance", "Light", "lx", "illuminance", "mdi:brightness-5" },
	{ "toon.air_quality", "Air quality", "", "", "mdi:air-filter" },
	{ "toon.boiler_water", "Flow temperature", "°C", "temperature", "mdi:thermometer-water" },
	{ "toon.return_water", "Return water", "°C", "temperature", "mdi:thermometer-water" },
	{ "toon.flow_setpoint", "Flow setpoint", "°C", "temperature", "mdi:thermometer-chevron-up" },
	{ "toon.hot_water_setpoint", "Hot water setpoint", "°C", "temperature", "mdi:water-thermometer" },
	{ "toon.hot_water", "Hot water", "", "", "mdi:water-boiler" },
	{ "toon.max_heating_setpoint", "Maximum heating setpoint", "°C", "temperature", "mdi:thermometer-high" },
	{ "toon.modulation", "Modulation", "%", "power_factor", "mdi:fire" },
	{ "toon.water_pressure", "Water pressure", "bar", "pressure", "mdi:gauge" },
	{ "toon.fault_code", "Fault code", "", "", "mdi:alert-circle-outline" },
	{ "toon.boiler_message", "Connection status", "", "", "mdi:message-text-outline" },
	{ "toon.flame", "Burner", "", "heat", "mdi:fire" },
	{ "toon.heating_active", "Heating active", "", "running", "mdi:radiator" },
	{ "toon.hot_water_active", "Hot water active", "", "running", "mdi:water-boiler" },
	{ "toon.cooling_active", "Cooling active", "", "running", "mdi:snowflake" },
	{ "toon.heat_request", "Heat request", "", "heat", "mdi:radiator" },
	{ "toon.cool_request", "Cool request", "", "cold", "mdi:snowflake" },
	{ "toon.boiler_fault", "Installation fault", "", "problem", "mdi:alert" },
	{ "toon.thermostat", "Thermostat", "", "", "mdi:thermostat" },
	{ "toon.boiler_connection", "Connection", "", "", "mdi:connection" },
	{ "toon.display", "Display", "", "", "mdi:monitor" },
	{ "toon.wifi", "WiFi signal", "dBm", "signal_strength", "mdi:wifi" },
};

/* The air-quality word that toon-m4d learns from this Toon's own history,
   with what is going on: measuring after a start (with the word from before
   it), a clear trend over the last 15 minutes, or still learning (the first
   day). In the screen's language. */
static const char *air_word(const char *q)
{
	static const char *const key[] = { "good", "fair", "moderate", "poor" };
	static const char *const word[] = { "Good", "Fair", "Moderate", "Poor" };

	for (int i = 0; i < 4; i++)
		if (!strcmp(q, key[i]))
			return tr(word[i]);
	return NULL;
}

static const char *air_text(const char *state, char *out, size_t len)
{
	char q[16] = "", v[8] = "";
	const char *w;

	if (!strcmp(state, "warmup")) {	/* the first 5 minutes after a start */
		if (!file_value(VOC, "quality_last", q, sizeof(q)) && (w = air_word(q)))
			snprintf(out, len, tr("%s · measuring…"), w);
		else
			snprintf(out, len, "%s", tr("Measuring…"));
		return out;
	}
	if (file_value(VOC, "quality", q, sizeof(q)) || !(w = air_word(q)))
		return "unknown";
	if (!file_value(VOC, "quality_trend", v, sizeof(v)) && atoi(v))
		snprintf(out, len, "%s", tr(atoi(v) > 0 ? "Getting worse" : "Getting better"));
	else if (!file_value(VOC, "quality_learning", v, sizeof(v)) && atoi(v))
		snprintf(out, len, tr("%s · learning"), w);
	else
		snprintf(out, len, "%s", w);
	return out;
}

/* The Toon 1 has only a temperature sensor: no humidity, light or air quality. */
static bool toon1;

static bool absent(const char *id)
{
	return toon1 && (!strcmp(id, "toon.humidity") || !strcmp(id, "toon.illuminance") ||
			 !strcmp(id, "toon.air_quality"));
}

static char bl_dir[300];
static long bl_max = 100;
static uint32_t t_next;

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

int backlight_get(void)
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
		v = (atol(buf) * 100 + bl_max / 2) / bl_max;
	fclose(f);
	return v;
}

void backlight_set(int percent)
{
	char path[400];
	FILE *f;

	if (!bl_dir[0])
		find_backlight();	/* rescue mode comes here without local_init() */
	if (!bl_dir[0])
		return;
	percent = LV_CLAMP(0, percent, 100);
	snprintf(path, sizeof(path), "%s/brightness", bl_dir);
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%ld\n", percent * bl_max / 100);
	fclose(f);
}

static void control_poll(void);

static void set_num_attr(struct entity *e, const char *name, double v)
{
	cJSON *old = cJSON_GetObjectItemCaseSensitive(e->attr, name);

	if (cJSON_IsNumber(old) && old->valuedouble == v)
		return;
	cJSON_DeleteItemFromObjectCaseSensitive(e->attr, name);
	cJSON_AddNumberToObject(e->attr, name, v);
	e->dirty = true;
}

static void set_str_attr(struct entity *e, const char *name, const char *v)
{
	cJSON *old = cJSON_GetObjectItemCaseSensitive(e->attr, name);

	if (cJSON_IsString(old) && !strcmp(old->valuestring, v))
		return;
	cJSON_DeleteItemFromObjectCaseSensitive(e->attr, name);
	cJSON_AddStringToObject(e->attr, name, v);
	e->dirty = true;
}

/* An OpenTherm data id from toon-boilerd: 0 if the boiler has a value. */
static int ot_raw(int id, int *val)
{
	char key[16], buf[32];
	unsigned int type, v;

	snprintf(key, sizeof(key), "id%d", id);
	if (file_value(BOILER, key, buf, sizeof(buf)) ||
	    sscanf(buf, "%u %x", &type, &v) != 2 || type == 6 || type == 7)
		return -1;
	*val = v;
	return 0;
}

static void set_ot(const char *id, int ot_id, bool ot, const char *fmt)
{
	char out[32];
	int v;

	if (!ot || ot_raw(ot_id, &v)) {
		ent_set(ent_get(id), "unavailable");
		return;
	}
	snprintf(out, sizeof(out), fmt, (int16_t)v / 256.0);
	ent_set(ent_get(id), out);
}

static void set_file(const char *id, const char *path, const char *key)
{
	char buf[96];

	ent_set(ent_get(id), file_value(path, key, buf, sizeof(buf)) ? "unavailable" : buf);
}

/*
 * The meter adapters' readings: "n<node>_<what>=<value>" in retoon/meter
 * become toon.meter_n<node>_<what>, made when they first come. A reading
 * that is no longer there (the adapter was turned off) is unavailable.
 */
#define MAX_METER	64
static char meter_ids[MAX_METER][48];
static int nmeter;

static void meter_attrs(struct entity *e, const char *what)
{
	static const char *const names[][2] = {
		{ "gas", "Gas" }, { "gas_flow", "Gas flow" },
		{ "pulse_import", "Electricity import (meter sensor)" },
		{ "pulse_export", "Electricity export (meter sensor)" },
		{ "import_t1", "Electricity import tariff 1" }, { "import_t2", "Electricity import tariff 2" },
		{ "export_t1", "Electricity export tariff 1" }, { "export_t2", "Electricity export tariff 2" },
		{ "heat", "Heat" }, { "water", "Water" }, { "water_flow", "Water flow" },
	};
	char base[32], name[96];
	size_t n;
	const char *unit = "kWh", *dclass = "energy", *icon = "mdi:meter-electric";

	snprintf(base, sizeof(base), "%s", what);
	n = strlen(base);
	if (n > 6 && !strcmp(base + n - 6, "_power")) {
		base[n - 6] = 0;
		unit = "kW";
		dclass = "power";
		icon = "mdi:flash";
	} else if (n > 5 && !strcmp(base + n - 5, "_flow")) {
		unit = "m³/h";
		dclass = "volume_flow_rate";
		icon = strncmp(base, "water", 5) ? "mdi:meter-gas" : "mdi:water";
	} else if (!strcmp(base, "gas")) {
		unit = "m³";
		dclass = "gas";
		icon = "mdi:meter-gas";
	} else if (!strcmp(base, "water")) {
		unit = "m³";
		dclass = "water";
		icon = "mdi:water";
	} else if (!strncmp(base, "heat", 4) || strstr(base, "_s")) {
		unit = "";
		dclass = "";
	}
	snprintf(name, sizeof(name), "%s", what);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(names[i][0], base))
			snprintf(name, sizeof(name), strcmp(base, what) ? tr("%s, power") : "%s",
				 tr(names[i][1]));
	cJSON_AddStringToObject(e->attr, "friendly_name", name);
	cJSON_AddStringToObject(e->attr, "icon", icon);
	if (unit[0])
		cJSON_AddStringToObject(e->attr, "unit_of_measurement", unit);
	if (dclass[0])
		cJSON_AddStringToObject(e->attr, "device_class", dclass);
}

static void meter_poll(void)
{
	char line[96], id[64];
	bool seen[MAX_METER] = { 0 };
	FILE *f = state_open(METER);

	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *us = strchr(line, '_');
		struct entity *e;
		int i;

		if (line[0] != 'n' || !eq || !us || us > eq)
			continue;
		*eq = 0;
		eq[1 + strcspn(eq + 1, "\n")] = 0;
		snprintf(id, sizeof(id), "toon.meter_%s", line);
		for (i = 0; i < nmeter && strcmp(meter_ids[i], id); i++)
			;
		e = ent_get(id);
		if (i == nmeter && nmeter < MAX_METER) {
			snprintf(meter_ids[nmeter++], sizeof(meter_ids[0]), "%s", id);
			if (!cJSON_GetObjectItemCaseSensitive(e->attr, "friendly_name"))
				meter_attrs(e, us + 1);
		}
		if (i < MAX_METER)
			seen[i] = true;
		ent_set(e, eq + 1);
	}
	if (f)
		fclose(f);
	for (int i = 0; i < nmeter; i++)
		if (!seen[i])
			ent_set(ent_get(meter_ids[i]), "unavailable");
}

/*
 * The boiler's other OpenTherm values (toon-boilerd): every line of
 * retoon/opentherm, "<key>=<value>|<unit>|<device class>|<state
 * class>|<name>", is toon.<key>, made when it first comes. A value the
 * boiler stops answering is unavailable.
 */
#define MAX_OT	48	/* toon-boilerd sends 31 at most now */
static char ot_ids[MAX_OT][48];
static int nots;

static void opentherm_poll(void)
{
	char line[192], id[64];
	bool seen[MAX_OT] = { 0 };
	FILE *f = state_open(OPENTHERM);

	while (f && fgets(line, sizeof(line), f)) {
		char *val = strchr(line, '='), *unit, *dclass, *sclass, *name;
		struct entity *e;
		int i;

		if (!val)
			continue;
		*val++ = 0;
		line[strcspn(line, "\n")] = 0;
		val[strcspn(val, "\n")] = 0;
		if (!(unit = strchr(val, '|')) || !(dclass = strchr(unit + 1, '|')) ||
		    !(sclass = strchr(dclass + 1, '|')) || !(name = strchr(sclass + 1, '|')))
			continue;
		*unit++ = *dclass++ = *sclass++ = *name++ = 0;
		snprintf(id, sizeof(id), "toon.%s", line);
		for (i = 0; i < nots && strcmp(ot_ids[i], id); i++)
			;
		e = ent_get(id);
		if (i == nots && nots < MAX_OT) {
			snprintf(ot_ids[nots++], sizeof(ot_ids[0]), "%s", id);
			if (!cJSON_GetObjectItemCaseSensitive(e->attr, "friendly_name")) {
				cJSON_AddStringToObject(e->attr, "friendly_name", tr(name));
				cJSON_AddStringToObject(e->attr, "icon",
					!strcmp(dclass, "temperature") ? "mdi:thermometer" :
					!strcmp(dclass, "duration") ? "mdi:timer-outline" :
					!strcmp(dclass, "frequency") ? "mdi:fan" :
					!strcmp(sclass, "total_increasing") ? "mdi:counter" :
					!strcmp(dclass, "power") ? "mdi:fire" : "mdi:information-outline");
				if (unit[0])
					cJSON_AddStringToObject(e->attr, "unit_of_measurement", unit);
				if (dclass[0])
					cJSON_AddStringToObject(e->attr, "device_class", dclass);
			}
		}
		if (i < MAX_OT)
			seen[i] = true;
		ent_set(e, val);
	}
	if (f)
		fclose(f);
	for (int i = 0; i < nots; i++)
		if (!seen[i])
			ent_set(ent_get(ot_ids[i]), "unavailable");
}

void local_poll(void)
{
	char buf[96], air[96], mode[32] = "", hvac[16] = "off";
	struct entity *e;
	bool link, ot;
	int v, t, status = 0;

	/* a changed state is shown at once (on_state); the rest is read
	   again every few seconds */
	if ((int32_t)(now_ms() - t_next) < 0)
		return;
	t_next = now_ms() + 10000;

	meter_poll();
	opentherm_poll();
	/* the room temperature is what the thermostat works with (with sensors
	   chosen for it their average), the internal one the Toon's own sensor */
	if (file_value(BOILER, "room", buf, sizeof(buf)) &&
	    file_value(CLIMATE, "temperature", buf, sizeof(buf)))
		snprintf(buf, sizeof(buf), "unavailable");
	ent_set(ent_get("toon.temperature"), buf);
	set_file("toon.internal_temperature", CLIMATE, "temperature");
	/* the one the thermostat works with: OpenTherm's, or the sensor chosen */
	set_file("toon.outside_temperature", BOILER, "outside");
	if (!toon1) {
		set_file("toon.humidity", CLIMATE, "humidity");
		set_file("toon.illuminance", CLIMATE, "illuminance");
		if (file_value(VOC, "state", buf, sizeof(buf)))
			ent_set(ent_get("toon.air_quality"), "unavailable");
		else
			ent_set(ent_get("toon.air_quality"), air_text(buf, air, sizeof(air)));
	}

	link = !file_value(BOILER, "link", buf, sizeof(buf)) && !strcmp(buf, "ok");
	file_value(BOILER, "mode", mode, sizeof(mode));
	ot = link && !strcmp(mode, "opentherm");
	set_ot("toon.boiler_water", 25, ot, "%.1f");
	set_ot("toon.return_water", 28, ot, "%.1f");
	set_ot("toon.flow_setpoint", 1, ot, "%.1f");
	set_ot("toon.hot_water_setpoint", 56, ot, "%.0f");
	set_ot("toon.max_heating_setpoint", 57, ot, "%.0f");
	set_ot("toon.modulation", 17, ot, "%.0f");
	set_ot("toon.water_pressure", 18, ot, "%.1f");
	/* the boiler's own fault code means something only while its status
	   says fault (id 0 bit 0); many boilers send 255 otherwise */
	if (ot && !ot_raw(0, &status) && !(status & 0x01)) {
		ent_set(ent_get("toon.fault_code"), "none");
	} else if (ot && !ot_raw(5, &v)) {
		snprintf(buf, sizeof(buf), "%d", v & 0xff);
		ent_set(ent_get("toon.fault_code"), buf);
	} else {
		ent_set(ent_get("toon.fault_code"), "unavailable");
	}
	if (ot && !ot_raw(0, &status)) {
		ent_set(ent_get("toon.flame"), status & 0x08 ? "on" : "off");
		ent_set(ent_get("toon.boiler_fault"), status & 0x01 ? "on" : "off");
		ent_set(ent_get("toon.heating_active"), status & 0x02 ? "on" : "off");
		ent_set(ent_get("toon.hot_water_active"), status & 0x04 ? "on" : "off");
		ent_set(ent_get("toon.cooling_active"), status & 0x10 ? "on" : "off");
	} else {
		ent_set(ent_get("toon.flame"), "unavailable");
		ent_set(ent_get("toon.boiler_fault"), "unavailable");
		ent_set(ent_get("toon.heating_active"), "unavailable");
		ent_set(ent_get("toon.hot_water_active"), "unavailable");
		ent_set(ent_get("toon.cooling_active"), "unavailable");
	}
	e = ent_get("toon.hot_water");
	if (ot && !ot_raw(56, &v) && !ot_raw(3, &t) && (t & 0x0100)) {
		char temp[16] = "";

		if (!ot_raw(26, &t))
			snprintf(temp, sizeof(temp), "%.0f °C · ", (int16_t)t / 256.0);
		snprintf(buf, sizeof(buf), "%s%s %.0f °C%s%s", temp, tr("Set to"),
			 (int16_t)v / 256.0, status & 0x04 ? " · " : "",
			 status & 0x04 ? tr("Active") : "");
		set_str_attr(e, "status_text", buf);
		ent_set(e, status & 0x04 ? "on" : "off");
	} else {
		ent_set(e, "unavailable");
	}
	if (!link)
		ent_set(ent_get("toon.boiler_message"), "No boiler module");
	else
		set_file("toon.boiler_message", BOILER, "message");
	set_file("toon.heat_request", BOILER, "heat");
	e = ent_get("toon.heat_request");
	if (e->state && strcmp(e->state, "unavailable"))
		ent_set(e, !strcmp(e->state, "1") ? "on" : "off");
	set_file("toon.cool_request", BOILER, "cool");
	e = ent_get("toon.cool_request");
	if (e->state && strcmp(e->state, "unavailable"))
		ent_set(e, !strcmp(e->state, "1") ? "on" : "off");

	e = ent_get("toon.thermostat");
	if (file_value(BOILER, "hvac", hvac, sizeof(hvac))) {
		ent_set(e, "unavailable");	/* toon-boilerd is not running */
	} else {
		char preset[16] = "", next[32] = "", until[8] = "", info[96] = "", *sp;
		bool flag[3];
		static const char *keys[] = { "frost", "preheat", "hold" };

		/* presets and program work without a boiler module too */
		ent_set(e, hvac);
		if (!file_value(BOILER, "target", buf, sizeof(buf)))
			set_num_attr(e, "temperature", atof(buf));
		if (!file_value(BOILER, "room", buf, sizeof(buf)) ||
		    !file_value(CLIMATE, "temperature", buf, sizeof(buf)))
			set_num_attr(e, "current_temperature", atof(buf));
		file_value(BOILER, "heat", buf, sizeof(buf));
		v = !strcmp(buf, "1");
		file_value(BOILER, "cool", buf, sizeof(buf));
		set_str_attr(e, "hvac_action", v ? "heating" : !strcmp(buf, "1") ? "cooling" :
			     !strcmp(hvac, "off") ? "off" : "idle");
		/* only where cooling is in use: the dial then says which side it is on */
		buf[0] = 0;
		if (!file_value(BOILER, "cool_on", buf, sizeof(buf)) && !strcmp(buf, "1"))
			file_value(BOILER, "season", buf, sizeof(buf));
		else
			buf[0] = 0;
		set_str_attr(e, "season", buf);
		file_value(BOILER, "preset", preset, sizeof(preset));
		set_str_attr(e, "preset_mode", preset[0] && strcmp(preset, "manual") ? preset : "none");
		for (int i = 0; i < 3; i++)
			flag[i] = !file_value(BOILER, keys[i], buf, sizeof(buf)) && buf[0] == '1';
		file_value(BOILER, "next", next, sizeof(next));	/* "22:30 sleep" */
		file_value(BOILER, "until", until, sizeof(until));
		sp = strchr(next, ' ');
		if (sp) {
			*sp++ = 0;
			if (state_text(sp))
				sp = (char *)state_text(sp);
			else
				sp[0] = sp[0] >= 'a' && sp[0] <= 'z' ? sp[0] - 32 : sp[0];
		}
		/* no boiler module: the card says so top right; the line under
		   what it is doing still tells what the program does */
		set_num_attr(e, "no_boiler", !link);
		set_num_attr(e, "external", !file_value(BOILER, "external", buf, sizeof(buf)) &&
			     !strcmp(buf, "1"));
		if (flag[0])
			snprintf(info, sizeof(info), "%s", tr("Frost protection"));
		else if (!strcmp(hvac, "off"))
			info[0] = 0;
		else if (flag[1] && sp)
			snprintf(info, sizeof(info), tr("Warming up for %s at %s"), sp, next);
		else if (!file_value(BOILER, "hold", buf, sizeof(buf)) && buf[0] == '2' &&
			 !file_value(BOILER, "program", buf, sizeof(buf)) && buf[0] == '1')
			snprintf(info, sizeof(info), "%s", tr("Locked"));
		else if (flag[2] && until[0])
			snprintf(info, sizeof(info), tr("Temporary until %s"), until);
		else if (sp)
			snprintf(info, sizeof(info), tr("Then %s at %s"), sp, next);
		set_str_attr(e, "status_text", info);
		/* for the card: a choice by hand (1 until the program's next step,
		   2 locked) and whether the week program is on and running */
		set_num_attr(e, "hold", !file_value(BOILER, "hold", buf, sizeof(buf)) ? atoi(buf) : 0);
		set_num_attr(e, "program_enabled",
			     !file_value(BOILER, "program_enabled", buf, sizeof(buf)) && buf[0] == '1');
		set_num_attr(e, "program",
			     !file_value(BOILER, "program", buf, sizeof(buf)) && buf[0] == '1');
	}

	e = ent_get("toon.boiler_connection");
	ent_set(e, !link ? "unavailable" : !strcmp(mode, "opentherm") ? "OpenTherm" :
		!strcmp(mode, "onoff") ? "On/off" : !strcmp(mode, "detecting") ? "Detecting" : "unknown");

	e = ent_get("toon.display");
	v = backlight_get();
	if (v >= 0) {
		ent_set(e, v ? "on" : "off");
		set_num_attr(e, "brightness", v * 255 / 100);
	}

	control_poll();

	e = ent_get("toon.wifi");
	if (wifi_signal()) {
		snprintf(buf, sizeof(buf), "%d", wifi_signal());
		ent_set(e, buf);
	} else {
		ent_set(e, "unavailable");
	}
	ent_flush();
}

/*
 * The temperature the thermostat works with: the average of the sensors
 * chosen in the web interface ("toon" = the Toon's own, the rest are
 * entities of Home Assistant or MQTT). toon-boilerd reads it from
 * retoon/control and falls back to the Toon's own sensor when that is
 * missing or old - so it is cleared when only "toon" is chosen.
 */
/* a value from Home Assistant or MQTT is only current while that
   connection is up: a sensor that stopped changing is normal, but one whose
   link is gone could hold an old value for hours */
static bool source_up(const char *id)
{
	return !strncmp(id, "mqtt.", 5) ? mqtt_state() == 2 : ha_ready();
}

/* a short code for the set of sensors in the average (and the correction
   of the Toon's own): toon-boilerd sees any change of them */
static unsigned long set_code(unsigned long h, const char *s)
{
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h * 33 + '|';
}

static void outside_poll(const char *id)
{
	struct entity *e;
	char *end;
	double v;
	char kv[48];

	if (!id[0])
		return;
	e = ent_get(id);
	if (!e->state || !source_up(id))
		return;
	v = strtod(e->state, &end);
	if (end == e->state || *end || !isfinite(v) || v < -40 || v > 60)
		return;		/* it grows old: no outside temperature */
	snprintf(kv, sizeof(kv), "temperature=%.1f\n", v);
	bus_put("outside", kv);
}

/*
 * Which of the chosen sensors is used ("sensor_use", from the Home Assistant
 * select through toon-boilerd): "Average" of them all (also when not set),
 * "Toon" for its own sensor only, or one of them. SENSOR_CHOICE holds the
 * options and the choice for toon-mqttd.
 */
#define SENSOR_CHOICE	"sensor-choice"

static void control_poll(void)
{
	static char ids[16][96], outside[96], use[96];
	static const char *list[17];
	static int n, own = 1;
	static long long mt = -1;
	struct stat st;
	/* to the nanosecond (and the size): two changes within a second count */
	long long now_mt = stat(THERMOSTAT_CONF, &st) ? 0 :
			   st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
	char buf[64], *end;
	double sum = 0;
	int have = 0;
	unsigned long code = 5381;	/* the set of sensors in the average */
	FILE *f;

	if (now_mt != mt) {
		cJSON *doc = NULL, *s;
		char *txt = NULL;
		long len;

		mt = now_mt;
		n = 0;
		own = 1;
		f = fopen(THERMOSTAT_CONF, "r");
		if (f) {
			fseek(f, 0, SEEK_END);
			len = ftell(f);
			rewind(f);
			txt = calloc(1, len + 1);
			if (fread(txt, 1, len, f) != (size_t)len)
				txt[0] = 0;
			fclose(f);
			doc = cJSON_Parse(txt);
			free(txt);
		}
		s = cJSON_GetObjectItemCaseSensitive(doc, "sensors");
		if (cJSON_IsArray(s) && cJSON_GetArraySize(s)) {
			cJSON *i;

			own = 0;
			cJSON_ArrayForEach(i, s) {
				if (!cJSON_IsString(i))
					continue;
				if (!strcmp(i->valuestring, "toon")) {
					own = 1;
				} else if (n < 16) {
					snprintf(ids[n], sizeof(ids[n]), "%s", i->valuestring);
					list[n] = ids[n];
					n++;
				}
			}
		}
		/* the outside sensor: for the whole thermostat, before 1.24 only
		 * under "cooling" */
		s = cJSON_GetObjectItemCaseSensitive(doc, "outside_entity");
		if (!cJSON_IsString(s) || !s->valuestring[0])
			s = cJSON_GetObjectItemCaseSensitive(
				cJSON_GetObjectItemCaseSensitive(doc, "cooling"), "outside_entity");
		snprintf(outside, sizeof(outside), "%s", cJSON_IsString(s) ? s->valuestring : "");
		s = cJSON_GetObjectItemCaseSensitive(doc, "sensor_use");
		snprintf(use, sizeof(use), "%s", cJSON_IsString(s) ? s->valuestring : "Average");
		if (strcmp(use, "Toon")) {
			int i;

			for (i = 0; i < n && strcmp(ids[i], use); i++)
				;
			if (i == n)
				snprintf(use, sizeof(use), "Average");
		}
		if (!n) {
			bus_put(SENSOR_CHOICE, NULL);	/* only the Toon's own: nothing to choose */
		} else {
			char *kv = NULL;
			size_t kv_len = 0;

			f = open_memstream(&kv, &kv_len);
			if (f) {
				fprintf(f, "options=Average|Toon");
				for (int i = 0; i < n; i++)
					fprintf(f, "|%s", ids[i]);
				fprintf(f, "\nuse=%s\n", use);
				fclose(f);
				bus_put(SENSOR_CHOICE, kv);
				free(kv);
			}
		}
		list[n] = outside;
		cJSON_Delete(doc);
		ent_want(list, n + !!outside[0]);
		ha_restart();
		mqtt_restart();
	}
	outside_poll(outside);
	if (!n || !strcmp(use, "Toon")) {
		bus_put("control", NULL);	/* toon-boilerd uses the Toon's own sensor */
		return;
	}
	if (own && !strcmp(use, "Average") && !file_value(CLIMATE, "temperature", buf, sizeof(buf))) {
		sum += atof(buf);
		have++;
		code = set_code(code, "toon");
		if (!file_value(CLIMATE, "temp_offset", buf, sizeof(buf)))
			code = set_code(code, buf);
	}
	for (int i = 0; i < n; i++) {
		struct entity *e = ent_find(ids[i]);
		double v;

		if (strcmp(use, "Average") && strcmp(use, ids[i]))
			continue;	/* one sensor chosen: only that one */
		if (!e || !e->state || !source_up(ids[i]))
			continue;
		v = strtod(e->state, &end);
		if (end == e->state || *end || !isfinite(v) || v < -30 || v > 60)
			continue;	/* "unavailable", "nan", or not a temperature */
		sum += v;
		have++;
		code = set_code(code, ids[i]);
	}
	if (!have)
		return;		/* it grows old and the Toon's own sensor takes over */
	char kv[128];

	snprintf(kv, sizeof(kv), "temperature=%.2f\nsensors=%d\nof=%d\nset=%08lx\n", sum / have, have,
		 !strcmp(use, "Average") ? n + own : 1, code & 0xffffffffUL);
	bus_put("control", kv);
}

/* each line marked as from the screen, for the thermostat's change log */
static void boiler_cmd(const char *fmt, ...)
{
	char cmds[96], line[256], *p = line, *s, *nl;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmds, sizeof(cmds), fmt, ap);
	va_end(ap);
	line[0] = 0;
	for (s = cmds; *s; s = nl + 1) {
		nl = strchr(s, '\n');
		if (!nl)
			break;
		p += snprintf(p, line + sizeof(line) - p, "@screen %.*s\n", (int)(nl - s), s);
	}
	bus_send(BOILER_CMD, line);
	t_next = now_ms() + 600;	/* show the result soon */
}

void local_call(const char *domain, const char *service, const char *entity, cJSON *data)
{
	cJSON *v;

	(void)domain;
	if (!strcmp(entity, "toon.thermostat")) {
		/* a choice made on the screen also switches the thermostat on */
		v = cJSON_GetObjectItemCaseSensitive(data, "temperature");
		if (!strcmp(service, "set_temperature") && cJSON_IsNumber(v))
			boiler_cmd("hvac heat\ntarget %.1f\n", v->valuedouble);
		v = cJSON_GetObjectItemCaseSensitive(data, "preset_mode");
		if (!strcmp(service, "set_preset_mode") && cJSON_IsString(v) &&
		    strcmp(v->valuestring, "none") && !strpbrk(v->valuestring, " \n"))
			boiler_cmd("hvac heat\npreset %s\n", v->valuestring);
		/* ReToon's own: lock a preset, follow the program again, the
		   program on or off (from the card) */
		v = cJSON_GetObjectItemCaseSensitive(data, "preset_mode");
		if (!strcmp(service, "retoon_lock_preset") && cJSON_IsString(v) &&
		    strcmp(v->valuestring, "none") && !strpbrk(v->valuestring, " \n"))
			boiler_cmd("hvac heat\npreset %s lock\n", v->valuestring);
		if (!strcmp(service, "retoon_resume_program"))
			boiler_cmd("program resume\n");
		if (!strcmp(service, "retoon_program_on"))
			boiler_cmd("program on\n");
		if (!strcmp(service, "retoon_program_off"))
			boiler_cmd("program off\n");
		v = cJSON_GetObjectItemCaseSensitive(data, "hvac_mode");
		if (!strcmp(service, "set_hvac_mode") && cJSON_IsString(v))
			boiler_cmd("hvac %s\n", !strcmp(v->valuestring, "heat") ? "heat" : "off");
		if (!strcmp(service, "turn_off"))
			boiler_cmd("hvac off\n");
		if (!strcmp(service, "turn_on"))
			boiler_cmd("hvac heat\n");
	} else if (!strcmp(entity, "toon.boiler_connection")) {
		v = cJSON_GetObjectItemCaseSensitive(data, "option");
		if (cJSON_IsString(v))
			boiler_cmd("mode %s\n", !strcmp(v->valuestring, "OpenTherm") ? "ot" : "onoff");
	} else if (!strcmp(entity, "toon.display")) {
		v = cJSON_GetObjectItemCaseSensitive(data, "brightness_pct");
		if (!strcmp(service, "turn_off"))
			backlight_set(0);
		else if (cJSON_IsNumber(v))
			backlight_set(LV_MAX(5, v->valueint));	/* never fully dark from the screen */
		else if (!strcmp(service, "turn_on") || !strcmp(service, "toggle"))
			backlight_set(backlight_get() ? 0 : 80);
		t_next = now_ms() + 200;
	}
}

/* a new state from one of the programs: shown soon (several often come in
   a row); not control and outside, this program sends those itself */
static void on_state(const char *topic, const char *payload)
{
	const char *f = topic + sizeof("retoon");

	(void)payload;
	if (!strcmp(f, "climate") || !strcmp(f, "voc") || !strcmp(f, "boiler") ||
	    !strcmp(f, "meter") || !strcmp(f, "opentherm"))
		if ((int32_t)(t_next - now_ms()) > 50)
			t_next = now_ms() + 50;
}

void local_init(void)
{
	static const char *modes[] = { "off", "heat" }, *conns[] = { "OpenTherm", "On/off" };
	static const char *presets[] = { "comfort", "home", "sleep", "away" };
	struct entity *e;
	char model[64] = "";
	FILE *f = fopen("/proc/device-tree/model", "r");

	if (f) {
		if (!fgets(model, sizeof(model), f))
			model[0] = 0;
		fclose(f);
	}
	toon1 = strstr(model, "Toon 1") != NULL;
	find_backlight();
	bus_watch("climate");
	bus_watch("voc");
	bus_watch("boiler");
	bus_watch("opentherm");
	bus_watch("meter");
	bus_on("retoon/+", on_state);
	for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
		if (absent(defs[i].id))
			continue;
		e = ent_get(defs[i].id);
		cJSON_AddStringToObject(e->attr, "friendly_name", tr(defs[i].name));
		cJSON_AddStringToObject(e->attr, "icon", defs[i].icon);
		if (defs[i].unit[0])
			cJSON_AddStringToObject(e->attr, "unit_of_measurement", defs[i].unit);
		if (defs[i].dclass[0])
			cJSON_AddStringToObject(e->attr, "device_class", defs[i].dclass);
	}
	e = ent_get("toon.thermostat");
	cJSON_AddItemToObject(e->attr, "hvac_modes", cJSON_CreateStringArray(modes, 2));
	cJSON_AddItemToObject(e->attr, "preset_modes", cJSON_CreateStringArray(presets, 4));
	cJSON_AddNumberToObject(e->attr, "min_temp", 6);
	cJSON_AddNumberToObject(e->attr, "max_temp", 30);
	cJSON_AddNumberToObject(e->attr, "target_temp_step", 0.5);
	e = ent_get("toon.boiler_connection");
	cJSON_AddItemToObject(e->attr, "options", cJSON_CreateStringArray(conns, 2));
}
