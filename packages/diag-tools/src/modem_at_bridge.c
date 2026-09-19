/*
 * modem_at_bridge - Bi-directional AT command bridge between USB Gadget ACM
 * (e.g. /dev/ttyGS0) and Qualcomm modem AT character device (e.g. /dev/wwan0at1).
 *
 * Allows MikroTik RouterOS or other USB hosts to directly send AT commands,
 * monitor signal strength, manage APNs, and read/send SMS via RouterOS LTE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>

#define BUF_SIZE 4096
#define DEFAULT_GADGET_PORT "/dev/ttyGS0"
#define DEFAULT_MODEM_PORT   "/dev/wwan0at1"

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
	(void)sig;
	g_running = 0;
}

static const char *detect_modem_port(void) {
	static const char *candidates[] = {
		"/dev/wwan0at1",
		"/dev/wwan0at0",
		"/dev/smd11",
		"/dev/smd7",
		"/dev/ttyUSB1",
		"/dev/ttyUSB2",
		NULL
	};
	for (int i = 0; candidates[i]; i++) {
		if (access(candidates[i], R_OK | W_OK) == 0) {
			return candidates[i];
		}
	}
	return DEFAULT_MODEM_PORT;
}

static int configure_port(int fd, int is_tty) {
	if (!is_tty)
		return 0;

	struct termios tio;
	if (tcgetattr(fd, &tio) < 0) {
		return -1;
	}

	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);

	tio.c_cflag |= (CLOCAL | CREAD | CS8);
	tio.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
	tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR);
	tio.c_oflag &= ~(OPOST | ONLCR | OCRNL);
	tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		return -1;
	}
	return 0;
}

int main(int argc, char **argv) {
	const char *gadget_path = DEFAULT_GADGET_PORT;
	const char *modem_path = NULL;
	int daemon_mode = 0;
	int opt;

	while ((opt = getopt(argc, argv, "g:m:dh")) != -1) {
		switch (opt) {
		case 'g':
			gadget_path = optarg;
			break;
		case 'm':
			modem_path = optarg;
			break;
		case 'd':
			daemon_mode = 1;
			break;
		case 'h':
		default:
			fprintf(stderr, "Usage: %s [-g <gadget_port>] [-m <modem_port>] [-d]\n", argv[0]);
			fprintf(stderr, "  -g <path>   USB gadget serial port (default: %s)\n", DEFAULT_GADGET_PORT);
			fprintf(stderr, "  -m <path>   Modem AT port (default: auto-detect / %s)\n", DEFAULT_MODEM_PORT);
			fprintf(stderr, "  -d          Run as background daemon\n");
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (!modem_path) {
		modem_path = detect_modem_port();
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	if (daemon_mode) {
		if (daemon(0, 0) < 0) {
			perror("daemon");
			return 1;
		}
	}

	fprintf(stderr, "modem_at_bridge: Starting bridge: %s <--> %s\n", gadget_path, modem_path);

	while (g_running) {
		int g_fd = -1;
		int m_fd = -1;

		while (g_running && (g_fd < 0 || m_fd < 0)) {
			if (g_fd < 0) {
				g_fd = open(gadget_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
			}
			if (m_fd < 0) {
				m_fd = open(modem_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
			}

			if (g_fd >= 0 && m_fd >= 0)
				break;

			sleep(1);
		}

		if (!g_running) {
			if (g_fd >= 0) close(g_fd);
			if (m_fd >= 0) close(m_fd);
			break;
		}

		configure_port(g_fd, isatty(g_fd));
		configure_port(m_fd, isatty(m_fd));

		struct pollfd fds[2];
		fds[0].fd = g_fd;
		fds[0].events = POLLIN;
		fds[1].fd = m_fd;
		fds[1].events = POLLIN;

		unsigned char buf[BUF_SIZE];

		while (g_running) {
			fds[0].revents = 0;
			fds[1].revents = 0;

			int ret = poll(fds, 2, 1000);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (ret == 0)
				continue;

			if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				break;
			}
			if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				break;
			}

			if (fds[0].revents & POLLIN) {
				ssize_t n = read(g_fd, buf, sizeof(buf));
				if (n > 0) {
					ssize_t total_written = 0;
					while (total_written < n) {
						ssize_t w = write(m_fd, buf + total_written, n - total_written);
						if (w < 0) {
							if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
								usleep(1000);
								continue;
							}
							break;
						}
						total_written += w;
					}
				} else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
					break;
				}
			}

			if (fds[1].revents & POLLIN) {
				ssize_t n = read(m_fd, buf, sizeof(buf));
				if (n > 0) {
					ssize_t total_written = 0;
					while (total_written < n) {
						ssize_t w = write(g_fd, buf + total_written, n - total_written);
						if (w < 0) {
							if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
								usleep(1000);
								continue;
							}
							break;
						}
						total_written += w;
					}
				} else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
					break;
				}
			}
		}

		if (g_fd >= 0) close(g_fd);
		if (m_fd >= 0) close(m_fd);

		if (g_running) {
			sleep(1);
		}
	}

	fprintf(stderr, "modem_at_bridge: Stopped.\n");
	return 0;
}
