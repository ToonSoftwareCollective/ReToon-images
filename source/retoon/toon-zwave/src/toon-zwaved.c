// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-zwaved: the Toon 2's Z-Wave 500 controller on a TCP port.
 *
 * The chip speaks the Z-Wave Serial API on /dev/ttymxc1 (Toon 2) or
 * /dev/ttymxc2 (Toon 1), 115200 8N1; the port is the third argument. This
 * passes the bytes on unchanged, to one client at a time, so the software
 * that knows Z-Wave can run elsewhere: Z-Wave JS in Home Assistant with the
 * address "tcp://<toon>:6639", or a program on the Toon itself.
 *
 *   toon-zwaved <bind address> [allowed client address]
 *
 * The network, its devices and their routes are kept by the chip, so they
 * stay whatever connects. A new client takes the place of the one before
 * it: a client that lost its connection must be able to come back.
 *
 * retoon/zwave (the Toon's own MQTT broker, libtoonbus) tells the web
 * interface what is going on.
 *
 * Meter adapters (meter.c) are read from the bytes as they pass. With no
 * client connected and adapters to read, this acknowledges the chip's
 * frames itself, as a client would; it sends nothing else.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "meter.h"
#include <toonbus.h>

#define TTY	"/dev/ttymxc1"
static const char *tty_path = TTY;
#define PORT	6639
#define STATUS	"zwave"

static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

static void status(const char *state, const char *client, const char *bind_addr)
{
	char kv[256];

	snprintf(kv, sizeof(kv), "state=%s\nclient=%s\nbind=%s\nport=%d\n", state, client, bind_addr, PORT);
	bus_put(STATUS, kv);
}

static int open_tty(void)
{
	struct termios t;
	int fd = open(tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (fd < 0 || tcgetattr(fd, &t)) {
		perror(tty_path);
		return -1;
	}
	cfmakeraw(&t);
	cfsetspeed(&t, B115200);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &t)) {
		perror(tty_path);
		return -1;
	}
	return fd;
}

/* everything or nothing: a Serial API frame cut short helps nobody */
static int write_all(int fd, const unsigned char *buf, int n)
{
	while (n > 0) {
		int w = write(fd, buf, n);

		if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
			struct pollfd p = { fd, POLLOUT, 0 };

			if (poll(&p, 1, 2000) <= 0)
				return -1;
			continue;
		}
		if (w <= 0)
			return -1;
		buf += w;
		n -= w;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *bind_addr = argc > 1 ? argv[1] : "127.0.0.1";
	const char *allow = argc > 2 ? argv[2] : "";
	if (argc > 3 && argv[3][0]) tty_path = argv[3];
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	char peer[INET_ADDRSTRLEN] = "";
	unsigned char buf[1024];
	int tty, lfd, cfd = -1, one = 1;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	bus_open("zwaved");

	if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
		fprintf(stderr, "toon-zwaved: bad address %s\n", bind_addr);
		return 1;
	}
	tty = open_tty();
	if (tty < 0)
		return 1;
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) || listen(lfd, 2)) {
		perror("toon-zwaved: listen");
		return 1;
	}
	fprintf(stderr, "toon-zwaved: %s on %s:%d%s%s\n", tty_path, bind_addr, PORT,
		allow[0] ? ", only for " : "", allow);
	status("waiting", "", bind_addr);

	while (!stop) {
		struct pollfd p[3] = { { lfd, POLLIN, 0 }, { tty, POLLIN, 0 }, { cfd, POLLIN, 0 } };
		int n;

		if (poll(p, cfd >= 0 ? 3 : 2, 1000) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (p[0].revents & POLLIN) {
			struct sockaddr_in from;
			socklen_t len = sizeof(from);
			int fd = accept(lfd, (struct sockaddr *)&from, &len);
			char who[INET_ADDRSTRLEN] = "?";

			if (fd >= 0) {
				inet_ntop(AF_INET, &from.sin_addr, who, sizeof(who));
				if (allow[0] && strcmp(who, allow) && strcmp(who, "127.0.0.1")) {
					fprintf(stderr, "toon-zwaved: %s turned away\n", who);
					close(fd);
				} else {
					if (cfd >= 0) {
						fprintf(stderr, "toon-zwaved: %s makes way for %s\n", peer, who);
						close(cfd);
					}
					cfd = fd;
					snprintf(peer, sizeof(peer), "%s", who);
					setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
					setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
					fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK);
					/* what the chip said to nobody is of no use to this one */
					tcflush(tty, TCIFLUSH);
					fprintf(stderr, "toon-zwaved: %s connected\n", peer);
					status("connected", peer, bind_addr);
					continue;	/* p[2] was about the client before */
				}
			}
		}
		meter_reload();
		if (p[1].revents & POLLIN) {
			n = read(tty, buf, sizeof(buf));
			if (n > 0) {
				int frames = meter_feed(buf, n);

				if (cfd >= 0 && write_all(cfd, buf, n))
					p[2].revents |= POLLHUP;
				/* nobody to acknowledge them: the chip would send
				   each frame three times */
				if (cfd < 0 && meter_any_enabled())
					while (frames-- > 0)
						write_all(tty, (const unsigned char *)"\x06", 1);
			}
		}
		if (p[1].revents & (POLLERR | POLLHUP)) {
			fprintf(stderr, "toon-zwaved: %s is gone\n", tty_path);
			break;
		}
		if (cfd >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) {
			n = p[2].revents & POLLIN ? read(cfd, buf, sizeof(buf)) : 0;
			if (n > 0 && !(p[2].revents & POLLERR)) {
				if (write_all(tty, buf, n))
					fprintf(stderr, "toon-zwaved: could not write to the chip\n");
			} else if (!(n < 0 && (errno == EAGAIN || errno == EINTR))) {
				fprintf(stderr, "toon-zwaved: %s left\n", peer);
				close(cfd);
				cfd = -1;
				status("waiting", "", bind_addr);
			}
		}
	}
	if (cfd >= 0)
		close(cfd);
	bus_put(STATUS, NULL);
	bus_put("meter", NULL);
	bus_put("meters", NULL);
	bus_close();
	return 0;
}
