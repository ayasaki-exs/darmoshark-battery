// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Read the Darmoshark battery straight from the vendor hidraw node.
 *
 * This is the protocol diagnostic: it does not load or need the kernel module,
 * and it reuses the exact same darmoshark_protocol.h implementation. Run it
 * with enough permission to open the node (normally root), or add a hidraw
 * udev rule for 1915:0723 / 1915:073c.
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "darmoshark_protocol.h"

#define DS_VENDOR_ID 0x1915
#define DS_WIRED_PID 0x073c
#define DS_RECEIVER_PID 0x0723
#define DS_READ_TIMEOUT_MS 1500
#define DS_DESCRIPTOR_MAX 4096

/*
 * The receiver exposes four HID interfaces with the same VID/PID; only the
 * configuration one carries the vendor usage page. Match it in the raw report
 * descriptor (`Usage Page 0xFF0A` is `06 0a ff`).
 */
static int descriptor_is_config(const char *uevent_path)
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

	for (i = 0; i + 2 < n; i++) {
		if ((unsigned char)buf[i] == 0x06 &&
		    (unsigned char)buf[i + 1] == 0x0a &&
		    (unsigned char)buf[i + 2] == 0xff)
			return 1;
	}
	return 0;
}

static int find_config_node(char *dev, size_t dev_len, int *wireless)
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

		if (vid != DS_VENDOR_ID)
			continue;
		if (pid != DS_WIRED_PID && pid != DS_RECEIVER_PID)
			continue;
		if (!descriptor_is_config(paths.gl_pathv[i]))
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
		*wireless = pid == DS_RECEIVER_PID;
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

int main(void)
{
	char dev[64];
	ds_u8 cmd[DS_REPORT_LEN];
	ds_u8 resp[DS_REPORT_LEN];
	ds_u8 percent = 0;
	int wireless = 0;
	int fd;
	int ret;
	int i;

	if (find_config_node(dev, sizeof(dev), &wireless) < 0) {
		fprintf(stderr,
			"no Darmoshark configuration channel found "
			"(1915:073c / 1915:0723); is the mouse plugged in?\n");
		return 1;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", dev, strerror(errno));
		fprintf(stderr, "run as root or grant hidraw access to 1915:0723/1915:073c\n");
		return 1;
	}

	darmoshark_build_power(cmd, wireless);
	printf("device: %s (%s)\n", dev, wireless ? "2.4 GHz" : "wired");
	printf("command:");
	print_hex(cmd, 8);
	printf(" ...\n");

	ret = (int)write(fd, cmd, sizeof(cmd));
	if (ret != (int)sizeof(cmd)) {
		fprintf(stderr, "write failed: %s\n", ret < 0 ? strerror(errno) : "short write");
		close(fd);
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
			fprintf(stderr, "no response within %d ms\n", DS_READ_TIMEOUT_MS);
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

		printf("response:");
		print_hex(resp, 16);
		printf(" ...\n");
		if (darmoshark_parse_power(resp, (unsigned int)n, &percent)) {
			printf("battery: %u%% (%s layout)\n", (unsigned int)percent,
			       (resp[6] == 0x15 && resp[7] == 0x19) ? "dms_v2" : "dms");
			close(fd);
			return 0;
		}
		fprintf(stderr, "power response did not carry a usable percentage\n");
	}

	close(fd);
	return 1;
}
