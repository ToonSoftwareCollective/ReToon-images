// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-btaddr: give an unconfigured Bluetooth controller its public address
 * through the kernel management interface (MGMT_OP_SET_PUBLIC_ADDRESS).
 *
 * The BCM43430 comes up with Broadcom's default address, which the kernel
 * refuses to use. Usage: toon-btaddr <hci index> <aa:bb:cc:dd:ee:ff>
 */

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH	31
#endif
#define BTPROTO_HCI		1
#define HCI_DEV_NONE		0xffff
#define HCI_CHANNEL_CONTROL	3

#define MGMT_OP_SET_PUBLIC_ADDRESS	0x0039
#define MGMT_EV_CMD_COMPLETE		0x0001
#define MGMT_EV_CMD_STATUS		0x0002

struct sockaddr_hci {
	sa_family_t hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

int main(int argc, char **argv)
{
	struct sockaddr_hci addr = {
		.hci_family = AF_BLUETOOTH,
		.hci_dev = HCI_DEV_NONE,
		.hci_channel = HCI_CHANNEL_CONTROL,
	};
	unsigned int b[6];
	uint8_t cmd[12], ev[64];
	int fd, i, index, tries;

	if (argc != 3 || sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &b[5], &b[4],
				&b[3], &b[2], &b[1], &b[0]) != 6) {
		fprintf(stderr, "usage: %s <hci index> <aa:bb:cc:dd:ee:ff>\n",
			argv[0]);
		return 2;
	}
	index = atoi(argv[1]);

	fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("mgmt socket");
		return 1;
	}

	put_le16(cmd, MGMT_OP_SET_PUBLIC_ADDRESS);
	put_le16(cmd + 2, index);
	put_le16(cmd + 4, 6);
	for (i = 0; i < 6; i++)
		cmd[6 + i] = b[i];
	if (write(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		perror("mgmt write");
		return 1;
	}

	/* other events may arrive first; wait for the reply to our command */
	for (tries = 0; tries < 20; tries++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int n;

		if (poll(&pfd, 1, 500) <= 0)
			continue;
		n = read(fd, ev, sizeof(ev));
		if (n < 9)
			continue;
		if ((ev[0] | ev[1] << 8) != MGMT_EV_CMD_COMPLETE &&
		    (ev[0] | ev[1] << 8) != MGMT_EV_CMD_STATUS)
			continue;
		if ((ev[6] | ev[7] << 8) != MGMT_OP_SET_PUBLIC_ADDRESS)
			continue;
		if (ev[8]) {
			fprintf(stderr, "set public address failed, mgmt status 0x%02x\n",
				ev[8]);
			return 1;
		}
		printf("hci%d public address set to %s\n", index, argv[2]);
		return 0;
	}
	fprintf(stderr, "no reply from the kernel\n");
	return 1;
}
