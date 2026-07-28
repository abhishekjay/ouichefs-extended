#!/bin/bash

# Exit immediately if a command fails

set -e

MNT="/home/abhishek/mnt_ouiche"
IMG="disk.img"
TEST_FILE="$MNT/sparse.txt"

if [ ! -d "$MNT" ]; then
	echo "[*] Creating mount directory..."
	mkdir -p "$MNT"
fi

if [ ! -f "$IMG" ]; then
	echo "[*] Creating new 100MB disk image..."
	dd if=/dev/zero of="$IMG" bs=1M count=100 status=none
fi

echo ""
echo "   OuicheFS Comprehensive Feature Test    "
echo ""

echo "[*] Compiling IOCTL tools..."
gcc test_ioctl.c -o test_ioctl 2>/dev/null || true
gcc test_defrag.c -o test_defrag 2>/dev/null || true

echo "[*] Unmounting and reloading module..."
sudo umount $MNT 2>/dev/null || true
sudo rmmod ouichefs 2>/dev/null || true
sudo insmod ouichefs.ko

echo "[*] Re-formatting disk to ensure a clean state..."
./mkfs/mkfs.ouichefs $IMG > /dev/null

echo "[*] Mounting OuicheFS..."
sudo mount -o loop -t ouichefs $IMG $MNT

# Dynamically find the sysfs directory (usually loop0 or loop1)

SYSFS_DIR=$(ls -d /sys/fs/ouichefs/loop* 2>/dev/null || ls -d /sys/ouichefs/loop* 2>/dev/null | head -n 1)

echo ""
echo "--- 1. Contiguous Allocation (Tasks 1.4 - 1.6) ---"

# Write a 5MB file. Since disk is empty, the contiguous allocator should grab it all at once.

dd if=/dev/zero of=$MNT/contig.txt bs=1M count=5 status=none
./test_ioctl $MNT/contig.txt
echo "> dmesg output:"
dmesg | tail -n 2

echo ""
echo "--- 2. Sparse Files & Holes (Task 1.9) ---"

# Write 1MB at offset 0, then 1MB at offset 10MB -> Creates a 9MB logical hole

dd if=/dev/zero of=$TEST_FILE bs=1M count=1 status=none
dd if=/dev/zero of=$TEST_FILE bs=1M seek=10 count=1 status=none
echo "> File size (should be ~11MB):"
ls -lh $TEST_FILE

echo ""
echo "--- 3. Forced Fragmentation (Task 1.5) ---"

# Write exactly into the middle of the hole to force extent splitting

dd if=/dev/zero of=$TEST_FILE bs=1M seek=5 count=1 conv=notrunc status=none
./test_ioctl $TEST_FILE
echo "> dmesg output (should show ~3-4 scattered extents):"
dmesg | tail -n 5

echo ""
sync
echo "--- 4. Sysfs Statistics (Task 1.8) ---"
if [ -d "$SYSFS_DIR" ]; then
echo "Free blocks: $(cat $SYSFS_DIR/free_blocks)"
echo "Total extents: $(cat $SYSFS_DIR/total_extents)"
echo "Fragmentation (x100): $(cat $SYSFS_DIR/fragmentation)"
else
echo "Sysfs directory not found. Skipping stat check."
fi

echo ""
echo "--- 5. Manual Defragmentation (Task 1.10) ---"
./test_defrag $TEST_FILE
./test_ioctl $TEST_FILE
echo "> dmesg output (should now show exactly 1 perfectly contiguous extent!):"
dmesg | tail -n 2

echo ""
echo "--- 6. Auto-Defragmentation & Sysfs Tuning (Task 1.10) ---"
if [ -d "$SYSFS_DIR" ]; then
echo "> Lowering defrag threshold to 130 (2 extents/file limit)..."
sudo sh -c "echo 130 > $SYSFS_DIR/defrag_threshold"

# Intentionally fragment a new file
dd if=/dev/zero of=$MNT/auto.txt bs=1M count=1 status=none
dd if=/dev/zero of=$MNT/auto.txt bs=1M seek=5 count=1 status=none

echo "> Overwriting the hole to trigger the threshold..."
sync
# This write exceeds the threshold, triggering auto-defrag before returning
dd if=/dev/zero of=$MNT/auto.txt bs=1M seek=2 count=1 conv=notrunc status=none

./test_ioctl $MNT/auto.txt
echo "> dmesg output (should show 1 extent due to auto-intervention):"
dmesg | tail -n 2


fi

echo ""
echo ""
echo "         All Tests Completed!             "
echo ""

sudo umount $MNT
sudo rmmod ouichefs
