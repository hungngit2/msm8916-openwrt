/*
 * Phase 2c: all-in-one -- create the DIAG endpoint, drain any unsolicited
 * packet(s), send DIAG_VERNO_F, drain response(s), all in one process so
 * the endpoint stays alive for the whole exchange.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/rpmsg.h>
#include "diag_proto.h"

static void list_rpmsg_devs(char names[][64], int *count) {
	DIR *d = opendir("/dev");
	*count = 0;
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "rpmsg", 5) == 0 &&
		    strncmp(e->d_name, "rpmsg_ctrl", 10) != 0) {
			snprintf(names[*count], 64, "%.63s", e->d_name);
			(*count)++;
		}
	}
	closedir(d);
}

static void dump_and_decode(const uint8_t *rx, ssize_t rn) {
	printf("RX (%zd bytes):", rn);
	for (ssize_t i = 0; i < rn; i++) printf(" %02x", rx[i]);
	printf("\n");
	uint8_t payload[512];
	int plen = diag_frame_decode(rx, rn, payload, sizeof(payload));
	if (plen < 0) {
		printf("  -> frame decode/CRC failed\n");
	} else {
		printf("  -> decoded payload (%d bytes, CRC OK):", plen);
		for (int i = 0; i < plen; i++) printf(" %02x", payload[i]);
		printf("\n");
	}
}

static void drain_pending(int fd, const char *label, int timeout_ms) {
	for (int i = 0; i < 5; i++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, timeout_ms);
		if (pr <= 0) break;
		uint8_t rx[512];
		ssize_t rn = read(fd, rx, sizeof(rx));
		if (rn <= 0) break;
		printf("[%s #%d] ", label, i);
		dump_and_decode(rx, rn);
	}
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s <ctrl-device> <channel-name>\n", argv[0]);
		return 2;
	}
	const char *ctrl_path = argv[1];
	const char *chan_name = argv[2];

	char before[64][64], after[64][64];
	int n_before, n_after;
	list_rpmsg_devs(before, &n_before);

	int cfd = open(ctrl_path, O_RDWR);
	if (cfd < 0) { fprintf(stderr, "open(%s): %s\n", ctrl_path, strerror(errno)); return 1; }

	struct rpmsg_endpoint_info info;
	memset(&info, 0, sizeof(info));
	strncpy(info.name, chan_name, sizeof(info.name) - 1);
	info.src = RPMSG_ADDR_ANY;
	info.dst = RPMSG_ADDR_ANY;

	if (ioctl(cfd, RPMSG_CREATE_EPT_IOCTL, &info) < 0) {
		fprintf(stderr, "RPMSG_CREATE_EPT_IOCTL: %s\n", strerror(errno));
		return 1;
	}

	/* Give mdev a moment to create the node. */
	for (int tries = 0; tries < 20; tries++) {
		list_rpmsg_devs(after, &n_after);
		int extra = n_after - n_before;
		if (extra > 0) break;
		usleep(100000);
	}

	char new_dev[80] = {0};
	for (int i = 0; i < n_after; i++) {
		int found = 0;
		for (int j = 0; j < n_before; j++)
			if (strcmp(after[i], before[j]) == 0) { found = 1; break; }
		if (!found) { snprintf(new_dev, sizeof(new_dev), "/dev/%s", after[i]); break; }
	}
	if (!new_dev[0]) { fprintf(stderr, "no new rpmsg device appeared\n"); return 1; }
	printf("endpoint device: %s\n", new_dev);

	int fd = open(new_dev, O_RDWR);
	if (fd < 0) { fprintf(stderr, "open(%s): %s\n", new_dev, strerror(errno)); return 1; }

	printf("--- draining pre-existing packets ---\n");
	drain_pending(fd, "pre", 800);

	uint8_t req[1] = { 0x00 };
	uint8_t framed[16];
	size_t framed_len = diag_frame_encode(req, sizeof(req), framed, sizeof(framed));
	printf("--- sending DIAG_VERNO_F ---\nTX (%zu bytes):", framed_len);
	for (size_t i = 0; i < framed_len; i++) printf(" %02x", framed[i]);
	printf("\n");
	write(fd, framed, framed_len);

	printf("--- draining response(s) ---\n");
	drain_pending(fd, "post", 3000);

	close(fd);
	close(cfd);
	return 0;
}
