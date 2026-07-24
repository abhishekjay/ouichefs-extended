// SPDX-License-Identifier: GPL-2.0
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/buffer_head.h>
#include <linux/spinlock.h>

#include "ouichefs.h"

extern uint32_t reservation_size;

struct kobject *ouichefs_root_kobj = NULL;

struct ouichefs_stats {
	uint32_t free_blocks;
	uint32_t committed_blocks;
	uint32_t reserved_blocks;
	uint32_t files;
	uint32_t total_extents;
	uint32_t avg_extent_size;
	uint64_t max_file_size;
	uint32_t fragmentation;
	uint32_t gc_runs;
};

//stats aggregation logic
static void ouichefs_get_stats(struct ouichefs_sb_info *sbi, struct ouichefs_stats *stats)
{
	struct super_block *sb = sbi->sb;
	struct inode *inode;
	uint32_t i, ino;

	memset(stats, 0, sizeof(*stats));
	stats->free_blocks = sbi->nr_free_blocks;
	stats->gc_runs = sbi->gc_runs;

	//get reserved blocks from in-memory inodes safely
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

		spin_lock(&inode->i_lock);
		stats->reserved_blocks += ci->i_reserved_count;

		spin_unlock(&inode->i_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);

	//scan on-disk inodes for exact metrics without locking VFS cache
	for (ino = 1; ino < sbi->nr_inodes; ino++) {
		uint32_t inode_block = (ino / OUICHEFS_INODES_PER_BLOCK) + 1;
		uint32_t inode_shift = ino % OUICHEFS_INODES_PER_BLOCK;
		struct buffer_head *bh = sb_bread(sb, inode_block);

		if (!bh)
			continue;

		struct ouichefs_inode *disk_inode = (struct ouichefs_inode *)bh->b_data + inode_shift;

		if (le32_to_cpu(disk_inode->i_nlink) > 0 &&
				S_ISREG(le32_to_cpu(disk_inode->i_mode))) {
			stats->files++;

			uint64_t size = le32_to_cpu(disk_inode->i_size);

			if (size > stats->max_file_size)
				stats->max_file_size = size;

// calculating extent and fragmentation stats
			uint32_t idx_blk = le32_to_cpu(disk_inode->index_block);

			if (idx_blk) {
				struct buffer_head *idx_bh = sb_bread(sb, idx_blk);

				if (idx_bh) {
					struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)idx_bh->b_data;

					for (i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
						uint32_t count = le32_to_cpu(index->extents[i].count);

						if (count == 0)
							break;

						// only count real extents, skip holes (start == 0)
						if (le32_to_cpu(index->extents[i].start) != 0) {
							stats->total_extents++;
							stats->committed_blocks += count;
						}
					}
					brelse(idx_bh);
				}
			}
		}
		brelse(bh);
	}

	// Compute derived metrics (multiplied by 100 for decimals)
	if (stats->total_extents > 0)
		stats->avg_extent_size = (stats->committed_blocks * 100) / stats->total_extents;

	if (stats->files > 0)
		stats->fragmentation = (stats->total_extents * 100) / stats->files;

}

// Macro to quickly generate read-only sysfs files
#define OUICHEFS_RO_ATTR(name) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{ \
	struct ouichefs_sb_info *sbi = container_of(kobj, struct ouichefs_sb_info, kobj); \
	struct ouichefs_stats stats; \
	ouichefs_get_stats(sbi, &stats); \
	return sysfs_emit(buf, "%llu\n", (unsigned long long)stats.name); \
} \
static struct kobj_attribute name##_attr = __ATTR_RO(name);

OUICHEFS_RO_ATTR(free_blocks)
OUICHEFS_RO_ATTR(committed_blocks)
OUICHEFS_RO_ATTR(reserved_blocks)
OUICHEFS_RO_ATTR(files)
OUICHEFS_RO_ATTR(total_extents)
OUICHEFS_RO_ATTR(avg_extent_size)
OUICHEFS_RO_ATTR(max_file_size)
OUICHEFS_RO_ATTR(fragmentation)
OUICHEFS_RO_ATTR(gc_runs)

// Custom Read/Write file for reservation_size
static ssize_t reservation_size_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", reservation_size);
}

static ssize_t reservation_size_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	uint32_t val;

	if (kstrtouint(buf, 10, &val) < 0)
		return -EINVAL;
	reservation_size = val;
	return count;
}
static struct kobj_attribute reservation_size_attr = __ATTR(reservation_size, 0644, reservation_size_show, reservation_size_store);

// Group them all together
static struct attribute *ouichefs_attrs[] = {
	&free_blocks_attr.attr,
	&committed_blocks_attr.attr,
	&reserved_blocks_attr.attr,
	&files_attr.attr,
	&total_extents_attr.attr,
	&avg_extent_size_attr.attr,
	&max_file_size_attr.attr,
	&fragmentation_attr.attr,
	&gc_runs_attr.attr,
	&reservation_size_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ouichefs);

static void ouichefs_kobj_release(struct kobject *kobj)
{
	// Freed by put_super
}

static struct kobj_type ouichefs_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.release = ouichefs_kobj_release,
	.default_groups = ouichefs_groups,
};

// Global Initialization called when module loads
int ouichefs_sysfs_init(void)
{
	ouichefs_root_kobj = kobject_create_and_add("ouichefs", NULL);

	if (!ouichefs_root_kobj)
		return -ENOMEM;
	return 0;
}

void ouichefs_sysfs_exit(void)
{
	kobject_put(ouichefs_root_kobj);
}

// Per-Partition Initialization called on Mount
int ouichefs_sysfs_sb_init(struct ouichefs_sb_info *sbi, struct super_block *sb)
{
	sbi->sb = sb;
	return kobject_init_and_add(&sbi->kobj, &ouichefs_ktype, ouichefs_root_kobj, "%s", sb->s_id);
}

void ouichefs_sysfs_sb_exit(struct ouichefs_sb_info *sbi)
{
	kobject_del(&sbi->kobj);
	kobject_put(&sbi->kobj);
}


