// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gp711-rcpd: exposes the Toon 2's GP711 radio as an OpenThread-style radio
 * co-processor (Spinel over HDLC) on a TCP port, for host Zigbee stacks such
 * as zigbee2mqtt's "zoh" adapter (Zigbee on Host).
 *
 *   gp711-rcpd [-v] [port]            default port 6638
 *   gp711-rcpd [-v] -t <path>         a pseudo-terminal at <path> instead, for
 *                                     a host on the Toon itself (otbr-agent:
 *                                     spinel+hdlc+uart://<path>)
 *
 * The Spinel subset that Zigbee on Host and OpenThread's RCP host (ot-cli,
 * otbr-agent) use is implemented. The chip does auto-ACK, CSMA and retries
 * itself, and sets frame pending for the children in the source-match
 * tables; MAC security is left to the host. Not done:
 * energy scan (answers UNIMPLEMENTED), TX power levels (value is stored, the
 * radio stays at its "0 dBm" setting), CCA threshold, and the
 * RSSI figure is an uncalibrated estimate.
 * Frame-pending handling for sleepy end devices is whatever the chip does by
 * default (untested).
 */

#define _GNU_SOURCE		/* posix_openpt, ptsname */
#define GP711_NO_MAIN
#include "gp711.c"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/socket.h>
#include <termios.h>

#define SPINEL_HEADER_FLAG	0x80
#define SPINEL_TID_MASK		0x0f

enum {
	CMD_NOOP = 0,
	CMD_RESET = 1,
	CMD_PROP_VALUE_GET = 2,
	CMD_PROP_VALUE_SET = 3,
	CMD_PROP_VALUE_INSERT = 4,
	CMD_PROP_VALUE_REMOVE = 5,
	CMD_PROP_VALUE_IS = 6,
	CMD_PROP_VALUE_INSERTED = 7,
	CMD_PROP_VALUE_REMOVED = 8,
};

enum {
	PROP_LAST_STATUS = 0x00,
	PROP_PROTOCOL_VERSION = 0x01,
	PROP_NCP_VERSION = 0x02,
	PROP_INTERFACE_TYPE = 0x03,
	PROP_CAPS = 0x05,
	PROP_HWADDR = 0x08,
	PROP_PHY_ENABLED = 0x20,
	PROP_PHY_CHAN = 0x21,
	PROP_PHY_CHAN_SUPPORTED = 0x22,
	PROP_PHY_CHAN_PREFERRED = 0x29,
	PROP_PHY_FEM_LNA_GAIN = 0x2a,
	PROP_PHY_CCA_THRESHOLD = 0x24,
	PROP_PHY_TX_POWER = 0x25,
	PROP_PHY_RSSI = 0x26,
	PROP_PHY_RX_SENSITIVITY = 0x27,
	PROP_MAC_SCAN_STATE = 0x30,
	PROP_MAC_SCAN_MASK = 0x31,
	PROP_MAC_SCAN_PERIOD = 0x32,
	PROP_MAC_15_4_LADDR = 0x34,
	PROP_MAC_15_4_SADDR = 0x35,
	PROP_MAC_15_4_PANID = 0x36,
	PROP_MAC_RAW_STREAM_ENABLED = 0x37,
	PROP_MAC_PROMISCUOUS_MODE = 0x38,
	PROP_MAC_RX_ON_WHEN_IDLE_MODE = 0x3b,
	PROP_MAC_15_4_ALT_SADDR = 0x3c,
	PROP_STREAM_RAW = 0x71,
	PROP_RCP_API_VERSION = 0xb0,
	PROP_RCP_MIN_HOST_API_VERSION = 0xb1,
	PROP_RCP_MAC_KEY = 0x800,
	PROP_RCP_MAC_FRAME_COUNTER = 0x801,
	PROP_RCP_TIMESTAMP = 0x802,
	PROP_RCP_ENH_ACK_PROBING = 0x803,
	PROP_RADIO_CAPS = 0x120b,
	PROP_RADIO_COEX_ENABLE = 0x120d,
	PROP_MAC_SRC_MATCH_ENABLED = 0x1303,
	PROP_MAC_SRC_MATCH_SHORT = 0x1304,
	PROP_MAC_SRC_MATCH_EXTENDED = 0x1305,
};

/* SPINEL_CAP_*: radio co-processor, raw MAC, both API version properties */
static const uint16_t caps[] = { 34, 513, 64, 65 };

/* OT_RADIO_CAPS_*: ACK timeout, retries, CSMA backoff (all in the chip),
   rx-on-when-idle */
#define RADIO_CAPS	(1 << 0 | 1 << 2 | 1 << 3 | 1 << 8)

#define SRC_MATCH_MAX	32

enum {
	STATUS_OK = 0,
	STATUS_FAILURE = 1,
	STATUS_UNIMPLEMENTED = 2,
	STATUS_INVALID_ARGUMENT = 3,
	STATUS_PROP_NOT_FOUND = 13,
	STATUS_NO_ACK = 17,
	STATUS_CCA_FAILURE = 18,
	STATUS_RESET_SOFTWARE = 114,
};

#define TX_STATUS_OK		0x00
#define TX_STATUS_NO_ACK	0xe9

static struct {
	int phy_enabled, raw_enabled, rx_on_idle, promiscuous;
	int channel;
	int8_t tx_power, cca_threshold, last_rssi;
	uint16_t short_addr, pan_id;
	uint8_t ext_addr[8];		/* as Spinel carries it, most significant first */
	uint8_t eui[8];			/* the factory address, same order */
	int receiving;
	/* source-match tables: the children that have data waiting get the
	   frame-pending bit in the ACK to their data request (the chip's own
	   tables hold 12 short and 4 extended addresses) */
	int dp;				/* the chip's data pending runs */
	int src_match;
	int n_short, n_ext;
	uint16_t src_short[SRC_MATCH_MAX];
	uint8_t src_ext[SRC_MATCH_MAX][8];
} st;

static int client = -1;
static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static void logv(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---- HDLC-lite framing ---------------------------------------------------- */

static uint16_t hdlc_fcs(uint16_t fcs, uint8_t b)
{
	int i;

	fcs ^= b;
	for (i = 0; i < 8; i++)
		fcs = fcs & 1 ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
	return fcs;
}

static int hdlc_put(uint8_t *out, int n, uint8_t b)
{
	if (b == 0x7e || b == 0x7d || b == 0x11 || b == 0x13 || b == 0xf8) {
		out[n++] = 0x7d;
		b ^= 0x20;
	}
	out[n++] = b;
	return n;
}

static void hdlc_send(const uint8_t *data, int len)
{
	uint8_t out[2 * 320 + 8];
	uint16_t fcs = 0xffff;
	int i, n = 0, off = 0;

	if (client < 0 || len > 318)
		return;
	out[n++] = 0x7e;
	for (i = 0; i < len; i++) {
		fcs = hdlc_fcs(fcs, data[i]);
		n = hdlc_put(out, n, data[i]);
	}
	fcs ^= 0xffff;
	n = hdlc_put(out, n, fcs & 0xff);
	n = hdlc_put(out, n, fcs >> 8);
	out[n++] = 0x7e;

	while (off < n) {
		ssize_t w = write(client, out + off, n - off);

		if (w <= 0) {
			if (w < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			close(client);
			client = -1;
			return;
		}
		off += w;
	}
}

/* ---- Spinel helpers -------------------------------------------------------- */

static int put_uint(uint8_t *p, unsigned int v)
{
	int n = 0;

	while (v >= 0x80) {
		p[n++] = (v & 0x7f) | 0x80;
		v >>= 7;
	}
	p[n++] = v;
	return n;
}

static int get_uint(const uint8_t *p, int len, unsigned int *v)
{
	int n = 0, shift = 0;

	*v = 0;
	while (n < len) {
		*v |= (unsigned int)(p[n] & 0x7f) << shift;
		shift += 7;
		if (!(p[n++] & 0x80))
			return n;
	}
	return 0;
}

/* Send PROP_VALUE_IS for one property with a raw value. */
static void send_prop(uint8_t tid, unsigned int prop, const uint8_t *val, int len)
{
	uint8_t buf[320];
	int n = 0;

	buf[n++] = SPINEL_HEADER_FLAG | tid;
	buf[n++] = CMD_PROP_VALUE_IS;
	n += put_uint(buf + n, prop);
	memcpy(buf + n, val, len);
	hdlc_send(buf, n + len);
}

static void send_status(uint8_t tid, unsigned int status)
{
	uint8_t v[4];

	send_prop(tid, PROP_LAST_STATUS, v, put_uint(v, status));
}

static void send_u8(uint8_t tid, unsigned int prop, uint8_t v)
{
	send_prop(tid, prop, &v, 1);
}

static void send_u16(uint8_t tid, unsigned int prop, uint16_t v)
{
	uint8_t b[] = { v & 0xff, v >> 8 };

	send_prop(tid, prop, b, sizeof(b));
}

/* ---- radio glue ------------------------------------------------------------- */

/* 802.15.4 FCS (CRC-16/KERMIT); the radio strips it, the host expects it. */
static uint16_t mac_fcs(const uint8_t *d, int len)
{
	uint16_t crc = 0;
	int i, j;

	for (i = 0; i < len; i++) {
		crc ^= d[i];
		for (j = 0; j < 8; j++)
			crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}

/*
 * Uncalibrated estimate from the per-frame quality byte (PBM+0a), which rises
 * with signal strength (0x22 for faint frames, 0x65 for a nearby router). The
 * byte at PBM+04 is constant per session and is not a signal level.
 */
static int8_t quality_to_rssi(int quality)
{
	int rssi = quality / 2 - 100;

	return rssi > 0 ? 0 : rssi < -127 ? -127 : rssi;
}

static void apply_rx(void)
{
	int want = st.phy_enabled && st.channel && (st.raw_enabled || st.rx_on_idle);

	if (want) {
		radio_rx_enable(st.channel);
		st.receiving = 1;
		logv("rx on, channel %d", st.channel);
	} else if (st.receiving) {
		radio_rx_disable();
		st.receiving = 0;
		logv("rx off");
	}
}

static void radio_restart(void)
{
	uint8_t eui[8];
	int i;

	memset(&st, 0, sizeof(st));
	st.cca_threshold = -75;
	st.last_rssi = -127;
	st.short_addr = 0xfffe;
	st.pan_id = 0xffff;
	current_channel = 0;
	tx_status = -1;

	radio_init();
	radio_set_promiscuous(0);
	radio_rx_disable();
	rd_stream(REG_EXT_ADDR, eui, 8);
	for (i = 0; i < 8; i++)
		st.ext_addr[i] = eui[7 - i];
	memcpy(st.eui, st.ext_addr, 8);
	st.dp = dp_start();
	if (!st.dp)
		fprintf(stderr, "gp711-rcpd: data pending did not start, no frame pending for sleepy devices\n");
}

/* Put the source-match tables into the chip (extended addresses as on air). */
static void dp_update(void)
{
	uint8_t ext[DP_MAX_EXT][8];
	int i, j, ns = st.src_match ? st.n_short : 0, ne = st.src_match ? st.n_ext : 0;

	if (!st.dp)
		return;
	if (ns > DP_MAX_SHORT || ne > DP_MAX_EXT)
		logv("src match: %d short, %d extended, the chip holds %d and %d",
		     ns, ne, DP_MAX_SHORT, DP_MAX_EXT);
	for (i = 0; i < ne && i < DP_MAX_EXT; i++)
		for (j = 0; j < 8; j++)
			ext[i][j] = st.src_ext[i][7 - j];
	if (!dp_set(st.src_short, ns, (const uint8_t (*)[8])ext, ne))
		logv("src match: the chip did not take the tables");
}

/*
 * The source of a MAC data request that asks for an ACK, as on air: returns
 * its length (2 short, 8 extended), or 0 for any other frame. The command
 * identifier is outside the encrypted part, also in secured frames.
 */
static int data_request_source(const uint8_t *f, int len, const uint8_t **src)
{
	static const int key_id_len[] = { 0, 1, 5, 9 };
	int fcf, dmode, smode, dpan, span, comp, n = 3;

	if (len < 3)
		return 0;
	fcf = f[0] | f[1] << 8;
	if ((fcf & 0x07) != 3 || !(fcf & 0x20) || (fcf & 0x200))
		return 0;			/* not a command, no ACK, IEs */
	dmode = fcf >> 10 & 3;
	smode = fcf >> 14 & 3;
	comp = fcf & 0x40;
	if (smode < 2)
		return 0;
	if ((fcf >> 12 & 3) < 2) {		/* 2003/2006 */
		dpan = dmode != 0;
		span = !comp;
	} else if (!dmode) {			/* 2015, the cases with a source */
		dpan = 0;
		span = !comp;
	} else if (dmode == 3 && smode == 3) {
		dpan = !comp;
		span = 0;
	} else {
		dpan = 1;
		span = !comp;
	}
	n += (dpan ? 2 : 0) + (dmode == 3 ? 8 : dmode == 2 ? 2 : 0) + (span ? 2 : 0);
	*src = f + n;
	n += smode == 3 ? 8 : 2;
	if (fcf & 0x08) {			/* auxiliary security header */
		if (n >= len)
			return 0;
		n += 1 + (f[n] & 0x20 ? 0 : 4) + key_id_len[f[n] >> 3 & 3];
	}
	if (n >= len || f[n] != 0x04)
		return 0;
	return smode == 3 ? 8 : 2;
}

/* Did the chip set frame pending in its ACK to this frame? */
static int acked_with_pending(const uint8_t *frame, int len)
{
	const uint8_t *src;
	int i, j, n = st.dp && st.src_match ? data_request_source(frame, len, &src) : 0;

	for (i = 0; n == 2 && i < st.n_short && i < DP_MAX_SHORT; i++)
		if (src[0] == (st.src_short[i] & 0xff) && src[1] == st.src_short[i] >> 8)
			return 1;
	for (i = 0; n == 8 && i < st.n_ext && i < DP_MAX_EXT; i++) {
		for (j = 0; j < 8 && src[j] == st.src_ext[i][7 - j]; j++)
			;
		if (j == 8)
			return 1;
	}
	return 0;
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static int put_u64(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = v >> (8 * i);
	return 8;
}

static void forward_frame(const uint8_t *frame, int len, int quality, int level)
{
	uint8_t v[160];
	uint16_t fcs = mac_fcs(frame, len);
	int n = 0;

	(void)level;
	st.last_rssi = quality_to_rssi(quality);
	if (!st.raw_enabled)
		return;

	v[n++] = (len + 2) & 0xff;
	v[n++] = (len + 2) >> 8;
	memcpy(v + n, frame, len);
	n += len;
	v[n++] = fcs & 0xff;
	v[n++] = fcs >> 8;
	v[n++] = st.last_rssi;		/* RSSI */
	v[n++] = 0x80;			/* noise floor: unknown */
	v[n++] = acked_with_pending(frame, len) ? 0x10 : 0;	/* flags: acked with FP */
	v[n++] = 0;
	v[n++] = 10;			/* PHY data: channel, LQI, timestamp */
	v[n++] = 0;
	v[n++] = st.channel;
	v[n++] = quality;
	n += put_u64(v + n, now_us());
	v[n++] = 1;			/* Spinel vendor data: receive error */
	v[n++] = 0;
	v[n++] = 0;
	send_prop(0, PROP_STREAM_RAW, v, n);
}

static void service_radio(void)
{
	uint8_t frame[128];
	int quality = 0, level = 0, len;

	while (irq_pending()) {
		len = radio_service(frame, &quality, &level);
		if (len)
			forward_frame(frame, len, quality, level);
	}
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/*
 * The transmit result: status, frame pending, header updated and, when the
 * frame was acknowledged, the ACK as a received frame. The chip does not
 * hand over the ACK, so it is rebuilt (an immediate ACK, frame pending
 * unknown, so clear). Zigbee on Host reads only the status.
 */
static void send_tx_done(uint8_t tid, unsigned int status, const uint8_t *frame)
{
	uint8_t v[48];
	uint8_t ack[3] = { 0x02, 0x00, frame[2] };
	uint16_t fcs = mac_fcs(ack, 3);
	int n = put_uint(v, status);

	v[n++] = 0;			/* frame pending */
	v[n++] = 0;			/* header updated */
	if (status == STATUS_OK && (frame[0] & 0x20)) {
		v[n++] = 5;
		v[n++] = 0;
		memcpy(v + n, ack, 3);
		n += 3;
		v[n++] = fcs & 0xff;
		v[n++] = fcs >> 8;
		v[n++] = st.last_rssi;
		v[n++] = 0x80;		/* noise floor: unknown */
		v[n++] = 0;		/* flags */
		v[n++] = 0;
		v[n++] = 10;		/* PHY data: channel, LQI, timestamp */
		v[n++] = 0;
		v[n++] = st.channel;
		v[n++] = 0;
		memset(v + n, 0, 8);
		n += 8;
		v[n++] = 1;		/* Spinel vendor data: receive error */
		v[n++] = 0;
		v[n++] = 0;
	}
	send_prop(tid, PROP_LAST_STATUS, v, n);
}

static void transmit(uint8_t tid, const uint8_t *p, int len)
{
	unsigned int flen;
	long long end;
	int channel, tries, cca_tries, ack;

	if (len < 5)
		return send_status(tid, STATUS_INVALID_ARGUMENT);
	flen = p[0] | p[1] << 8;
	if (flen < 3 || flen > 127 || (int)flen + 2 > len)
		return send_status(tid, STATUS_INVALID_ARGUMENT);
	channel = (int)flen + 2 < len ? p[2 + flen] : st.channel;
	if (channel < 11 || channel > 26 || !st.phy_enabled)
		return send_status(tid, STATUS_INVALID_ARGUMENT);

	/*
	 * Retries as the host asks (OpenThread sends max CSMA backoffs and
	 * max frame retries after the channel; Zigbee on Host does not, then
	 * 4 and 3): the chip sends a frame once per start.
	 */
	cca_tries = 1 + ((int)flen + 3 < len ? p[3 + flen] : 4);
	tries = 1 + ((int)flen + 4 < len ? p[4 + flen] : 3);
	ack = p[2] & 0x20;
	for (;;) {
		/* the host includes two FCS bytes, the radio adds its own */
		tx_status = -1;
		radio_tx_start(channel, p + 2, flen - 2);
		end = now_ms() + 1000;
		while (tx_status < 0 && now_ms() < end) {
			if (irq_pending())
				service_radio();
			else
				usleep(200);
		}
		logv("tx %u bytes ch %d fcf %02x%02x -> %02x", flen - 2, channel, p[3], p[2], tx_status & 0xff);
		if (tx_status == TX_STATUS_OK || tx_status < 0)
			break;
		if (tx_status == TX_STATUS_NO_ACK ? !ack || --tries <= 0 : --cca_tries <= 0)
			break;
		usleep(320 * (rand() % 8));	/* a few backoff periods */
	}

	if (tx_status == TX_STATUS_OK)
		send_tx_done(tid, STATUS_OK, p + 2);
	else if (tx_status == TX_STATUS_NO_ACK)
		send_tx_done(tid, STATUS_NO_ACK, p + 2);
	else if (tx_status < 0)
		send_tx_done(tid, STATUS_FAILURE, p + 2);
	else
		send_tx_done(tid, STATUS_CCA_FAILURE, p + 2);
}

/* PROP_VALUE_INSERT / REMOVE on the source-match tables */
static void src_match_change(uint8_t tid, int insert, unsigned int prop, const uint8_t *p, int len)
{
	int i, size = prop == PROP_MAC_SRC_MATCH_SHORT ? 2 : 8;
	int *count = size == 2 ? &st.n_short : &st.n_ext;
	uint8_t buf[16];
	int n = 0;

	if (len < size)
		return send_status(tid, STATUS_INVALID_ARGUMENT);
	for (i = 0; i < *count; i++)
		if (!memcmp(size == 2 ? (void *)&st.src_short[i] : st.src_ext[i], p, size))
			break;
	if (insert && i == *count) {
		if (*count == SRC_MATCH_MAX)
			return send_status(tid, STATUS_FAILURE);
		memcpy(size == 2 ? (void *)&st.src_short[i] : st.src_ext[i], p, size);
		(*count)++;
	} else if (!insert && i < *count) {
		(*count)--;
		if (size == 2)
			st.src_short[i] = st.src_short[*count];
		else
			memcpy(st.src_ext[i], st.src_ext[*count], 8);
	}
	dp_update();
	buf[n++] = SPINEL_HEADER_FLAG | tid;
	buf[n++] = insert ? CMD_PROP_VALUE_INSERTED : CMD_PROP_VALUE_REMOVED;
	n += put_uint(buf + n, prop);
	memcpy(buf + n, p, size);
	hdlc_send(buf, n + size);
}

/* ---- Spinel commands --------------------------------------------------------- */

static void prop_get(uint8_t tid, unsigned int prop)
{
	static const char version[] = "GP711-TOON2/0.1; GP711; " __DATE__ " " __TIME__;
	uint8_t v[16];
	int i, n;

	switch (prop) {
	case PROP_CAPS:
		for (i = n = 0; i < (int)ARRAY_SIZE(caps); i++)
			n += put_uint(v + n, caps[i]);
		return send_prop(tid, prop, v, n);
	case PROP_HWADDR:
		return send_prop(tid, prop, st.eui, 8);
	case PROP_RCP_TIMESTAMP:
		return send_prop(tid, prop, v, put_u64(v, now_us()));
	case PROP_RADIO_CAPS:
		return send_prop(tid, prop, v, put_uint(v, RADIO_CAPS));
	case PROP_PHY_CHAN_SUPPORTED:
	case PROP_PHY_CHAN_PREFERRED:
		for (i = 0; i < 16; i++)
			v[i] = 11 + i;
		return send_prop(tid, prop, v, 16);
	case PROP_MAC_SRC_MATCH_ENABLED:
		return send_u8(tid, prop, st.src_match);
	case PROP_PROTOCOL_VERSION:
		v[0] = 4;
		v[1] = 3;
		return send_prop(tid, prop, v, 2);
	case PROP_NCP_VERSION:
		return send_prop(tid, prop, (const uint8_t *)version, sizeof(version));
	case PROP_INTERFACE_TYPE:
		return send_u8(tid, prop, 3);
	case PROP_RCP_API_VERSION:
		return send_u8(tid, prop, 10);
	case PROP_RCP_MIN_HOST_API_VERSION:
		return send_u8(tid, prop, 4);
	case PROP_PHY_ENABLED:
		return send_u8(tid, prop, st.phy_enabled);
	case PROP_PHY_CHAN:
		return send_u8(tid, prop, st.channel);
	case PROP_PHY_CCA_THRESHOLD:
		return send_u8(tid, prop, st.cca_threshold);
	case PROP_PHY_TX_POWER:
		return send_u8(tid, prop, st.tx_power);
	case PROP_PHY_RSSI:
		return send_u8(tid, prop, st.last_rssi);
	case PROP_PHY_RX_SENSITIVITY:
		return send_u8(tid, prop, (uint8_t)-100);
	case PROP_MAC_15_4_LADDR:
		return send_prop(tid, prop, st.ext_addr, 8);
	case PROP_MAC_15_4_SADDR:
		return send_u16(tid, prop, st.short_addr);
	case PROP_MAC_15_4_PANID:
		return send_u16(tid, prop, st.pan_id);
	case PROP_MAC_RAW_STREAM_ENABLED:
		return send_u8(tid, prop, st.raw_enabled);
	case PROP_MAC_PROMISCUOUS_MODE:
		return send_u8(tid, prop, st.promiscuous);
	case PROP_MAC_RX_ON_WHEN_IDLE_MODE:
		return send_u8(tid, prop, st.rx_on_idle);
	case PROP_MAC_SCAN_STATE:
		return send_u8(tid, prop, 0);
	}
	logv("get: unknown property 0x%x", prop);
	send_status(tid, STATUS_PROP_NOT_FOUND);
}

static void prop_set(uint8_t tid, unsigned int prop, const uint8_t *p, int len)
{
	uint8_t eui[8];
	int i;

	if (prop == PROP_STREAM_RAW)
		return transmit(tid, p, len);
	if (prop == PROP_MAC_SRC_MATCH_SHORT || prop == PROP_MAC_SRC_MATCH_EXTENDED) {
		/* a set replaces the table; OpenThread only clears it this way */
		if (prop == PROP_MAC_SRC_MATCH_SHORT)
			st.n_short = 0;
		else
			st.n_ext = 0;
		dp_update();
		return send_prop(tid, prop, p, 0);
	}
	if (len < 1)
		return send_status(tid, STATUS_INVALID_ARGUMENT);

	switch (prop) {
	case PROP_PHY_ENABLED:
		st.phy_enabled = p[0] != 0;
		apply_rx();
		break;
	case PROP_PHY_CHAN:
		if (p[0] < 11 || p[0] > 26)
			return send_status(tid, STATUS_INVALID_ARGUMENT);
		st.channel = p[0];
		apply_rx();
		break;
	case PROP_MAC_RAW_STREAM_ENABLED:
		st.raw_enabled = p[0] != 0;
		apply_rx();
		break;
	case PROP_MAC_RX_ON_WHEN_IDLE_MODE:
		st.rx_on_idle = p[0] != 0;
		apply_rx();
		break;
	case PROP_MAC_PROMISCUOUS_MODE:
		st.promiscuous = p[0];
		radio_set_promiscuous(p[0] != 0);
		break;
	case PROP_MAC_15_4_LADDR:
		if (len < 8)
			return send_status(tid, STATUS_INVALID_ARGUMENT);
		memcpy(st.ext_addr, p, 8);
		for (i = 0; i < 8; i++)
			eui[i] = p[7 - i];
		radio_set_ext_addr(eui);
		return send_prop(tid, prop, st.ext_addr, 8);
	case PROP_MAC_15_4_SADDR:
	case PROP_MAC_15_4_PANID:
		if (len < 2)
			return send_status(tid, STATUS_INVALID_ARGUMENT);
		if (prop == PROP_MAC_15_4_SADDR) {
			st.short_addr = p[0] | p[1] << 8;
			radio_set_short_addr(st.short_addr);
		} else {
			st.pan_id = p[0] | p[1] << 8;
			radio_set_pan_id(st.pan_id);
		}
		return send_prop(tid, prop, p, 2);
	case PROP_PHY_TX_POWER:
		st.tx_power = p[0];	/* stored only, see the header comment */
		break;
	case PROP_PHY_CCA_THRESHOLD:
		st.cca_threshold = p[0];
		break;
	case PROP_MAC_SCAN_MASK:
	case PROP_MAC_SCAN_PERIOD:
		return send_prop(tid, prop, p, len);
	case PROP_MAC_SCAN_STATE:
		if (p[0] == 0)
			break;
		return send_status(tid, STATUS_UNIMPLEMENTED);
	case PROP_MAC_SRC_MATCH_ENABLED:
		st.src_match = p[0] != 0;
		dp_update();
		break;
	/*
	 * Accepted and not used: MAC security is done by the host (no
	 * tx-security capability), and the rest has no counterpart here.
	 */
	case PROP_RCP_MAC_KEY:
	case PROP_RCP_MAC_FRAME_COUNTER:
	case PROP_RCP_ENH_ACK_PROBING:
	case PROP_MAC_15_4_ALT_SADDR:
	case PROP_PHY_FEM_LNA_GAIN:
	case PROP_RADIO_COEX_ENABLE:
		return send_prop(tid, prop, p, len);
	default:
		logv("set: unknown property 0x%x", prop);
		return send_status(tid, STATUS_PROP_NOT_FOUND);
	}
	send_u8(tid, prop, p[0]);
}

static void handle_frame(const uint8_t *f, int len)
{
	unsigned int cmd, prop;
	uint8_t tid;
	int n, m;

	if (len < 2 || (f[0] & 0xc0) != SPINEL_HEADER_FLAG)
		return;
	tid = f[0] & SPINEL_TID_MASK;
	n = get_uint(f + 1, len - 1, &cmd);
	if (!n)
		return;
	n += 1;

	switch (cmd) {
	case CMD_NOOP:
		return send_status(tid, STATUS_OK);
	case CMD_RESET:
		logv("reset");
		radio_restart();
		return send_status(0, STATUS_RESET_SOFTWARE);
	case CMD_PROP_VALUE_GET:
	case CMD_PROP_VALUE_SET:
	case CMD_PROP_VALUE_INSERT:
	case CMD_PROP_VALUE_REMOVE:
		m = get_uint(f + n, len - n, &prop);
		if (!m)
			return send_status(tid, STATUS_INVALID_ARGUMENT);
		n += m;
		if (cmd == CMD_PROP_VALUE_GET)
			prop_get(tid, prop);
		else if (cmd == CMD_PROP_VALUE_SET)
			prop_set(tid, prop, f + n, len - n);
		else if (prop == PROP_MAC_SRC_MATCH_SHORT || prop == PROP_MAC_SRC_MATCH_EXTENDED)
			src_match_change(tid, cmd == CMD_PROP_VALUE_INSERT, prop, f + n, len - n);
		else
			send_status(tid, STATUS_PROP_NOT_FOUND);
		return;
	}
	logv("unknown command %u", cmd);
	send_status(tid, STATUS_FAILURE);
}

/* ---- byte stream from the host -------------------------------------------------- */

static void feed(const uint8_t *in, int count)
{
	static uint8_t frame[512];
	static int flen, escaped;
	uint16_t fcs;
	int i, j;

	for (i = 0; i < count; i++) {
		uint8_t b = in[i];

		if (b == 0x7e) {
			if (flen >= 4) {
				fcs = 0xffff;
				for (j = 0; j < flen; j++)
					fcs = hdlc_fcs(fcs, frame[j]);
				if (fcs == 0xf0b8)
					handle_frame(frame, flen - 2);
				else
					logv("hdlc: bad fcs, %d bytes dropped", flen);
			}
			flen = 0;
			escaped = 0;
			continue;
		}
		if (b == 0x7d) {
			escaped = 1;
			continue;
		}
		if (escaped) {
			b ^= 0x20;
			escaped = 0;
		}
		if (flen < (int)sizeof(frame))
			frame[flen++] = b;
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	int lfd = -1, one = 1, port = 6638, a = 1;
	const char *pty = NULL;

	if (a < argc && !strcmp(argv[a], "-v")) {
		verbose = 1;
		a++;
	}
	if (a + 1 < argc && !strcmp(argv[a], "-t"))
		pty = argv[a + 1];
	else if (a < argc)
		port = atoi(argv[a]);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	hw_init();
	radio_restart();
	fprintf(stderr, "gp711-rcpd: radio %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		st.ext_addr[0], st.ext_addr[1], st.ext_addr[2], st.ext_addr[3],
		st.ext_addr[4], st.ext_addr[5], st.ext_addr[6], st.ext_addr[7]);

	if (pty) {
		struct termios t;
		int slave;

		/* our own open slave keeps the master from hanging up when
		   the host closes it */
		client = posix_openpt(O_RDWR | O_NOCTTY);
		if (client < 0 || grantpt(client) || unlockpt(client) ||
		    (slave = open(ptsname(client), O_RDWR | O_NOCTTY)) < 0) {
			perror("pty");
			return 1;
		}
		tcgetattr(slave, &t);
		cfmakeraw(&t);
		tcsetattr(slave, TCSANOW, &t);
		unlink(pty);
		if (symlink(ptsname(client), pty)) {
			perror(pty);
			return 1;
		}
		fprintf(stderr, "gp711-rcpd: pseudo-terminal %s\n", pty);
		goto serve;
	}

	fprintf(stderr, "gp711-rcpd: listening on tcp port %d\n", port);
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) || listen(lfd, 1)) {
		perror("listen");
		return 1;
	}

serve:
	while (!quit) {
		struct pollfd pfd[2] = {
			{ .fd = lfd, .events = POLLIN },
			{ .fd = client, .events = POLLIN },
		};
		uint8_t buf[512];

		poll(pfd, client >= 0 ? 2 : 1, 1);

		if (pfd[0].revents & POLLIN) {
			int fd = accept(lfd, NULL, NULL);

			if (fd >= 0) {
				if (client >= 0)
					close(client);
				client = fd;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
				fprintf(stderr, "gp711-rcpd: host connected\n");
			}
		} else if (client >= 0 && pfd[1].revents) {
			ssize_t r = read(client, buf, sizeof(buf));

			if (r > 0) {
				feed(buf, r);
			} else if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
				fprintf(stderr, "gp711-rcpd: host disconnected\n");
				close(client);
				client = -1;
				st.raw_enabled = st.rx_on_idle = st.phy_enabled = 0;
				apply_rx();
			}
		}

		if (irq_pending())
			service_radio();
	}

	radio_rx_disable();
	return 0;
}
