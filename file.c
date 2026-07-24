// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>

#include "ouichefs.h"
#include "bitmap.h"
#include "extent_ioctl.h"

//module parameter for reservation window size
uint32_t reservation_size = 8;
module_param(reservation_size, uint, 0644);
MODULE_PARM_DESC(reservation_size, "Default extent block reservation size");

//garbage collector
static void ouichefs_run_gc(struct super_block *sb)
{
	struct inode *inode;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t j;

	sbi->gc_runs++;

	//iterate safely over master list of all loaded inodes
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
		uint32_t r_start = 0, r_count = 0;

		//lock individual inode to steal its reservation
		spin_lock(&inode->i_lock);
		if (ci->i_reserved_count > 0) {
			r_start = ci->i_reserved_start;
			r_count = ci->i_reserved_count;
			ci->i_reserved_count = 0;
			ci->i_reserved_start = 0;
		}
		spin_unlock(&inode->i_lock);

		//return blocks to disk safely outside the lock
		if (r_count > 0) {
			for (j = 0; j < r_count; j++)
				put_block(sbi, r_start + j);
		}
	}
	spin_unlock(&sb->s_inode_list_lock);
}

//contiguous block allocator
static uint32_t ouichefs_alloc_contiguous(struct super_block *sb, uint32_t requested, uint32_t *block)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t best_start = 0, best_len = 0;
	uint32_t cur_start = 0, cur_len = 0;
	uint32_t i;

	if (requested == 0)
		return 0;

	//Scanning from block 1 ( block 0 is the superblock)
	for (i = 1; i < sbi->nr_blocks; i++) {
		//1 in bitmap means free
		if (test_bit(i, (unsigned long *)sbi->bfree_bitmap)) {
			if (cur_len == 0)
				cur_start = i;
			cur_len++;

			//stop when we find a long enough run
			if (cur_len == requested) {
				best_start = cur_start;
				best_len = cur_len;
				break;
			}
		} else {
			//we hit an allocated block
			if (cur_len > best_len) {
				best_start = cur_start;
				best_len = cur_len;
			}
			cur_len = 0; //reset counter
		}
	}

	//catch runs that reach end of disk
	if (cur_len > best_len) {
		best_start = cur_start;
		best_len = cur_len;
	}

	if (best_len == 0)
		return 0;

	//mark chosen blocks as ALLOCATED in bitmap
	for (i = 0; i < best_len; i++)
		clear_bit(best_start + i, (unsigned long *)sbi->bfree_bitmap);

	sbi->nr_free_blocks -= best_len;
	*block = best_start;

	return best_len;
}

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */

static uint32_t ouichefs_extent_get_block(struct ouichefs_extent *extents, uint32_t logical_block)
{
	uint32_t cumul = 0;
	int i;

	for (i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
		uint32_t start = le32_to_cpu(extents[i].start);
		uint32_t count = le32_to_cpu(extents[i].count);

		if (count == 0)
			break;

		if (logical_block < cumul + count) {

			if (start == 0) //denotes a sparse file hole
				return OUICHEFS_HOLE_BLOCK;

			return start + (logical_block - cumul);
		}

		cumul += count;
	}
	return 0;
}

//mapping blocks for page cache
static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0;
	uint32_t bno;

	/* If block number exceeds filesize, fail */
	//if (iblock >= OUICHEFS_FILE_MAX_BLOCKS)
		//return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */

	bno = ouichefs_extent_get_block(index->extents, iblock);

	//if it is a hole, do not map the buffer
	if (bno == OUICHEFS_HOLE_BLOCK) {
		ret = 0;
		goto brelse_index;
	}

	if (bno == 0) {
		if (!create) {
			ret = 0;
			goto brelse_index;
		}
		//for sparse files, we bypass page cache, so we return error
		//if page cache tries to allocate past EOF
		ret = -ENOSPC;
	} else
		map_bh(bh_result, sb, bno); //map physical block to given buffer_head

brelse_index:
	brelse(bh_index);
	return ret;
}

/*
 * Called by the page cache to read a page from the physical disk and map it in
 * memory
 */
static void ouichefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ouichefs_file_get_block);
}

/*
 * Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int ouichefs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, ouichefs_file_get_block, wbc);
}

/*
 * Called by the VFS when a write() syscall occurs on file before writing the
 * data in the page cache. This functions checks if the write will be able to
 * complete and allocates the necessary blocks through block_write_begin().
 */
static int ouichefs_write_begin(struct file *file,
				struct address_space *mapping, loff_t pos,
				unsigned int len, struct page **pagep,
				void **fsdata)
{
	int err;

	/* prepare the write */
	err = block_write_begin(mapping, pos, len, pagep,
				ouichefs_file_get_block);
	/* if this failed, reclaim newly allocated blocks */
	if (err < 0) {
		truncate_pagecache(file->f_inode, file->f_inode->i_size);
		if (ouichefs_truncate(file->f_inode) < 0)
			pr_err("%s:%d: truncate failed\n", __func__, __LINE__);
		goto out;
	}

	return 0;

out:
	return err;
}

/*
 * Called by the VFS after writing data from a write() syscall to the page
 * cache. This functions updates inode metadata and truncates the file if
 * necessary.
 */
static int ouichefs_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata)
{
	int ret;
	struct inode *inode = file->f_inode;

	/* Complete the write() */
	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len) {
		pr_err("%s:%d: wrote less than asked... what do I do? nothing for now...\n",
		       __func__, __LINE__);
	} else {
		/* Update inode metadata */
		inode->i_mtime = inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
	}

	return ret;
}

//implementing read operation with hole support
ssize_t ouichefs_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh_data, *bh_index;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	char *data;
	size_t block_size = sb->s_blocksize;
	loff_t filesize = i_size_read(inode);
	uint32_t phys_block;

	if (*ppos >= filesize)
		return 0;

	if (*ppos + count > filesize)
		count = filesize - *ppos;

	unsigned long block_idx = *ppos / block_size;
	size_t offset = *ppos % block_size;

	size_t to_copy = min_t(size_t, count, block_size - offset);

	// read block index
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;

	index = (struct ouichefs_file_index_block *)bh_index->b_data;
	// find real physical block number
	phys_block = ouichefs_extent_get_block(index->extents, block_idx);
	brelse(bh_index); //done with index block

	if (phys_block == 0)
		return -EIO;

	//if reading a hole, feed zeros to user space
	if (phys_block == OUICHEFS_HOLE_BLOCK) {
		if (clear_user(buf, to_copy))
			return -EFAULT;

		*ppos += to_copy;
		return to_copy;
	}

	bh_data = sb_bread(sb, phys_block);
	if (!bh_data)
		return -EIO;

	data = bh_data->b_data + offset;
	if (copy_to_user(buf, data, to_copy)) {
		brelse(bh_data);
		return -EFAULT;
	}

	brelse(bh_data);
	*ppos += to_copy;
	return to_copy;
}

//write operation and gap analysis
ssize_t ouichefs_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh_data, *bh_index;
	struct ouichefs_file_index_block *index;
	char *data;
	size_t block_size = sb->s_blocksize;
	uint32_t phys_block;

	if (file->f_flags & O_APPEND)
		*ppos = i_size_read(inode);

	unsigned long block_idx = *ppos / block_size;

	size_t offset = *ppos % block_size;
	size_t to_copy = min_t(size_t, count, block_size - offset);

	bh_index = sb_bread(sb, ci->index_block); // read index block
	if (!bh_index)
		return -EIO;

	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	phys_block = ouichefs_extent_get_block(index->extents, block_idx);

	if (phys_block == 0 || phys_block == OUICHEFS_HOLE_BLOCK) {
		//allocate a new block from disk
		int last_ext = -1, hole_idx = -1;
		uint32_t cumul = 0, hole_offset = 0;
		int num_extents = 0, i;

		//analyse array for gap insertion points or hole indices
		for (i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
			uint32_t c_ext = le32_to_cpu(index->extents[i].count);

			if (c_ext == 0)
				break;

			if (block_idx >= cumul && block_idx < cumul + c_ext) {
				hole_idx = i;

				hole_offset = block_idx - cumul;

			}

			cumul += c_ext;

			last_ext = i;

			num_extents++;
		}

		//insert a hole extent if writing past EOF
		if (phys_block == 0 && block_idx > cumul) {

			uint32_t gap = block_idx - cumul;

			if (last_ext >= 0 && le32_to_cpu(index->extents[last_ext].start) == 0) {
				uint32_t c = le32_to_cpu(index->extents[last_ext].count);

				index->extents[last_ext].count = cpu_to_le32(c + gap);

			} else {
				if (num_extents >= OUICHEFS_MAX_EXTENTS)
					brelse(bh_index); return -ENOSPC;

				last_ext++;
				index->extents[last_ext].start = cpu_to_le32(0);
				index->extents[last_ext].count = cpu_to_le32(gap);
				num_extents++;
			}
			cumul += gap;
		}

		// Block Allocator and Reservation Engine
		uint32_t requested = (count + block_size - 1) / block_size;
		uint32_t allocated = 0;

		spin_lock(&inode->i_lock);
		if (ci->i_reserved_count > 0) {
			phys_block = ci->i_reserved_start;
			allocated = min_t(uint32_t, requested, ci->i_reserved_count);
			ci->i_reserved_start += allocated;
			ci->i_reserved_count -= allocated;
			spin_unlock(&inode->i_lock);
		} else {
			spin_unlock(&inode->i_lock);

			uint32_t to_alloc = max_t(uint32_t, requested, reservation_size);
			uint32_t actual_alloc = ouichefs_alloc_contiguous(sb, to_alloc, &phys_block);

			if (actual_alloc == 0) {
				ouichefs_run_gc(sb);
				actual_alloc = ouichefs_alloc_contiguous(sb, to_alloc, &phys_block);

				if (actual_alloc == 0) {
					brelse(bh_index);
					return -ENOSPC;
				}
			}

			if (actual_alloc > requested) {
				allocated = requested;
				spin_lock(&inode->i_lock);
				ci->i_reserved_start = phys_block + requested;
				ci->i_reserved_count = actual_alloc - requested;
				spin_unlock(&inode->i_lock);
			} else
				allocated = actual_alloc;
		}

		// splitting an existing hole safely
		if (hole_idx >= 0) {
			uint32_t orig_count = le32_to_cpu(index->extents[hole_idx].count);

			allocated = min_t(uint32_t, allocated, orig_count - hole_offset);

			int slots_needed = 0;

			if (hole_offset > 0)
				slots_needed++;
			if (hole_offset + allocated < orig_count)
				slots_needed++;

			if (num_extents + slots_needed - 1 >= OUICHEFS_MAX_EXTENTS) {
				brelse(bh_index);
				return -ENOSPC;
			}

			if (hole_offset == 0 && allocated == orig_count) {
				index->extents[hole_idx].start = cpu_to_le32(phys_block);

			} else if (hole_offset == 0) {
				memmove(&index->extents[hole_idx + 2], &index->extents[hole_idx + 1],
						(OUICHEFS_MAX_EXTENTS - 2 - hole_idx) * sizeof(struct ouichefs_extent));
				index->extents[hole_idx].start = cpu_to_le32(phys_block);
				index->extents[hole_idx].count = cpu_to_le32(allocated);
				index->extents[hole_idx + 1].start = cpu_to_le32(0);
				index->extents[hole_idx + 1].count = cpu_to_le32(orig_count - allocated);
			} else if (hole_offset + allocated == orig_count) {
				memmove(&index->extents[hole_idx + 2], &index->extents[hole_idx + 1],
						(OUICHEFS_MAX_EXTENTS - 2 - hole_idx) * sizeof(struct ouichefs_extent));
				index->extents[hole_idx].count = cpu_to_le32(hole_offset);
				index->extents[hole_idx + 1].start = cpu_to_le32(phys_block);
				index->extents[hole_idx + 1].count = cpu_to_le32(allocated);
			} else {
				memmove(&index->extents[hole_idx + 3], &index->extents[hole_idx + 1],
						(OUICHEFS_MAX_EXTENTS - 3 - hole_idx) * sizeof(struct ouichefs_extent));
				index->extents[hole_idx].count = cpu_to_le32(hole_offset);
				index->extents[hole_idx + 1].start = cpu_to_le32(phys_block);
				index->extents[hole_idx + 1].count = cpu_to_le32(allocated);
				index->extents[hole_idx + 2].start = cpu_to_le32(0);
				index->extents[hole_idx + 2].count = cpu_to_le32(orig_count - hole_offset - allocated);
			}
		} else {
			// Normal Append
			if (last_ext >= 0 &&
					le32_to_cpu(index->extents[last_ext].start) != 0 &&
					le32_to_cpu(index->extents[last_ext].start) +
					le32_to_cpu(index->extents[last_ext].count) == phys_block) {

				uint32_t c = le32_to_cpu(index->extents[last_ext].count);

				index->extents[last_ext].count = cpu_to_le32(c + allocated);

			} else {
				if (num_extents >= OUICHEFS_MAX_EXTENTS) {
					brelse(bh_index);
					return -ENOSPC;
				}
				last_ext++;
				index->extents[last_ext].start = cpu_to_le32(phys_block);

				index->extents[last_ext].count = cpu_to_le32(allocated);

			}
		}

		mark_buffer_dirty(bh_index);
		inode->i_blocks += (allocated * (sb->s_blocksize >> 9));
		mark_inode_dirty(inode);
	}
	brelse(bh_index);

	//append standard extents and copy user data
	bh_data = sb_bread(sb, phys_block);
	if (!bh_data)
		return -EIO;

	lock_buffer(bh_data);
	data = bh_data->b_data + offset;

	if (to_copy < block_size)
		memset(data, 0, block_size);

	if (copy_from_user(data, buf, to_copy)) {
		unlock_buffer(bh_data);
		brelse(bh_data);
		return -EFAULT;
	}

	set_buffer_uptodate(bh_data);
	mark_buffer_dirty(bh_data);
	unlock_buffer(bh_data);
	brelse(bh_data);

	*ppos += to_copy;
	if (*ppos > i_size_read(inode)) {
		i_size_write(inode, *ppos);
		mark_inode_dirty(inode);
	}
	return to_copy;
}

//release unused reservations when file is closed
static int ouichefs_file_release(struct inode *inode, struct file *file)
{
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t r_start = 0, r_count = 0, j;

	spin_lock(&inode->i_lock);
	if (ci->i_reserved_count > 0) {
		r_start = ci->i_reserved_start;
		r_count = ci->i_reserved_count;
		ci->i_reserved_count = 0;
		ci->i_reserved_start = 0;
	}
	spin_unlock(&inode->i_lock);

	if (r_count > 0) {
		for (j = 0; j < r_count ; j++)
			put_block(sbi, r_count + j);
	}
	return 0;
}

long ouichefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	struct ouichefs_file_index_block *index;
	int i, num_extents = 0;

	if (cmd != OUICHEFS_IOC_GET_EXTENTS)
		return -ENOTTY;

	bh = sb_bread(sb, ci->index_block);
	if (!bh)
		return -EIO;

	index = (struct ouichefs_file_index_block *)bh->b_data;

	for (i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
		if (le32_to_cpu(index->extents[i].count) == 0)
			break;
		num_extents++;
	}

	pr_info("ouichefs: extents for inode %lu: %d extent(s)\n", inode->i_ino, num_extents);

	for (i = 0; i < num_extents; i++) {
		uint32_t start = le32_to_cpu(index->extents[i].start);

		uint32_t count = le32_to_cpu(index->extents[i].count);

		pr_info(" [%d] start=%u count=%u (blocks %u-%u)\n",
				i, start, count, start, start + count - 1);
	}

	brelse(bh);
	return 0;
}

const struct address_space_operations ouichefs_aops = {
	.readahead = ouichefs_readahead,
	.writepage = ouichefs_writepage,
	.write_begin = ouichefs_write_begin,
	.write_end = ouichefs_write_end
};

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.llseek = generic_file_llseek,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.unlocked_ioctl = ouichefs_ioctl,
	.fsync = generic_file_fsync,
	.release = ouichefs_file_release,
};

int ouichefs_truncate(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	struct ouichefs_file_index_block *index;
	size_t i;

	bh = sb_bread(sb, inode_info->index_block);

	if (!bh)
		return -EIO;

	index = (struct ouichefs_file_index_block *)bh->b_data;

	for (i = 0; i < OUICHEFS_MAX_EXTENTS; ++i) {
		uint32_t start = le32_to_cpu(index->extents[i].start);
		uint32_t count = le32_to_cpu(index->extents[i].count);
		uint32_t j;

		if (!count)
			break;

		// skip freeing physical blocks if it is a hole
		if (!start) {
			index->extents[i].count = cpu_to_le32(0);
			continue;
		}

		for (j = 0; j < count; j++)
			inode->i_blocks -= (sb->s_blocksize >> 9);

		index->extents[i].start = cpu_to_le32(0);
		index->extents[i].count = cpu_to_le32(0);
	}

	mark_buffer_dirty(bh);
	brelse(bh);
	mark_inode_dirty(inode);

	return 0;
}
