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
#include <linux/uio.h>

#include "ouichefs.h"
#include "bitmap.h"

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
	if (index->extents[iblock].start == 0) {
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
		++inode->i_blocks;

		mark_inode_dirty(inode);
		mark_buffer_dirty(bh_index);
	} else {
		bno = le32_to_cpu(index->extents[iblock].start);
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

const struct address_space_operations ouichefs_aops = {
	.readahead = ouichefs_readahead,
	.writepage = ouichefs_writepage,
	.write_begin = ouichefs_write_begin,
	.write_end = ouichefs_write_end
};

static ssize_t ouichefs_file_read_iter(struct kiocb *iocb,
                                       struct iov_iter *to)
{
        struct file *file = iocb->ki_filp;
        struct inode *inode = file_inode(file);
        struct super_block *sb = inode->i_sb;
        struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

        struct buffer_head *bh_index;
        struct buffer_head *bh_data;
        struct ouichefs_file_index_block *index;

        loff_t pos = iocb->ki_pos;
        size_t requested = iov_iter_count(to);
        size_t total_copied = 0;

        if (pos < 0)
                return -EINVAL;

        if (requested == 0)
                return 0;

        if (pos >= inode->i_size)
                return 0;

        if (requested > inode->i_size - pos)
                requested = inode->i_size - pos;

        bh_index = sb_bread(sb, ci->index_block);
        if (!bh_index)
                return -EIO;

        index = (struct ouichefs_file_index_block *)bh_index->b_data;

        while (total_copied < requested) {
                sector_t logical_block;
                unsigned int block_offset;
                unsigned int block_number;
                size_t bytes_to_copy;
                size_t copied;

                logical_block = pos >> sb->s_blocksize_bits;
                block_offset = pos & (sb->s_blocksize - 1);

                if (logical_block >= OUICHEFS_FILE_MAX_BLOCKS)
                        break;

                bytes_to_copy = min_t(size_t,
                                      requested - total_copied,
                                      sb->s_blocksize - block_offset);

                block_number =
                        le32_to_cpu(index->extents[logical_block].start);

                if (block_number == 0) {
                        copied = iov_iter_zero(bytes_to_copy, to);
                } else {
                        bh_data = sb_bread(sb, block_number);
                        if (!bh_data) {
                                brelse(bh_index);

                                if (total_copied == 0)
                                        return -EIO;

                                iocb->ki_pos = pos;
                                return total_copied;
                        }

                        copied = copy_to_iter(
                                bh_data->b_data + block_offset,
                                bytes_to_copy,
                                to
                        );

                        brelse(bh_data);
                }

                if (copied == 0)
                        break;

                pos += copied;
                total_copied += copied;

                if (copied < bytes_to_copy)
                        break;
        }

        brelse(bh_index);

        iocb->ki_pos = pos;

        return total_copied;
}

static ssize_t ouichefs_file_write_iter(struct kiocb *iocb,
                                        struct iov_iter *from)
{
        struct file *file = iocb->ki_filp;
        struct inode *inode = file_inode(file);
        struct super_block *sb = inode->i_sb;
        struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
        struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

        struct buffer_head *bh_index;
        struct buffer_head *bh_data;
        struct ouichefs_file_index_block *index;

        loff_t pos = iocb->ki_pos;
        size_t requested = iov_iter_count(from);
        size_t total_copied = 0;

        if (file->f_flags & O_APPEND)
                pos = inode->i_size;

        if (pos < 0)
                return -EINVAL;

        if (requested == 0)
                return 0;

        bh_index = sb_bread(sb, ci->index_block);
        if (!bh_index)
                return -EIO;

        index = (struct ouichefs_file_index_block *)bh_index->b_data;

        while (total_copied < requested) {
                sector_t logical_block;
                unsigned int block_offset;
                unsigned int block_number;
                size_t bytes_to_copy;
                size_t copied;

                logical_block = pos >> sb->s_blocksize_bits;
                block_offset = pos & (sb->s_blocksize - 1);

                if (logical_block >= OUICHEFS_FILE_MAX_BLOCKS)
                        break;

                bytes_to_copy = min_t(size_t,
                                      requested - total_copied,
                                      sb->s_blocksize - block_offset);

                block_number =
                        le32_to_cpu(index->extents[logical_block].start);

                if (block_number == 0) {
                        block_number = get_free_block(sbi);

                        if (block_number == 0) {
                                if (total_copied == 0) {
                                        brelse(bh_index);
                                        return -ENOSPC;
                                }

                                break;
                        }

                        index->extents[logical_block].start =
                                cpu_to_le32(block_number);
                        index->extents[logical_block].count =
                                cpu_to_le32(1);

                        inode->i_blocks++;

                        mark_buffer_dirty(bh_index);
                        sync_dirty_buffer(bh_index);
                }

                bh_data = sb_bread(sb, block_number);
                if (!bh_data) {
                        if (total_copied == 0) {
                                brelse(bh_index);
                                return -EIO;
                        }

                        break;
                }

                copied = copy_from_iter(
                        bh_data->b_data + block_offset,
                        bytes_to_copy,
                        from
                );

                if (copied > 0) {
                        mark_buffer_dirty(bh_data);
                        sync_dirty_buffer(bh_data);
                }

                brelse(bh_data);

                if (copied == 0)
                        break;

                pos += copied;
                total_copied += copied;

                if (copied < bytes_to_copy)
                        break;
        }

        brelse(bh_index);

        if (pos > inode->i_size)
                i_size_write(inode, pos);

        inode->i_mtime = inode_set_ctime_current(inode);
        mark_inode_dirty(inode);

        iocb->ki_pos = pos;

        return total_copied;
}

static long ouichefs_file_ioctl(struct file *file,
				unsigned int cmd,
				unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	unsigned int i;
	unsigned int extent_count = 0;

	if (cmd != OUICHEFS_IOC_GET_EXTENTS)
		return -ENOTTY;

	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;

	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	for (i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
		if (le32_to_cpu(index->extents[i].count) == 0)
			continue;

		extent_count++;
	}

	pr_info("ouichefs: extents for inode %lu: %u extent(s)\n",
		inode->i_ino,
		extent_count);

	for (i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
		uint32_t start;
		uint32_t count;

		start = le32_to_cpu(index->extents[i].start);
		count = le32_to_cpu(index->extents[i].count);

		if (count == 0)
			continue;

		pr_info("[%u] start=%u count=%u\n",
			i,
			start,
			count);
	}

	brelse(bh_index);

	return 0;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.llseek = generic_file_llseek,
	.read_iter = ouichefs_file_read_iter,
	.write_iter = ouichefs_file_write_iter,
        .unlocked_ioctl = ouichefs_file_ioctl,
	.fsync = generic_file_fsync,
};

int ouichefs_truncate(struct inode *inode)
{
	int ret;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	size_t next_num_blocks;

	bh = sb_bread(sb, inode_info->index_block);
	if (!bh) {
		ret = -EIO;
		goto out;
	}

	ret = block_truncate_page(inode->i_mapping, inode->i_size, ouichefs_file_get_block);
	if (ret < 0)
		goto out_brelse;

	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh->b_data;

	next_num_blocks = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	for (size_t i = next_num_blocks;i < OUICHEFS_FILE_MAX_BLOCKS;++i) {
		uint32_t bno;
                bno = le32_to_cpu(index->extents[i].start);

		if (!bno)
			continue;

		put_block(sbi, bno);
		--inode->i_blocks;

		index->extents[i].start = cpu_to_le32(0);
                index->extents[i].count = cpu_to_le32(0);
	}

	mark_buffer_dirty(bh);
	brelse(bh);

	mark_inode_dirty(inode);

	return 0;

out_brelse:
	brelse(bh);
out:
	return ret;
}
