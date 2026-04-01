/*
 * Part 3 Test: Verify v4l2_subdev_call from dmesg
 *
 * Usage: ./test_stream
 * (Run after: insmod soc_hw_platform.ko && insmod vsoc_sensor.ko &&
 *             insmod vsoc_test_bridge.ko)
 */

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	printf("=== Part 3: v4l2_subdev_ops Test ===\n\n");
	printf("This test verifies v4l2_subdev_call() by checking dmesg.\n\n");
	printf("Expected dmesg output after loading modules:\n");
	printf("  1. 'VSOC-3000 sensor detected'\n");
	printf("  2. 'v4l2_subdev_call(sensor, video, s_stream, 1)'\n");
	printf("  3. 'sensor streaming ON'\n");
	printf("  4. '=== Sensor Status ===' (from log_status)\n");
	printf("  5. 'sensor streaming OFF'\n\n");
	printf("Run: dmesg | tail -20 | grep -E 'vsoc|sensor'\n");
	return 0;
}
