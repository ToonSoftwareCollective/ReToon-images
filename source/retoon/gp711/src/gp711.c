// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gp711: native userspace access to the GreenPeak GP711 802.15.4 radio of the
 * Toon 2: its SPI protocol and the register sequences that set up, receive
 * and transmit (gp711_tables.h).
 *
 * SPI is bit-banged on the GPIO alternates of the radio's pads via /dev/mem.
 *
 *   gp711 rx <channel 11-26> <seconds>      print received frames
 *   gp711 tx <channel> <frame hex, no FCS>  transmit one frame
 *   gp711 scan <first> <last>               beacon request per channel
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { OP_R, OP_W, OP_WM, OP_RS, OP_WS, OP_RESET, OP_WAKEUP, OP_INTEN };

struct gp_op {
	uint8_t type;
	uint16_t addr;
	uint8_t len;
	uint8_t data[16];
};

#include "gp711_tables.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

#define IOMUXC	0x020e0000
#define GPIO2	0x020a0000
#define GPIO4	0x020a8000
#define GPIO7	0x020b4000

#define SCLK	10	/* GPIO2 */
#define MISO	11	/* GPIO2 */
#define MOSI	15	/* GPIO2 */
#define SSN	16	/* GPIO2 */
#define RESET	6	/* GPIO4 */
#define IRQ	4	/* GPIO7, active low */
#define WAKEUP	5	/* GPIO7 */

/* registers, as far as they are understood */
#define REG_IRQ_SOURCE		0x0502
#define REG_PBM_RELEASE		0x0f03
#define REG_PBM_STATE		0x0f04
#define REG_EVENT		0x101c
#define REG_EVENT_RX_PBM	0x1015
#define REG_EVENT_TX_PBM	0x1016
#define REG_EVENT_ES_PBM	0x1017
#define REG_TIME		0x0608

#define EVENT_RX		0x01
#define EVENT_TX		0x02
#define EVENT_ES		0x04

#define PBM_BASE(n)		(0x3800 + (n) * 0x100)
#define PBM_LENGTH		0x03
#define PBM_LEVEL		0x04
#define PBM_TIMESTAMP		0x05
#define PBM_QUALITY		0x0a
#define PBM_FRAME		0x13

static volatile uint32_t *iomux, *gpio2, *gpio4, *gpio7;
static int verbose;

static void gset(volatile uint32_t *g, int pin, int val)
{
	if (val)
		g[0] |= 1u << pin;
	else
		g[0] &= ~(1u << pin);
}

static int gget(volatile uint32_t *g, int pin)
{
	return (g[2] >> pin) & 1;
}

static void settle(void)
{
	volatile int i;

	for (i = 0; i < 20; i++)
		;
}

static uint8_t xfer(uint8_t out)
{
	uint8_t in = 0;
	int i;

	for (i = 7; i >= 0; i--) {
		gset(gpio2, MOSI, (out >> i) & 1);
		settle();
		gset(gpio2, SCLK, 1);
		settle();
		in = in << 1 | gget(gpio2, MISO);
		gset(gpio2, SCLK, 0);
	}
	return in;
}

static void select_chip(int on)
{
	gset(gpio2, SSN, !on);
	usleep(on ? 5 : 20);
}

static int wait_ack(void)
{
	uint8_t ack = 0;
	int i;

	xfer(0);
	for (i = 0; i < 16 && !ack; i++)
		ack = xfer(0);
	return ack;
}

static void bad_ack(const char *what, unsigned int addr, int ack)
{
	fprintf(stderr, "gp711: %s %04x: unexpected ack %02x\n", what, addr, ack);
}

static uint8_t rd(uint16_t addr)
{
	uint8_t val = 0;
	int ack;

	select_chip(1);
	xfer(addr >> 8);
	xfer(addr & 0xff);
	ack = wait_ack();
	if (ack == 3)
		val = xfer(0);
	else
		bad_ack("read", addr, ack);
	select_chip(0);
	return val;
}

static void wr(uint16_t addr, uint8_t val)
{
	int ack;

	select_chip(1);
	xfer(addr >> 8 | 0x40);
	xfer(addr & 0xff);
	xfer(val);
	ack = wait_ack();
	if (ack != 1)
		bad_ack("write", addr, ack);
	select_chip(0);
}

static void wr_masked(uint16_t addr, uint8_t mask, uint8_t val)
{
	int ack;

	select_chip(1);
	xfer((addr >> 8 & 0x3f) | 0xc0);
	xfer(addr & 0xff);
	xfer(mask);
	xfer(val);
	ack = wait_ack();
	if (ack != 1)
		bad_ack("masked write", addr, ack);
	select_chip(0);
}

/* streams move at most 0x7f bytes per transfer */
static void rd_stream(uint16_t addr, uint8_t *d, unsigned int n)
{
	unsigned int i, chunk;
	int ack;

	for (; n; n -= chunk, addr += chunk, d += chunk) {
		chunk = n > 0x7f ? 0x7f : n;
		select_chip(1);
		xfer((addr >> 8 & 0x7f) | 0x80);
		xfer(addr & 0xff);
		xfer(chunk);
		ack = wait_ack();
		if (ack != 7)
			bad_ack("stream read", addr, ack);
		for (i = 0; i < chunk; i++)
			d[i] = xfer(0);
		select_chip(0);
	}
}

static void wr_stream(uint16_t addr, const uint8_t *d, unsigned int n)
{
	unsigned int i, chunk;
	int ack;

	for (; n; n -= chunk, addr += chunk, d += chunk) {
		chunk = n > 0x7f ? 0x7f : n;
		select_chip(1);
		xfer((addr >> 8 & 0x7f) | 0x80);
		xfer(addr & 0xff);
		xfer(chunk | 0x80);
		for (i = 0; i < chunk; i++)
			xfer(d[i]);
		ack = wait_ack();
		if (ack != 5)
			bad_ack("stream write", addr, ack);
		select_chip(0);
	}
}

static void run_ops(const struct gp_op *ops, unsigned int n)
{
	uint8_t buf[16];
	unsigned int i;

	for (i = 0; i < n; i++) {
		const struct gp_op *op = &ops[i];

		switch (op->type) {
		case OP_R:
			buf[0] = rd(op->addr);
			if (verbose && buf[0] != op->data[0])
				printf("  op %u: R %04x = %02x (table has %02x)\n",
				       i, op->addr, buf[0], op->data[0]);
			break;
		case OP_W:
			wr(op->addr, op->data[0]);
			break;
		case OP_WM:
			wr_masked(op->addr, op->data[0], op->data[1]);
			break;
		case OP_RS:
			rd_stream(op->addr, buf, op->len);
			break;
		case OP_WS:
			wr_stream(op->addr, op->data, op->len);
			break;
		case OP_RESET:
			gset(gpio4, RESET, 0);
			usleep(50000);
			gset(gpio4, RESET, 1);
			usleep(50000);
			break;
		case OP_WAKEUP:
			gset(gpio7, WAKEUP, 1);
			usleep(50);
			gset(gpio7, WAKEUP, 0);
			break;
		case OP_INTEN:
			break;
		}
	}
}

static volatile uint32_t *map(int fd, off_t base)
{
	void *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return p;
}

static void hw_init(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);

	if (fd < 0) {
		perror("/dev/mem");
		exit(1);
	}
	iomux = map(fd, IOMUXC);
	gpio2 = map(fd, GPIO2);
	gpio4 = map(fd, GPIO4);
	gpio7 = map(fd, GPIO7);
	close(fd);

	/* all seven pads to their GPIO alternate (ALT5) */
	iomux[0xa4 / 4] = iomux[0xa8 / 4] = iomux[0xb8 / 4] = iomux[0xbc / 4] = 5;
	iomux[0x158 / 4] = iomux[0x260 / 4] = iomux[0x264 / 4] = 5;

	gset(gpio2, SSN, 1);
	gset(gpio2, SCLK, 0);
	gset(gpio2, MOSI, 0);
	gpio2[1] |= 1u << SSN | 1u << SCLK | 1u << MOSI;
	gpio2[1] &= ~(1u << MISO);
	gpio7[1] &= ~(1u << IRQ);
	gset(gpio7, WAKEUP, 0);
	gpio7[1] |= 1u << WAKEUP;
	gset(gpio4, RESET, 1);
	gpio4[1] |= 1u << RESET;
}

static int current_channel;

static void radio_init(void)
{
	run_ops(gp711_init_ops, ARRAY_SIZE(gp711_init_ops));
}

static void radio_rx_enable(int channel)
{
	struct gp_op first = gp711_rx_enable_ops[0];

	/* the block starts with the channel index write */
	first.data[0] = channel - 11;
	run_ops(&first, 1);
	run_ops(gp711_rx_enable_ops + 1, ARRAY_SIZE(gp711_rx_enable_ops) - 1);
	current_channel = channel;
}

#define REG_PBM_GET		0x0f02
#define REG_PBM_START		0x1014
#define REG_TX_POWER		0x0004
#define REG_MAC_TX_OPTIONS	0x032d
#define PBM_STATUS		0x01
#define PBM_OPTIONS		0x09
#define PBM_FLAGS		0x0a
#define PBM_FLAG_ACK_REQUEST	0x04
#define PBM_FLAG_START		0x40

static void radio_set_channel(int channel)
{
	if (channel == current_channel)
		return;
	wr(0x034e, channel - 11);
	wr(0x034d, 0x00);
	rd(0x0003);
	rd(0x0348);
	rd(0x034c);
	wr(0x034b, 0x01);
	rd(0x034c);
	rd(0x034f);
	current_channel = channel;
}

/* Queue a frame (without FCS); the confirm arrives as an EVENT_TX. */
static int radio_tx_start(int channel, const uint8_t *frame, int len)
{
	static const uint8_t options[] = { 0x20, 0x00, 0x00, 0x00 };
	static const uint8_t head[] = { 0xaa, 0xaa, 0x00, 0x00 };
	static const uint8_t zero[] = { 0x00, 0x00 };
	int ack = len > 0 && (frame[0] & 0x20);
	int n = rd(REG_PBM_GET);
	uint16_t base = PBM_BASE(n);

	wr_stream(base + PBM_FRAME, frame, len);
	wr_stream(base + PBM_OPTIONS, options, sizeof(options));
	wr_stream(base, head, sizeof(head));
	wr_stream(base + 0x0d, zero, sizeof(zero));
	wr_masked(base + PBM_FLAGS, PBM_FLAG_ACK_REQUEST,
		  ack ? PBM_FLAG_ACK_REQUEST : 0);
	wr(base + PBM_LENGTH, len);
	wr(base + 0x02, 0x00);
	wr_masked(REG_MAC_TX_OPTIONS, 0x0e, 0x06);
	wr(base + PBM_OPTIONS, rd(base + PBM_OPTIONS));
	wr(base + PBM_FLAGS, rd(base + PBM_FLAGS) | PBM_FLAG_START);
	wr(REG_TX_POWER, 0xa6);
	radio_set_channel(channel);
	/*
	 * gp711_tx_fem_pre_ops / _post_ops are not run around a transmission:
	 * with them the chip missed nearly every ACK (about 1 in 100 seen,
	 * without them most).
	 */
	wr(REG_PBM_START, n);
	return n;
}

#define REG_SHORT_ADDR		0x0300
#define REG_EXT_ADDR		0x0302
#define REG_PAN_ID		0x030a
#define REG_RX_CONTROL		0x0320
#define RX_CONTROL_PROMISCUOUS	0x01
#define RX_CONTROL_RX_ON	0x40
#define REG_FRAME_FILTER	0x0321	/* 5 bytes of frame type filtering */

static void radio_set_short_addr(uint16_t addr)
{
	uint8_t b[] = { addr & 0xff, addr >> 8 };

	wr_stream(REG_SHORT_ADDR, b, sizeof(b));
}

/* EUI-64, least significant byte first */
static void radio_set_ext_addr(const uint8_t *eui)
{
	wr_stream(REG_EXT_ADDR, eui, 8);
}

static void radio_set_pan_id(uint16_t pan)
{
	uint8_t b[] = { pan & 0xff, pan >> 8 };

	wr_stream(REG_PAN_ID, b, sizeof(b));
}

static void radio_set_promiscuous(int on)
{
	static const uint8_t filtered[] = { 0xed, 0x10, 0xff, 0x04, 0xf3 };

	if (!on) {
		wr_masked(REG_RX_CONTROL, RX_CONTROL_PROMISCUOUS, 0);
		wr_stream(REG_FRAME_FILTER, filtered, sizeof(filtered));
		return;
	}
	wr_masked(REG_RX_CONTROL, RX_CONTROL_PROMISCUOUS, RX_CONTROL_PROMISCUOUS);
	wr(REG_FRAME_FILTER + 4, 0xff);
	wr(REG_FRAME_FILTER, 0x00);
	wr_masked(REG_FRAME_FILTER, 0x01, 0x01);
	wr(REG_FRAME_FILTER + 1, 0x00);
	wr(REG_FRAME_FILTER + 2, 0xff);
	wr(REG_FRAME_FILTER + 3, 0x00);
}

static void radio_rx_disable(void)
{
	wr_masked(REG_RX_CONTROL, RX_CONTROL_RX_ON, 0);
}

static int irq_pending(void)
{
	return !gget(gpio7, IRQ);
}

static void release_pbm(int n)
{
	rd(REG_PBM_STATE);
	rd(REG_PBM_STATE);
	wr(REG_PBM_RELEASE, n);
}

/*
 * Data pending: the chip's own processor sets the frame-pending bit in its
 * ACK to a MAC data request whose source is in one of two tables (12 short,
 * 4 extended addresses). The processor is claimed (0x0e14/0x0818,
 * 0x25ff = 1), the tables are written while it is paused (0x25f5 = 2,
 * 0x25f8 -> 0) and it is resumed (0x25f4 = 2, 0x25f8 -> 1).
 */
#define REG_CPU_VECTOR		0x0e14
#define REG_CPU_WAKE		0x0818
#define REG_CPU_CLAIMED		0x25ff
#define REG_DP_RUN		0x25f4
#define REG_DP_PAUSE		0x25f5
#define REG_DP_STATE		0x25f8
#define DP_SHORT_TABLE		0x2500
#define DP_EXT_TABLE		0x24c0
#define REG_DP_N_SHORT		0x2554
#define REG_DP_N_EXT		0x2555
#define REG_DP_SHORT_PTR	0x2556
#define REG_DP_EXT_PTR		0x2558
#define DP_MAX_SHORT		12
#define DP_MAX_EXT		4

/* wait up to 10 ms for a register to become zero or non-zero */
static int wait_reg(uint16_t addr, int nonzero)
{
	int i;

	for (i = 0; i < 313; i++) {
		if ((rd(addr) != 0) == nonzero)
			return 1;
		usleep(32);
	}
	return 0;
}

static int dp_pause(void)
{
	wr(REG_DP_PAUSE, 2);
	return wait_reg(REG_DP_STATE, 0);
}

static int dp_resume(void)
{
	wr(REG_DP_RUN, 2);
	return wait_reg(REG_DP_STATE, 1);
}

/* Claim the chip's processor and start data pending with empty tables. */
static int dp_start(void)
{
	static const uint8_t vector[] = { 0x84, 0x82 };
	uint8_t ptr[2];

	if (!rd(REG_CPU_CLAIMED)) {
		wr_stream(REG_CPU_VECTOR, vector, 2);
		wr(REG_CPU_WAKE, 4);
		if (!wait_reg(REG_CPU_CLAIMED, 1))
			return 0;
	}
	dp_pause();
	ptr[0] = DP_SHORT_TABLE & 0xff;
	ptr[1] = DP_SHORT_TABLE >> 8;
	wr_stream(REG_DP_SHORT_PTR, ptr, 2);
	ptr[0] = DP_EXT_TABLE & 0xff;
	ptr[1] = DP_EXT_TABLE >> 8;
	wr_stream(REG_DP_EXT_PTR, ptr, 2);
	wr(REG_DP_N_SHORT, 0);
	wr(REG_DP_N_EXT, 0);
	return dp_resume();
}

/* Short addresses as on air (little endian), extended ones too. */
static int dp_set(const uint16_t *s, int ns, const uint8_t (*e)[8], int ne)
{
	uint8_t b[2 * DP_MAX_SHORT];
	int i;

	if (ns > DP_MAX_SHORT)
		ns = DP_MAX_SHORT;
	if (ne > DP_MAX_EXT)
		ne = DP_MAX_EXT;
	for (i = 0; i < ns; i++) {
		b[2 * i] = s[i] & 0xff;
		b[2 * i + 1] = s[i] >> 8;
	}
	dp_pause();
	if (ns)
		wr_stream(DP_SHORT_TABLE, b, 2 * ns);
	if (ne)
		wr_stream(DP_EXT_TABLE, e[0], 8 * ne);
	wr(REG_DP_N_SHORT, ns);
	wr(REG_DP_N_EXT, ne);
	return dp_resume();
}

static int tx_status = -1;	/* status of the last TX confirm, -1 = none yet */

/* Returns the frame length, or 0 if the event was something else. */
static int radio_service(uint8_t *frame, int *quality, int *level)
{
	uint8_t stamp[4], event;
	int n, len;

	rd(REG_IRQ_SOURCE);
	event = rd(REG_EVENT);

	if (event & EVENT_RX) {
		n = rd(REG_EVENT_RX_PBM);
		len = rd(PBM_BASE(n) + PBM_LENGTH);
		*level = rd(PBM_BASE(n) + PBM_LEVEL);
		if (len > 127)
			len = 127;
		rd_stream(PBM_BASE(n) + PBM_FRAME, frame, len);
		rd(REG_PBM_STATE);
		*quality = rd(PBM_BASE(n) + PBM_QUALITY);
		rd_stream(PBM_BASE(n) + PBM_TIMESTAMP, stamp, 4);
		rd_stream(REG_TIME, stamp, 4);
		release_pbm(n);
		return len;
	}
	if (event & EVENT_TX) {
		n = rd(REG_EVENT_TX_PBM);
		tx_status = rd(PBM_BASE(n) + PBM_STATUS);
		rd(PBM_BASE(n) + PBM_LENGTH);
		rd(PBM_BASE(n) + 0x02);
		rd(PBM_BASE(n) + PBM_FLAGS);
		rd(REG_PBM_STATE);
		rd_stream(PBM_BASE(n) + PBM_TIMESTAMP, stamp, 4);
		rd_stream(REG_TIME, stamp, 4);
		release_pbm(n);
		return 0;
	}
	if (event & EVENT_ES) {
		release_pbm(rd(REG_EVENT_ES_PBM));
		return 0;
	}
	fprintf(stderr, "gp711: unknown event %02x\n", event);
	return 0;
}

#ifndef GP711_NO_MAIN
static void print_frame(const char *tag, const uint8_t *frame, int len, int quality)
{
	int i;

	printf("%s len %3d q %02x:", tag, len, quality);
	for (i = 0; i < len; i++)
		printf(" %02x", frame[i]);
	printf("\n");
	fflush(stdout);
}

/* Service events for the given time; returns the number of frames received. */
static int listen_ms(int ms)
{
	uint8_t frame[128];
	struct timespec ts;
	long long end, now;
	int quality, level, len, count = 0;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	end = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000 + ms;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
		if (now >= end)
			break;
		if (!irq_pending()) {
			usleep(200);
			continue;
		}
		len = radio_service(frame, &quality, &level);
		if (len) {
			print_frame("rx", frame, len, quality);
			count++;
		}
	}
	return count;
}

static int hex_frame(const char *hex, uint8_t *out)
{
	int n = 0;
	unsigned int b;

	while (n < 125 && sscanf(hex + 2 * n, "%2x", &b) == 1)
		out[n++] = b;
	return n;
}

int main(int argc, char **argv)
{
	uint8_t frame[128], eui[8];
	int channel, len, a = 1;

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		verbose = 1;
		a++;
	}
	if (argc - a == 3 && !strcmp(argv[a], "regs")) {
		unsigned int addr = strtoul(argv[a + 1], NULL, 16), n = strtoul(argv[a + 2], NULL, 0), i;
		uint8_t d[256];

		hw_init();
		if (n > sizeof(d))
			n = sizeof(d);
		rd_stream(addr, d, n);
		for (i = 0; i < n; i++)
			printf("%s%02x", i % 16 ? " " : i ? "\n" : "", d[i]);
		printf("\n");
		return 0;
	}
	if (argc - a == 2 && !strcmp(argv[a], "dp")) {
		static const uint16_t s[] = { 0x1234, 0xabcd };
		static const uint8_t e[1][8] = { { 1, 2, 3, 4, 5, 6, 7, 8 } };

		hw_init();
		radio_init();
		radio_rx_enable(atoi(argv[a + 1]));
		printf("claimed %02x state %02x\n", rd(REG_CPU_CLAIMED), rd(REG_DP_STATE));
		printf("dp_start %d: state %02x\n", dp_start(), rd(REG_DP_STATE));
		printf("dp_set %d: state %02x n %02x %02x\n", dp_set(s, 2, e, 1), rd(REG_DP_STATE),
		       rd(REG_DP_N_SHORT), rd(REG_DP_N_EXT));
		return 0;
	}
	if (argc - a != 3 || (strcmp(argv[a], "rx") && strcmp(argv[a], "tx") &&
			      strcmp(argv[a], "scan"))) {
		fprintf(stderr,
			"usage: %s [-v] rx <channel> <seconds>\n"
			"       %s [-v] tx <channel> <frame hex, no FCS>\n"
			"       %s [-v] scan <first channel> <last channel>   (beacon requests)\n",
			argv[0], argv[0], argv[0]);
		return 1;
	}
	channel = atoi(argv[a + 1]);
	if (channel < 11 || channel > 26)
		return 1;

	hw_init();
	radio_init();
	rd_stream(0x0302, eui, 8);
	printf("gp711: EUI-64 %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
	       eui[7], eui[6], eui[5], eui[4], eui[3], eui[2], eui[1], eui[0]);

	if (!strcmp(argv[a], "rx")) {
		radio_rx_enable(channel);
		printf("gp711: %d frames on channel %d\n",
		       listen_ms(atoi(argv[a + 2]) * 1000), channel);
	} else if (!strcmp(argv[a], "tx")) {
		len = hex_frame(argv[a + 2], frame);
		radio_rx_enable(channel);
		tx_status = -1;
		radio_tx_start(channel, frame, len);
		listen_ms(500);
		printf("gp711: tx %d bytes on channel %d, status %02x\n", len,
		       channel, tx_status & 0xff);
	} else {
		/* 802.15.4 beacon request: FCF 0x0803, broadcast PAN and address */
		static const uint8_t beacon_req[] = { 0x03, 0x08, 0x00, 0xff, 0xff, 0xff, 0xff, 0x07 };
		int last = atoi(argv[a + 2]), n;

		for (; channel <= last && channel <= 26; channel++) {
			radio_rx_enable(channel);
			tx_status = -1;
			radio_tx_start(channel, beacon_req, sizeof(beacon_req));
			n = listen_ms(1000);
			printf("gp711: channel %d: tx status %02x, %d frames\n",
			       channel, tx_status & 0xff, n);
		}
	}
	return 0;
}
#endif /* GP711_NO_MAIN */
