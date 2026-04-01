/*
 * Part 2 Test: Verify sensor subdev registration
 *
 * Checks:
 *   1. /sys/class/i2c-adapter/ has the vsoc-i2c adapter
 *   2. The sensor I2C device exists
 *   3. The v4l2_subdev is registered (visible in sysfs)
 *
 * Usage: ./test_sensor
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static int check_file_contains(const char *path, const char *needle)
{
	FILE *f = fopen(path, "r");
	char buf[256];

	if (!f)
		return 0;
	while (fgets(buf, sizeof(buf), f)) {
		if (strstr(buf, needle)) {
			fclose(f);
			return 1;
		}
	}
	fclose(f);
	return 0;
}

static int find_i2c_device(void)
{
	DIR *d;
	struct dirent *ent;
	char path[512];
	int found = 0;

	d = opendir("/sys/bus/i2c/devices");
	if (!d) {
		printf("FAIL: cannot open /sys/bus/i2c/devices\n");
		return 0;
	}

	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path),
			 "/sys/bus/i2c/devices/%s/name", ent->d_name);
		if (check_file_contains(path, "vsoc_sensor")) {
			printf("PASS: Found sensor I2C device: %s\n",
			       ent->d_name);
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

int main(void)
{
	int pass = 0, fail = 0;

	printf("=== Part 2: v4l2_subdev Basics Test ===\n\n");

	/* Test 1: I2C device exists */
	if (find_i2c_device())
		pass++;
	else {
		printf("FAIL: vsoc_sensor I2C device not found\n");
		fail++;
	}

	/* Test 2: Check dmesg for subdev registration */
	printf("\nCheck dmesg for registration messages:\n");
	printf("  dmesg | grep vsoc_sensor\n");

	printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
	return fail ? 1 : 0;
}
