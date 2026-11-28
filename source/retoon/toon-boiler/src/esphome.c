#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ESPHome native API client (see esphome.h).
 *
 * Transport, as ESPHome's api_frame_helper_noise.cpp has it: frames of
 * [0x01][length, 2 bytes big-endian][data]. The client sends an empty
 * "hello" frame and a handshake frame ([0x00][Noise message]); the device
 * answers with its own hello ([0x01][name]\0[mac]\0) and handshake
 * ([0x00][Noise message], or a non-zero byte and a reason). The handshake
 * is Noise_NNpsk0_25519_ChaChaPoly_SHA256 with the API key as PSK and the
 * prologue "NoiseAPIInit" + the client hello frame's length (2 bytes) and
 * data. After it, every frame is encrypted; inside: [type, 2 bytes]
 * [length, 2 bytes][protobuf message].
 *
 * Messages (api.proto, ESPHome 2026): HelloRequest 1 (client_info 1,
 * api_version_major 2, api_version_minor 3), HelloResponse 2,
 * DisconnectRequest/Response 5/6, PingRequest/Response 7/8,
 * DeviceInfoRequest/Response 9/10 (name 2, esphome_version 4,
 * project_name 8, project_version 9), ListEntitiesRequest 11, list
 * responses binary_sensor 12, sensor 16, switch 17, number 49 (object_id
 * 1, empty since 2026: made from the name; key 2 fixed32, name 3),
 * ListEntitiesDoneResponse 19,
 * SubscribeStatesRequest 20, states binary_sensor 21, sensor 25, switch 26,
 * number 50 (key 1 fixed32, state 2, missing_state 3; switch: 4),
 * SwitchCommandRequest 33 (key 1 fixed32, state 2), NumberCommandRequest
 * 51 (key 1 fixed32, state 2 float).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include "esphome.h"

#define IO_TIMEOUT_MS	5000	/* each step of setting up the connection */
#define PING_S		20	/* our ping when the line is quiet */
#define DEAD_S		60	/* nothing at all for this long: lost */
#define MAX_FRAME	4096
#define MAX_ENTITIES	96

enum { ENT_BINARY, ENT_SENSOR, ENT_SWITCH, ENT_NUMBER };

struct entity {
	int kind;
	uint32_t key;
	char object_id[48];
	int known;
	double value;
	long long when;
};

static int sock = -1;
static struct entity ents[MAX_ENTITIES];
static int n_ents;
static char dev_name[64], dev_version[32], dev_project[64], dev_project_version[32];
static uint8_t rxbuf[MAX_FRAME + 3];
static size_t rxlen;
static long long last_rx, last_ping;

/* ---- time ------------------------------------------------------------- */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* ---- Noise_NNpsk0_25519_ChaChaPoly_SHA256, initiator ------------------- */

struct cipher {
	uint8_t k[32];
	uint64_t n;
};

static struct {
	uint8_t ck[32], h[32];
	struct cipher c;
	int has_k;
} ss;

static struct cipher tx_cipher, rx_cipher;

static void sha256(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t out[32])
{
	EVP_MD_CTX *m = EVP_MD_CTX_new();
	unsigned int len = 32;

	EVP_DigestInit_ex(m, EVP_sha256(), NULL);
	EVP_DigestUpdate(m, a, alen);
	if (blen)
		EVP_DigestUpdate(m, b, blen);
	EVP_DigestFinal_ex(m, out, &len);
	EVP_MD_CTX_free(m);
}

static void hmac(const uint8_t key[32], const uint8_t *d, size_t len, uint8_t out[32])
{
	unsigned int olen = 32;

	HMAC(EVP_sha256(), key, 32, d, len, out, &olen);
}

/* Noise HKDF: two or three outputs of 32 bytes */
static void hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
		 uint8_t o1[32], uint8_t o2[32], uint8_t *o3)
{
	uint8_t prk[32], in[33];

	hmac(ck, ikm, ikm_len, prk);
	in[0] = 1;
	hmac(prk, in, 1, o1);
	memcpy(in, o1, 32);
	in[32] = 2;
	hmac(prk, in, 33, o2);
	if (o3) {
		memcpy(in, o2, 32);
		in[32] = 3;
		hmac(prk, in, 33, o3);
	}
}

static void nonce(uint64_t n, uint8_t iv[12])
{
	int i;

	memset(iv, 0, 4);
	for (i = 0; i < 8; i++)
		iv[4 + i] = n >> (8 * i);
}

/* ChaCha20-Poly1305: out gets len + 16 bytes */
static int aead_encrypt(struct cipher *c, const uint8_t *ad, size_t adlen,
			const uint8_t *in, size_t len, uint8_t *out)
{
	EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
	uint8_t iv[12];
	int l, ok;

	nonce(c->n++, iv);
	ok = x && EVP_EncryptInit_ex(x, EVP_chacha20_poly1305(), NULL, NULL, NULL) &&
	     EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) &&
	     EVP_EncryptInit_ex(x, NULL, NULL, c->k, iv) &&
	     (!adlen || EVP_EncryptUpdate(x, NULL, &l, ad, adlen)) &&
	     (!len || EVP_EncryptUpdate(x, out, &l, in, len)) &&
	     EVP_EncryptFinal_ex(x, out + len, &l) &&
	     EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_GET_TAG, 16, out + len);
	EVP_CIPHER_CTX_free(x);
	return ok ? 0 : -1;
}

/* in has len bytes including the 16-byte tag; out gets len - 16 */
static int aead_decrypt(struct cipher *c, const uint8_t *ad, size_t adlen,
			const uint8_t *in, size_t len, uint8_t *out)
{
	EVP_CIPHER_CTX *x;
	uint8_t iv[12];
	int l, ok;

	if (len < 16)
		return -1;
	x = EVP_CIPHER_CTX_new();
	nonce(c->n++, iv);
	ok = x && EVP_DecryptInit_ex(x, EVP_chacha20_poly1305(), NULL, NULL, NULL) &&
	     EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) &&
	     EVP_DecryptInit_ex(x, NULL, NULL, c->k, iv) &&
	     (!adlen || EVP_DecryptUpdate(x, NULL, &l, ad, adlen)) &&
	     (len == 16 || EVP_DecryptUpdate(x, out, &l, in, len - 16)) &&
	     EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_SET_TAG, 16, (void *)(in + len - 16)) &&
	     EVP_DecryptFinal_ex(x, out + len - 16, &l) > 0;
	EVP_CIPHER_CTX_free(x);
	return ok ? 0 : -1;
}

static void mix_hash(const uint8_t *d, size_t len)
{
	sha256(ss.h, 32, d, len, ss.h);
}

static void mix_key(const uint8_t *ikm, size_t len)
{
	uint8_t k[32];

	hkdf(ss.ck, ikm, len, ss.ck, k, NULL);
	memcpy(ss.c.k, k, 32);
	ss.c.n = 0;
	ss.has_k = 1;
}

static void mix_key_and_hash(const uint8_t *ikm, size_t len)
{
	uint8_t th[32], k[32];

	hkdf(ss.ck, ikm, len, ss.ck, th, k);
	mix_hash(th, 32);
	memcpy(ss.c.k, k, 32);
	ss.c.n = 0;
	ss.has_k = 1;
}

/* ---- X25519 ----------------------------------------------------------- */

static EVP_PKEY *x25519_new(uint8_t pub[32])
{
	EVP_PKEY *k = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
	size_t len = 32;

	if (k && EVP_PKEY_get_raw_public_key(k, pub, &len) != 1) {
		EVP_PKEY_free(k);
		k = NULL;
	}
	return k;
}

static int x25519(EVP_PKEY *mine, const uint8_t peer_pub[32], uint8_t out[32])
{
	EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_pub, 32);
	EVP_PKEY_CTX *c = peer ? EVP_PKEY_CTX_new(mine, NULL) : NULL;
	size_t len = 32;
	int ok = c && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, peer) == 1 &&
		 EVP_PKEY_derive(c, out, &len) == 1 && len == 32;

	EVP_PKEY_CTX_free(c);
	EVP_PKEY_free(peer);
	return ok ? 0 : -1;
}

/* ---- socket and frames ------------------------------------------------ */

static int send_all(const uint8_t *d, size_t len)
{
	while (len) {
		ssize_t w = send(sock, d, len, MSG_NOSIGNAL);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		d += w;
		len -= w;
	}
	return 0;
}

static int send_frame(const uint8_t *d, size_t len)
{
	uint8_t hdr[3] = { 1, len >> 8, len & 0xff };

	return send_all(hdr, 3) || (len && send_all(d, len)) ? -1 : 0;
}

/* a whole frame from rxbuf, if there is one: its data and length */
static int take_frame(uint8_t *out, size_t *len)
{
	size_t flen;

	if (rxlen < 3)
		return 0;
	if (rxbuf[0] != 1)
		return -1;
	flen = rxbuf[1] << 8 | rxbuf[2];
	if (flen > MAX_FRAME)
		return -1;
	if (rxlen < 3 + flen)
		return 0;
	memcpy(out, rxbuf + 3, flen);
	*len = flen;
	memmove(rxbuf, rxbuf + 3 + flen, rxlen - 3 - flen);
	rxlen -= 3 + flen;
	return 1;
}

/* read what is there without waiting (wait_ms 0) or until wait_ms */
static int fill(int wait_ms)
{
	struct pollfd p = { .fd = sock, .events = POLLIN };
	ssize_t r;

	if (wait_ms && poll(&p, 1, wait_ms) <= 0)
		return 0;
	r = recv(sock, rxbuf + rxlen, sizeof(rxbuf) - rxlen, MSG_DONTWAIT);
	if (r == 0)
		return -1;
	if (r < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
	rxlen += r;
	last_rx = now_ms();
	return 1;
}

/* one frame, waiting up to IO_TIMEOUT_MS (during the set-up) */
static int read_frame(uint8_t *out, size_t *len)
{
	long long until = now_ms() + IO_TIMEOUT_MS;
	int r;

	for (;;) {
		r = take_frame(out, len);
		if (r)
			return r > 0 ? 0 : -1;
		if (now_ms() >= until)
			return -1;
		if (fill(until - now_ms()) < 0)
			return -1;
	}
}

/* ---- protobuf --------------------------------------------------------- */

struct pb {
	uint8_t d[512];
	size_t len;
};

static void pb_byte(struct pb *b, uint8_t c)
{
	if (b->len < sizeof(b->d))
		b->d[b->len++] = c;
}

static void pb_varint(struct pb *b, uint64_t v)
{
	while (v >= 0x80) {
		pb_byte(b, v | 0x80);
		v >>= 7;
	}
	pb_byte(b, v);
}

static void pb_uint(struct pb *b, int field, uint64_t v)
{
	pb_varint(b, field << 3);
	pb_varint(b, v);
}

static void pb_fixed32(struct pb *b, int field, uint32_t v)
{
	int i;

	pb_varint(b, field << 3 | 5);
	for (i = 0; i < 4; i++)
		pb_byte(b, v >> (8 * i));
}

static void pb_str(struct pb *b, int field, const char *s)
{
	size_t n = strlen(s), i;

	pb_varint(b, field << 3 | 2);
	pb_varint(b, n);
	for (i = 0; i < n; i++)
		pb_byte(b, s[i]);
}

struct field {
	int num, type;
	uint64_t v;		/* varint, fixed32 */
	const uint8_t *p;	/* length-delimited */
	size_t len;
};

static int rd_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
	int shift = 0;

	*v = 0;
	while (*p < end && shift < 64) {
		uint8_t c = *(*p)++;

		*v |= (uint64_t)(c & 0x7f) << shift;
		if (!(c & 0x80))
			return 0;
		shift += 7;
	}
	return -1;
}

static int rd_field(const uint8_t **p, const uint8_t *end, struct field *f)
{
	uint64_t tag;
	int i;

	if (*p >= end || rd_varint(p, end, &tag))
		return -1;
	f->num = tag >> 3;
	f->type = tag & 7;
	f->v = 0;
	switch (f->type) {
	case 0:
		return rd_varint(p, end, &f->v);
	case 1:
		if (end - *p < 8)
			return -1;
		for (i = 0; i < 8; i++)
			f->v |= (uint64_t)(*p)[i] << (8 * i);
		*p += 8;
		return 0;
	case 2:
		if (rd_varint(p, end, &f->v) || (uint64_t)(end - *p) < f->v)
			return -1;
		f->p = *p;
		f->len = f->v;
		*p += f->len;
		return 0;
	case 5:
		if (end - *p < 4)
			return -1;
		for (i = 0; i < 4; i++)
			f->v |= (uint32_t)(*p)[i] << (8 * i);
		*p += 4;
		return 0;
	}
	return -1;
}

static void copy_str(char *dst, size_t size, const struct field *f)
{
	size_t n = f->len < size - 1 ? f->len : size - 1;

	memcpy(dst, f->p, n);
	dst[n] = 0;
}

static float as_float(uint32_t v)
{
	float f;

	memcpy(&f, &v, 4);
	return f;
}

/* ---- messages --------------------------------------------------------- */

static int send_msg(int type, const struct pb *b)
{
	uint8_t plain[4 + sizeof(b->d)], enc[sizeof(plain) + 16];
	size_t len = b ? b->len : 0;

	plain[0] = type >> 8;
	plain[1] = type;
	plain[2] = len >> 8;
	plain[3] = len;
	if (len)
		memcpy(plain + 4, b->d, len);
	if (aead_encrypt(&tx_cipher, NULL, 0, plain, 4 + len, enc))
		return -1;
	return send_frame(enc, 4 + len + 16);
}

static struct entity *ent_by_key(uint32_t key)
{
	int i;

	for (i = 0; i < n_ents; i++)
		if (ents[i].key == key)
			return &ents[i];
	return NULL;
}

static struct entity *ent_by_id(const char *object_id)
{
	int i;

	for (i = 0; i < n_ents; i++)
		if (!strcmp(ents[i].object_id, object_id))
			return &ents[i];
	return NULL;
}

static void on_list(int kind, const uint8_t *p, const uint8_t *end)
{
	struct entity e = { .kind = kind };
	struct field f;

	char name[sizeof(e.object_id)] = "";
	size_t i;

	while (p < end && !rd_field(&p, end, &f)) {
		if (f.num == 1 && f.type == 2)
			copy_str(e.object_id, sizeof(e.object_id), &f);
		else if (f.num == 2 && f.type == 5)
			e.key = f.v;
		else if (f.num == 3 && f.type == 2)
			copy_str(name, sizeof(name), &f);
	}
	/* ESPHome 2026 sends no object id any more: it is the name in snake
	   case, as ESPHome makes it (lower case; besides letters, digits, '-'
	   and '_' everything becomes '_') */
	if (!e.object_id[0]) {
		size_t o = 0;

		for (i = 0; name[i] && o < sizeof(e.object_id) - 1; i++) {
			unsigned char c = name[i];

			/* one '_' for a whole UTF-8 character (ESPHome turns a '/'
			   into a fraction slash, three bytes) */
			if (c >= 0x80 && c < 0xc0)
				continue;
			e.object_id[o++] = c >= 'A' && c <= 'Z' ? c + 32 :
				(c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ? c : '_';
		}
		e.object_id[o] = 0;
	}
	if (e.object_id[0] && n_ents < MAX_ENTITIES && !ent_by_key(e.key))
		ents[n_ents++] = e;
}

static void on_state(int kind, const uint8_t *p, const uint8_t *end)
{
	struct field f;
	struct entity *e = NULL;
	int missing = 0, have = 0;
	double v = 0;

	while (p < end && !rd_field(&p, end, &f)) {
		if (f.num == 1 && f.type == 5)
			e = ent_by_key(f.v);
		else if (f.num == 2) {
			v = kind == ENT_SENSOR || kind == ENT_NUMBER ? as_float(f.v) : (f.v != 0);
			have = 1;
		} else if ((f.num == 3 && kind != ENT_SWITCH) || (f.num == 4 && kind == ENT_SWITCH))
			missing = f.v != 0;
	}
	if (!e)
		return;
	/* proto3 leaves out a false bool and a 0.0 float */
	if (!have)
		v = 0;
	e->known = !missing && v == v;	/* NaN: no value */
	e->value = v;
	e->when = now_ms();
}

static void on_device_info(const uint8_t *p, const uint8_t *end)
{
	struct field f;

	while (p < end && !rd_field(&p, end, &f)) {
		if (f.type != 2)
			continue;
		if (f.num == 2)
			copy_str(dev_name, sizeof(dev_name), &f);
		else if (f.num == 4)
			copy_str(dev_version, sizeof(dev_version), &f);
		else if (f.num == 8)
			copy_str(dev_project, sizeof(dev_project), &f);
		else if (f.num == 9)
			copy_str(dev_project_version, sizeof(dev_project_version), &f);
	}
}

/* one decrypted message: 1 = the end of the entity list, -1 = disconnect */
static int handle(int type, const uint8_t *p, size_t len)
{
	const uint8_t *end = p + len;

	switch (type) {
	case 5:		/* the device closes */
		send_msg(6, NULL);
		return -1;
	case 7:
		return send_msg(8, NULL) ? -1 : 0;
	case 10:
		on_device_info(p, end);
		return 0;
	case 12: on_list(ENT_BINARY, p, end); return 0;
	case 16: on_list(ENT_SENSOR, p, end); return 0;
	case 17: on_list(ENT_SWITCH, p, end); return 0;
	case 49: on_list(ENT_NUMBER, p, end); return 0;
	case 19:
		return 1;
	case 21: on_state(ENT_BINARY, p, end); return 0;
	case 25: on_state(ENT_SENSOR, p, end); return 0;
	case 26: on_state(ENT_SWITCH, p, end); return 0;
	case 50: on_state(ENT_NUMBER, p, end); return 0;
	}
	return 0;
}

/* decrypt one data frame and handle it */
static int handle_frame(const uint8_t *d, size_t len)
{
	uint8_t plain[MAX_FRAME];
	size_t mlen;

	if (len < 16 + 4 || aead_decrypt(&rx_cipher, NULL, 0, d, len, plain))
		return -1;
	mlen = plain[2] << 8 | plain[3];
	if (mlen > len - 16 - 4)
		return -1;
	return handle(plain[0] << 8 | plain[1], plain + 4, mlen);
}

/* wait for a message of this type during the set-up (handling the rest) */
static int wait_for(int type)
{
	uint8_t d[MAX_FRAME];
	size_t len;

	for (;;) {
		uint8_t plain[MAX_FRAME];
		size_t mlen;
		int t;

		if (read_frame(d, &len) || len < 20 || aead_decrypt(&rx_cipher, NULL, 0, d, len, plain))
			return -1;
		mlen = plain[2] << 8 | plain[3];
		if (mlen > len - 20)
			return -1;
		t = plain[0] << 8 | plain[1];
		if (handle(t, plain + 4, mlen) < 0)
			return -1;
		if (t == type)
			return 0;
	}
}

/* ---- name lookup: DNS, else mDNS for <name>.local --------------------- */

static int mdns_lookup(const char *host, struct in_addr *out)
{
	uint8_t q[300], r[1500];
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
	long long until;
	size_t n = 12;
	const char *s = host;
	int fd, i;

	memset(q, 0, 12);
	q[5] = 1;			/* one question */
	while (*s) {
		const char *dot = strchr(s, '.');
		size_t l = dot ? (size_t)(dot - s) : strlen(s);

		if (!l || l > 63 || n + l + 6 > sizeof(q))
			return -1;
		q[n++] = l;
		memcpy(q + n, s, l);
		n += l;
		s += l + (dot ? 1 : 0);
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = 1;		/* type A */
	q[n++] = 0x80; q[n++] = 1;	/* class IN, unicast answer please */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
	until = now_ms() + 1500;
	for (i = 0; i < 2 && now_ms() < until; i++) {
		long long next = now_ms() + 700;

		sendto(fd, q, n, 0, (struct sockaddr *)&to, sizeof(to));
		while (now_ms() < next) {
			struct pollfd p = { .fd = fd, .events = POLLIN };
			ssize_t len;
			size_t pos;
			int an, k;

			if (poll(&p, 1, next - now_ms()) <= 0)
				break;
			len = recv(fd, r, sizeof(r), 0);
			if (len < 12 || !(r[2] & 0x80))
				continue;
			an = r[6] << 8 | r[7];
			pos = 12;
			/* skip the questions it repeats */
			for (k = r[4] << 8 | r[5]; k > 0 && pos < (size_t)len; k--) {
				while (pos < (size_t)len && r[pos] && !(r[pos] & 0xc0))
					pos += r[pos] + 1;
				pos += pos < (size_t)len && (r[pos] & 0xc0) ? 2 : 1;
				pos += 4;
			}
			for (k = 0; k < an && pos + 10 < (size_t)len; k++) {
				int type, rdlen;

				while (pos < (size_t)len && r[pos] && !(r[pos] & 0xc0))
					pos += r[pos] + 1;
				pos += pos < (size_t)len && (r[pos] & 0xc0) ? 2 : 1;
				if (pos + 10 > (size_t)len)
					break;
				type = r[pos] << 8 | r[pos + 1];
				rdlen = r[pos + 8] << 8 | r[pos + 9];
				pos += 10;
				if (type == 1 && rdlen == 4 && pos + 4 <= (size_t)len) {
					memcpy(out, r + pos, 4);
					close(fd);
					return 0;
				}
				pos += rdlen;
			}
		}
	}
	close(fd);
	return -1;
}

static int resolve(const char *host, struct in_addr *out)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
	size_t l = strlen(host);

	if (inet_pton(AF_INET, host, out) == 1)
		return 0;
	if (!getaddrinfo(host, NULL, &hints, &res)) {
		*out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
		freeaddrinfo(res);
		return 0;
	}
	if (l > 6 && !strcasecmp(host + l - 6, ".local"))
		return mdns_lookup(host, out);
	return -1;
}

/* ---- connect ---------------------------------------------------------- */

static int fail(char *err, size_t errlen, const char *msg)
{
	snprintf(err, errlen, "%s", msg);
	esp_close();
	return -1;
}

static char last_addr[INET_ADDRSTRLEN], fallback[INET_ADDRSTRLEN];

const char *esp_address(void)
{
	return last_addr;
}

void esp_set_fallback(const char *ip)
{
	struct in_addr a;

	fallback[0] = 0;
	if (ip && inet_pton(AF_INET, ip, &a) == 1)
		snprintf(fallback, sizeof(fallback), "%s", ip);
}

int esp_connect(const char *host, int port, const char *key_b64, char *err, size_t errlen)
{
	static const char proto[] = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
	static const uint8_t prologue[] = "NoiseAPIInit\0\0";
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
	uint8_t psk[48], epub[32], msg[64], d[MAX_FRAME], dh[32], k1[32], k2[32];
	size_t len, keylen = strlen(key_b64);
	EVP_PKEY *e;
	struct pb b = { .len = 0 };
	int r, flags, pad = 0;

	esp_close();
	/* the key: 32 bytes, base64 */
	if (keylen != 44)
		return fail(err, errlen, "the API key is not 32 bytes in base64");
	pad = key_b64[43] == '=';
	if (EVP_DecodeBlock(psk, (const uint8_t *)key_b64, 44) - pad != 32)
		return fail(err, errlen, "the API key is not valid base64");
	if (resolve(host, &addr.sin_addr) &&
	    (!fallback[0] || inet_pton(AF_INET, fallback, &addr.sin_addr) != 1))
		return fail(err, errlen, "address not found");

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return fail(err, errlen, "no socket");
	flags = fcntl(sock, F_GETFL);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);
	r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (r < 0 && errno == EINPROGRESS) {
		struct pollfd p = { .fd = sock, .events = POLLOUT };
		int so = 0;
		socklen_t sl = sizeof(so);

		r = poll(&p, 1, 3000) == 1 && !getsockopt(sock, SOL_SOCKET, SO_ERROR, &so, &sl) && !so ? 0 : -1;
	}
	if (r < 0)
		return fail(err, errlen, "no connection (is the ESP on and on the network?)");
	fcntl(sock, F_SETFL, flags);
	rxlen = 0;
	last_rx = last_ping = now_ms();

	/* handshake state */
	memset(&ss, 0, sizeof(ss));
	sha256((const uint8_t *)proto, sizeof(proto) - 1, NULL, 0, ss.h);
	memcpy(ss.ck, ss.h, 32);
	mix_hash(prologue, sizeof(prologue) - 1);

	/* -> psk, e */
	mix_key_and_hash(psk, 32);
	e = x25519_new(epub);
	if (!e)
		return fail(err, errlen, "no key pair");
	mix_hash(epub, 32);
	mix_key(epub, 32);
	msg[0] = 0;		/* handshake status: OK */
	memcpy(msg + 1, epub, 32);
	if (aead_encrypt(&ss.c, ss.h, 32, NULL, 0, msg + 33)) {
		EVP_PKEY_free(e);
		return fail(err, errlen, "encryption failed");
	}
	mix_hash(msg + 33, 16);
	if (send_frame(NULL, 0) || send_frame(msg, 49)) {
		EVP_PKEY_free(e);
		return fail(err, errlen, "connection closed");
	}

	/* <- server hello: [1][name]\0[mac]\0 */
	if (read_frame(d, &len) || len < 1 || d[0] != 1) {
		EVP_PKEY_free(e);
		return fail(err, errlen, "no answer as an encrypted ESPHome device (is encryption on?)");
	}
	/* <- e, ee */
	if (read_frame(d, &len) || len < 1) {
		EVP_PKEY_free(e);
		return fail(err, errlen, "the handshake was not answered");
	}
	if (d[0] != 0) {
		EVP_PKEY_free(e);
		/* "Handshake MAC failure" is a wrong key */
		snprintf(err, errlen, "refused: %.*s%s", (int)(len > 60 ? 60 : len - 1), d + 1,
			 len > 6 && memmem(d + 1, len - 1, "MAC", 3) ? " (wrong API key?)" : "");
		esp_close();
		return -1;
	}
	if (len != 1 + 32 + 16) {
		EVP_PKEY_free(e);
		return fail(err, errlen, "unexpected handshake reply");
	}
	mix_hash(d + 1, 32);
	mix_key(d + 1, 32);
	r = x25519(e, d + 1, dh);
	EVP_PKEY_free(e);
	if (r)
		return fail(err, errlen, "key agreement failed");
	mix_key(dh, 32);
	if (aead_decrypt(&ss.c, ss.h, 32, d + 33, 16, msg))
		return fail(err, errlen, "wrong API key");
	mix_hash(d + 33, 16);
	hkdf(ss.ck, NULL, 0, k1, k2, NULL);
	memcpy(tx_cipher.k, k1, 32);
	memcpy(rx_cipher.k, k2, 32);
	tx_cipher.n = rx_cipher.n = 0;

	/* hello, device info, entities, states */
	pb_str(&b, 1, "ReToon toon-boilerd");
	pb_uint(&b, 2, 1);
	pb_uint(&b, 3, 10);
	if (send_msg(1, &b) || wait_for(2))
		return fail(err, errlen, "no hello from the device");
	dev_name[0] = dev_version[0] = dev_project[0] = dev_project_version[0] = 0;
	if (send_msg(9, NULL) || wait_for(10))
		return fail(err, errlen, "no device info");
	n_ents = 0;
	if (send_msg(11, NULL) || wait_for(19))
		return fail(err, errlen, "no entity list");
	if (send_msg(20, NULL))
		return fail(err, errlen, "connection closed");
	inet_ntop(AF_INET, &addr.sin_addr, last_addr, sizeof(last_addr));
	return 0;
}

void esp_close(void)
{
	if (sock >= 0)
		close(sock);
	sock = -1;
	rxlen = 0;
}

int esp_connected(void) { return sock >= 0; }
int esp_fd(void) { return sock; }

int esp_poll(void)
{
	uint8_t d[MAX_FRAME];
	size_t len;
	int r;

	if (sock < 0)
		return -1;
	r = fill(0);
	if (r < 0) {
		esp_close();
		return -1;
	}
	while ((r = take_frame(d, &len)) > 0)
		if (handle_frame(d, len) < 0) {
			esp_close();
			return -1;
		}
	if (r < 0) {
		esp_close();
		return -1;
	}
	return 0;
}

int esp_tick(void)
{
	long long t = now_ms();

	if (sock < 0)
		return -1;
	if (t - last_rx > DEAD_S * 1000LL) {
		esp_close();
		return -1;
	}
	if (t - last_rx > PING_S * 1000LL && t - last_ping > PING_S * 1000LL) {
		last_ping = t;
		if (send_msg(7, NULL)) {
			esp_close();
			return -1;
		}
	}
	return 0;
}

int esp_state(const char *object_id, double *value, long long *age_ms)
{
	struct entity *e = ent_by_id(object_id);

	if (!e || !e->known)
		return -1;
	*value = e->value;
	if (age_ms)
		*age_ms = now_ms() - e->when;
	return 0;
}

int esp_has_entity(const char *object_id)
{
	return ent_by_id(object_id) != NULL;
}

int esp_set_switch(const char *object_id, int on)
{
	struct entity *e = ent_by_id(object_id);
	struct pb b = { .len = 0 };

	if (sock < 0 || !e || e->kind != ENT_SWITCH)
		return -1;
	pb_fixed32(&b, 1, e->key);
	pb_uint(&b, 2, !!on);
	return send_msg(33, &b);
}

int esp_set_number(const char *object_id, double value)
{
	struct entity *e = ent_by_id(object_id);
	struct pb b = { .len = 0 };
	float f = value;
	uint32_t v;

	if (sock < 0 || !e || e->kind != ENT_NUMBER)
		return -1;
	memcpy(&v, &f, 4);
	pb_fixed32(&b, 1, e->key);
	pb_fixed32(&b, 2, v);
	return send_msg(51, &b);
}

const char *esp_name(void) { return dev_name; }
const char *esp_version(void) { return dev_version; }
const char *esp_project(void) { return dev_project; }
const char *esp_project_version(void) { return dev_project_version; }
