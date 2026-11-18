// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Read the Darmoshark battery straight from the vendor hidraw node.
 *
 * This is the protocol diagnostic: it does not load or need the kernel module,
 * and it reuses the exact same darmoshark_protocol.h implementation. Run it
 * with enough permission to open the node (normally root), or add a hidraw
 * udev rule for 1915:0723 / 1915:073c or 248a:ff10 / ff12 / ff18 / ff30 / ff31.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "darmoshark_protocol.h"

#define DS_VENDOR_ID 0x1915
#define DS_WIRED_PID 0x073c
#define DS_RECEIVER_PID 0x0723

#define DS_TELINK_VENDOR_ID 0x248a
#define DS_N3_WIRED_PID 0xff10
#define DS_M3_WIRED_PID 0xff12
#define DS_M3S_WIRED_PID 0xff18
#define DS_TELINK_RECEIVER_PID 0xff30
#define DS_TELINK_2K_RECEIVER_PID 0xff31

#define DS_READ_TIMEOUT_MS 1500
#define DS_PRIMER_TIMEOUT_MS 800
#define DS_TELINK_POLL_STEP_MS 15
#define DS_DESCRIPTOR_MAX 4096

static int is_supported_device(unsigned int vid, unsigned int pid,
			       int *wireless, int *telink)
{
	if (vid == DS_VENDOR_ID) {
		if (pid != DS_WIRED_PID && pid != DS_RECEIVER_PID)
			return 0;
		*telink = 0;
		*wireless = pid == DS_RECEIVER_PID;
		return 1;
	}
	if (vid == DS_TELINK_VENDOR_ID) {
		switch (pid) {
		case DS_N3_WIRED_PID:
		case DS_M3_WIRED_PID:
		case DS_M3S_WIRED_PID:
			*telink = 1;
			*wireless = 0;
			return 1;
		case DS_TELINK_RECEIVER_PID:
		case DS_TELINK_2K_RECEIVER_PID:
			*telink = 1;
			*wireless = 1;
			return 1;
		default:
			return 0;
		}
	}
	return 0;
}

/*
 * Both Nordic (1915) and Telink (248a) receivers expose multiple HID
 * interfaces with the same VID/PID; only the configuration one carries the
 * vendor usage page. Match it in the raw report descriptor:
 *   Nordic: `Usage Page 0xFF0A` (`06 0a ff`)
 *   Telink: `Usage Page 0x008C, Usage 0x0001` (`05 8c 09 01`)
 */
static int descriptor_is_config(const char *uevent_path, int telink)
{
	char path[512];
	char buf[DS_DESCRIPTOR_MAX];
	const char *slash;
	size_t len;
	size_t n;
	size_t i;
	FILE *file;

	len = strlen(uevent_path);
	if (len + 32 >= sizeof(path))
		return 0;
	memcpy(path, uevent_path, len + 1);
	slash = strrchr(path, '/');
	if (!slash || strcmp(slash, "/uevent") != 0)
		return 0;
	strcpy((char *)slash, "/report_descriptor");

	file = fopen(path, "rb");
	if (!file)
		return 0;
	n = fread(buf, 1, sizeof(buf), file);
	fclose(file);

	if (telink) {
		for (i = 0; i + 3 < n; i++) {
			if ((unsigned char)buf[i] == 0x05 &&
			    (unsigned char)buf[i + 1] == 0x8c &&
			    (unsigned char)buf[i + 2] == 0x09 &&
			    (unsigned char)buf[i + 3] == 0x01)
				return 1;
		}
		return 0;
	}

	for (i = 0; i + 2 < n; i++) {
		if ((unsigned char)buf[i] == 0x06 &&
		    (unsigned char)buf[i + 1] == 0x0a &&
		    (unsigned char)buf[i + 2] == 0xff)
			return 1;
	}
	return 0;
}

static int find_config_node(char *dev, size_t dev_len, int *wireless,
			    int *telink)
{
	glob_t paths;
	size_t i;
	int found = -1;

	if (glob("/sys/class/hidraw/hidraw*/device/uevent", 0, NULL, &paths) != 0) {
		fprintf(stderr, "no /sys/class/hidraw nodes found\n");
		return -1;
	}

	for (i = 0; i < paths.gl_pathc && found < 0; i++) {
		FILE *file;
		char line[256];
		unsigned int bus = 0;
		unsigned int vid = 0;
		unsigned int pid = 0;
		int node_wireless = 0;
		int node_telink = 0;
		const char *device;
		const char *start;
		size_t name_len;

		file = fopen(paths.gl_pathv[i], "r");
		if (!file)
			continue;
		while (fgets(line, sizeof(line), file)) {
			if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) {
				(void)bus;
				break;
			}
		}
		fclose(file);

		if (!is_supported_device(vid, pid, &node_wireless, &node_telink))
			continue;
		if (!descriptor_is_config(paths.gl_pathv[i], node_telink))
			continue;

		/*
		 * /sys/class/hidraw/hidraw10/device/uevent -> /dev/hidraw10:
		 * walk back from "/device/" to the node directory name.
		 */
		device = strstr(paths.gl_pathv[i], "/device/");
		if (!device)
			continue;
		start = device;
		while (start > paths.gl_pathv[i] && start[-1] != '/')
			start--;
		name_len = (size_t)(device - start);
		if (name_len + 6 >= dev_len)
			continue;
		snprintf(dev, dev_len, "/dev/%.*s", (int)name_len, start);
		*wireless = node_wireless;
		*telink = node_telink;
		found = 0;
	}

	globfree(&paths);
	return found;
}

static void print_hex(const ds_u8 *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		printf("%02x%s", buf[i], i + 1 == len ? "" : " ");
}

static void sleep_ms(int ms)
{
	struct timespec req;

	req.tv_sec = ms / 1000;
	req.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&req, &req) < 0 && errno == EINTR)
		;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int telink_get(int fd, ds_u8 *buf)
{
	memset(buf, 0, DS_TELINK_REPORT_LEN);
	buf[0] = DS_TELINK_REPORT_ID;
	return ioctl(fd, HIDIOCGFEATURE(DS_TELINK_REPORT_LEN), buf);
}

static int telink_send(int fd, ds_u8 opcode)
{
	ds_u8 buf[DS_TELINK_REPORT_LEN];

	darmoshark_build_telink_feature(buf, opcode);
	return ioctl(fd, HIDIOCSFEATURE(DS_TELINK_REPORT_LEN), buf);
}

static int telink_await(int fd, ds_u8 opcode, int timeout_ms, ds_u8 *out)
{
	long long deadline = now_ms() + timeout_ms;
	int ret;

	for (;;) {
		ret = telink_get(fd, out);
		if (ret >= DS_TELINK_REPORT_LEN &&
		    darmoshark_is_telink_echo(out, (unsigned int)ret, opcode))
			return 0;
		if (now_ms() >= deadline)
			return -1;
		sleep_ms(DS_TELINK_POLL_STEP_MS);
	}
}

static int probe_telink(int fd, int wireless)
{
	ds_u8 cmd[DS_TELINK_REPORT_LEN];
	ds_u8 resp[DS_TELINK_REPORT_LEN];
	ds_u8 percent = 0;
	int got = 0;
	int ret;
	int i;

	darmoshark_build_telink_feature(cmd, DS_TELINK_OP_IDENTIFY);
	printf("command: ");
	print_hex(cmd, 8);
	printf(" ...\n");

	if (wireless) {
		ret = telink_get(fd, resp);
		if (ret >= DS_TELINK_REPORT_LEN &&
		    darmoshark_is_telink_echo(resp, (unsigned int)ret,
					      DS_TELINK_OP_IDENTIFY)) {
			if (telink_send(fd, DS_TELINK_OP_BOND) < 0) {
				fprintf(stderr, "primer write failed: %s\n",
					strerror(errno));
				return 1;
			}
			if (telink_await(fd, DS_TELINK_OP_BOND,
					 DS_PRIMER_TIMEOUT_MS, resp) < 0) {
				fprintf(stderr,
					"primer did not complete within %d ms\n",
					DS_PRIMER_TIMEOUT_MS);
				return 1;
			}
		}
		if (telink_send(fd, DS_TELINK_OP_IDENTIFY) < 0) {
			fprintf(stderr, "HIDIOCSFEATURE failed: %s\n",
				strerror(errno));
			return 1;
		}
		if (telink_await(fd, DS_TELINK_OP_IDENTIFY, DS_READ_TIMEOUT_MS,
				 resp) == 0)
			got = 1;
	} else {
		for (i = 0; i < 3; i++) {
			if (telink_send(fd, DS_TELINK_OP_IDENTIFY) < 0) {
				fprintf(stderr, "HIDIOCSFEATURE failed: %s\n",
					strerror(errno));
				return 1;
			}
			sleep_ms(80);
			ret = telink_get(fd, resp);
			if (ret >= DS_TELINK_REPORT_LEN &&
			    darmoshark_is_telink_echo(resp, (unsigned int)ret,
						      DS_TELINK_OP_IDENTIFY)) {
				got = 1;
				break;
			}
			sleep_ms(100);
		}
	}

	if (!got) {
		fprintf(stderr, "no response within %d ms\n", DS_READ_TIMEOUT_MS);
		return 1;
	}

	printf("response: ");
	print_hex(resp, 16);
	printf(" ...\n");
	if (darmoshark_parse_telink_power(resp, DS_TELINK_REPORT_LEN, &percent)) {
		printf("battery: %u%% (telink_dms layout)\n",
		       (unsigned int)percent);
		return 0;
	}
	fprintf(stderr, "power response did not carry a usable percentage\n");
	return 1;
}

static int probe_nordic(int fd, int wireless)
{
	ds_u8 cmd[DS_REPORT_LEN];
	ds_u8 resp[DS_REPORT_LEN];
	ds_u8 percent = 0;
	int ret;
	int i;

	darmoshark_build_power(cmd, wireless);
	printf("command: ");
	print_hex(cmd, 8);
	printf(" ...\n");

	ret = (int)write(fd, cmd, sizeof(cmd));
	if (ret != (int)sizeof(cmd)) {
		fprintf(stderr, "write failed: %s\n",
			ret < 0 ? strerror(errno) : "short write");
		return 1;
	}

	for (i = 0; i < 8; i++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;

		ret = poll(&pfd, 1, DS_READ_TIMEOUT_MS);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			break;
		}
		if (ret == 0) {
			fprintf(stderr, "no response within %d ms\n",
				DS_READ_TIMEOUT_MS);
			break;
		}
		if (!(pfd.revents & POLLIN))
			continue;

		n = read(fd, resp, sizeof(resp));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "read failed: %s\n", strerror(errno));
			break;
		}
		if (!darmoshark_is_power_response(resp, (unsigned int)n)) {
			printf("ignoring unrelated report (%zd bytes)\n", n);
			continue;
		}

		printf("response: ");
		print_hex(resp, 16);
		printf(" ...\n");
		if (darmoshark_parse_power(resp, (unsigned int)n, &percent)) {
			printf("battery: %u%% (%s layout)\n",
			       (unsigned int)percent,
			       (resp[6] == 0x15 && resp[7] == 0x19) ? "dms_v2"
								     : "dms");
			return 0;
		}
		fprintf(stderr,
			"power response did not carry a usable percentage\n");
	}

	return 1;
}

int main(void)
{
	char dev[64];
	int wireless = 0;
	int telink = 0;
	int fd;
	int ret;

	if (find_config_node(dev, sizeof(dev), &wireless, &telink) < 0) {
		fprintf(stderr,
			"no Darmoshark configuration channel found "
			"(1915:073c/0723 or 248a:ff10/ff12/ff18/ff30/ff31); "
			"is the mouse plugged in?\n");
		return 1;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", dev, strerror(errno));
		fprintf(stderr,
			"run as root or grant hidraw access to the Darmoshark configuration node\n");
		return 1;
	}

	printf("device: %s (%s, %s)\n", dev,
	       telink ? "telink 0x008c" : "nordic 0xff0a",
	       wireless ? "2.4 GHz" : "wired");

	ret = telink ? probe_telink(fd, wireless)
		     : probe_nordic(fd, wireless);
	close(fd);
	return ret;
}
