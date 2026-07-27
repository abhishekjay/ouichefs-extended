#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "extent_ioctl.h"

int main(int argc, char *argv[]) {
	/* 1. Default to our test file, but allow passing a custom file path */
	const char *filepath = "/home/abhishek/mnt_ouiche/test.txt";
	
	if (argc > 1)
		filepath = argv[1];
	printf("Testing extents for: %s\n", filepath);
	/* 2. Open the file located inside your mounted OuicheFS */
	int fd = open(filepath, O_RDONLY);

	if (fd < 0) {
		perror("Failed to open file");
		return 1;
	}

	/* 3. Send the magical custom command! */
	if (ioctl(fd, OUICHEFS_IOC_GET_EXTENTS) < 0) {
		perror("IOCTL failed");
	} else {
		printf("Success! Run 'dmesg | tail -n 10' to see the extents printed by the kernel.\n");
	}

	close(fd);
	return 0;
}
