// SPDX-License-Identifier: GPL-2.0-or-later
/* toon-ui: small helpers - key=value files, the clock, background commands */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

/* "retoon:<name>" is the newest retoon/<name> (bus_watch'ed in local.c) */
FILE *state_open(const char *path)
{
	if (!strncmp(path, STATE, strlen(STATE)))
		return bus_fopen(path + strlen(STATE));
	return fopen(path, "r");
}

int file_value(const char *path, const char *key, char *out, size_t len)
{
	char line[4096];
	size_t klen = strlen(key);
	FILE *f;
	int found = -1;

	if (!strncmp(path, STATE, strlen(STATE)))
		return bus_value(path + strlen(STATE), key, out, len);
	f = fopen(path, "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			line[strcspn(line, "\r\n")] = 0;
			snprintf(out, len, "%s", line + klen + 1);
			found = 0;
		}
	fclose(f);
	return found;
}

uint32_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

/* retoon/ui: what the web interface shows about this program */
static char status_keys[8][24], status_vals[8][1000];	/* pages: dash.c */

void set_status(const char *key, const char *value)
{
	char *out = NULL;
	size_t out_len = 0;
	FILE *f;
	int i;

	for (i = 0; i < 8 && status_keys[i][0] && strcmp(status_keys[i], key); i++)
		;
	if (i == 8 || (status_keys[i][0] && !strcmp(status_vals[i], value)))
		return;
	snprintf(status_keys[i], sizeof(status_keys[i]), "%s", key);
	snprintf(status_vals[i], sizeof(status_vals[i]), "%s", value);
	f = open_memstream(&out, &out_len);
	if (!f)
		return;
	for (i = 0; i < 8 && status_keys[i][0]; i++)
		fprintf(f, "%s=%s\n", status_keys[i], status_vals[i]);
	fclose(f);
	bus_put("ui", out);
	free(out);
}

struct job {
	pid_t pid;
	int fd;
	char *out;
	size_t len;
	job_cb cb;
	void *user;
	struct job *next;
};
static struct job *jobs;

int job_run(char *const argv[], job_cb cb, void *user)
{
	int p[2];
	struct job *j;
	pid_t pid;

	if (pipe(p))
		return -1;
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (!pid) {
		int null = open("/dev/null", O_RDWR);

		dup2(null, 0);
		dup2(p[1], 1);
		dup2(null, 2);
		for (int fd = 3; fd < 64; fd++)
			close(fd);
		execv(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	j = calloc(1, sizeof(*j));
	j->pid = pid;
	j->fd = p[0];
	j->cb = cb;
	j->user = user;
	j->out = calloc(1, 1);
	j->next = jobs;
	jobs = j;
	return 0;
}

void jobs_poll(void)
{
	struct job **pp = &jobs, *j;
	char buf[4096];

	while ((j = *pp)) {
		ssize_t n = read(j->fd, buf, sizeof(buf));

		if (n > 0) {
			j->out = realloc(j->out, j->len + n + 1);
			memcpy(j->out + j->len, buf, n);
			j->len += n;
			j->out[j->len] = 0;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			pp = &j->next;
			continue;
		}
		/* end of output: the command is done */
		int status = 0;

		close(j->fd);
		waitpid(j->pid, &status, 0);
		*pp = j->next;
		if (j->cb)
			j->cb(WIFEXITED(status) ? WEXITSTATUS(status) : -1, j->out, j->user);
		free(j->out);
		free(j);
	}
}
