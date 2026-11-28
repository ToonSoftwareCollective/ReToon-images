// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef METER_H
#define METER_H

/* the readings of meter adapters, taken from the chip's bytes as they pass */
int meter_feed(const unsigned char *buf, int n);	/* complete frames seen */
void meter_reload(void);		/* config/zwave.conf "meters=" when changed */
void meter_write(void);
int meter_any_enabled(void);

#endif
