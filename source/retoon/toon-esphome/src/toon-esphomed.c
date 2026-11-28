/*
 * toon-esphomed: the ReToon as an ESPHome device for Home Assistant.
 *
 * Speaks the ESPHome native API (plaintext frames, TCP port 6053) and
 * announces itself with Avahi as _esphomelib._tcp, so Home Assistant's
 * built-in ESPHome integration discovers it like an ESP32. What it offers
 * is the Bluetooth proxy: BLE advertisements picked up by the Toon's own
 * BCM43430 are forwarded raw (BluetoothLERawAdvertisementsResponse), and
 * every Bluetooth integration in Home Assistant then sees them as if a
 * local adapter had received them. Only passive/active scanning, no GATT
 * connections (feature flags say so, Home Assistant does not ask).
 *
 * Scanning only runs while a client is subscribed, with a duty cycle
 * (window/interval) because WiFi and Bluetooth share the radio.
 *
 * Config /mnt/data/toon-os/config/esphome.conf (all optional):
 *   allow=192.168.1.10     the one address that may connect
 *   scan=passive|active    how to scan when Home Assistant does not say
 *   scan_window_ms=40      listening window per interval (BLE scan window)
 *   scan_interval_ms=100   BLE scan interval
 * State in retoon/esphome (the Toon's own MQTT broker, libtoonbus): state=,
 * clients=, scanning=, adverts=.
 *
 * Usage: toon-esphomed [-d]   (-d: log to stderr)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/alternative.h>
#include <toonbus.h>

#define PORT		6053
#define MAX_CLIENTS	4
#define CONFIG_FILE	"/mnt/data/toon-os/config/esphome.conf"
#define STATE_BUS	"esphome"
#define VERSION_FILE	"/etc/retoon-version"
#define API_MAJOR	1
#define API_MINOR	10
/* BluetoothProxyFeature: passive scan, raw advertisements, state+mode */
#define BT_FEATURES	((1 << 0) | (1 << 5) | (1 << 6))
#define ADV_BATCH	16	/* advertisements per response message */
#define ADV_FLUSH_MS	50

/* message ids of the ESPHome native API (api.proto) */
enum {
	M_HELLO_REQ = 1, M_HELLO_RESP = 2, M_AUTH_REQ = 3, M_AUTH_RESP = 4, M_DISCONNECT_REQ = 5, M_DISCONNECT_RESP = 6,
	M_PING_REQ = 7, M_PING_RESP = 8, M_DEVICEINFO_REQ = 9, M_DEVICEINFO_RESP = 10,
	M_LIST_ENTITIES_REQ = 11, M_LIST_ENTITIES_DONE = 19, M_SUBSCRIBE_STATES = 20,
	M_BT_SUBSCRIBE_ADV = 66, M_BT_SUBSCRIBE_FREE = 80, M_BT_CONNECTIONS_FREE = 81,
	M_BT_UNSUBSCRIBE_ADV = 87, M_BT_RAW_ADVS = 93,
	M_BT_SCANNER_STATE = 126, M_BT_SCANNER_SET_MODE = 127,
	M_DEVICE_CAPABILITIES_REQ = 149, M_DEVICE_CAPABILITIES_RESP = 150,
};
enum { SCAN_IDLE = 0, SCAN_STARTING, SCAN_RUNNING, SCAN_FAILED, SCAN_STOPPING, SCAN_STOPPED };
enum { MODE_PASSIVE = 0, MODE_ACTIVE = 1 };

static int debug;
static char dev_name[32], friendly[64], version[32] = "dev";
static char wifi_mac[18], bt_mac[18], wifi_mac_plain[13];
static char allow[64];
static int cfg_mode = MODE_PASSIVE, cfg_window_ms = 40, cfg_interval_ms = 100;

static struct client {
	int fd;
	int hello;		/* HelloRequest seen */
	int subscribed;		/* wants advertisements */
	unsigned char in[4096];
	size_t inlen;
	char peer[48];
} clients[MAX_CLIENTS];
static int nclients;

static int hci_fd = -1;
static int scan_state = SCAN_IDLE, scan_mode = MODE_PASSIVE;
static int scan_want;		/* someone is subscribed */
static int scan_enabled;	/* what the controller was last told */
static unsigned char enable_q[8];	/* values of LE Set Scan Enable commands awaiting their result */
static int enable_qn;
static int scan_fail_logged;
static volatile sig_atomic_t quit;
static time_t hci_retry_at;
static unsigned long adverts_total, adverts_minute, adverts_last_minute;
static time_t minute_at;

/* pending advertisement batch */
static unsigned char advbuf[ADV_BATCH * 96];
static size_t advlen;
static int advcount;
static struct timeval advfirst;

static void logmsg(int prio, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (debug) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else
		vsyslog(prio, fmt, ap);
	va_end(ap);
}
#define info(...)	logmsg(LOG_INFO, __VA_ARGS__)
#define warn(...)	logmsg(LOG_WARNING, __VA_ARGS__)
#define dbg(...)	do { if (debug) logmsg(LOG_DEBUG, __VA_ARGS__); } while (0)

/* ---------- protobuf writer ---------- */
struct pb { unsigned char *p; size_t len, cap; };

static void pb_byte(struct pb *b, unsigned c) { if (b->len < b->cap) b->p[b->len] = c; b->len++; }
static void pb_varint(struct pb *b, uint64_t v)
{
	while (v >= 0x80) { pb_byte(b, (v & 0x7f) | 0x80); v >>= 7; }
	pb_byte(b, v);
}
static void pb_uint(struct pb *b, int field, uint64_t v) { pb_varint(b, field << 3); pb_varint(b, v); }
static void pb_sint(struct pb *b, int field, int32_t v) { pb_uint(b, field, ((uint32_t)v << 1) ^ (uint32_t)(v >> 31)); }
static void pb_bytes(struct pb *b, int field, const void *d, size_t n)
{
	pb_varint(b, (field << 3) | 2); pb_varint(b, n);
	for (size_t i = 0; i < n; i++) pb_byte(b, ((const unsigned char *)d)[i]);
}
static void pb_str(struct pb *b, int field, const char *s) { pb_bytes(b, field, s, strlen(s)); }
static size_t varint_size(uint64_t v) { size_t n = 1; while (v >= 0x80) { v >>= 7; n++; } return n; }

/* ---------- protobuf reader (only what the requests need) ---------- */
struct field { int num, wt; uint64_t v; const unsigned char *data; size_t len; };

static int rd_varint(const unsigned char **p, const unsigned char *end, uint64_t *v)
{
	int shift = 0; *v = 0;
	while (*p < end && shift < 64) {
		unsigned c = *(*p)++;
		*v |= (uint64_t)(c & 0x7f) << shift;
		if (!(c & 0x80)) return 0;
		shift += 7;
	}
	return -1;
}
/* next field; 1 = got one, 0 = end, -1 = malformed */
static int rd_field(const unsigned char **p, const unsigned char *end, struct field *f)
{
	uint64_t key;
	if (*p >= end) return 0;
	if (rd_varint(p, end, &key) < 0) return -1;
	f->num = key >> 3; f->wt = key & 7; f->data = NULL; f->len = 0; f->v = 0;
	switch (f->wt) {
	case 0: return rd_varint(p, end, &f->v) < 0 ? -1 : 1;
	case 1: if (end - *p < 8) return -1; memcpy(&f->v, *p, 8); *p += 8; return 1;
	case 5: if (end - *p < 4) return -1; { uint32_t u; memcpy(&u, *p, 4); f->v = u; } *p += 4; return 1;
	case 2: if (rd_varint(p, end, &f->v) < 0 || (uint64_t)(end - *p) < f->v) return -1;
		f->data = *p; f->len = f->v; *p += f->len; return 1;
	default: return -1;
	}
}

/* ---------- framing ---------- */
static int send_all(int fd, const void *d, size_t n)
{
	const unsigned char *p = d;
	while (n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w < 0) { if (errno == EINTR) continue; return -1; }
		p += w; n -= w;
	}
	return 0;
}
static int send_msg(struct client *c, int type, const unsigned char *payload, size_t len)
{
	unsigned char hdr[16];
	struct pb h = { hdr, 0, sizeof(hdr) };
	pb_byte(&h, 0); pb_varint(&h, len); pb_varint(&h, type);
	if (send_all(c->fd, hdr, h.len) < 0 || send_all(c->fd, payload, len) < 0) return -1;
	return 0;
}

static void close_client(struct client *c, const char *why)
{
	info("client %s left (%s)", c->peer, why);
	close(c->fd);
	*c = clients[--nclients];
	clients[nclients].fd = -1;
}

static void send_scanner_state(struct client *c)
{
	unsigned char buf[16];
	struct pb b = { buf, 0, sizeof(buf) };
	pb_uint(&b, 1, scan_state); pb_uint(&b, 2, scan_mode); pb_uint(&b, 3, cfg_mode);
	send_msg(c, M_BT_SCANNER_STATE, buf, b.len);
}
static void broadcast_scanner_state(void)
{
	for (int i = 0; i < nclients; i++)
		if (clients[i].subscribed) send_scanner_state(&clients[i]);
}

/* ---------- HCI: passive/active LE scan on hci0 ---------- */
static int hci_cmd(uint16_t opcode, const void *param, int plen)
{
	unsigned char pkt[64] = { HCI_COMMAND_PKT, opcode & 0xff, opcode >> 8, plen };
	memcpy(pkt + 4, param, plen);
	return write(hci_fd, pkt, 4 + plen) == 4 + plen ? 0 : -1;
}
static void hci_set_enable(int on)
{
	unsigned char p[2] = { on, 0 };	/* filter duplicates off: every advert */
	if (hci_cmd(cmd_opcode_pack(OGF_LE_CTL, OCF_LE_SET_SCAN_ENABLE), p, 2) == 0 && enable_qn < (int)sizeof(enable_q))
		enable_q[enable_qn++] = on;
	scan_enabled = on;
}
static void hci_set_params(void)
{
	uint16_t interval = cfg_interval_ms * 1000 / 625, window = cfg_window_ms * 1000 / 625;
	if (interval < 4) interval = 4;
	if (interval > 0x4000) interval = 0x4000;
	if (window < 4) window = 4;
	if (window > interval) window = interval;
	unsigned char p[7] = { scan_mode == MODE_ACTIVE, interval & 0xff, interval >> 8,
			       window & 0xff, window >> 8, 0 /* public own address */, 0 };
	hci_cmd(cmd_opcode_pack(OGF_LE_CTL, OCF_LE_SET_SCAN_PARAMETERS), p, 7);
}

static int hci_open(void)
{
	struct sockaddr_hci a = { .hci_family = AF_BLUETOOTH, .hci_dev = 0, .hci_channel = HCI_CHANNEL_RAW };
	struct hci_filter flt;
	int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_HCI);
	if (fd < 0) { warn("hci socket: %m"); return -1; }
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { warn("hci0: %m"); close(fd); return -1; }
	hci_filter_clear(&flt);
	hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
	hci_filter_set_event(EVT_LE_META_EVENT, &flt);
	hci_filter_set_event(EVT_CMD_COMPLETE, &flt);
	hci_filter_set_event(EVT_CMD_STATUS, &flt);
	if (setsockopt(fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) { warn("hci filter: %m"); close(fd); return -1; }
	return fd;
}

static void scan_start(void)
{
	if (hci_fd < 0 && (hci_fd = hci_open()) < 0) {
		scan_state = SCAN_FAILED; hci_retry_at = time(NULL) + 10;
		broadcast_scanner_state();
		return;
	}
	scan_state = SCAN_STARTING;
	/* always stop first: the controller may still scan for an earlier
	 * instance, and parameters only change while it is off */
	hci_set_enable(0);
	hci_set_params();
	hci_set_enable(1);
	/* the command complete event confirms (or fails) it */
}
static void scan_stop(void)
{
	if (hci_fd >= 0 && scan_enabled) hci_set_enable(0);
	scan_state = SCAN_STOPPED;
	advlen = 0; advcount = 0;
}
static void scan_update(void)
{
	int want = 0;
	for (int i = 0; i < nclients; i++) want |= clients[i].subscribed;
	if (want && !scan_want) { scan_want = 1; info("scanning %s (window %d/%d ms)", scan_mode ? "active" : "passive", cfg_window_ms, cfg_interval_ms); scan_start(); }
	else if (!want && scan_want) { scan_want = 0; info("scanning stopped"); scan_stop(); }
}

static void adv_flush(void)
{
	if (!advcount) return;
	for (int i = 0; i < nclients; i++)
		if (clients[i].subscribed && send_msg(&clients[i], M_BT_RAW_ADVS, advbuf, advlen) < 0)
			close_client(&clients[i--], "write failed");
	advlen = 0; advcount = 0;
}
static void adv_add(const unsigned char *addr, int addr_type, const unsigned char *data, int len, int8_t rssi)
{
	unsigned char item[80];
	struct pb b = { item, 0, sizeof(item) };
	uint64_t a = 0;
	for (int i = 5; i >= 0; i--) a = (a << 8) | addr[i];	/* HCI little-endian */
	pb_uint(&b, 1, a);
	pb_sint(&b, 2, rssi);
	if (addr_type) pb_uint(&b, 3, addr_type);
	pb_bytes(&b, 4, data, len);
	if (b.len > sizeof(item)) return;
	size_t need = 1 + varint_size(b.len) + b.len;
	if (advlen + need > sizeof(advbuf)) adv_flush();
	struct pb o = { advbuf + advlen, 0, sizeof(advbuf) - advlen };
	pb_bytes(&o, 1, item, b.len);
	advlen += o.len;
	if (!advcount++) gettimeofday(&advfirst, NULL);
	adverts_total++; adverts_minute++;
	if (advcount >= ADV_BATCH) adv_flush();
}

static void hci_read(void)
{
	unsigned char buf[HCI_MAX_EVENT_SIZE + 1];
	ssize_t n;
	while ((n = read(hci_fd, buf, sizeof(buf))) > 0) {
		if (n < 3 || buf[0] != HCI_EVENT_PKT) continue;
		int ev = buf[1], plen = buf[2];
		const unsigned char *p = buf + 3, *end = buf + 3 + (plen < n - 3 ? plen : n - 3);
		if (ev == EVT_CMD_COMPLETE && end - p >= 4) {
			uint16_t opcode = p[1] | p[2] << 8;
			int status = p[3];
			if (opcode == cmd_opcode_pack(OGF_LE_CTL, OCF_LE_SET_SCAN_ENABLE)) {
				int on = enable_qn ? enable_q[0] : scan_enabled;
				if (enable_qn) { enable_qn--; memmove(enable_q, enable_q + 1, enable_qn); }
				if (!on) continue;	/* a stop; its status does not matter */
				if (status == 0) {
					if (scan_state != SCAN_RUNNING) info("scan running");
					scan_state = SCAN_RUNNING; scan_fail_logged = 0;
				} else {
					/* 0x0c command disallowed: someone else (bluetoothd) scans */
					if (!scan_fail_logged++) warn("LE scan enable failed, status 0x%02x; retrying every 10 s", status);
					scan_enabled = 0; scan_state = SCAN_FAILED; hci_retry_at = time(NULL) + 10;
				}
				broadcast_scanner_state();
			} else if (opcode == cmd_opcode_pack(OGF_LE_CTL, OCF_LE_SET_SCAN_PARAMETERS) && status)
				dbg("LE scan parameters refused, status 0x%02x", status);
			continue;
		}
		if (ev != EVT_LE_META_EVENT || end - p < 2 || p[0] != EVT_LE_ADVERTISING_REPORT) continue;
		int reports = p[1];
		p += 2;
		for (int r = 0; r < reports && end - p >= 9; r++) {
			int type = p[0], atype = p[1];
			const unsigned char *addr = p + 2;
			int len = p[8];
			if (end - p < 9 + len + 1) break;
			const unsigned char *data = p + 9;
			int8_t rssi = (int8_t)p[9 + len];
			p += 10 + len;
			(void)type;	/* ADV_IND..SCAN_RSP all go through; HA merges per address */
			if (!scan_want) continue;
			/* 2/3 = resolved identity addresses: report as public/random */
			adv_add(addr, atype == 1 || atype == 3, data, len, rssi);
		}
	}
	if (n < 0 && errno != EAGAIN && errno != EINTR) {
		warn("hci0 read: %m, reopening");
		close(hci_fd); hci_fd = -1; scan_enabled = 0;
		scan_state = SCAN_FAILED; hci_retry_at = time(NULL) + 5;
	}
}

/* ---------- the API messages ---------- */
static void send_device_info(struct client *c)
{
	unsigned char buf[512];
	struct pb b = { buf, 0, sizeof(buf) };
	pb_str(&b, 2, dev_name);
	pb_str(&b, 3, wifi_mac);
	pb_str(&b, 4, version);		/* esphome_version */
	pb_str(&b, 6, "Toon 2");	/* model */
	pb_str(&b, 8, "ReToon.Toon 2");	/* project_name: manufacturer.model in HA */
	pb_str(&b, 9, version);		/* project_version */
	pb_uint(&b, 10, 443);		/* webserver_port */
	pb_str(&b, 12, "ReToon");	/* manufacturer */
	pb_str(&b, 13, friendly);
	pb_uint(&b, 15, BT_FEATURES);
	pb_str(&b, 18, bt_mac);
	send_msg(c, M_DEVICEINFO_RESP, buf, b.len);
}
static void send_capabilities(struct client *c)
{
	unsigned char sub[32], buf[64];
	struct pb s = { sub, 0, sizeof(sub) }, b = { buf, 0, sizeof(buf) };
	pb_uint(&s, 1, BT_FEATURES); pb_str(&s, 2, bt_mac);
	pb_bytes(&b, 1, sub, s.len);
	send_msg(c, M_DEVICE_CAPABILITIES_RESP, buf, b.len);
}

static int handle_msg(struct client *c, int type, const unsigned char *p, size_t len)
{
	const unsigned char *end = p + len;
	struct field f;
	unsigned char buf[128];
	struct pb b = { buf, 0, sizeof(buf) };

	if (!c->hello && type != M_HELLO_REQ) { warn("%s: message %d before hello", c->peer, type); return -1; }
	switch (type) {
	case M_HELLO_REQ: {
		char who[64] = "?";
		unsigned major = 0, minor = 0;
		while (rd_field(&p, end, &f) > 0) {
			if (f.num == 1 && f.wt == 2) snprintf(who, sizeof(who), "%.*s", (int)(f.len < 63 ? f.len : 63), f.data);
			if (f.num == 2) major = f.v;
			if (f.num == 3) minor = f.v;
		}
		info("client %s: %s (api %u.%u)", c->peer, who, major, minor);
		c->hello = 1;
		pb_uint(&b, 1, API_MAJOR); pb_uint(&b, 2, API_MINOR);
		pb_str(&b, 3, "ReToon toon-esphomed"); pb_str(&b, 4, dev_name);
		return send_msg(c, M_HELLO_RESP, buf, b.len);
	}
	case M_AUTH_REQ:	/* password login of older clients: no password here */
		return send_msg(c, M_AUTH_RESP, NULL, 0);
	case M_DISCONNECT_REQ:
		send_msg(c, M_DISCONNECT_RESP, NULL, 0);
		return -2;
	case M_DISCONNECT_RESP:
		return -2;
	case M_PING_REQ:
		return send_msg(c, M_PING_RESP, NULL, 0);
	case M_PING_RESP:
		return 0;
	case M_DEVICEINFO_REQ:
		send_device_info(c);
		return 0;
	case M_DEVICE_CAPABILITIES_REQ:
		send_capabilities(c);
		return 0;
	case M_LIST_ENTITIES_REQ:
		return send_msg(c, M_LIST_ENTITIES_DONE, NULL, 0);
	case M_BT_SUBSCRIBE_ADV:
		c->subscribed = 1;
		scan_update();
		send_scanner_state(c);
		return 0;
	case M_BT_UNSUBSCRIBE_ADV:
		c->subscribed = 0;
		scan_update();
		return 0;
	case M_BT_SCANNER_SET_MODE: {
		int mode = MODE_PASSIVE;
		while (rd_field(&p, end, &f) > 0) if (f.num == 1) mode = f.v ? MODE_ACTIVE : MODE_PASSIVE;
		if (mode != scan_mode) {
			scan_mode = mode;
			info("scan mode set to %s by %s", mode ? "active" : "passive", c->peer);
			if (scan_want) scan_start();
		}
		send_scanner_state(c);
		return 0;
	}
	case M_BT_SUBSCRIBE_FREE:
		pb_uint(&b, 1, 0); pb_uint(&b, 2, 0);
		return send_msg(c, M_BT_CONNECTIONS_FREE, buf, b.len);
	default:
		dbg("%s: ignoring message %d (%zu bytes)", c->peer, type, len);
		return 0;
	}
}

static void client_read(struct client *c)
{
	ssize_t n = recv(c->fd, c->in + c->inlen, sizeof(c->in) - c->inlen, 0);
	if (n <= 0) { close_client(c, n ? "read error" : "closed"); return; }
	c->inlen += n;
	for (;;) {
		const unsigned char *p = c->in, *end = c->in + c->inlen;
		uint64_t len, type;
		if (c->inlen == 0) return;
		if (p[0] != 0) {
			warn("%s: %s", c->peer, p[0] == 1 ? "wants encryption (noise), not supported: add it in Home Assistant without a key" : "bad frame");
			close_client(c, "protocol");
			return;
		}
		p++;
		if (rd_varint(&p, end, &len) < 0 || rd_varint(&p, end, &type) < 0) {
			if (c->inlen >= 12) { close_client(c, "bad header"); return; }
			return;	/* wait for more */
		}
		if (len > sizeof(c->in) - 12) { close_client(c, "frame too big"); return; }
		if ((size_t)(end - p) < len) return;
		int r = handle_msg(c, type, p, len);
		size_t used = p + len - c->in;
		memmove(c->in, c->in + used, c->inlen - used);
		c->inlen -= used;
		if (r == -2) { close_client(c, "disconnect"); return; }
		if (r < 0) { close_client(c, "error"); return; }
	}
}

static void accept_client(int lfd)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof(sa);
	int fd = accept(lfd, (struct sockaddr *)&sa, &sl);
	if (fd < 0) return;
	char peer[48];
	snprintf(peer, sizeof(peer), "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
	if (allow[0] && strcmp(allow, inet_ntoa(sa.sin_addr))) {
		warn("refused %s (only %s may connect)", peer, allow);
		close(fd); return;
	}
	if (nclients >= MAX_CLIENTS) { warn("refused %s: too many clients", peer); close(fd); return; }
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	struct timeval tv = { 3, 0 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct client *c = &clients[nclients++];
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	snprintf(c->peer, sizeof(c->peer), "%s", peer);
}

/* ---------- Avahi: _esphomelib._tcp ---------- */
static AvahiThreadedPoll *poll_api;
static AvahiEntryGroup *group;
static char *service_name;

static void create_services(AvahiClient *c);
static void group_cb(AvahiEntryGroup *g, AvahiEntryGroupState state, void *ud)
{
	(void)ud;
	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		info("announced as %s (_esphomelib._tcp)", service_name);
		break;
	case AVAHI_ENTRY_GROUP_COLLISION: {
		char *n = avahi_alternative_service_name(service_name);
		avahi_free(service_name); service_name = n;
		warn("mDNS name collision, now %s", n);
		create_services(avahi_entry_group_get_client(g));
		break;
	}
	case AVAHI_ENTRY_GROUP_FAILURE:
		warn("mDNS group failed: %s", avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
		break;
	default: break;
	}
}
static void create_services(AvahiClient *c)
{
	char mac[32], fn[80], ver[48], pv[48];
	if (!group && !(group = avahi_entry_group_new(c, group_cb, NULL))) {
		warn("avahi_entry_group_new: %s", avahi_strerror(avahi_client_errno(c)));
		return;
	}
	if (!avahi_entry_group_is_empty(group)) return;
	snprintf(mac, sizeof(mac), "mac=%s", wifi_mac_plain);
	snprintf(fn, sizeof(fn), "friendly_name=%s", friendly);
	snprintf(ver, sizeof(ver), "version=%s", version);
	snprintf(pv, sizeof(pv), "project_version=%s", version);
	int r = avahi_entry_group_add_service(group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, 0,
			service_name, "_esphomelib._tcp", NULL, NULL, PORT,
			ver, mac, fn, "platform=LINUX", "board=toon2", "network=wifi",
			"project_name=ReToon.Toon 2", pv, NULL);
	if (r == AVAHI_ERR_COLLISION) { group_cb(group, AVAHI_ENTRY_GROUP_COLLISION, NULL); return; }
	if (r < 0) { warn("avahi add service: %s", avahi_strerror(r)); return; }
	if ((r = avahi_entry_group_commit(group)) < 0) warn("avahi commit: %s", avahi_strerror(r));
}
static void client_cb(AvahiClient *c, AvahiClientState state, void *ud)
{
	(void)ud;
	switch (state) {
	case AVAHI_CLIENT_S_RUNNING: create_services(c); break;
	case AVAHI_CLIENT_S_COLLISION:
	case AVAHI_CLIENT_S_REGISTERING: if (group) avahi_entry_group_reset(group); break;
	case AVAHI_CLIENT_FAILURE:
		warn("avahi: %s", avahi_strerror(avahi_client_errno(c)));
		avahi_threaded_poll_quit(poll_api);
		break;
	default: break;
	}
}
static void avahi_start(void)
{
	int err;
	if (!(poll_api = avahi_threaded_poll_new())) { warn("avahi poll failed"); return; }
	service_name = avahi_strdup(dev_name);
	if (!avahi_client_new(avahi_threaded_poll_get(poll_api), AVAHI_CLIENT_NO_FAIL, client_cb, NULL, &err)) {
		warn("avahi client: %s", avahi_strerror(err));
		return;
	}
	avahi_threaded_poll_start(poll_api);
}

/* ---------- config, identity, state ---------- */
static void read_config(void)
{
	FILE *f = fopen(CONFIG_FILE, "r");
	char line[256];
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');
		if (!v) continue;
		*v++ = 0;
		v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(line, "allow")) snprintf(allow, sizeof(allow), "%s", v);
		else if (!strcmp(line, "scan")) cfg_mode = !strcmp(v, "active") ? MODE_ACTIVE : MODE_PASSIVE;
		else if (!strcmp(line, "scan_window_ms")) cfg_window_ms = atoi(v);
		else if (!strcmp(line, "scan_interval_ms")) cfg_interval_ms = atoi(v);
	}
	fclose(f);
	if (cfg_interval_ms < 10 || cfg_interval_ms > 10000) cfg_interval_ms = 100;
	if (cfg_window_ms < 5 || cfg_window_ms > cfg_interval_ms) cfg_window_ms = cfg_interval_ms < 40 ? cfg_interval_ms : 40;
	scan_mode = cfg_mode;
}
static void read_identity(void)
{
	FILE *f;
	char buf[64] = "";
	gethostname(dev_name, sizeof(dev_name) - 1);
	for (char *p = dev_name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
	snprintf(friendly, sizeof(friendly), "ReToon %s", strncmp(dev_name, "toon-", 5) ? dev_name : dev_name + 5);
	if ((f = fopen(VERSION_FILE, "r"))) {
		if (fgets(buf, sizeof(buf), f) && buf[strspn(buf, " \r\n")]) {
			buf[strcspn(buf, "\r\n")] = 0;
			snprintf(version, sizeof(version), "%s", buf);
		}
		fclose(f);
	}
	if ((f = fopen("/sys/class/net/wlan0/address", "r"))) {
		if (fgets(buf, sizeof(buf), f)) {
			buf[strcspn(buf, "\r\n")] = 0;
			for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'f') *p -= 32;
			snprintf(wifi_mac, sizeof(wifi_mac), "%s", buf);
		}
		fclose(f);
	}
	if (!wifi_mac[0]) snprintf(wifi_mac, sizeof(wifi_mac), "00:00:00:00:00:00");
	for (char *s = wifi_mac, *d = wifi_mac_plain; *s && d < wifi_mac_plain + 12; s++)
		if (*s != ':') *d++ = *s >= 'A' && *s <= 'F' ? *s + 32 : *s;
	/* Bluetooth address: ask the controller */
	int s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	struct hci_dev_info di = { .dev_id = 0 };
	if (s >= 0 && ioctl(s, HCIGETDEVINFO, &di) == 0)
		snprintf(bt_mac, sizeof(bt_mac), "%02X:%02X:%02X:%02X:%02X:%02X", di.bdaddr.b[5], di.bdaddr.b[4],
			 di.bdaddr.b[3], di.bdaddr.b[2], di.bdaddr.b[1], di.bdaddr.b[0]);
	else
		snprintf(bt_mac, sizeof(bt_mac), "%s", wifi_mac);
	if (s >= 0) close(s);
}
static void write_state(void)
{
	char *out = NULL;
	size_t out_len = 0;
	FILE *f = open_memstream(&out, &out_len);
	int subs = 0;
	if (!f) return;
	for (int i = 0; i < nclients; i++) subs += clients[i].subscribed;
	fprintf(f, "state=%s\nclients=%d\n", nclients ? "connected" : "listening", nclients);
	for (int i = 0; i < nclients; i++) fprintf(f, "client=%s\n", clients[i].peer);
	fprintf(f, "scanning=%s\nmode=%s\nadverts_per_minute=%lu\nadverts=%lu\n",
		!scan_want ? "off" : scan_state == SCAN_RUNNING ? "on" : scan_state == SCAN_FAILED ? "failed" : "starting",
		scan_mode ? "active" : "passive", adverts_last_minute, adverts_total);
	fclose(f);
	bus_put(STATE_BUS, out);
	free(out);
}

static void on_term(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv)
{
	int lfd, one = 1;
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = INADDR_ANY };
	time_t last_state = 0;

	if (argc > 1 && !strcmp(argv[1], "-d")) debug = 1;
	if (!debug) openlog("toon-esphomed", LOG_PID, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	bus_open("esphomed");
	read_config();
	read_identity();
	info("%s: %s, WiFi %s, Bluetooth %s, ReToon %s", dev_name, friendly, wifi_mac, bt_mac, version);

	if ((lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) { warn("socket: %m"); return 1; }
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 4) < 0) { warn("port %d: %m", PORT); return 1; }
	for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
	avahi_start();
	minute_at = time(NULL);
	write_state();

	for (;;) {
		fd_set rfds;
		struct timeval tv = { 1, 0 };
		int maxfd = lfd;
		FD_ZERO(&rfds);
		FD_SET(lfd, &rfds);
		for (int i = 0; i < nclients; i++) { FD_SET(clients[i].fd, &rfds); if (clients[i].fd > maxfd) maxfd = clients[i].fd; }
		if (hci_fd >= 0) { FD_SET(hci_fd, &rfds); if (hci_fd > maxfd) maxfd = hci_fd; }
		if (advcount) {
			struct timeval now, d;
			gettimeofday(&now, NULL);
			timersub(&now, &advfirst, &d);
			long left = ADV_FLUSH_MS - (d.tv_sec * 1000 + d.tv_usec / 1000);
			tv.tv_sec = 0; tv.tv_usec = left > 0 ? left * 1000 : 0;
		}
		if (quit) break;
		if (select(maxfd + 1, &rfds, NULL, NULL, &tv) < 0) { if (errno == EINTR) continue; warn("select: %m"); return 1; }
		if (FD_ISSET(lfd, &rfds)) accept_client(lfd);
		if (hci_fd >= 0 && FD_ISSET(hci_fd, &rfds)) hci_read();
		for (int i = 0; i < nclients; i++)
			if (clients[i].fd >= 0 && FD_ISSET(clients[i].fd, &rfds)) { int n = nclients; client_read(&clients[i]); if (nclients < n) i--; }
		scan_update();
		if (advcount) {
			struct timeval now, d;
			gettimeofday(&now, NULL);
			timersub(&now, &advfirst, &d);
			if (d.tv_sec * 1000 + d.tv_usec / 1000 >= ADV_FLUSH_MS) adv_flush();
		}
		time_t now = time(NULL);
		if (scan_want && scan_state == SCAN_FAILED && now >= hci_retry_at) scan_start();
		if (now - minute_at >= 60) { adverts_last_minute = adverts_minute; adverts_minute = 0; minute_at = now; }
		if (now - last_state >= 5) { write_state(); last_state = now; }
	}
	/* leave the controller quiet and the clients informed */
	if (scan_want) scan_stop();
	usleep(100000);
	for (int i = 0; i < nclients; i++) { send_msg(&clients[i], M_DISCONNECT_REQ, NULL, 0); close(clients[i].fd); }
	bus_put(STATE_BUS, NULL);
	bus_close();
	return 0;
}
