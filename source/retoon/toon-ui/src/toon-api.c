// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-api: cgi-bin/api of the web interface. The pages ask GET /status
 * every few seconds, GET /settings on every tab and GET /thermostat/state
 * every 10 s on the Thermostat tab; those three only collect
 * key=value files and the programs' states, and the shell script that did
 * it started a process for nearly every value (over a second per request on
 * a Toon 1). They are answered here, in one process, with the same JSON.
 * Every other request goes on to the shell script (API_SH), which does the
 * changes.
 *
 * The programs' states are on the Toon's own MQTT broker (retoon/<name>,
 * libtoonbus): fetched once per request, the first time one is needed, and
 * read as the key=value files they used to be ("/run/toon/<name>" below).
 */
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <toonbus.h>

#define API_SH	"/usr/share/toon/api.sh"
#define DIR	"/mnt/data/toon-os/config"

/* the programs' states, all at once from the broker */
static const char *const states[] = {
	"boiler", "climate", "control", "esphome", "m4", "meter", "meters", "motion",
	"opentherm", "outside", "sensor-choice", "thread", "ui", "voc", "zwave",
};
#define N_STATES	(sizeof(states) / sizeof(states[0]))
static char *state_kv[N_STATES];	/* each as key=value text, made once */
static int fetched;

static void keep_state(const char *name, const char *json, void *ud)
{
	(void)ud;
	for (size_t i = 0; i < N_STATES; i++)
		if (!strcmp(states[i], name)) {
			size_t len = strlen(json) + 64;

			free(state_kv[i]);
			state_kv[i] = malloc(len);
			if (state_kv[i])
				bus_json_kv(json, state_kv[i], len);
		}
}

/* "/run/toon/<name>" of a state: its key=value text, or NULL; -1 for a path
   that is no state */
static int state_text(const char *path, char **out)
{
	const char *name = path + strlen("/run/toon/");

	if (strncmp(path, "/run/toon/", strlen("/run/toon/")))
		return -1;
	for (size_t i = 0; i < N_STATES; i++)
		if (!strcmp(states[i], name)) {
			if (!fetched) {
				fetched = 1;
				bus_snapshot(keep_state, NULL);
			}
			*out = state_kv[i] ? strdup(state_kv[i]) : NULL;
			return 0;
		}
	return -1;
}

/* A whole file (at most 64 kB), or a state as text; NULL if there is none. */
static char *slurp(const char *path)
{
	FILE *f;
	char *buf;
	size_t n;

	if (!state_text(path, &buf))
		return buf;
	f = fopen(path, "r");
	if (!f)
		return NULL;
	buf = malloc(65537);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	n = fread(buf, 1, 65536, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

/* The output of a program (no shell), or "" when it cannot run. */
static char *run(char *const argv[])
{
	static char none[1];
	char *buf;
	size_t n = 0;
	ssize_t r;
	int p[2];
	pid_t pid;

	if (pipe(p))
		return none;
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);

		dup2(p[1], 1);
		dup2(null, 2);
		close(p[0]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	buf = malloc(65537);
	while (buf && pid > 0 && n < 65536 && (r = read(p[0], buf + n, 65536 - n)) > 0)
		n += r;
	close(p[0]);
	if (pid > 0)
		waitpid(pid, NULL, 0);
	if (!buf)
		return none;
	buf[n] = 0;
	return buf;
}

/* The last value of key= in a text, "" when the key is not there. */
static const char *val_in(const char *text, const char *key)
{
	static char out[8][512];
	static int slot;
	size_t klen = strlen(key);
	const char *l, *found = NULL;
	char *o = out[slot++ % 8];
	size_t n;

	for (l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL)
		if (!strncmp(l, key, klen) && l[klen] == '=')
			found = l + klen + 1;
	o[0] = 0;
	if (found) {
		n = strcspn(found, "\n");
		if (n >= sizeof(out[0]))
			n = sizeof(out[0]) - 1;
		memcpy(o, found, n);
		o[n] = 0;
	}
	return o;
}

/* The same from a file; NULL when the file is not there. */
static const char *val(const char *path, const char *key)
{
	char *text = slurp(path);
	const char *v;

	if (!text)
		return NULL;
	v = val_in(text, key);
	free(text);
	return v;
}

/* val, or "" for a missing file */
static const char *v(const char *path, const char *key)
{
	const char *r = val(path, key);

	return r ? r : "";
}

/* a line in the file starts with this */
static int has_line(const char *path, const char *start)
{
	char *text = slurp(path), *l;
	int found = 0;

	for (l = text; l && *l && !found; l = strchr(l, '\n'), l = l ? l + 1 : NULL)
		found = !strncmp(l, start, strlen(start));
	free(text);
	return found;
}

static int exists(const char *path) { return access(path, F_OK) == 0; }
static int executable(const char *path) { return access(path, X_OK) == 0; }

static int nonempty(const char *path)
{
	struct stat st;

	return !stat(path, &st) && st.st_size > 0;
}

/* A JSON string: quotes and backslashes escaped, CR and LF left out. */
static void js(const char *name, const char *s, int first)
{
	printf("%s\"%s\":\"", first ? "" : ",", name);
	for (; s && *s; s++) {
		if (*s == '"' || *s == '\\')
			putchar('\\');
		if (*s != '\r' && *s != '\n')
			putchar(*s);
	}
	putchar('"');
}

static void reply_json(void)
{
	printf("Status: 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n\r\n");
}

static const char *wlan_ip(void)
{
	static char ip[INET_ADDRSTRLEN];
	struct ifaddrs *ifa, *i;

	ip[0] = 0;
	if (getifaddrs(&ifa))
		return ip;
	for (i = ifa; i; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "wlan0"))
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
	freeifaddrs(ifa);
	return ip;
}

static const char *host(void)
{
	static char h[256];

	if (gethostname(h, sizeof(h) - 1))
		h[0] = 0;
	return h;
}

/*
 * The signal in dBm, as "iw dev wlan0 link" gives it. A stick with only the
 * old wireless extensions (the Toon 1's Realtek, r8712u) has it in
 * /proc/net/wireless instead: a level above 0 there is a percentage, about
 * pct / 2 - 100 dBm.
 */
static const char *wifi_signal(void)
{
	static char out[16];
	char *argv[] = { "/usr/sbin/iw", "dev", "wlan0", "link", NULL };
	char *text = run(argv), *p, line[256];
	float link, level;
	FILE *f;

	out[0] = 0;
	p = strstr(text, "\n\tsignal: ");
	if (p) {
		snprintf(out, sizeof(out), "%d", atoi(p + 10));
		return out;
	}
	f = fopen("/proc/net/wireless", "r");
	if (!f)
		return out;
	while (fgets(line, sizeof(line), f)) {
		p = strstr(line, "wlan0:");
		if (p && sscanf(p + 6, "%*x %f %f", &link, &level) == 2 && level != 0) {
			snprintf(out, sizeof(out), "%d", level > 0 ? (int)(level / 2 - 100) : (int)level);
			break;
		}
	}
	fclose(f);
	return out;
}

static void status(void)
{
	char *rel = slurp("/etc/retoon-version"), *os = slurp("/etc/os-release");
	const char *osv = os ? val_in(os, "VERSION_ID") : "";
	char sys[200], up[32] = "0";
	struct utsname u;
	FILE *f;

	if (uname(&u))
		u.release[0] = 0;
	snprintf(sys, sizeof(sys), "%s %s", osv, u.release);
	f = fopen("/proc/uptime", "r");
	if (f) {
		if (fscanf(f, "%31[0-9]", up) != 1)
			strcpy(up, "0");
		fclose(f);
	}
	reply_json();
	putchar('{');
	js("hostname", host(), 1);
	js("ip", wlan_ip(), 0);
	js("wifi_signal", wifi_signal(), 0);
	printf(",\"uptime\":%s", up);
	js("ha", v("/run/toon/ui", "ha"), 0);
	js("view", v("/run/toon/ui", "view"), 0);
	js("boiler_link", v("/run/toon/boiler", "link"), 0);
	js("boiler_mode", v("/run/toon/boiler", "mode"), 0);
	js("boiler_message", v("/run/toon/boiler", "message"), 0);
	js("temperature", v("/run/toon/climate", "temperature"), 0);
	js("humidity", v("/run/toon/climate", "humidity"), 0);
	js("mqtt", v("/run/toon/ui", "mqtt"), 0);
	js("m4", v("/run/toon/m4", "text"), 0);
	js("air", !strcmp(v("/run/toon/voc", "state"), "warmup") ? "measuring" :
		  v("/run/toon/voc", "quality"), 0);
	js("air_last", v("/run/toon/voc", "quality_last"), 0);
	js("air_trend", v("/run/toon/voc", "quality_trend"), 0);
	js("air_learning", v("/run/toon/voc", "quality_learning"), 0);
	js("version", rel ? rel : "", 0);
	js("system", sys, 0);
	printf("}\n");
	free(rel);
	free(os);
}

/*
 * The Z-Wave network keys of the original software, taken over by the setup
 * app (config/zwave-keys.xml) or still in its own place on the data
 * partition: "S0 <key> S2U <key> S2A <key> S2C <key>", for Z-Wave JS.
 */
static void zwave_keys(char *out, size_t len)
{
	static const struct { const char *tag, *name; } keys[] = {
		{ "<s0NetworkKey>", "S0 " },
		{ "<s2NetworkKeyUnauthenticated>", "S2U " },
		{ "<s2NetworkKeyAuthenticated>", "S2A " },
		{ "<s2NetworkKeyAccess>", "S2C " },
	};
	const char *path = DIR "/zwave-keys.xml";
	char *xml, *s;
	size_t n = 0;
	int in_tag = 0, skip = 0, after_key = 0;
	unsigned int k;

	out[0] = 0;
	if (!nonempty(path))
		path = "/mnt/data/qmf/var/hdrv_zwave/s2keystore.xml";
	if (!nonempty(path) || !(xml = slurp(path)))
		return;
	/* the key names in front of the values, the private key left out, the
	 * tags removed, spaces and line ends folded into single spaces */
	for (s = xml; *s && n + 8 < len; s++) {
		if (*s == '<') {
			if (after_key && (!n || out[n - 1] != ' '))
				out[n++] = ' ';
			after_key = 0;
			for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
				if (!strncmp(s, keys[k].tag, strlen(keys[k].tag)))
					break;
			if (k < sizeof(keys) / sizeof(keys[0])) {
				n += snprintf(out + n, len - n, "%s", keys[k].name);
				s += strlen(keys[k].tag) - 1;
				after_key = 1;
				continue;
			}
			skip = !strncmp(s, "<ecdhPrivateKey>", 16);
			in_tag = 1;
			continue;
		}
		if (in_tag) {
			in_tag = *s != '>';
			continue;
		}
		if (skip)
			continue;
		if (after_key && !strchr("0123456789abcdefABCDEF", *s))
			after_key = 0;
		if (*s == ' ' || *s == '\n') {
			if (n && out[n - 1] == ' ')
				continue;
			out[n++] = ' ';
		} else {
			out[n++] = *s;
		}
	}
	out[n] = 0;
	free(xml);
}

/* the Toon's own broker (S41toon-broker): its settings, whether it runs, and
   the network addresses it listens on ("wlan0 192.168.1.20" lines) */
static void broker(void)
{
	const char *bc = DIR "/broker.conf";
	char proc[64], ips[200] = "", *text, *l;
	size_t n = 0;

	js("broker_lan", v(bc, "lan"), 0);
	js("broker_username", v(bc, "username"), 0);
	printf(",\"broker_password_set\":%d", *v(bc, "password") != 0);
	text = slurp("/var/run/toon-broker.pid");
	snprintf(proc, sizeof(proc), "/proc/%d", text ? atoi(text) : 0);
	free(text);
	printf(",\"broker_running\":%d", proc[6] != '0' && exists(proc));
	text = slurp("/run/toon/broker.ips");
	for (l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
		char *sp = strchr(l, ' '), *end = strchr(l, '\n');
		size_t len;

		if (!sp || (end && sp > end))
			continue;
		len = (end ? (size_t)(end - sp - 1) : strlen(sp + 1));
		if (n + len + 2 < sizeof(ips)) {
			n += snprintf(ips + n, sizeof(ips) - n, "%s%.*s", n ? " " : "", (int)len, sp + 1);
		}
	}
	free(text);
	js("broker_listening", ips, 0);
}

static void settings(void)
{
	const char *ha = DIR "/ha.conf", *mq = DIR "/mqtt.conf", *ui = DIR "/ui.conf";
	const char *zw = DIR "/zwave.conf", *eh = DIR "/esphome.conf";
	const char *auth = "none", *lang;
	char radio[64], vol[16];
	char *thread = NULL, *own, keys[1024], def[300];

	if (*v(ha, "refresh_token"))
		auth = "login";
	if (*v(ha, "token"))
		auth = "token";
	if (!strcmp(v(ha, "made_by"), "login"))
		auth = "login";
	snprintf(radio, sizeof(radio), "%s", executable("/usr/bin/gp711-rcpd") ?
		 v(DIR "/radio.conf", "mode") : "none");
	if (!strcmp(radio, "thread")) {
		char *argv[] = { "/usr/sbin/toon-thread", "status", NULL };

		thread = run(argv);
	}
	own = slurp(DIR "/hostname");
	if (own)
		own[strcspn(own, "\n")] = 0;
	snprintf(def, sizeof(def), "toon-%s", v("/run/toon/id", "id"));
	zwave_keys(keys, sizeof(keys));
	snprintf(vol, sizeof(vol), "%s", v(DIR "/sound.conf", "volume"));
	if (!*vol) {
		char *model = slurp("/proc/device-tree/model");

		strcpy(vol, model && strstr(model, "Toon 1") ? "30" : "70");
		free(model);
	}

	reply_json();
	putchar('{');
	js("hostname", host(), 1);
	js("own_hostname", own ? own : "", 0);
	js("default_hostname", def, 0);
	js("autoboot", exists(DIR "/autoboot") ? "1" : "0", 0);
	printf(",\"root_pw_own\":%d", nonempty(DIR "/root.hash"));
	js("zwave_mode", v(zw, "mode"), 0);
	js("zwave_allow", v(zw, "allow"), 0);
	js("zwave_state", v("/run/toon/zwave", "state"), 0);
	js("zwave_client", v("/run/toon/zwave", "client"), 0);
	js("zwave_meters", v(zw, "meters"), 0);
	js("zwave_found", v("/run/toon/meters", "found"), 0);
	js("update_auto", v(DIR "/update.conf", "auto"), 0);
	printf(",\"apps_auto\":%d", exists(DIR "/apps-autoupdate"));
	{
		char *model = slurp("/proc/device-tree/model");

		js("board", model && strstr(model, "Toon 1") ? "toon1" : "toon2", 0);
		free(model);
	}
	{
		/* the boiler's other OpenTherm values (toon-boilerd), lines
		   "<key>=<value>|<unit>|...|<name>", for the card editor */
		char *ot = slurp("/run/toon/opentherm");

		for (char *p = ot; p && *p; p++)
			if (*p == '\n')
				*p = ';';	/* js() leaves out line ends */
		js("opentherm", ot ? ot : "", 0);
		free(ot);
	}
	js("zwave_keys", keys, 0);
	js("sound_available", executable("/usr/bin/toon-sound") ? "1" : "0", 0);
	js("sound_volume", vol, 0);
	js("sound_ready", exists("/dev/snd/pcmC0D0p") || has_line("/run/toon/m4", "sound=on") ?
			  "1" : "0", 0);
	js("esphome_available", executable("/usr/bin/toon-esphomed") ? "1" : "0", 0);
	js("esphome_mode", v(eh, "mode"), 0);
	js("esphome_allow", v(eh, "allow"), 0);
	js("esphome_state", v("/run/toon/esphome", "state"), 0);
	js("esphome_client", v("/run/toon/esphome", "client"), 0);
	js("esphome_scanning", v("/run/toon/esphome", "scanning"), 0);
	js("esphome_adverts", v("/run/toon/esphome", "adverts_per_minute"), 0);
	js("radio_mode", *radio ? radio : "zigbee", 0);
	js("thread_state", thread ? val_in(thread, "state") : "", 0);
	js("thread_name", thread ? val_in(thread, "name") : "", 0);
	js("thread_channel", thread ? val_in(thread, "channel") : "", 0);
	js("thread_children", thread ? val_in(thread, "children") : "", 0);
	js("thread_routers", thread ? val_in(thread, "routers") : "", 0);
	js("ip", wlan_ip(), 0);
	js("ha_url", v(ha, "url"), 0);
	js("ha_auth", auth, 0);
	js("ha_state", v("/run/toon/ui", "ha"), 0);
	js("ha_addr", v("/run/toon/ui", "ha_addr"), 0);
	js("ha_user", v(ha, "user"), 0);
	js("ha_made", v(ha, "made"), 0);
	js("ha_verify", v(ha, "verify"), 0);
	js("web_http", v(DIR "/web.conf", "http"), 0);
	js("web_fingerprint", v("/run/toon/web", "fingerprint"), 0);
	js("mqtt_host", v(mq, "host"), 0);
	js("mqtt_port", v(mq, "port"), 0);
	js("mqtt_username", v(mq, "username"), 0);
	printf(",\"mqtt_password_set\":%d", *v(mq, "password") != 0);
	js("mqtt_discovery_prefix", v(mq, "discovery_prefix"), 0);
	broker();
	js("brightness", v(ui, "brightness"), 0);
	js("dim_after", v(ui, "dim_after"), 0);
	js("dim_level", v(ui, "dim_level"), 0);
	js("dim_grey", v(ui, "dim_grey"), 0);
	js("sleep_off", v(ui, "sleep_off"), 0);
	js("off_from", v(ui, "off_from"), 0);
	js("off_to", v(ui, "off_to"), 0);
	js("home_auto", v(ui, "home_auto"), 0);
	js("home_after", v(ui, "home_after"), 0);
	/* not set: on, except on a Toon 1 (too slow to slide smoothly) */
	if (*v(ui, "page_anim")) {
		js("page_anim", v(ui, "page_anim"), 0);
	} else {
		char *model = slurp("/proc/device-tree/model");

		js("page_anim", model && strstr(model, "Toon 1") ? "0" : "1", 0);
		free(model);
	}
	js("thermostat_buttons", v(ui, "thermostat_buttons"), 0);	/* not set: on */
	lang = val(DIR "/language", "lang");
	js("language", lang ? lang : "nl", 0);
	printf("}\n");
	free(own);
}

static void thermostat_state(void)
{
	static const char *const keys[] = {
		"hvac", "target", "room", "room_source", "preset", "program", "hold",
		"preheat", "frost", "next", "until", "eff_target", "heat", "heat_rate",
		"cool_rate", "measured", "flow", "link", "mode", "message", "ot", "ot_known",
		"ot_dhw", "ot_cool", "cool", "cool_on", "season", "auto_ok",
		"outside", "outside_source", "hw_wanted", "hw_setpoint", "hw_temp", "hw_active", "hw_min", "hw_max",
		"external", "kloss_n", "from_vendor", "cool_per_10", "cool_rate_now", "flow_ignored",
		"after_10", "preheat_extra", "preheat_n",
		"transport", "esp_host", "esp_name", "esp_version", "esp_project_version", "esp_temperature",
	};
	char *boiler = slurp("/run/toon/boiler");
	unsigned int k;

	reply_json();
	putchar('{');
	for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
		js(keys[k], boiler ? val_in(boiler, keys[k]) : "", k == 0);
	js("sensors", v("/run/toon/control", "sensors"), 0);
	js("of", v("/run/toon/control", "of"), 0);
	/* the Toon's own sensor, with and without the user's correction */
	js("own", v("/run/toon/climate", "temperature"), 0);
	js("own_uncorrected", v("/run/toon/climate", "uncorrected"), 0);
	printf("}\n");
	free(boiler);
}

int main(int argc, char **argv)
{
	const char *method = getenv("REQUEST_METHOD"), *path = getenv("PATH_INFO");

	if (method && path && !strcmp(method, "GET")) {
		if (!strcmp(path, "/status")) {
			status();
			return 0;
		}
		if (!strcmp(path, "/settings")) {
			settings();
			return 0;
		}
		if (!strcmp(path, "/thermostat/state")) {
			thermostat_state();
			return 0;
		}
	}
	argv[0] = API_SH;
	execv(API_SH, argv);
	printf("Status: 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nNo " API_SH "\n");
	return 0;
}
