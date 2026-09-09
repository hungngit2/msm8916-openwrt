/*
 * DIAG EFS2 (file-based NV) access -- a completely different NV storage
 * subsystem from the classic numbered NV items (DIAG_NV_READ_F/WRITE_F,
 * see diag_nv_read.c/diag_nv_write.c). Modern Qualcomm platforms gate
 * IMS/VoLTE config through file-based items like
 * "/nv/item_files/ims/IMS_enable", not numbered items -- this tool reads
 * and writes those.
 *
 * Protocol: DIAG_SUBSYS_CMD_F (0x4B), subsystem "FS/EFS2" = 19. Byte
 * layouts below were derived from the JohnBel/EfsTools C# implementation
 * (an open-source EFS2 diag client), not from Qualcomm documentation --
 * verify against a real device before trusting results.
 *
 * Usage:
 *   diag_efs <ctrl> <chan> read  <path>
 *   diag_efs <ctrl> <chan> write <path> <hex-bytes-one-arg>
 *
 * "write" always reads the file first and prints the original bytes
 * before writing anything, mirroring diag_nv_write.c's safety pattern.
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

#define DIAG_SUBSYS_CMD_F 0x4B
#define SUBSYS_EFS        19

#define EFS_HELLO 0
#define EFS_OPEN  2
#define EFS_CLOSE 3
#define EFS_READ  4
#define EFS_WRITE 5

/* EfsFileFlag (octal in the original C#, kept as-is) */
#define EFS_O_RDONLY 000000
#define EFS_O_WRONLY 000001
#define EFS_O_RDWR   000002
#define EFS_O_CREAT  000100
#define EFS_O_TRUNC  001000

static int open_diag_endpoint(const char *ctrl_path, const char *chan_name) {
	char before[64][64], after[64][64];
	int n_before, n_after;

	DIR *d = opendir("/dev");
	n_before = 0;
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (strncmp(e->d_name, "rpmsg", 5) == 0 &&
			    strncmp(e->d_name, "rpmsg_ctrl", 10) != 0) {
				snprintf(before[n_before], 64, "%.63s", e->d_name);
				n_before++;
			}
		}
		closedir(d);
	}

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
		d = opendir("/dev");
		n_after = 0;
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) != NULL) {
				if (strncmp(e->d_name, "rpmsg", 5) == 0 &&
				    strncmp(e->d_name, "rpmsg_ctrl", 10) != 0) {
					snprintf(after[n_after], 64, "%.63s", e->d_name);
					n_after++;
				}
			}
			closedir(d);
		}
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

/* A single read() on the rpmsg char device can return more than one
 * complete HDLC-framed DIAG packet concatenated together (and in
 * principle a frame split across two reads). This accumulator extracts
 * exactly one trailer-delimited frame at a time, escape-aware (a
 * 0x7d-escaped byte pair must not be mistaken for the real trailer). */
static uint8_t g_acc[4096];
static size_t g_acc_len = 0;

static int read_one_frame(int fd, uint8_t *out, size_t out_cap) {
	for (;;) {
		size_t trailer_idx = (size_t)-1;
		for (size_t i = 0; i < g_acc_len; i++) {
			if (g_acc[i] == DIAG_ESC) { i++; continue; }
			if (g_acc[i] == DIAG_TRAILER) { trailer_idx = i; break; }
		}
		if (trailer_idx != (size_t)-1) {
			size_t frame_len = trailer_idx + 1;
			int plen = diag_frame_decode(g_acc, frame_len, out, out_cap);
			memmove(g_acc, g_acc + frame_len, g_acc_len - frame_len);
			g_acc_len -= frame_len;
			if (plen < 0) continue;
			return plen;
		}
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, 3000) <= 0) return -1;
		if (g_acc_len >= sizeof(g_acc)) return -1;
		ssize_t rn = read(fd, g_acc + g_acc_len, sizeof(g_acc) - g_acc_len);
		if (rn <= 0) return -1;
		g_acc_len += (size_t)rn;
	}
}

/* Send one DIAG_SUBSYS_CMD_F request, return decoded response payload
 * length, or -1. Response's own header (echoing cmd/subsys/subcmd) is
 * left in place at the start of `resp` -- callers index from there,
 * matching the offsets documented per-command below.
 *
 * Two different response header shapes coexist on this firmware:
 * EFS_OPEN/EFS_OPENDIR echo the request's 4-byte header verbatim;
 * EFS_HELLO/EFS_READDIR/EFS_CLOSEDIR prepend one extra byte before that
 * same 4-byte echo. Both are recognized and normalized to the same
 * resp+4-relative offsets. */
static int efs_xfer(int fd, const uint8_t *req, size_t req_len,
		     uint8_t *resp, size_t resp_cap) {
	uint8_t framed[1024];
	size_t fl = diag_frame_encode(req, req_len, framed, sizeof(framed));
	if (!fl || write(fd, framed, fl) < 0) return -1;

	uint8_t raw[1024];
	int plen = read_one_frame(fd, raw, sizeof(raw));
	if (plen < 4) return -1;

	if (memcmp(raw, req, 4) == 0) {
		if ((size_t)plen > resp_cap) return -1;
		memcpy(resp, raw, (size_t)plen);
		return plen; /* plain 4-byte header */
	}
	if (plen >= 5 && memcmp(raw + 1, req, 4) == 0) {
		if ((size_t)(plen - 1) > resp_cap) return -1;
		memcpy(resp, raw + 1, (size_t)plen - 1);
		return plen - 1; /* 1-byte-prefixed header, normalized */
	}
	fprintf(stderr, "efs_xfer: gave up waiting for matching response\n");
	return -1;
}

static void put_header(uint8_t *buf, uint16_t subsys_cmd) {
	buf[0] = DIAG_SUBSYS_CMD_F;
	buf[1] = SUBSYS_EFS;
	buf[2] = subsys_cmd & 0xff;
	buf[3] = (subsys_cmd >> 8) & 0xff;
}

static void put_u32(uint8_t *buf, uint32_t v) {
	buf[0] = v & 0xff;
	buf[1] = (v >> 8) & 0xff;
	buf[2] = (v >> 16) & 0xff;
	buf[3] = (v >> 24) & 0xff;
}

static uint32_t get_u32(const uint8_t *buf) {
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* Best-effort: some firmware requires a DIAG EFS_HELLO handshake before
 * anything else works, some don't care. Log and continue either way. */
static void efs_hello(int fd) {
	uint8_t req[44];
	put_header(req, EFS_HELLO);
	put_u32(req + 4,  0x100000); /* window size */
	put_u32(req + 8,  0x100000); /* window byte size */
	put_u32(req + 12, 0x100000);
	put_u32(req + 16, 0x100000);
	put_u32(req + 20, 0x100000);
	put_u32(req + 24, 0x100000);
	put_u32(req + 28, 1); /* version */
	put_u32(req + 32, 1); /* min version */
	put_u32(req + 36, 1); /* max version */
	memset(req + 40, 0xff, 4);

	uint8_t resp[512];
	int rl = efs_xfer(fd, req, sizeof(req), resp, sizeof(resp));
	if (rl < 0)
		fprintf(stderr, "EFS_HELLO: no/bad response (continuing anyway)\n");
	else
		fprintf(stderr, "EFS_HELLO: ok (%d bytes)\n", rl);
}

/* Returns fd >= 0 on success, or -1. *out_err set to the EFS error code. */
static int efs_open(int fd, const char *path, uint32_t flags, uint32_t perm,
		     int32_t *out_err) {
	uint8_t req[512];
	size_t plen = strlen(path);
	if (13 + plen > sizeof(req)) return -1;
	put_header(req, EFS_OPEN);
	put_u32(req + 4, flags);
	put_u32(req + 8, perm);
	memcpy(req + 12, path, plen);
	req[12 + plen] = 0;

	uint8_t resp[512];
	int rl = efs_xfer(fd, req, 13 + plen, resp, sizeof(resp));
	if (rl < 12) { fprintf(stderr, "EFS_OPEN: bad/short response\n"); return -1; }
	int32_t efs_fd = (int32_t)get_u32(resp + 4);
	*out_err = (int32_t)get_u32(resp + 8);
	return efs_fd; /* caller checks efs_fd < 0 */
}

static int efs_read(int fd, int32_t efs_fd, uint32_t size, uint32_t offset,
		     uint8_t *out, uint32_t *out_len, int32_t *out_err) {
	uint8_t req[16];
	put_header(req, EFS_READ);
	put_u32(req + 4, (uint32_t)efs_fd);
	put_u32(req + 8, size);
	put_u32(req + 12, offset);

	uint8_t resp[2048];
	int rl = efs_xfer(fd, req, sizeof(req), resp, sizeof(resp));
	if (rl < 20) {
		fprintf(stderr, "EFS_READ: short first frame (rl=%d):", rl);
		for (int i = 0; i < rl && i < 64; i++) fprintf(stderr, " %02x", resp[i]);
		fprintf(stderr, "\n");
		/* Maybe the data comes as a second, separate frame. */
		uint8_t raw2[2048];
		int rl2 = read_one_frame(fd, raw2, sizeof(raw2));
		fprintf(stderr, "EFS_READ: follow-up frame (rl2=%d):", rl2);
		for (int i = 0; i < rl2 && i < 64; i++) fprintf(stderr, " %02x", raw2[i]);
		fprintf(stderr, "\n");
		return -1;
	}
	uint32_t bytes_read = get_u32(resp + 12);
	*out_err = (int32_t)get_u32(resp + 16);
	if ((size_t)rl < 20 + bytes_read) { fprintf(stderr, "EFS_READ: truncated payload\n"); return -1; }
	if (bytes_read > *out_len) bytes_read = *out_len;
	memcpy(out, resp + 20, bytes_read);
	*out_len = bytes_read;
	return 0;
}

static int efs_write(int fd, int32_t efs_fd, uint32_t offset,
		      const uint8_t *data, uint32_t data_len,
		      uint32_t *out_written, int32_t *out_err) {
	uint8_t req[2048];
	if (12 + data_len > sizeof(req)) return -1;
	put_header(req, EFS_WRITE);
	put_u32(req + 4, (uint32_t)efs_fd);
	put_u32(req + 8, offset);
	memcpy(req + 12, data, data_len);

	uint8_t resp[512];
	int rl = efs_xfer(fd, req, 12 + data_len, resp, sizeof(resp));
	if (rl < 20) { fprintf(stderr, "EFS_WRITE: bad/short response\n"); return -1; }
	*out_written = get_u32(resp + 12);
	*out_err = (int32_t)get_u32(resp + 16);
	return 0;
}

static int efs_close(int fd, int32_t efs_fd, int32_t *out_err) {
	uint8_t req[8];
	put_header(req, EFS_CLOSE);
	put_u32(req + 4, (uint32_t)efs_fd);

	uint8_t resp[64];
	int rl = efs_xfer(fd, req, sizeof(req), resp, sizeof(resp));
	if (rl < 8) { fprintf(stderr, "EFS_CLOSE: bad/short response\n"); return -1; }
	*out_err = (int32_t)get_u32(resp + 4);
	return 0;
}

static void print_hex(const char *label, const uint8_t *d, uint32_t n) {
	printf("%s (%u bytes):", label, n);
	for (uint32_t i = 0; i < n; i++) printf(" %02x", d[i]);
	printf("\n");
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_cap) {
	size_t n = strlen(hex);
	if (n % 2 != 0 || n / 2 > out_cap) return -1;
	for (size_t i = 0; i < n / 2; i++) {
		unsigned int b;
		if (sscanf(hex + 2 * i, "%2x", &b) != 1) return -1;
		out[i] = (uint8_t)b;
	}
	return (int)(n / 2);
}

int main(int argc, char **argv) {
	if (argc < 5) {
		fprintf(stderr,
			"usage:\n"
			"  %s <ctrl> <chan> read  <efs-path>\n"
			"  %s <ctrl> <chan> write <efs-path> <hex-bytes>\n",
			argv[0], argv[0]);
		return 2;
	}
	const char *ctrl_path = argv[1];
	const char *chan_name = argv[2];
	const char *mode = argv[3];
	const char *path = argv[4];

	int fd = open_diag_endpoint(ctrl_path, chan_name);
	if (fd < 0) return 1;

	efs_hello(fd);

	int32_t err;
	uint32_t open_flags = (strcmp(mode, "write") == 0) ? EFS_O_RDWR : EFS_O_RDONLY;
	int32_t efs_fd = efs_open(fd, path, open_flags, 0, &err);
	if (efs_fd < 0) {
		fprintf(stderr, "open(%s) failed: efs_fd=%d err=%d\n", path, efs_fd, err);
		close(fd);
		return 1;
	}
	printf("open(%s) -> fd=%d\n", path, efs_fd);

	uint8_t buf[256];
	uint32_t buf_len = sizeof(buf);
	if (efs_read(fd, efs_fd, sizeof(buf), 0, buf, &buf_len, &err) < 0 || err != 0) {
		fprintf(stderr, "read failed: err=%d\n", err);
		efs_close(fd, efs_fd, &err);
		close(fd);
		return 1;
	}
	print_hex("BEFORE", buf, buf_len);

	if (strcmp(mode, "write") == 0) {
		if (argc < 6) { fprintf(stderr, "write needs <hex-bytes>\n"); efs_close(fd, efs_fd, &err); close(fd); return 2; }
		uint8_t newdata[256];
		int newlen = hex_decode(argv[5], newdata, sizeof(newdata));
		if (newlen < 0) { fprintf(stderr, "bad hex bytes\n"); efs_close(fd, efs_fd, &err); close(fd); return 2; }

		uint32_t written = 0;
		if (efs_write(fd, efs_fd, 0, newdata, (uint32_t)newlen, &written, &err) < 0 || err != 0) {
			fprintf(stderr, "write failed: err=%d\n", err);
			efs_close(fd, efs_fd, &err);
			close(fd);
			return 1;
		}
		printf("WRITE: %u bytes written, err=%d\n", written, err);

		efs_close(fd, efs_fd, &err);
		efs_fd = efs_open(fd, path, EFS_O_RDONLY, 0, &err);
		if (efs_fd < 0) { fprintf(stderr, "reopen for verify failed\n"); close(fd); return 1; }
		uint32_t verify_len = sizeof(buf);
		if (efs_read(fd, efs_fd, sizeof(buf), 0, buf, &verify_len, &err) < 0 || err != 0) {
			fprintf(stderr, "verify read failed: err=%d\n", err);
			efs_close(fd, efs_fd, &err);
			close(fd);
			return 1;
		}
		print_hex("AFTER", buf, verify_len);
		if (verify_len == (uint32_t)newlen && memcmp(buf, newdata, newlen) == 0)
			printf("VERIFY: matches intended write\n");
		else
			printf("VERIFY: MISMATCH -- data differs from what we wrote!\n");
	}

	efs_close(fd, efs_fd, &err);
	close(fd);
	return 0;
}
