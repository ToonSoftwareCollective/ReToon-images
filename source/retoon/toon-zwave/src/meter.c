// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Meter adapters read on the side: the bytes the chip sends to the client
 * (Z-Wave JS) are looked at as they pass. A node that sends Meter Table
 * Monitor reports (command class 0x3D) is a meter adapter, such as the one
 * that came with the Toon for the P1 port of a smart meter. The readings of
 * the adapters the user turned on are published as retoon/meter, where the
 * rest of ReToon finds them as its own values (MQTT, Home Assistant, the
 * screen). Nothing is sent to the network: what Z-Wave JS sees is unchanged.
 *
 *   retoon/meters   found=<nodes>        every meter adapter seen
 *   retoon/meter    n<node>_<what>=<value> for the ones turned on
 *
 * Which ones are turned on: "meters=" in config/zwave.conf.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "meter.h"
#include <toonbus.h>

#ifndef CONF	/* other paths for a test */
#define CONF	"/mnt/data/toon-os/config/zwave.conf"
#define FOUND	"meters"	/* on the Toon's own MQTT broker (libtoonbus) */
#define VALUES	"meter"
#endif
#define MAX_NODES	8
#define MAX_VALUES	64

static int found[MAX_NODES], nfound;
static int enabled[MAX_NODES], nenabled;
static time_t conf_mtime = -1;
static struct value {
	int node;
	char what[24];
	char text[24];
} values[MAX_VALUES];
static int nvalues;

static int is_enabled(int node)
{
	for (int i = 0; i < nenabled; i++)
		if (enabled[i] == node)
			return 1;
	return 0;
}

int meter_any_enabled(void)
{
	return nenabled > 0;
}

/* config/zwave.conf "meters=5 17": the adapters whose readings are used */
void meter_reload(void)
{
	struct stat sb;
	char line[160];
	FILE *f;

	if (stat(CONF, &sb))
		sb.st_mtime = 0;
	if (sb.st_mtime == conf_mtime)
		return;
	conf_mtime = sb.st_mtime;
	nenabled = 0;
	f = fopen(CONF, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *p = line;

		if (strncmp(line, "meters=", 7))
			continue;
		p += 7;
		while (*p && nenabled < MAX_NODES) {
			char *end;
			long n = strtol(p, &end, 10);

			if (end == p) {
				p++;
				continue;
			}
			if (n > 0 && n < 233)
				enabled[nenabled++] = (int)n;
			p = end;
		}
	}
	if (f)
		fclose(f);
	/* readings of an adapter turned off are no longer ReToon's */
	for (int i = 0; i < nvalues; )
		if (!is_enabled(values[i].node))
			values[i] = values[--nvalues];
		else
			i++;
	meter_write();
}

static void write_file(const char *name, const char *text)
{
	bus_put(name, text);
}

void meter_write(void)
{
	char buf[MAX_VALUES * 56 + 64];
	size_t n = 0;

	buf[0] = 0;
	for (int i = 0; i < nvalues && n < sizeof(buf); i++)
		n += snprintf(buf + n, sizeof(buf) - n, "n%d_%s=%s\n", values[i].node,
			      values[i].what, values[i].text);
	write_file(VALUES, buf);
}

static void saw_node(int node)
{
	char buf[16 * MAX_NODES + 16];
	size_t n;

	for (int i = 0; i < nfound; i++)
		if (found[i] == node)
			return;
	if (nfound == MAX_NODES)
		return;
	found[nfound++] = node;
	n = snprintf(buf, sizeof(buf), "found=");
	for (int i = 0; i < nfound; i++)
		n += snprintf(buf + n, sizeof(buf) - n, "%s%d", i ? " " : "", found[i]);
	snprintf(buf + n, sizeof(buf) - n, "\n");
	write_file(FOUND, buf);
	fprintf(stderr, "toon-zwaved: meter adapter at node %d\n", node);
}

static void set_value(int node, const char *what, double v, int decimals)
{
	struct value *x = NULL;

	for (int i = 0; i < nvalues; i++)
		if (values[i].node == node && !strcmp(values[i].what, what))
			x = &values[i];
	if (!x) {
		if (nvalues == MAX_VALUES)
			return;
		x = &values[nvalues++];
		x->node = node;
		snprintf(x->what, sizeof(x->what), "%s", what);
	}
	snprintf(x->text, sizeof(x->text), "%.*f", decimals, v);
}

/*
 * What an endpoint of the adapter measures. The adapter that came with the
 * Toon 2 (and the Engie EN00, which adds endpoint 9, water):
 *
 *   1 gas, 2 and 3 pulse/laser in and out, 4 and 5 in and out on tariff 2,
 *   6 and 7 in and out on tariff 1, 8 heat
 *
 * The older adapter (v2) has no second pulse endpoint, so its electricity
 * is one lower:
 *
 *   1 gas, 2 pulse/laser in, 3 and 4 in and out on tariff 2,
 *   5 and 6 in and out on tariff 1
 *
 * Each reading says whether it is electricity in or out (its rate type), so
 * endpoints 3 to 6 are named by that, and the same name comes out for both.
 * Other meters get their endpoint number.
 */
static const char *endpoint_name(int ep, int rate)
{
	static const char *const quby[] = { NULL, "gas", "pulse_import", "pulse_export",
		"import_t2", "export_t2", "import_t1", "export_t1", "heat", "water" };
	static char other[8];

	if (rate == 1 && (ep == 3 || ep == 5))		/* v2: in */
		return ep == 3 ? "import_t2" : "import_t1";
	if (rate == 2 && (ep == 4 || ep == 6))		/* v2: out */
		return ep == 4 ? "export_t2" : "export_t1";
	if (ep >= 1 && ep <= 9)
		return quby[ep];
	snprintf(other, sizeof(other), "ep%d", ep);
	return other;
}

/* a Current Data Report (after 0x3D 0x0D) from node, endpoint ep */
static void current_data(int node, int ep, const unsigned char *p, int len)
{
	unsigned int datasets;
	int off = 12, changed = 0;
	const char *base;

	if (len < 12)
		return;
	base = endpoint_name(ep, p[1] & 3);	/* rate type: 1 in, 2 out */
	datasets = (unsigned)p[2] << 16 | (unsigned)p[3] << 8 | p[4];
	for (int ds = 0; ds < 24; ds++) {
		int precision, scale;
		unsigned long raw;
		double v;
		char what[24];

		if (!(datasets & (1u << ds)))
			continue;
		if (off + 5 > len)
			return;
		precision = p[off] >> 5;
		scale = p[off] & 0x1f;
		raw = (unsigned long)p[off + 1] << 24 | (unsigned long)p[off + 2] << 16 |
		      (unsigned long)p[off + 3] << 8 | p[off + 4];
		off += 5;
		v = (double)raw;
		for (int i = 0; i < precision; i++)
			v /= 10;
		/* the total (kWh, m3) under the endpoint's name; the other
		   dataset is the current value: power (kW) or flow (m3/h) */
		if (scale == 0)
			snprintf(what, sizeof(what), "%s", base);
		else if (scale == 7)
			snprintf(what, sizeof(what), "%s_power", base);
		else if (scale == 8 && (ep == 1 || ep == 9))
			snprintf(what, sizeof(what), "%s_flow", base);
		else
			snprintf(what, sizeof(what), "%s_s%d", base, scale);
		set_value(node, what, v, precision);
		changed = 1;
	}
	if (changed)
		meter_write();
}

/* a command from a node: unpacked from CRC-16 and Multi Channel */
static void command(int node, const unsigned char *c, int len)
{
	int ep = 0;

	for (int depth = 0; depth < 3 && len >= 2; depth++) {
		if (c[0] == 0x56 && c[1] == 0x01 && len > 4) {	/* CRC-16 encapsulation */
			c += 2;
			len -= 4;
		} else if (c[0] == 0x60 && c[1] == 0x0d && len > 4) {	/* Multi Channel */
			ep = c[2] & 0x7f;
			c += 4;
			len -= 4;
		} else {
			break;
		}
	}
	if (len < 2 || c[0] != 0x3d)
		return;
	saw_node(node);
	if (c[1] == 0x0d && is_enabled(node))
		current_data(node, ep, c + 2, len - 2);
}

/* one Serial API frame from the chip: SOF LEN TYPE FUNC DATA... CHECKSUM */
static void frame(const unsigned char *f, int n)
{
	unsigned char sum = 0xff;
	const unsigned char *d = f + 4;
	int dl = n - 5;

	for (int i = 1; i < n - 1; i++)
		sum ^= f[i];
	if (sum != f[n - 1] || f[2] != 0x00)	/* only requests */
		return;
	if (f[3] == 0x04 && dl >= 3 && 3 + d[2] <= dl)	/* ApplicationCommandHandler */
		command(d[1], d + 3, d[2]);
	else if (f[3] == 0xa8 && dl >= 4 && 4 + d[3] <= dl)	/* the bridge variant */
		command(d[2], d + 4, d[3]);
}

/* the chip's bytes as they pass; returns how many complete frames ended */
int meter_feed(const unsigned char *buf, int n)
{
	static unsigned char f[260];	/* LEN up to 255, plus SOF and LEN */
	static int have, want;
	int frames = 0;

	for (int i = 0; i < n; i++) {
		unsigned char b = buf[i];

		if (!have) {
			if (b != 0x01)	/* ACK, NAK, CAN or noise */
				continue;
			f[have++] = b;
			want = 0;
		} else if (have == 1) {
			if (b < 3) {
				have = 0;
				continue;
			}
			f[have++] = b;
			want = b + 2;
		} else {
			f[have++] = b;
			if (have == want) {
				frame(f, have);
				frames++;
				have = 0;
			}
		}
	}
	return frames;
}
