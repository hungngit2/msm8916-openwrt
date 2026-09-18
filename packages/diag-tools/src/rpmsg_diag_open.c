/*
 * Phase 1: create an rpmsg endpoint for the modem's "DIAG" SMD channel
 * and confirm we can open the resulting character device.
 *
 * Usage: rpmsg_diag_open <ctrl-device> <channel-name>
 *   e.g. rpmsg_diag_open /dev/rpmsg_ctrl2 DIAG
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/rpmsg.h>

/* Snapshot /dev for rpmsgN device nodes, to detect the one just created. */
static void list_rpmsg_devs(char names[][64], int *count) {
	DIR *d = opendir("/dev");
	*count = 0;
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "rpmsg", 5) == 0 &&
		    strncmp(e->d_name, "rpmsg_ctrl", 10) != 0) {
			snprintf(names[*count], 64, "%s", e->d_name);
			(*count)++;
		}
	}
	closedir(d);
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

	int fd = open(ctrl_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", ctrl_path, strerror(errno));
		return 1;
	}

	struct rpmsg_endpoint_info info;
	memset(&info, 0, sizeof(info));
	strncpy(info.name, chan_name, sizeof(info.name) - 1);
	info.src = RPMSG_ADDR_ANY;
	info.dst = RPMSG_ADDR_ANY;

	printf("Requesting endpoint for channel \"%s\" via %s ...\n", chan_name, ctrl_path);
	if (ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &info) < 0) {
		fprintf(stderr, "RPMSG_CREATE_EPT_IOCTL: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("ioctl succeeded.\n");

	list_rpmsg_devs(after, &n_after);

	/* Find the new node present in 'after' but not in 'before'. */
	char new_dev[80] = {0};
	for (int i = 0; i < n_after; i++) {
		int found = 0;
		for (int j = 0; j < n_before; j++) {
			if (strcmp(after[i], before[j]) == 0) { found = 1; break; }
		}
		if (!found) {
			snprintf(new_dev, sizeof(new_dev), "/dev/%s", after[i]);
			break;
		}
	}

	if (new_dev[0] == '\0') {
		fprintf(stderr, "ioctl succeeded but no new /dev/rpmsgN node detected\n");
		close(fd);
		return 1;
	}

	printf("New endpoint device: %s\n", new_dev);

	int efd = open(new_dev, O_RDWR);
	if (efd < 0) {
		fprintf(stderr, "open(%s): %s\n", new_dev, strerror(errno));
		close(fd);
		return 1;
	}
	printf("Successfully opened %s (fd=%d). Endpoint is live.\n", new_dev, efd);

	close(efd);
	close(fd);
	return 0;
}
