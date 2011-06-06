/*
 * linux/fs/ext4/snapshot_ctl.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots control functions.
 */

#include <linux/statfs.h>
#include "ext4_jbd2.h"
#include "snapshot.h"

/*
 * General snapshot locking semantics:
 *
 * The snapshot_mutex:
 * -------------------
 * The majority of the code in the snapshot_{ctl,debug}.c files is called from
 * very few entry points in the code:
 * 1. {init,exit}_ext4_fs() - calls {init,exit}_ext4_snapshot() under BGL.
 * 2. ext4_{fill,put}_super() - calls ext4_snapshot_{load,destroy}() under
 *    VFS sb_lock, while f/s is not accessible to users.
 * 3. ext4_ioctl() - only place that takes snapshot_mutex (after i_mutex)
 *    and only entry point to snapshot control functions below.
 *
 * From the rules above it follows that all fields accessed inside
 * snapshot_{ctl,debug}.c are protected by one of the following:
 * - snapshot_mutex during snapshot control operations.
 * - VFS sb_lock during f/s mount/umount time.
 * - Big kernel lock during module init time.
 * Needless to say, either of the above is sufficient.
 * So if a field is accessed only inside snapshot_*.c it should be safe.
 *
 * The transaction handle:
 * -----------------------
 * Snapshot COW code (in snapshot.c) is called from block access hooks during a
 * transaction (with a transaction handle). This guaranties safe read access to
 * s_active_snapshot, without taking snapshot_mutex, because the latter is only
 * changed under journal_lock_updates() (while no transaction handles exist).
 *
 * The transaction handle is a per task struct, so there is no need to protect
 * fields on that struct (i.e. h_cowing, h_cow_*).
 */

/*
 * ext4_snapshot_set_active - set the current active snapshot
 * First, if current active snapshot exists, it is deactivated.
 * Then, if @inode is not NULL, the active snapshot is set to @inode.
 *
 * Called from ext4_snapshot_take() and ext4_snapshot_update() under
 * journal_lock_updates() and snapshot_mutex.
 * Called from ext4_snapshot_{load,destroy}() under sb_lock.
 *
 * Returns 0 on success and <0 on error.
 */
static int ext4_snapshot_set_active(struct super_block *sb,
		struct inode *inode)
{
	struct inode *old = EXT4_SB(sb)->s_active_snapshot;
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (old == inode)
		return 0;

	/* add new active snapshot reference */
	if (inode && !igrab(inode))
		return -EIO;

	/* point of no return - replace old with new snapshot */
	if (old) {
		ext4_clear_inode_snapstate(old, EXT4_SNAPSTATE_ACTIVE);
		snapshot_debug(1, "snapshot (%u) deactivated\n",
			       old->i_generation);
		/* remove old active snapshot reference */
		iput(old);
	}
	if (inode) {
		/*
		 * Set up the jbd2_inode - we are about to file_inode soon...
		 */
		if (!ei->jinode) {
			struct jbd2_inode *jinode;
			jinode = jbd2_alloc_inode(GFP_KERNEL);

			spin_lock(&inode->i_lock);
			if (!ei->jinode) {
				if (!jinode) {
					spin_unlock(&inode->i_lock);
					return -ENOMEM;
				}
				ei->jinode = jinode;
				jbd2_journal_init_jbd_inode(ei->jinode, inode);
				jinode = NULL;
			}
			spin_unlock(&inode->i_lock);
			if (unlikely(jinode != NULL))
				jbd2_free_inode(jinode);
		}
		/* ACTIVE implies LIST */
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_LIST);
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE);
		snapshot_debug(1, "snapshot (%u) activated\n",
			       inode->i_generation);
	}
	EXT4_SB(sb)->s_active_snapshot = inode;

	return 0;
}
/*
 * ext4_snapshot_reset_bitmap_cache():
 *
 * Resets the COW/exclude bitmap cache for all block groups.
 *
 * Called from snapshot_take() under journal_lock_updates().
 */
static void ext4_snapshot_reset_bitmap_cache(struct super_block *sb)
{
	struct ext4_group_info *grp;
	int i;

	for (i = 0; i < EXT4_SB(sb)->s_groups_count; i++) {
		grp = ext4_get_group_info(sb, i);
		grp->bg_cow_bitmap = 0;
		cond_resched();
	}
}

/*
 * A modified version of ext4_orphan_add(), used to add a snapshot inode
 * to the head of the on-disk and in-memory lists.
 * in-memory i_orphan list field is overloaded, because inodes on snapshots
 * list cannot be unlinked nor truncated.
 */
static int ext4_inode_list_add(handle_t *handle, struct inode *inode,
		__u32 *i_next, __le32 *s_last,
		struct list_head *s_list, const char *name)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_iloc iloc;
	int err = 0, rc;

	if (!ext4_handle_valid(handle))
		return 0;

	mutex_lock(&EXT4_SB(sb)->s_orphan_lock);
	if (!list_empty(&EXT4_I(inode)->i_orphan))
		goto out_unlock;

	BUFFER_TRACE(EXT4_SB(sb)->s_sbh, "get_write_access");
	err = ext4_journal_get_write_access(handle, EXT4_SB(sb)->s_sbh);
	if (err)
		goto out_unlock;

	err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto out_unlock;

	snapshot_debug(4, "add inode %lu to %s list\n",
			inode->i_ino, name);

	/* Insert this inode at the head of the on-disk inode list... */
	*i_next = le32_to_cpu(*s_last);
	*s_last = cpu_to_le32(inode->i_ino);
	err = ext4_handle_dirty_metadata(handle, NULL, EXT4_SB(sb)->s_sbh);
	rc = ext4_mark_iloc_dirty(handle, inode, &iloc);
	if (!err)
		err = rc;

	/* Only add to the head of the in-memory list if all the
	 * previous operations succeeded. */
	if (!err)
		list_add(&EXT4_I(inode)->i_orphan, s_list);

	snapshot_debug(4, "last_%s will point to inode %lu\n",
			name, inode->i_ino);
	snapshot_debug(4, "%s inode %lu will point to inode %d\n",
			name, inode->i_ino, *i_next);
out_unlock:
	mutex_unlock(&EXT4_SB(sb)->s_orphan_lock);
	ext4_std_error(inode->i_sb, err);
	return err;
}

static int ext4_snapshot_list_add(handle_t *handle, struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

	return ext4_inode_list_add(handle, inode, &NEXT_SNAPSHOT(inode),
			&sbi->s_es->s_snapshot_list,
			&sbi->s_snapshot_list, "snapshot");
}

#define NEXT_INODE_OFFSET (((char *)inode)-((char *)i_next))
#define NEXT_INODE(i_prev) (*(__u32 *)(((char *)i_prev)-NEXT_INODE_OFFSET))

/*
 * A modified version of ext4_orphan_del(), used to remove a snapshot inode
 * from the on-disk and in-memory lists.
 * in-memory i_orphan list field is overloaded, because inodes on snapshots
 * list cannot be unlinked nor truncated.
 */
static int ext4_inode_list_del(handle_t *handle, struct inode *inode,
		__u32 *i_next, __le32 *s_last,
		struct list_head *s_list, const char *name)
{
	struct list_head *prev;
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi;
	__u32 ino_next;
	struct ext4_iloc iloc;
	int err = 0;

	/* ext4_handle_valid() assumes a valid handle_t pointer */
	if (handle && !ext4_handle_valid(handle))
		return 0;

	mutex_lock(&EXT4_SB(inode->i_sb)->s_orphan_lock);
	if (list_empty(&ei->i_orphan))
		goto out;

	ino_next = *i_next;
	prev = ei->i_orphan.prev;
	sbi = EXT4_SB(inode->i_sb);

	snapshot_debug(4, "remove inode %lu from %s list\n", inode->i_ino,
		       name);

	list_del_init(&ei->i_orphan);

	/* If we're on an error path, we may not have a valid
	 * transaction handle with which to update the orphan list on
	 * disk, but we still need to remove the inode from the linked
	 * list in memory. */
	if (sbi->s_journal && !handle)
		goto out;

	err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto out_err;

	if (prev == s_list) {
		snapshot_debug(4, "last_%s will point to inode %lu\n", name,
					   (long unsigned int)ino_next);
		BUFFER_TRACE(sbi->s_sbh, "get_write_access");
		err = ext4_journal_get_write_access(handle, sbi->s_sbh);
		if (err)
			goto out_brelse;
		*s_last = cpu_to_le32(ino_next);
		err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
	} else {
		struct ext4_iloc iloc2;
		struct inode *i_prev;
		i_prev = &list_entry(prev, struct ext4_inode_info,
				     i_orphan)->vfs_inode;

		snapshot_debug(4, "%s inode %lu will point to inode %lu\n",
			  name, i_prev->i_ino, (long unsigned int)ino_next);
		err = ext4_reserve_inode_write(handle, i_prev, &iloc2);
		if (err)
			goto out_brelse;
		NEXT_INODE(i_prev) = ino_next;
		err = ext4_mark_iloc_dirty(handle, i_prev, &iloc2);
	}
	if (err)
		goto out_brelse;
	*i_next = 0;
	err = ext4_mark_iloc_dirty(handle, inode, &iloc);

out_err:
	ext4_std_error(inode->i_sb, err);
out:
	mutex_unlock(&EXT4_SB(inode->i_sb)->s_orphan_lock);
	return err;

out_brelse:
	brelse(iloc.bh);
	goto out_err;
}

static int ext4_snapshot_list_del(handle_t *handle, struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

	return ext4_inode_list_del(handle, inode, &NEXT_SNAPSHOT(inode),
			&sbi->s_es->s_snapshot_list,
			&sbi->s_snapshot_list, "snapshot");
}


/*
 * Snapshot control functions
 *
 * Snapshot files are controlled by changing snapshot flags with chattr and
 * moving the snapshot file through the stages of its life cycle:
 *
 * 1. Creating a snapshot file
 * The snapfile flag is changed for directories only (chattr +x), so
 * snapshot files must be created inside a snapshots directory.
 * They inherit the flag at birth and they die with it.
 * This helps to avoid various race conditions when changing
 * regular files to snapshots and back.
 * Snapshot files are assigned with read-only address space operations, so
 * they are not writable for users.
 *
 * 2. Taking a snapshot
 * An empty snapshot file becomes the active snapshot after it is added to the
 * head on the snapshots list by setting its snapshot list flag (chattr -X +S).
 * snapshot_create() verifies that the file is empty and pre-allocates some
 * blocks during the ioctl transaction.  snapshot_take() locks journal updates
 * and copies some file system block to the pre-allocated blocks and then adds
 * the snapshot file to the on-disk list and sets it as the active snapshot.
 *
 * 3. Mounting a snapshot
 * A snapshot on the list can be enabled for user read access by setting the
 * enabled flag (chattr -X +n) and disabled by clearing the enabled flag.
 * An enabled snapshot can be mounted via a loop device and mounted as a
 * read-only ext2 filesystem.
 *
 * 4. Deleting a snapshot
 * A non-mounted and disabled snapshot may be marked for removal from the
 * snapshots list by requesting to clear its snapshot list flag (chattr -X -S).
 * The process of removing a snapshot from the list varies according to the
 * dependencies between the snapshot and older snapshots on the list:
 * - if all older snapshots are deleted, the snapshot is removed from the list.
 * - if some older snapshots are enabled, snapshot_shrink() is called to free
 *   unused blocks, but the snapshot remains on the list.
 * - if all older snapshots are disabled, snapshot_merge() is called to move
 *   used blocks to an older snapshot and the snapshot is removed from the list.
 *
 * 5. Unlinking a snapshot file
 * When a snapshot file is no longer (or never was) on the snapshots list, it
 * may be unlinked.  Snapshots on the list are protected from user unlink and
 * truncate operations.
 *
 * 6. Discarding all snapshots
 * An irregular way to abruptly end the lives of all snapshots on the list is by
 * detaching the snapshot list head using the command: tune2fs -O ^has_snapshot.
 * This action is applicable on an un-mounted ext4 filesystem.  After mounting
 * the filesystem, the discarded snapshot files will not be loaded, they will
 * not have the snapshot list flag and therefore, may be unlinked.
 */
static int ext4_snapshot_enable(struct inode *inode);
static int ext4_snapshot_disable(struct inode *inode);
static int ext4_snapshot_create(struct inode *inode);
static int ext4_snapshot_delete(struct inode *inode);

/*
 * ext4_snapshot_get_flags() check snapshot state
 * Called from ext4_ioctl() under i_mutex
 */
void ext4_snapshot_get_flags(struct inode *inode, struct file *filp)
{
	unsigned int open_count = filp->f_path.dentry->d_count;

	/*
	 * 1 count for ioctl (lsattr)
	 * greater count means the snapshot is open by user (mounted?)
	 * We rely on d_count because snapshot shouldn't have hard links.
	 */
	if (ext4_snapshot_list(inode) && open_count > 1)
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_OPEN);
	else
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_OPEN);
	/* copy persistent flags to dynamic state flags */
	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED))
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_DELETED);
	else
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_DELETED);
	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_SHRUNK))
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_SHRUNK);
	else
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_SHRUNK);
}

/*
 * ext4_snapshot_set_flags() monitors snapshot state changes
 * Called from ext4_ioctl() under i_mutex and snapshot_mutex
 */
int ext4_snapshot_set_flags(handle_t *handle, struct inode *inode,
			     unsigned int flags)
{
	unsigned int oldflags = ext4_get_snapstate_flags(inode);
	int err = 0;

	if ((flags ^ oldflags) & 1UL<<EXT4_SNAPSTATE_ENABLED) {
		/* enabled/disabled the snapshot during transaction */
		if (flags & 1UL<<EXT4_SNAPSTATE_ENABLED)
			err = ext4_snapshot_enable(inode);
		else
			err = ext4_snapshot_disable(inode);
	}
	if (err)
		goto out;

	if ((flags ^ oldflags) & 1UL<<EXT4_SNAPSTATE_LIST) {
		/* add/delete to snapshots list during transaction */
		if (flags & 1UL<<EXT4_SNAPSTATE_LIST)
			err = ext4_snapshot_create(inode);
		else
			err = ext4_snapshot_delete(inode);
	}
	if (err)
		goto out;

out:
	/*
	 * retake reserve inode write from ext4_ioctl() and mark inode
	 * dirty
	 */
	if (!err)
		err = ext4_mark_inode_dirty(handle, inode);
	return err;
}

/*
 * If we have fewer than nblocks credits,
 * extend transaction by at most EXT4_MAX_TRANS_DATA.
 * If that fails, restart the transaction &
 * regain write access for the inode block.
 */
int __extend_or_restart_transaction(const char *where,
		handle_t *handle, struct inode *inode, int nblocks)
{
	int err;

	if (ext4_handle_has_enough_credits(handle, nblocks))
		return 0;

	if (nblocks < EXT4_MAX_TRANS_DATA)
		nblocks = EXT4_MAX_TRANS_DATA;

	err = __ext4_journal_extend(where, handle, nblocks);
	if (err < 0)
		return err;
	if (err) {
		if (inode) {
			/* lazy way to do mark_iloc_dirty() */
			err = ext4_mark_inode_dirty(handle, inode);
			if (err)
				return err;
		}
		err = __ext4_journal_restart(where, handle, nblocks);
		if (err)
			return err;
		if (inode)
			/* lazy way to do reserve_inode_write() */
			err = ext4_mark_inode_dirty(handle, inode);
	}

	return err;
}

#define extend_or_restart_transaction(handle, nblocks)			\
	__extend_or_restart_transaction(__func__, (handle), NULL, (nblocks))
#define extend_or_restart_transaction_inode(handle, inode, nblocks)	\
	__extend_or_restart_transaction(__func__, (handle), (inode), (nblocks))

/*
 * helper function for snapshot_create().
 * places pre-allocated [d,t]ind blocks in position
 * after they have been allocated as direct blocks.
 */
static inline int ext4_snapshot_shift_blocks(struct ext4_inode_info *ei,
		int from, int to, int count)
{
	int i, err = -EIO;

	/* move from direct blocks range */
	BUG_ON(from < 0 || from + count > EXT4_NDIR_BLOCKS);
	/* to indirect blocks range */
	BUG_ON(to < EXT4_NDIR_BLOCKS || to + count > EXT4_SNAPSHOT_N_BLOCKS);

	/*
	 * truncate_mutex is held whenever allocating or freeing inode
	 * blocks.
	 */
	down_write(&ei->i_data_sem);

	/*
	 * verify that 'from' blocks are allocated
	 * and that 'to' blocks are not allocated.
	 */
	for (i = 0; i < count; i++)
		if (!ei->i_data[from+i] ||
				ei->i_data[(to+i)%EXT4_N_BLOCKS])
			goto out;

	/*
	 * shift 'count' blocks from position 'from' to 'to'
	 */
	for (i = 0; i < count; i++) {
		ei->i_data[(to+i)%EXT4_N_BLOCKS] = ei->i_data[from+i];
		ei->i_data[from+i] = 0;
	}
	err = 0;
out:
	up_write(&ei->i_data_sem);
	return err;
}

static ext4_fsblk_t ext4_get_inode_block(struct super_block *sb,
					 unsigned long ino,
					 struct ext4_iloc *iloc)
{
	ext4_fsblk_t block;
	struct ext4_group_desc *desc;
	int inodes_per_block, inode_offset;

	iloc->bh = NULL;
	iloc->offset = 0;
	iloc->block_group = 0;

	if (!ext4_valid_inum(sb, ino))
		return 0;

	iloc->block_group = (ino - 1) / EXT4_INODES_PER_GROUP(sb);
	desc = ext4_get_group_desc(sb, iloc->block_group, NULL);
	if (!desc)
		return 0;

	/*
	 * Figure out the offset within the block group inode table
	 */
	inodes_per_block = (EXT4_BLOCK_SIZE(sb) / EXT4_INODE_SIZE(sb));
	inode_offset = ((ino - 1) %
			EXT4_INODES_PER_GROUP(sb));
	block = ext4_inode_table(sb, desc) + (inode_offset / inodes_per_block);
	iloc->offset = (inode_offset % inodes_per_block) * EXT4_INODE_SIZE(sb);
	return block;
}

/*
 * ext4_snapshot_create() initializes a snapshot file
 * and adds it to the list of snapshots
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_create(struct inode *inode)
{
	handle_t *handle;
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	int i, err, ret;
	int count, nind;
	const long double_blocks = (1 << (2 * SNAPSHOT_ADDR_PER_BLOCK_BITS));
	struct buffer_head *bh = NULL;
	struct ext4_group_desc *desc;
	unsigned long ino;
	struct ext4_iloc iloc;
	ext4_fsblk_t bmap_blk = 0, imap_blk = 0, inode_blk = 0;
	ext4_fsblk_t prev_inode_blk = 0;
	ext4_fsblk_t snapshot_blocks = ext4_blocks_count(sbi->s_es);
	struct list_head *l, *list = &sbi->s_snapshot_list;

	if (!list_empty(list)) {
		struct inode *last_snapshot =
			&list_first_entry(list, struct ext4_inode_info,
					  i_snaplist)->vfs_inode;
		if (active_snapshot != last_snapshot) {
			snapshot_debug(1, "failed to add snapshot because last"
				       " snapshot (%u) is not active\n",
				       last_snapshot->i_generation);
			return -EINVAL;
		}
	}

	/* prevent take of unlinked snapshot file */
	if (!inode->i_nlink) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it has 0 nlink count\n",
				inode->i_ino);
		return -EINVAL;
	}

	/* prevent recycling of old snapshot files */
	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED)) {
		snapshot_debug(1, "deleted snapshot file (ino=%lu) cannot "
				"be reused - it may be unlinked\n",
				inode->i_ino);
		return -EINVAL;
	}

	/* verify that no inode blocks are allocated */
	for (i = 0; i < EXT4_N_BLOCKS; i++) {
		if (ei->i_data[i])
			break;
	}
	/* Don't need i_size_read because we hold i_mutex */
	if (i != EXT4_N_BLOCKS ||
		inode->i_size > 0 || ei->i_disksize > 0) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it is not empty (i_data[%d]=%u, "
				"i_size=%lld, i_disksize=%lld)\n",
				inode->i_ino, i, ei->i_data[i],
				inode->i_size, ei->i_disksize);
		return -EINVAL;
	}

	/*
	 * Take a reference to the small transaction that started in
	 * ext4_ioctl() We will extend or restart this transaction as we go
	 * along.  journal_start(n > 1) would not have increase the buffer
	 * credits.
	 */
	handle = ext4_journal_start(inode, 1);

	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

	/* record the new snapshot ID in the snapshot inode generation field */
	inode->i_generation = le32_to_cpu(sbi->s_es->s_snapshot_id) + 1;
	if (inode->i_generation == 0)
		/* 0 is not a valid snapshot id */
		inode->i_generation = 1;

	/* record the file system size in the snapshot inode disksize field */
	SNAPSHOT_SET_BLOCKS(inode, snapshot_blocks);

	/* add snapshot list reference */
	if (!igrab(inode)) {
		err = -EIO;
		goto out_handle;
	}
	/*
	 * First, the snapshot is added to the in-memory and on-disk list.
	 * At the end of snapshot_take(), it will become the active snapshot
	 * in-memory and on-disk.
	 * Finally, if snapshot_create() or snapshot_take() has failed,
	 * snapshot_update() will remove it from the in-memory and on-disk list.
	 */
	err = ext4_snapshot_list_add(handle, inode);
	/* add snapshot list reference */
	if (err) {
		snapshot_debug(1, "failed to add snapshot (%u) to list\n",
			       inode->i_generation);
		iput(inode);
		goto out_handle;
	}
	l = list->next;

	err = ext4_mark_inode_dirty(handle, inode);
	if (err)
		goto out_handle;

	/* small filesystems can be mapped with just 1 double indirect block */
	nind = 1;
	if (snapshot_blocks > double_blocks)
		/* add up to 4 triple indirect blocks to map 2^32 blocks */
		nind += ((snapshot_blocks - double_blocks) >>
			(3 * SNAPSHOT_ADDR_PER_BLOCK_BITS)) + 1;
	if (nind > 2 + EXT4_SNAPSHOT_EXTRA_TIND_BLOCKS) {
		snapshot_debug(1, "need too many [d,t]ind blocks (%d) "
				"for snapshot (%u)\n",
				nind, inode->i_generation);
		err = -EFBIG;
		goto out_handle;
	}

	err = extend_or_restart_transaction_inode(handle, inode,
			nind * EXT4_DATA_TRANS_BLOCKS(sb));
	if (err)
		goto out_handle;

	/* pre-allocate and zero out [d,t]ind blocks */
	for (i = 0; i < nind; i++) {
		brelse(bh);
		bh = ext4_getblk(handle, inode, i, SNAPMAP_WRITE, &err);
		if (!bh)
			break;
		/* zero out indirect block and journal as dirty metadata */
		err = ext4_journal_get_write_access(handle, bh);
		if (err)
			break;
		lock_buffer(bh);
		memset(bh->b_data, 0, bh->b_size);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		err = ext4_handle_dirty_metadata(handle, NULL, bh);
		if (err)
			break;
	}
	brelse(bh);
	if (!bh || err) {
		snapshot_debug(1, "failed to initiate [d,t]ind block (%d) "
				"for snapshot (%u)\n",
				i, inode->i_generation);
		goto out_handle;
	}
	/* place pre-allocated [d,t]ind blocks in position */
	err = ext4_snapshot_shift_blocks(ei, 0, EXT4_DIND_BLOCK, nind);
	if (err) {
		snapshot_debug(1, "failed to move pre-allocated [d,t]ind blocks"
				" for snapshot (%u)\n",
				inode->i_generation);
		goto out_handle;
	}

	/* allocate super block and group descriptors for snapshot */
	count = sbi->s_gdb_count + 1;
	err = count;
	for (i = 0; err > 0 && i < count; i += err) {
		err = extend_or_restart_transaction_inode(handle, inode,
				EXT4_DATA_TRANS_BLOCKS(sb));
		if (err)
			goto out_handle;
		err = ext4_snapshot_map_blocks(handle, inode, i, count - i,
						NULL, SNAPMAP_WRITE);
	}
	if (err <= 0) {
		snapshot_debug(1, "failed to allocate super block and %d "
			       "group descriptor blocks for snapshot (%u)\n",
			       count - 1, inode->i_generation);
		if (err)
			err = -EIO;
		goto out_handle;
	}

	/* start with root inode and continue with snapshot list */
	ino = EXT4_ROOT_INO;
alloc_inode_blocks:
	/*
	 * pre-allocate the following blocks in the new snapshot:
	 * - block and inode bitmap blocks of ino's block group
	 * - inode table block that contains ino
	 */
	err = extend_or_restart_transaction_inode(handle, inode,
			3 * EXT4_DATA_TRANS_BLOCKS(sb));
	if (err)
		goto out_handle;

	inode_blk = ext4_get_inode_block(sb, ino, &iloc);

	if (!inode_blk || inode_blk == prev_inode_blk)
		goto next_snapshot;

	/* not same inode and bitmap blocks as prev snapshot */
	prev_inode_blk = inode_blk;
	bmap_blk = 0;
	imap_blk = 0;
	desc = ext4_get_group_desc(sb, iloc.block_group, NULL);
	if (!desc)
		goto next_snapshot;

	bmap_blk = ext4_block_bitmap(sb, desc);
	imap_blk = ext4_inode_bitmap(sb, desc);
	if (!bmap_blk || !imap_blk)
		goto next_snapshot;

	count = 1;
	if (imap_blk == bmap_blk + 1)
		count++;
	if ((count > 1) && (inode_blk == imap_blk + 1))
		count++;
	/* try to allocate all blocks at once */
	err = ext4_snapshot_map_blocks(handle, inode,
			bmap_blk, count,
			NULL, SNAPMAP_WRITE);
	count = err;
	/* allocate remaining blocks one by one */
	if (err > 0 && count < 2)
		err = ext4_snapshot_map_blocks(handle, inode,
				imap_blk, 1,
				NULL,
				SNAPMAP_WRITE);
	if (err > 0 && count < 3)
		err = ext4_snapshot_map_blocks(handle, inode,
				inode_blk, 1,
				NULL,
				SNAPMAP_WRITE);
next_snapshot:
	if (!bmap_blk || !imap_blk || !inode_blk || err < 0) {
#ifdef CONFIG_EXT4_DEBUG
		ext4_fsblk_t blk0 = iloc.block_group *
			EXT4_BLOCKS_PER_GROUP(sb);
		snapshot_debug(1, "failed to allocate block/inode bitmap "
				"or inode table block of inode (%lu) "
				"(%llu,%llu,%llu/%u) for snapshot (%u)\n",
				ino, bmap_blk - blk0,
				imap_blk - blk0, inode_blk - blk0,
				iloc.block_group, inode->i_generation);
#endif
		if (!err)
			err = -EIO;
		goto out_handle;
	}
	if (l != list) {
		ino = list_entry(l, struct ext4_inode_info,
				i_snaplist)->vfs_inode.i_ino;
		l = l->next;
		goto alloc_inode_blocks;
	}
	snapshot_debug(1, "snapshot (%u) created\n", inode->i_generation);
	err = 0;
out_handle:
	ret = ext4_journal_stop(handle);
	if (!err)
		err = ret;
	return err;
}

/*
 * ext4_snapshot_copy_block() - copy block to new snapshot
 * @snapshot:	new snapshot to copy block to
 * @bh:		source buffer to be copied
 * @mask:	if not NULL, mask buffer data before copying to snapshot
 *		(used to mask block bitmap with exclude bitmap)
 * @name:	name of copied block to print
 * @idx:	index of copied block to print
 *
 * Called from ext4_snapshot_take() under journal_lock_updates()
 * Returns snapshot buffer on success, NULL on error
 */
static struct buffer_head *ext4_snapshot_copy_block(struct inode *snapshot,
		struct buffer_head *bh, const char *mask,
		const char *name, unsigned long idx)
{
	struct buffer_head *sbh = NULL;
	int err;

	if (!bh)
		return NULL;

	sbh = ext4_getblk(NULL, snapshot,
			SNAPSHOT_IBLOCK(bh->b_blocknr),
			SNAPMAP_READ, &err);

	if (!sbh || sbh->b_blocknr == bh->b_blocknr) {
		snapshot_debug(1, "failed to copy %s (%lu) "
				"block [%llu/%llu] to snapshot (%u)\n",
				name, idx,
				SNAPSHOT_BLOCK_TUPLE(bh->b_blocknr),
				snapshot->i_generation);
		brelse(sbh);
		return NULL;
	}

	ext4_snapshot_copy_buffer(sbh, bh, mask);

	snapshot_debug(4, "copied %s (%lu) block [%llu/%llu] "
			"to snapshot (%u)\n",
			name, idx,
			SNAPSHOT_BLOCK_TUPLE(bh->b_blocknr),
			snapshot->i_generation);
	return sbh;
}

/*
 * List of blocks which are copied to snapshot for every special inode.
 * Keep block bitmap first and inode table block last in the list.
 */
enum copy_inode_block {
	COPY_BLOCK_BITMAP,
	COPY_INODE_BITMAP,
	COPY_INODE_TABLE,
	COPY_INODE_BLOCKS_NUM
};

static char *copy_inode_block_name[COPY_INODE_BLOCKS_NUM] = {
	"block bitmap",
	"inode bitmap",
	"inode table"
};

/*
 * ext4_snapshot_take() makes a new snapshot file
 * into the active snapshot
 *
 * this function calls journal_lock_updates()
 * and should not be called during a journal transaction
 * Called from ext4_ioctl() under i_mutex and snapshot_mutex
 */
int ext4_snapshot_take(struct inode *inode)
{
	struct list_head *list = &EXT4_SB(inode->i_sb)->s_snapshot_list;
	struct list_head *l = list->next;
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = NULL;
	struct buffer_head *es_bh = NULL;
	struct buffer_head *sbh = NULL;
	struct buffer_head *bhs[COPY_INODE_BLOCKS_NUM] = { NULL };
	const char *mask = NULL;
	struct inode *curr_inode;
	struct ext4_iloc iloc;
	struct ext4_group_desc *desc;
	ext4_fsblk_t prev_inode_blk = 0;
	struct ext4_inode *raw_inode;
	blkcnt_t excluded_blocks = 0;
	int fixing = 0;
	int i;
	int err = -EIO;
	u64 snapshot_r_blocks;
	struct kstatfs statfs;

	if (!sbi->s_sbh)
		goto out_err;
	else if (sbi->s_sbh->b_blocknr != 0) {
		snapshot_debug(1, "warning: unexpected super block at block "
			"(%lld:%d)!\n", (long long)sbi->s_sbh->b_blocknr,
			(int)((char *)sbi->s_es - (char *)sbi->s_sbh->b_data));
	} else if (sbi->s_es->s_magic != cpu_to_le16(EXT4_SUPER_MAGIC)) {
		snapshot_debug(1, "warning: super block of snapshot (%u) is "
			       "broken!\n", inode->i_generation);
	} else
		es_bh = ext4_getblk(NULL, inode, SNAPSHOT_IBLOCK(0),
				   SNAPMAP_READ, &err);

	if (!es_bh || es_bh->b_blocknr == 0) {
		snapshot_debug(1, "warning: super block of snapshot (%u) not "
			       "allocated\n", inode->i_generation);
		goto out_err;
	} else {
		snapshot_debug(4, "super block of snapshot (%u) mapped to "
			       "block (%lld)\n", inode->i_generation,
			       (long long)es_bh->b_blocknr);
		es = (struct ext4_super_block *)(es_bh->b_data +
						  ((char *)sbi->s_es -
						   sbi->s_sbh->b_data));
	}

	err = -EIO;
	/* update fs statistics to calculate snapshot reserved space */
	if (ext4_statfs_sb(sb, &statfs)) {
		snapshot_debug(1, "failed to statfs before snapshot (%u) "
			       "take\n", inode->i_generation);
		goto out_err;
	}
	/*
	 * Estimate maximum disk space for snapshot file metadata based on:
	 * 1 indirect block per 1K fs blocks (to map moved data blocks)
	 * +1 data block per 1K fs blocks (to copy indirect blocks)
	 * +1 data block per fs meta block (to copy meta blocks)
	 * +1 data block per directory (to copy small directory index blocks)
	 * +1 data block per X inodes (to copy large directory index blocks)
	 *
	 * We estimate no. of dir blocks from no. of allocated inode, assuming
	 * an avg. dir record size of 64 bytes. This assumption can break in
	 * 2 cases:
	 *   1. long file names (in avg.)
	 *   2. large no. of hard links (many dir records for the same inode)
	 *
	 * Under estimation can lead to potential ENOSPC during COW, which
	 * will trigger an ext4_error(). Hopefully, error behavior is set to
	 * remount-ro, so snapshot will not be corrupted.
	 *
	 * XXX: reserved space may be too small in data jounaling mode,
	 *      which is currently not supported.
	 */
#define AVG_DIR_RECORD_SIZE_BITS 6 /* 64 bytes */
#define AVG_INODES_PER_DIR_BLOCK \
	(SNAPSHOT_BLOCK_SIZE_BITS - AVG_DIR_RECORD_SIZE_BITS)
	snapshot_r_blocks = 2 * (statfs.f_blocks >>
			SNAPSHOT_ADDR_PER_BLOCK_BITS) +
		statfs.f_spare[0] + statfs.f_spare[1] +
		((statfs.f_files - statfs.f_ffree) >>
		 AVG_INODES_PER_DIR_BLOCK);

	/* verify enough free space before taking the snapshot */
	if (statfs.f_bfree < snapshot_r_blocks) {
		err = -ENOSPC;
		goto out_err;
	}

	/*
	 * flush journal to disk and clear the RECOVER flag
	 * before taking the snapshot
	 */
	freeze_super(sb);
	lock_super(sb);

#ifdef CONFIG_EXT4_DEBUG
	if (snapshot_enable_test[SNAPTEST_TAKE]) {
		snapshot_debug(1, "taking snapshot (%u) ...\n",
				inode->i_generation);
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_TAKE);
	}
#endif

	/*
	 * copy group descriptors to snapshot
	 */
	for (i = 0; i < sbi->s_gdb_count; i++) {
		brelse(sbh);
		sbh = ext4_snapshot_copy_block(inode,
				sbi->s_group_desc[i], NULL,
				"GDT", i);
		if (!sbh)
			goto out_unlockfs;
	}

	/* start with root inode and continue with snapshot list */
	curr_inode = sb->s_root->d_inode;
copy_inode_blocks:
	/*
	 * copy the following blocks to the new snapshot:
	 * - block and inode bitmap blocks of curr_inode block group
	 * - inode table block that contains curr_inode
	 */
	iloc.block_group = 0;
	err = ext4_get_inode_loc(curr_inode, &iloc);
	brelse(bhs[COPY_INODE_TABLE]);
	bhs[COPY_INODE_TABLE] = iloc.bh;
	desc = ext4_get_group_desc(sb, iloc.block_group, NULL);
	if (err || !desc) {
		snapshot_debug(1, "failed to read inode and bitmap blocks "
			       "of inode (%lu)\n", curr_inode->i_ino);
		err = err ? : -EIO;
		goto out_unlockfs;
	}
	if (fixing)
		goto fix_inode_copy;
	if (iloc.bh->b_blocknr == prev_inode_blk)
		goto next_inode;
	prev_inode_blk = iloc.bh->b_blocknr;
	brelse(bhs[COPY_BLOCK_BITMAP]);
	bhs[COPY_BLOCK_BITMAP] = sb_bread(sb,
			ext4_block_bitmap(sb, desc));
	brelse(bhs[COPY_INODE_BITMAP]);
	bhs[COPY_INODE_BITMAP] = sb_bread(sb,
			ext4_inode_bitmap(sb, desc));
	err = -EIO;
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++) {
		brelse(sbh);
		sbh = ext4_snapshot_copy_block(inode, bhs[i], mask,
				copy_inode_block_name[i], curr_inode->i_ino);
		if (!sbh)
			goto out_unlockfs;
		mask = NULL;
	}
	/* this is the copy pass */
	goto next_inode;
fix_inode_copy:
	/* this is the fixing pass */
	/* get snapshot copy of raw inode */
	brelse(sbh);
	sbh = ext4_getblk(NULL, inode,
			SNAPSHOT_IBLOCK(iloc.bh->b_blocknr),
			SNAPMAP_READ, &err);
	if (!sbh)
		goto out_unlockfs;
	iloc.bh = sbh;
	raw_inode = ext4_raw_inode(&iloc);
	/*
	 * Snapshot inode blocks are excluded from COW bitmap,
	 * so they appear to be not allocated in the snapshot's
	 * block bitmap.  If we want the snapshot image to pass
	 * fsck with no errors, we need to detach those blocks
	 * from the copy of the snapshot inode, so we fix the
	 * snapshot inodes to appear as empty regular files.
	 */
	excluded_blocks += ext4_inode_blocks(raw_inode,
			EXT4_I(curr_inode)) >>
		(curr_inode->i_blkbits - 9);
	lock_buffer(sbh);
	ext4_isize_set(raw_inode, 0);
	raw_inode->i_blocks_lo = 0;
	raw_inode->i_blocks_high = 0;
	raw_inode->i_flags &= cpu_to_le32(~EXT4_SNAPFILE_FL);
	memset(raw_inode->i_block, 0, sizeof(raw_inode->i_block));
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);

next_inode:
	if (l == list && !fixing) {
		/* done with copy pass - start fixing pass */
		l = l->next;
		fixing = 1;
	}
	if (l != list) {
		curr_inode = &list_entry(l, struct ext4_inode_info,
				       i_snaplist)->vfs_inode;
		l = l->next;
		goto copy_inode_blocks;
	}

	/*
	 * copy super block to snapshot and fix it
	 */
	lock_buffer(es_bh);
	memcpy(es_bh->b_data, sbi->s_sbh->b_data, sb->s_blocksize);
	/* set the IS_SNAPSHOT flag to signal fsck this is a snapshot */
	es->s_flags |= cpu_to_le32(EXT4_FLAGS_IS_SNAPSHOT);
	/* reset snapshots list in snapshot's super block copy */
	es->s_snapshot_inum = 0;
	es->s_snapshot_list = 0;
	/* fix free blocks count after clearing old snapshot inode blocks */
	ext4_free_blocks_count_set(es, ext4_free_blocks_count(es) +
				excluded_blocks);
	set_buffer_uptodate(es_bh);
	unlock_buffer(es_bh);
	mark_buffer_dirty(es_bh);
	sync_dirty_buffer(es_bh);


	/* reset i_size and invalidate page cache */
	SNAPSHOT_SET_DISABLED(inode);
	/* reset COW bitmap cache */
	ext4_snapshot_reset_bitmap_cache(sb);
	/* set as in-memory active snapshot */
	err = ext4_snapshot_set_active(sb, inode);
	if (err)
		goto out_unlockfs;

	/* set as on-disk active snapshot */
	sbi->s_es->s_snapshot_r_blocks_count = cpu_to_le64(snapshot_r_blocks);

	sbi->s_es->s_snapshot_id =
		cpu_to_le32(le32_to_cpu(sbi->s_es->s_snapshot_id) + 1);
	if (sbi->s_es->s_snapshot_id == 0)
		/* 0 is not a valid snapshot id */
		sbi->s_es->s_snapshot_id = cpu_to_le32(1);
	sbi->s_es->s_snapshot_inum = cpu_to_le32(inode->i_ino);
	ext4_snapshot_set_tid(sb);

	err = 0;
out_unlockfs:
	unlock_super(sb);
	thaw_super(sb);

	if (err)
		goto out_err;

	snapshot_debug(1, "snapshot (%u) has been taken\n",
			inode->i_generation);

out_err:
	brelse(es_bh);
	brelse(sbh);
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++)
		brelse(bhs[i]);
	return err;
}

/*
 * ext4_snapshot_enable() enables snapshot mount
 * sets the in-use flag and the active snapshot
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_enable(struct inode *inode)
{
	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_enable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED)) {
		snapshot_debug(1, "enable of deleted snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to block device size to enable loop device mount
	 */
	SNAPSHOT_SET_ENABLED(inode);
	ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED);

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) enabled\n", inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_disable() disables snapshot mount
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_disable(struct inode *inode)
{
	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_disable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_OPEN)) {
		snapshot_debug(1, "disable of mounted snapshot (%u) "
			       "is not permitted\n",
			       inode->i_generation);
		return -EPERM;
	}

	/* reset i_size and invalidate page cache */
	SNAPSHOT_SET_DISABLED(inode);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED);

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
		       inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) disabled\n", inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_delete() marks snapshot for deletion
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_delete(struct inode *inode)
{
	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_delete() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED)) {
		snapshot_debug(1, "delete of enabled snapshot (%u) "
			       "is not permitted\n",
			       inode->i_generation);
		return -EPERM;
	}

	/* mark deleted for later cleanup to finish the job */
	ext4_set_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED);
	snapshot_debug(1, "snapshot (%u) marked for deletion\n",
		       inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_remove - removes a snapshot from the list
 * @inode: snapshot inode
 *
 * Removed the snapshot inode from in-memory and on-disk snapshots list of
 * and truncates the snapshot inode.
 * Called from ext4_snapshot_update/cleanup/merge() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int ext4_snapshot_remove(struct inode *inode)
{
	handle_t *handle;
	struct ext4_sb_info *sbi;
	int err = 0, ret;

	/* elevate ref count until final cleanup */
	if (!igrab(inode))
		return -EIO;

	if (ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE) ||
		ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED) ||
		ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_INUSE)) {
		snapshot_debug(1, "ext4_snapshot_remove() called with active/"
			       "enabled/in-use snapshot file (ino=%lu)\n",
			       inode->i_ino);
		err = -EINVAL;
		goto out_err;
	}

	/* start large truncate transaction that will be extended/restarted */
	handle = ext4_journal_start(inode, EXT4_MAX_TRANS_DATA);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_err;
	}
	sbi = EXT4_SB(inode->i_sb);


	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

	err = ext4_snapshot_list_del(handle, inode);
	if (err)
		goto out_handle;
	/* remove snapshot list reference - taken on snapshot_create() */
	iput(inode);
	/*
	 * At this point, this snapshot is empty and not on the snapshots list.
	 * As long as it was on the list it had to have the LIST flag to prevent
	 * truncate/unlink.  Now that it is removed from the list, the LIST flag
	 * and other snapshot status flags should be cleared.  It will still
	 * have the SNAPFILE and SNAPFILE_DELETED persistent flags to indicate
	 * this is a deleted snapshot that should not be recycled.
	 */
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_LIST);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_INUSE);

out_handle:
	ret = ext4_journal_stop(handle);
	if (!err)
		err = ret;
	if (err)
		goto out_err;

	snapshot_debug(1, "snapshot (%u) deleted\n", inode->i_generation);

	err = 0;
out_err:
	/* drop final ref count - taken on entry to this function */
	iput(inode);
	if (err) {
		snapshot_debug(1, "failed to delete snapshot (%u)\n",
				inode->i_generation);
	}
	return err;
}

/*
 * Snapshot constructor/destructor
 */
/*
 * ext4_snapshot_load - load the on-disk snapshot list to memory.
 * Start with last (or active) snapshot and continue to older snapshots.
 * If snapshot load fails before active snapshot, force read-only mount.
 * If snapshot load fails after active snapshot, allow read-write mount.
 * Called from ext4_fill_super() under sb_lock during mount time.
 *
 * Return values:
 * = 0 - on-disk snapshot list is empty or active snapshot loaded
 * < 0 - error loading active snapshot
 */
int ext4_snapshot_load(struct super_block *sb, struct ext4_super_block *es,
		int read_only)
{
	__u32 active_ino = le32_to_cpu(es->s_snapshot_inum);
	__u32 load_ino = le32_to_cpu(es->s_snapshot_list);
	int err = 0, num = 0, snapshot_id = 0;
	int has_active = 0;

	if (!list_empty(&EXT4_SB(sb)->s_snapshot_list)) {
		snapshot_debug(1, "warning: snapshots already loaded!\n");
		return -EINVAL;
	}

	if (!load_ino && active_ino) {
		/* snapshots list is empty and active snapshot exists */
		if (!read_only)
			/* reset list head to active snapshot */
			es->s_snapshot_list = es->s_snapshot_inum;
		/* try to load active snapshot */
		load_ino = le32_to_cpu(es->s_snapshot_inum);
	}

	while (load_ino) {
		struct inode *inode;

		inode = ext4_orphan_get(sb, load_ino);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
		} else if (!ext4_snapshot_file(inode)) {
			iput(inode);
			err = -EIO;
		}

		if (err && num == 0 && load_ino != active_ino) {
			/* failed to load last non-active snapshot */
			if (!read_only)
				/* reset list head to active snapshot */
				es->s_snapshot_list = es->s_snapshot_inum;
			snapshot_debug(1, "warning: failed to load "
					"last snapshot (%u) - trying to load "
					"active snapshot (%u).\n",
					load_ino, active_ino);
			/* try to load active snapshot */
			load_ino = active_ino;
			err = 0;
			continue;
		}

		if (err)
			break;

		snapshot_id = inode->i_generation;
		snapshot_debug(1, "snapshot (%d) loaded\n",
			       snapshot_id);
		num++;

		if (!has_active && load_ino == active_ino) {
			/* active snapshot was loaded */
			err = ext4_snapshot_set_active(sb, inode);
			if (err)
				break;
			has_active = 1;
		}

		list_add_tail(&EXT4_I(inode)->i_snaplist,
			      &EXT4_SB(sb)->s_snapshot_list);
		load_ino = NEXT_SNAPSHOT(inode);
		/* keep snapshot list reference */
	}

	if (err) {
		/* failed to load active snapshot */
		snapshot_debug(1, "warning: failed to load "
				"snapshot (ino=%u) - "
				"forcing read-only mount!\n",
				load_ino);
		/* force read-only mount */
		return read_only ? 0 : err;
	}

	if (num > 0) {
		err = ext4_snapshot_update(sb, 0, read_only);
		snapshot_debug(1, "%d snapshots loaded\n", num);
	}
	return err;
}

/*
 * ext4_snapshot_destroy() releases the in-memory snapshot list
 * Called from ext4_put_super() under sb_lock during umount time.
 * This function cannot fail.
 */
void ext4_snapshot_destroy(struct super_block *sb)
{
	struct list_head *l, *n;
	/* iterate safe because we are deleting from list and freeing the
	 * inodes */
	list_for_each_safe(l, n, &EXT4_SB(sb)->s_snapshot_list) {
		struct inode *inode = &list_entry(l, struct ext4_inode_info,
						  i_snaplist)->vfs_inode;
		list_del_init(&EXT4_I(inode)->i_snaplist);
		/* remove snapshot list reference */
		iput(inode);
	}
	/* deactivate in-memory active snapshot - cannot fail */
	(void) ext4_snapshot_set_active(sb, NULL);
}

/*
 * ext4_snapshot_update - iterate snapshot list and update snapshots status.
 * @sb: handle to file system super block.
 * @cleanup: if true, shrink/merge/cleanup all snapshots marked for deletion.
 * @read_only: if true, don't remove snapshot after failed take.
 *
 * Called from ext4_ioctl() under snapshot_mutex.
 * Called from snapshot_load() under sb_lock with @cleanup=0.
 * Returns 0 on success and <0 on error.
 */
int ext4_snapshot_update(struct super_block *sb, int cleanup, int read_only)
{
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	struct inode *used_by = NULL; /* last non-deleted snapshot found */
	int deleted;
	struct inode *inode;
	struct ext4_inode_info *ei;
	int found_active = 0;
	int found_enabled = 0;
	struct list_head *prev;
	int err = 0;

	BUG_ON(read_only && cleanup);
	if (active_snapshot) {
		/* ACTIVE implies LIST */
		ext4_set_inode_snapstate(active_snapshot,
					EXT4_SNAPSTATE_LIST);
		ext4_set_inode_snapstate(active_snapshot,
					EXT4_SNAPSTATE_ACTIVE);
	}

	/* iterate safe from oldest snapshot backwards */
	prev = EXT4_SB(sb)->s_snapshot_list.prev;
	if (list_empty(prev))
		return 0;

update_snapshot:
	ei = list_entry(prev, struct ext4_inode_info, i_snaplist);
	inode = &ei->vfs_inode;
	prev = ei->i_snaplist.prev;

	/* all snapshots on the list have the LIST flag */
	ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_LIST);
	/* set the 'No_Dump' flag on all snapshots */
	ext4_set_inode_flag(inode, EXT4_NODUMP_FL);

	/*
	 * snapshots later than active (failed take) should be removed.
	 * no active snapshot means failed first snapshot take.
	 */
	if (found_active || !active_snapshot) {
		if (!read_only)
			err = ext4_snapshot_remove(inode);
		goto prev_snapshot;
	}

	deleted = ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED);
	if (!deleted && read_only)
		/* auto enable snapshots on readonly mount */
		ext4_snapshot_enable(inode);

	/*
	 * after completion of a snapshot management operation,
	 * only the active snapshot can have the ACTIVE flag
	 */
	if (inode == active_snapshot) {
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE);
		found_active = 1;
		deleted = 0;
	} else
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE);

	if (found_enabled)
		/* snapshot is in use by an older enabled snapshot */
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_INUSE);
	else
		/* snapshot is not in use by older enabled snapshots */
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_INUSE);

	if (cleanup && deleted && !used_by)
		/* remove permanently unused deleted snapshot */
		err = ext4_snapshot_remove(inode);

	if (!deleted) {
		if (!found_active)
			/* newer snapshots are potentially used by
			 * this snapshot (when it is enabled) */
			used_by = inode;
		if (ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED))
			found_enabled = 1;
		else
			SNAPSHOT_SET_DISABLED(inode);
	} else
		SNAPSHOT_SET_DISABLED(inode);

prev_snapshot:
	if (err)
		return err;
	/* update prev snapshot */
	if (prev != &EXT4_SB(sb)->s_snapshot_list)
		goto update_snapshot;

	if (!active_snapshot || !cleanup || used_by)
		return 0;

	/* if all snapshots are deleted - deactivate active snapshot */
	deleted = ext4_test_inode_flag(active_snapshot,
				       EXT4_INODE_SNAPFILE_DELETED);
	if (deleted && igrab(active_snapshot)) {
		/* lock journal updates before deactivating snapshot */
		freeze_super(sb);
		lock_super(sb);
		/* deactivate in-memory active snapshot - cannot fail */
		(void) ext4_snapshot_set_active(sb, NULL);
		/* clear on-disk active snapshot */
		EXT4_SB(sb)->s_es->s_snapshot_inum = 0;
		unlock_super(sb);
		thaw_super(sb);
		/* remove unused deleted active snapshot */
		err = ext4_snapshot_remove(active_snapshot);
		/* drop the refcount to 0 */
		iput(active_snapshot);
	}
	return err;
}
