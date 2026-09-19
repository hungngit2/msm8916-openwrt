/*
 * Phase 4: sweep a list of NV items (read-only), reporting nv_stat and a
 * short hex preview for each, to narrow down candidates empirically.
 * Reuses one endpoint for the whole sweep (created once, not per-item).
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

#define DIAG_NV_READ_F 0x26
#define NV_DATA_SIZE   128

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

static int open_diag_endpoint(const char *ctrl_path, const char *chan_name) {
	char before[64][64], after[64][64];
	int n_before, n_after;
	list_rpmsg_devs(before, &n_before);

	int cfd = open(ctrl_path, O_RDWR);
	if (cfd < 0) { fprintf(stderr, "open(%s): %s\n", ctrl_path, strerror(errno)); return -1; }

	struct rpmsg_endpoint_info info;
	memset(&info, 0, sizeof(info));
	strncpy(info.name, chan_name, sizeof(info.name) - 1);
	info.src = RPMSG_ADDR_ANY;
	info.dst = RPMSG_ADDR_ANY;
	if (ioctl(cfd, RPMSG_CREATE_EPT_IOCTL, &info) < 0) {
		fprintf(stderr, "RPMSG_CREATE_EPT_IOCTL: %s\n", strerror(errno));
		close(cfd);
		return -1;
	}

	char new_dev[80] = {0};
	for (int tries = 0; tries < 20 && !new_dev[0]; tries++) {
		list_rpmsg_devs(after, &n_after);
		for (int i = 0; i < n_after; i++) {
			int found = 0;
			for (int j = 0; j < n_before; j++)
				if (strcmp(after[i], before[j]) == 0) { found = 1; break; }
			if (!found) { snprintf(new_dev, sizeof(new_dev), "/dev/%s", after[i]); break; }
		}
		if (!new_dev[0]) usleep(100000);
	}
	close(cfd); /* endpoint itself stays open via the fd returned below */
	if (!new_dev[0]) { fprintf(stderr, "no new rpmsg device appeared\n"); return -1; }

	int fd = open(new_dev, O_RDWR);
	if (fd < 0) { fprintf(stderr, "open(%s): %s\n", new_dev, strerror(errno)); return -1; }

	/* Drain any unsolicited packet on open. */
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	if (poll(&pfd, 1, 500) > 0) {
		uint8_t junk[512];
		read(fd, junk, sizeof(junk));
	}
	return fd;
}

static int nv_read(int fd, int nv_item, uint16_t *out_stat, uint8_t *out_data) {
	uint8_t req[1 + 2 + NV_DATA_SIZE + 2];
	memset(req, 0, sizeof(req));
	req[0] = DIAG_NV_READ_F;
	req[1] = nv_item & 0xff;
	req[2] = (nv_item >> 8) & 0xff;

	uint8_t framed[512];
	size_t framed_len = diag_frame_encode(req, sizeof(req), framed, sizeof(framed));
	if (!framed_len) return -1;
	if (write(fd, framed, framed_len) < 0) return -1;

	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int pr = poll(&pfd, 1, 2000);
	if (pr <= 0) return -1;

	uint8_t rx[512];
	ssize_t rn = read(fd, rx, sizeof(rx));
	if (rn < 0) return -1;

	uint8_t payload[512];
	int plen = diag_frame_decode(rx, rn, payload, sizeof(payload));
	if (plen < 3 + NV_DATA_SIZE + 2) return -1;

	if (payload[0] != DIAG_NV_READ_F) return -1;
	*out_stat = payload[3 + NV_DATA_SIZE] | (payload[3 + NV_DATA_SIZE + 1] << 8);
	memcpy(out_data, payload + 3, NV_DATA_SIZE);
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <ctrl-device> <channel-name> <nv-item> [nv-item ...]\n", argv[0]);
		return 2;
	}

	int fd = open_diag_endpoint(argv[1], argv[2]);
	if (fd < 0) return 1;

	for (int a = 3; a < argc; a++) {
		int item = atoi(argv[a]);
		uint16_t stat = 0xffff;
		uint8_t data[NV_DATA_SIZE];
		int rc = nv_read(fd, item, &stat, data);
		if (rc < 0) {
			printf("item %5d: READ FAILED (no/bad response)\n", item);
			continue;
		}
		int nonzero = 0;
		for (int i = 0; i < NV_DATA_SIZE; i++) if (data[i]) { nonzero = 1; break; }
		printf("item %5d: stat=%-3u %s  first16:", item, stat, nonzero ? "data" : "allzero");
		for (int i = 0; i < 16; i++) printf(" %02x", data[i]);
		printf("\n");
	}

	close(fd);
	return 0;
}
