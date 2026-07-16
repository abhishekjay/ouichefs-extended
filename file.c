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

#include "ouichefs.h"
#include "bitmap.h"
#include "extent_ioctl.h"

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */
static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0, bno;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_FILE_MAX_BLOCKS)
		return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */

	bno = le32_to_cpu(index->extents[iblock].start);

	if (bno == 0) {
		if (!create) {
			ret = 0;
			goto brelse_index;
		}

		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto brelse_index;
		}

		index->extents[iblock].start = cpu_to_le32(bno);
		index->extents[iblock].count = cpu_to_le32(1);

		inode->i_blocks++;

		mark_inode_dirty(inode);
		mark_buffer_dirty(bh_index);
	}

	/* Map the physical block to the given buffer_head */
	map_bh(bh_result, sb, bno);

brelse_index:
	brelse(bh_index);

	return ret;
}

/*
 * Called by the page cache to read a page from the physical disk and map it in
 * memory.
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

	//new max limit = 512
	if (block_idx >= OUICHEFS_MAX_EXTENTS)
		return -EIO;

	size_t offset = *ppos % block_size;

	size_t to_copy = min_t(size_t, count, block_size - offset);

	// read block index
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;

	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	// find real physical block number
	//phys_block = le32_to_cpu(index->blocks[block_idx]);
	// using extents
	phys_block = le32_to_cpu(index->extents[block_idx].start);
	brelse(bh_index); //done with index block

	if (phys_block == 0)
		return -EIO;

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

ssize_t ouichefs_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh_data, *bh_index;
	struct ouichefs_file_index_block *index;
	char *data;
	size_t block_size = sb->s_blocksize;
	uint32_t phys_block;

	if (file->f_flags & O_APPEND)
		*ppos = i_size_read(inode);

	unsigned long block_idx = *ppos / block_size;

	if (block_idx >= OUICHEFS_MAX_EXTENTS)
		return -ENOSPC;

	size_t offset = *ppos % block_size;
	size_t to_copy = min_t(size_t, count, block_size - offset);

	bh_index = sb_bread(sb, ci->index_block); // read index block
	if (!bh_index)
		return -EIO;

	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	phys_block = le32_to_cpu(index->extents[block_idx].start);

	if (phys_block == 0) {
		//allocate a new block from disk
		phys_block = get_free_block(sbi);
		if (!phys_block) {
			brelse(bh_index);
			return -ENOSPC;
		}

		//index->blocks[block_idx] = cpu_to_le32(phys_block); //update index block and save
		index->extents[block_idx].start = cpu_to_le32(phys_block);
		index->extents[block_idx].count = cpu_to_le32(1);

		mark_buffer_dirty(bh_index);

		inode->i_blocks++;
		mark_inode_dirty(inode);
	}
	brelse(bh_index);

	//now there is a physical block to write to
	bh_data = sb_bread(sb, phys_block); //load first using sb_bread
	if (!bh_data)
		return -EIO;

	lock_buffer(bh_data);
	data = bh_data->b_data + offset;
	if (copy_from_user(data, buf, to_copy)) {
		unlock_buffer(bh_data);
		brelse(bh_data);
		return -EFAULT;
	}

	set_buffer_uptodate(bh_data);
	mark_buffer_dirty(bh_data); // Mark for disk sync
	unlock_buffer(bh_data);
	brelse(bh_data);

	*ppos += to_copy;
	if (*ppos > i_size_read(inode)) {
		i_size_write(inode, *ppos);
		mark_inode_dirty(inode);
	}

	return to_copy;
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
};

int ouichefs_truncate(struct inode *inode)
{
	int ret;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	struct ouichefs_file_index_block *index;
	size_t next_num_blocks;
	size_t i;

	bh = sb_bread(sb, inode_info->index_block);

	if (!bh)
		ret = -EIO;

	index = (struct ouichefs_file_index_block *)bh->b_data;

	//ret = block_truncate_page(inode->i_mapping, inode->i_size, ouichefs_file_get_block);
	//if (ret < 0)
		//goto out_brelse;

	//struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh->b_data;

	next_num_blocks = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;

	for (i = next_num_blocks; i < OUICHEFS_MAX_EXTENTS; ++i) {
		uint32_t bno = le32_to_cpu(index->extents[i].start);

		if (!bno)
			continue;

		put_block(sbi, bno);
		inode->i_blocks--;

		index->extents[i].start = cpu_to_le32(0);
		index->extents[i].count = cpu_to_le32(0);
	}

	mark_buffer_dirty(bh);
	brelse(bh);

	mark_inode_dirty(inode);

	return ret;
}
