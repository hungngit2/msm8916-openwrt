/*
 * Phase 3: DIAG_NV_READ_F (cmd 0x26) - read-only NV item query.
 * Classic Qualcomm DIAG NV item request layout:
 *   u8  cmd_code;      // 0x26
 *   u16 nv_item;       // item number, little-endian
 *   u8  nv_data[128];  // data buffer (zeroed on request)
 *   u16 nv_stat;       // status (zeroed on request; meaningful in response)
 * Response has the same shape, cmd_code echoed, nv_data/nv_stat filled in.
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

int main(int argc, char **argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: %s <ctrl-device> <channel-name> <nv-item>\n", argv[0]);
		return 2;
	}
	const char *ctrl_path = argv[1];
	const char *chan_name = argv[2];
	int nv_item = atoi(argv[3]);

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
	if (!new_dev[0]) { fprintf(stderr, "no new rpmsg device appeared\n"); return 1; }

	int fd = open(new_dev, O_RDWR);
	if (fd < 0) { fprintf(stderr, "open(%s): %s\n", new_dev, strerror(errno)); return 1; }

	/* Drain any unsolicited packet on open. */
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, 500) > 0) {
			uint8_t junk[512];
			read(fd, junk, sizeof(junk));
		}
	}

	uint8_t req[1 + 2 + NV_DATA_SIZE + 2];
	memset(req, 0, sizeof(req));
	req[0] = DIAG_NV_READ_F;
	req[1] = nv_item & 0xff;
	req[2] = (nv_item >> 8) & 0xff;
	/* nv_data[128] stays zero, nv_stat[2] stays zero */

	uint8_t framed[512];
	size_t framed_len = diag_frame_encode(req, sizeof(req), framed, sizeof(framed));
	if (!framed_len) { fprintf(stderr, "encode failed\n"); return 1; }

	printf("Requesting NV item %d (0x%04x) ...\n", nv_item, nv_item);
	write(fd, framed, framed_len);

	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int pr = poll(&pfd, 1, 3000);
	if (pr <= 0) { printf("no response within 3s\n"); return 1; }

	uint8_t rx[512];
	ssize_t rn = read(fd, rx, sizeof(rx));
	if (rn < 0) { fprintf(stderr, "read: %s\n", strerror(errno)); return 1; }

	printf("RX (%zd bytes):", rn);
	for (ssize_t i = 0; i < rn; i++) printf(" %02x", rx[i]);
	printf("\n");

	uint8_t payload[512];
	int plen = diag_frame_decode(rx, rn, payload, sizeof(payload));
	if (plen < 0) { fprintf(stderr, "frame decode/CRC failed\n"); return 1; }

	printf("Decoded payload (%d bytes, CRC OK)\n", plen);
	if (plen < 3) { printf("payload too short to parse\n"); return 1; }

	uint8_t  cmd = payload[0];
	uint16_t item = payload[1] | (payload[2] << 8);
	printf("cmd_code=0x%02x (expect 0x%02x)  item=%d\n", cmd, DIAG_NV_READ_F, item);

	if (plen >= 3 + NV_DATA_SIZE + 2) {
		uint16_t stat = payload[3 + NV_DATA_SIZE] | (payload[3 + NV_DATA_SIZE + 1] << 8);
		printf("nv_stat=%d\n", stat);
		printf("nv_data (hex):");
		for (int i = 0; i < NV_DATA_SIZE; i++) printf(" %02x", payload[3 + i]);
		printf("\n");
		printf("nv_data (ascii): ");
		for (int i = 0; i < NV_DATA_SIZE; i++) {
			uint8_t c = payload[3 + i];
			putchar((c >= 32 && c < 127) ? c : '.');
		}
		printf("\n");
	} else {
		printf("payload shorter than expected fixed layout (%d vs %d)\n",
		       plen, 3 + NV_DATA_SIZE + 2);
	}

	close(fd);
	close(cfd);
	return 0;
}
