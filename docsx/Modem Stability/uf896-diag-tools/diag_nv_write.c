/*
 * Phase 5: DIAG_NV_WRITE_F (cmd 0x27), with mandatory read-back-first
 * safety: this tool ALWAYS reads the item first and prints the exact
 * original bytes before writing anything, so a revert is always possible.
 *
 * Usage: diag_nv_write <ctrl> <chan> <item> <byte-offset> <new-byte-value>
 *   Only ONE byte of the 128-byte buffer is changed; everything else is
 *   written back exactly as read.
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

#define DIAG_NV_READ_F  0x26
#define DIAG_NV_WRITE_F 0x27
#define NV_DATA_SIZE    128

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
	close(cfd);
	if (!new_dev[0]) { fprintf(stderr, "no new rpmsg device appeared\n"); return -1; }
	int fd = open(new_dev, O_RDWR);
	if (fd < 0) { fprintf(stderr, "open(%s): %s\n", new_dev, strerror(errno)); return -1; }
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
	size_t fl = diag_frame_encode(req, sizeof(req), framed, sizeof(framed));
	if (!fl || write(fd, framed, fl) < 0) return -1;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	if (poll(&pfd, 1, 2000) <= 0) return -1;
	uint8_t rx[512];
	ssize_t rn = read(fd, rx, sizeof(rx));
	if (rn < 0) return -1;
	uint8_t payload[512];
	int plen = diag_frame_decode(rx, rn, payload, sizeof(payload));
	if (plen < 3 + NV_DATA_SIZE + 2 || payload[0] != DIAG_NV_READ_F) return -1;
	*out_stat = payload[3 + NV_DATA_SIZE] | (payload[3 + NV_DATA_SIZE + 1] << 8);
	memcpy(out_data, payload + 3, NV_DATA_SIZE);
	return 0;
}

static int nv_write(int fd, int nv_item, const uint8_t *data, uint16_t *out_stat) {
	uint8_t req[1 + 2 + NV_DATA_SIZE + 2];
	memset(req, 0, sizeof(req));
	req[0] = DIAG_NV_WRITE_F;
	req[1] = nv_item & 0xff;
	req[2] = (nv_item >> 8) & 0xff;
	memcpy(req + 3, data, NV_DATA_SIZE);
	/* nv_stat left 0 in request */
	uint8_t framed[512];
	size_t fl = diag_frame_encode(req, sizeof(req), framed, sizeof(framed));
	if (!fl || write(fd, framed, fl) < 0) return -1;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	if (poll(&pfd, 1, 3000) <= 0) return -1;
	uint8_t rx[512];
	ssize_t rn = read(fd, rx, sizeof(rx));
	if (rn < 0) return -1;
	uint8_t payload[512];
	int plen = diag_frame_decode(rx, rn, payload, sizeof(payload));
	if (plen < 3 + NV_DATA_SIZE + 2 || payload[0] != DIAG_NV_WRITE_F) return -1;
	*out_stat = payload[3 + NV_DATA_SIZE] | (payload[3 + NV_DATA_SIZE + 1] << 8);
	return 0;
}

static void print_hex(const char *label, const uint8_t *d, int n) {
	printf("%s:", label);
	for (int i = 0; i < n; i++) printf(" %02x", d[i]);
	printf("\n");
}

int main(int argc, char **argv) {
	/* modes:
	 *   diag_nv_write <ctrl> <chan> read <item>
	 *   diag_nv_write <ctrl> <chan> setbyte <item> <offset> <value>
	 *   diag_nv_write <ctrl> <chan> restore <item> <hex128bytes-as-one-arg>
	 */
	if (argc < 5) {
		fprintf(stderr, "usage:\n"
			"  %s <ctrl> <chan> read <item>\n"
			"  %s <ctrl> <chan> setbyte <item> <offset> <value>\n",
			argv[0], argv[0]);
		return 2;
	}
	const char *ctrl_path = argv[1];
	const char *chan_name = argv[2];
	const char *mode = argv[3];
	int item = atoi(argv[4]);

	int fd = open_diag_endpoint(ctrl_path, chan_name);
	if (fd < 0) return 1;

	uint16_t stat;
	uint8_t data[NV_DATA_SIZE];
	if (nv_read(fd, item, &stat, data) < 0) {
		fprintf(stderr, "initial read failed\n");
		close(fd);
		return 1;
	}
	printf("BEFORE: item=%d stat=%u\n", item, stat);
	print_hex("BEFORE data", data, NV_DATA_SIZE);

	if (strcmp(mode, "read") == 0) {
		close(fd);
		return 0;
	}

	if (strcmp(mode, "setbyte") == 0) {
		if (argc < 7) { fprintf(stderr, "setbyte needs <offset> <value>\n"); close(fd); return 2; }
		int off = atoi(argv[5]);
		int val = atoi(argv[6]);
		if (off < 0 || off >= NV_DATA_SIZE) { fprintf(stderr, "offset out of range\n"); close(fd); return 2; }

		uint8_t newdata[NV_DATA_SIZE];
		memcpy(newdata, data, NV_DATA_SIZE);
		newdata[off] = (uint8_t)val;

		printf("Writing item %d, offset %d: %02x -> %02x (rest unchanged)\n",
		       item, off, data[off], newdata[off]);

		uint16_t wstat;
		if (nv_write(fd, item, newdata, &wstat) < 0) {
			fprintf(stderr, "write failed (no/bad response) -- NOT confirmed changed\n");
			close(fd);
			return 1;
		}
		printf("WRITE nv_stat=%u\n", wstat);

		uint16_t stat2;
		uint8_t verify[NV_DATA_SIZE];
		if (nv_read(fd, item, &stat2, verify) < 0) {
			fprintf(stderr, "verify read failed\n");
			close(fd);
			return 1;
		}
		printf("AFTER: item=%d stat=%u\n", item, stat2);
		print_hex("AFTER data", verify, NV_DATA_SIZE);
		if (memcmp(verify, newdata, NV_DATA_SIZE) == 0)
			printf("VERIFY: matches intended write\n");
		else
			printf("VERIFY: MISMATCH -- data differs from what we wrote!\n");

		close(fd);
		return 0;
	}

	fprintf(stderr, "unknown mode: %s\n", mode);
	close(fd);
	return 2;
}
