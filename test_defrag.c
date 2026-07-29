// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define OUICHEFS_IOC_DEFRAG_FILE _IO('O', 2)

int main(int argc, char *argv[])
{
	const char *filepath = "/home/abhishek/mnt_ouiche/sparse.txt";

	if (argc > 1)
		filepath = argv[1];

	printf("Starting defragmentation for: %s\n", filepath);

	int fd = open(filepath, O_RDWR);

	if (fd < 0) {
		perror("Failed to open file (do you have write permissions?)");
		return 1;
	}

	/* Send the custom defrag command */
	if (ioctl(fd, OUICHEFS_IOC_DEFRAG_FILE) < 0)
		perror("Defrag IOCTL failed");
	else
		printf("Defrag complete! Run your test_ioctl tool again "
			"to see the new layout.\n");

	close(fd);
	return 0;
}
