# OUICHEFS EXTENDED
The aim of this project was to extend the functionalities of the simple OuicheFS filesystem (more info below) by introducing key modifications and upgrades to its software architecture. The project was part of the course on Linux Kernel Programming at RWTH Aachen University (https://teaching.os.rwth-aachen.de/LKP/)

### Implemented and Functional Features
- Extent-Based Storage: Successfully replaced the flat block limit with an Endian-safe 512-slot array.
- Contiguous Block Allocation: Functional First-Fit bitmap scanning supporting "Best-Effort" fallbacks.
- Write-Time Reservations: spin_lock-protected RAM caching to prevent interleaved fragmentation.
- Global Garbage Collection: Safe revocation of unused reservations across all VFS inodes upon ENOSPC.
- Sparse File & Hole Punching: Zero-fill reading and memory-safe extent array splitting (memmove).
- Sysfs Global Statistics: Read-only metrics (bypassing VFS locks via sb_bread) and writable tunables.
- Online Manual Defragmentation: IOCTL-driven block migration, consolidation, and dirty-block sanitization.

### Implemented but Not Fully Functional / Known Issues
- Auto-Defragmentation Scanner Synchronization
Problem: The auto-defragmentation does not trigger accurately on checking fragmentation after a write() operation.
Could be due to a syncing issue between the RAM and the disk. The scanner checks the disk at the end of a write, but the write which caused the fragmentation has not been reflected in the disk yet.
Potential Fix: We used sync command in extent.sh to force this behaviour during testing
- Ghost Leak
Problem: We encounter a ghost leak when running automated test script 22: Sysfs block invariant.
Why we think it occurs: This could be due to a faulty hole-splitting algorithm which in specific scenarios doesn’t use the entire allocated blocks. 
Potential Fix: Calculate leftover blocks during splitting, put them into the file’s temporary memory stash
Running Test 22: Sysfs block invariant… [FAIL] Sysfs invariant broken! Before IO: 12508, After IO: 12507 

### Not Implemented (Future directions)
- Task 1.11 – Bonus: Advanced Block Allocator
Task 1.11 was not implemented due to time constraints. Replacing the linear bitmap-based contiguous allocator with a complex mechanism like buddy-allocator could not be attempted. We decided to prioritize the challenges in the existing tasks 1.2 through 1.10 and think of mitigating edge-case scenarios that may arise within these tasks.

The following was the initial OuicheFS filesystem design
-------------------------------------------------------------------------------

# ouiche_fs - a simple educational filesystem for Linux
The main objective of this project is to provide a simple Linux filesystem for students to build on.

## Summary
- [Usage](#Usage)
- [Design](#Design)
- [Roadmap](#Roadmap)

## Usage
### Building the kernel module
You can build the kernel module for your currently running kernel with `make`. If you wish to build the module against a different kernel, run `make KERNELDIR=<path>`. Insert the module with `insmod ouichefs.ko`.

This code was tested on a 6.5.7 kernel.

### Formatting a partition
First, build `mkfs.ouichefs` from the mkfs directory. Run `mkfs.ouichefs img` to format img as a ouiche_fs partition. For example, create a zeroed file of 50 MiB with `dd if=/dev/zero of=test.img bs=1M count=50` and run `mkfs.ouichefs test.img`. You can then mount this image on a system with the ouiche_fs kernel module installed.

## Design
This filesystem does not provide any fancy feature to ease understanding.

### Partition layout
    +------------+-------------+-------------------+-------------------+-------------+
    | superblock | inode store | inode free bitmap | block free bitmap | data blocks |
    +------------+-------------+-------------------+-------------------+-------------+
Each block is 4 KiB large.

### Superblock
The superblock is the first block of the partition (block 0). It contains the partition's metadata, such as the number of blocks, number of inodes, number of free inodes/blocks, ...

### Inode store
Contains all the inodes of the partition. The maximum number of inodes is equal to the number of blocks of the partition. Each inode contains 80B of data: standard data such as file size and number of used blocks, as well as a ouiche_fs-specific field called `index_block`. This block contains:
  - for a directory: the list of files in this directory. A directory can contain at most 128 files, and filenames are limited to 28 characters to fit in a single block.
  
![directory block](docs/dir_block.png)
  - for a file: the list of blocks containing the actual data of this file. Since block IDs are stored as 32-bit values, at most 1024 links fit in a single block, limiting the size of a file to 4 MiB.

![file block](docs/file_block.png)

### Inode and block free bitmaps
These two bitmaps track if inodes/blocks are used or not.

### Data blocks
The remainder of the partition is used to store actual data on disk.

### Data structure relations in the Linux kernel
![Linux VFS](docs/vfs_struct_relations.png)

## Roadmap
### Current features
#### Directories
- Creation and deletion
- List content
- Renaming

#### Regular files
- Creation and deletion
- Reading and writing (through the page cache)
- Renaming

### Future features
- Hard and symbolic link support
