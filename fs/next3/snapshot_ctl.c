/*
 * linux/fs/next3/snapshot_ctl.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Next3 snapshots control functions.
 */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_RESERVE
#include <linux/statfs.h>
#endif
#include "snapshot.h"

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
/*
 * General snapshot locking semantics:
 *
 * The snapshot_mutex:
 * -------------------
 * The majority of the code in the snapshot_{ctl,debug}.c files is called from
 * very few entry points in the code:
 * 1. {init,exit}_next3_fs() - calls {init,exit}_next3_snapshot() under BGL.
 * 2. next3_{fill,put}_super() - calls next3_snapshot_{load,destroy}() under
 *    VFS sb_lock, while f/s is not accessible to users.
 * 3. next3_ioctl() - only place that takes snapshot_mutex (after i_mutex)
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
 * next3_snapshot_set_active - set the current active snapshot
 * First, if current active snapshot exists, it is deactivated.
 * Then, if @inode is not NULL, the active snapshot is set to @inode.
 *
 * Called from next3_snapshot_take() and next3_snapshot_update() under
 * journal_lock_updates() and snapshot_mutex.
 * Called from next3_snapshot_{load,destroy}() under sb_lock.
 *
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_set_active(struct super_block *sb,
		struct inode *inode)
{
	struct inode *old = NEXT3_SB(sb)->s_active_snapshot;

	if (old == inode)
		return 0;

	/* add new active snapshot reference */
	if (inode && !igrab(inode))
		return -EIO;

	/* point of no return - replace old with new snapshot */
	if (old) {
		NEXT3_I(old)->i_flags &= ~NEXT3_SNAPFILE_ACTIVE_FL;
		snapshot_debug(1, "snapshot (%u) deactivated\n",
			       old->i_generation);
		/* remove old active snapshot reference */
		iput(old);
	}
	if (inode) {
		NEXT3_I(inode)->i_flags |=
			NEXT3_SNAPFILE_ACTIVE_FL|NEXT3_SNAPFILE_LIST_FL;
		snapshot_debug(1, "snapshot (%u) activated\n",
			       inode->i_generation);
	}
	NEXT3_SB(sb)->s_active_snapshot = inode;

	return 0;
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BLOCK_BITMAP
/*
 * next3_snapshot_reset_bitmap_cache():
 *
 * Resets the COW/exclude bitmap cache for all block groups.
 *
 * Called from init_bitmap_cache() with @init=1 under sb_lock during mount time.
 * Called from snapshot_take() with @init=0 under journal_lock_updates().
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_reset_bitmap_cache(struct super_block *sb, int init)
{
	struct next3_group_info *gi = NEXT3_SB(sb)->s_group_info;
	int i;

	for (i = 0; i < NEXT3_SB(sb)->s_groups_count; i++, gi++) {
		gi->bg_cow_bitmap = 0;
		if (init)
			gi->bg_exclude_bitmap = 0;
		cond_resched();
	}
	return 0;
}
#else
#define next3_snapshot_reset_bitmap_cache(sb, init) 0
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
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
 * This action is applicable on an un-mounted next3 filesystem.  After mounting
 * the filesystem, the discarded snapshot files will not be loaded, they will
 * not have the snapshot list flag and therefore, may be unlinked.
 */
static int next3_snapshot_enable(struct inode *inode);
static int next3_snapshot_disable(struct inode *inode);
static int next3_snapshot_create(struct inode *inode);
static int next3_snapshot_delete(struct inode *inode);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
static int next3_snapshot_exclude(handle_t *handle, struct inode *inode);
#endif

/*
 * next3_snapshot_get_flags() check snapshot state
 * Called from next3_ioctl() under i_mutex
 */
void next3_snapshot_get_flags(struct next3_inode_info *ei, struct file *filp)
{
	int open_count = atomic_read(&filp->f_path.dentry->d_count);
	/*
	 * 1 count for ioctl (lsattr)
	 * greater count means the snapshot is open by user (mounted?)
	 */
	if ((ei->i_flags & NEXT3_SNAPFILE_LIST_FL) && open_count > 1)
		ei->i_flags |= NEXT3_SNAPFILE_OPEN_FL;
	else
		ei->i_flags &= ~NEXT3_SNAPFILE_OPEN_FL;
}

/*
 * next3_snapshot_set_flags() monitors snapshot state changes
 * Called from next3_ioctl() under i_mutex and snapshot_mutex
 */
int next3_snapshot_set_flags(handle_t *handle, struct inode *inode,
			     unsigned int flags)
{
	unsigned int oldflags = NEXT3_I(inode)->i_flags;
	int err = 0;

	if (S_ISDIR(inode->i_mode)) {
		/* only the snapfile flag may be set for directories */
		NEXT3_I(inode)->i_flags &= ~NEXT3_SNAPFILE_FL;
		NEXT3_I(inode)->i_flags |= flags & NEXT3_SNAPFILE_FL;
		goto non_snapshot;
	}

	if (!next3_snapshot_file(inode)) {
		if ((flags ^ oldflags) & NEXT3_FL_SNAPSHOT_MASK) {
			/* snapflags can only be changed for snapfiles */
			snapshot_debug(1, "changing snapflags for non snapfile"
					" (ino=%lu) is not allowed\n",
					inode->i_ino);
			return -EINVAL;
		}
		goto non_snapshot;
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_DUMP
#ifdef CONFIG_NEXT3_FS_DEBUG
	if ((oldflags ^ flags) & NEXT3_NODUMP_FL) {
		/* print snapshot inode map on chattr -d */
		next3_snapshot_dump(1, inode);
		/* restore the 'No_Dump' flag */
		flags |= NEXT3_NODUMP_FL;
	}
#endif
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
	if (!(flags & NEXT3_SNAPFILE_FL))
		/* test snapshot blocks are excluded on chattr -x */
		err = next3_snapshot_exclude(handle, inode);
	if (err)
		goto out;
#endif

	if ((flags ^ oldflags) & NEXT3_SNAPFILE_ENABLED_FL) {
		/* enabled/disabled the snapshot during transaction */
		if (flags & NEXT3_SNAPFILE_ENABLED_FL)
			err = next3_snapshot_enable(inode);
		else
			err = next3_snapshot_disable(inode);
	}
	if (err)
		goto out;

	if ((flags ^ oldflags) & NEXT3_SNAPFILE_LIST_FL) {
		/* add/delete to snapshots list during transaction */
		if (flags & NEXT3_SNAPFILE_LIST_FL)
			err = next3_snapshot_create(inode);
		else
			err = next3_snapshot_delete(inode);
	}
	if (err)
		goto out;

	/* set snapshot user flags */
	NEXT3_I(inode)->i_flags &= ~NEXT3_FL_SNAPSHOT_USER_MASK;
	NEXT3_I(inode)->i_flags |= flags & NEXT3_FL_SNAPSHOT_USER_MASK;
non_snapshot:
	/* set only non-snapshot flags here */
	flags &= ~NEXT3_FL_SNAPSHOT_MASK;
	flags |= (NEXT3_I(inode)->i_flags & NEXT3_FL_SNAPSHOT_MASK);
	NEXT3_I(inode)->i_flags = flags;

out:
	/*
	 * retake reserve inode write from next3_ioctl() and mark inode
	 * dirty
	 */
	if (!err)
		err = next3_mark_inode_dirty(handle, inode);
	return err;
}

/*
 * If we have fewer than nblocks credits,
 * extend transaction by a minimum of NEXT3_MAX_TRANS_DATA.
 * If that fails, restart the transaction &
 * regain write access for the inode block.
 */
static int __extend_or_restart_transaction(const char *where,
		handle_t *handle, struct inode *inode, int nblocks)
{
	int err;

	if (NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, nblocks))
		return 0;

	if (nblocks < NEXT3_MAX_TRANS_DATA)
		nblocks = NEXT3_MAX_TRANS_DATA;

	err = __next3_journal_extend(where,
			(next3_handle_t *)handle, nblocks);
	if (err < 0)
		return err;
	if (err) {
		if (inode) {
			/* lazy way to do mark_iloc_dirty() */
			err = next3_mark_inode_dirty(handle, inode);
			if (err)
				return err;
		}
		err = __next3_journal_restart(where,
				(next3_handle_t *)handle, nblocks);
		if (err)
			return err;
		if (inode)
			/* lazy way to do reserve_inode_write() */
			err = next3_mark_inode_dirty(handle, inode);
	}

	return err;
}

#define extend_or_restart_transaction(handle, nblocks)			\
	__extend_or_restart_transaction(__func__, (handle), NULL, (nblocks))
#define extend_or_restart_transaction_inode(handle, inode, nblocks)	\
	__extend_or_restart_transaction(__func__, (handle), (inode), (nblocks))

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_INIT
/*
 * helper function for snapshot_create().
 * places pre-allocated [d,t]ind blocks in position
 * after they have been allocated as direct blocks.
 */
static inline int next3_snapshot_shift_blocks(struct next3_inode_info *ei,
		int from, int to, int count)
{
	int i, err = -EIO;

	/* move from direct blocks range */
	BUG_ON(from < 0 || from + count > NEXT3_NDIR_BLOCKS);
	/* to indirect blocks range */
	BUG_ON(to < NEXT3_NDIR_BLOCKS || to + count > NEXT3_SNAPSHOT_N_BLOCKS);

	/*
	 * truncate_mutex is held whenever allocating or freeing inode
	 * blocks.
	 */
	mutex_lock(&ei->truncate_mutex);

	/*
	 * verify that 'from' blocks are allocated
	 * and that 'to' blocks are not allocated.
	 */
	for (i = 0; i < count; i++)
		if (!ei->i_data[from+i] ||
				ei->i_data[to+i])
			goto out;

	/*
	 * shift 'count' blocks from position 'from' to 'to'
	 */
	for (i = 0; i < count; i++) {
		ei->i_data[to+i] = ei->i_data[from+i];
		ei->i_data[from+i] = 0;
	}
	err = 0;
out:
	mutex_unlock(&ei->truncate_mutex);
	return err;
}
#endif

/*
 * next3_snapshot_create() initializes a snapshot file
 * and adds it to the list of snapshots
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_create(struct inode *inode)
{
	handle_t *handle;
	struct super_block *sb = inode->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct inode *active_snapshot = next3_snapshot_has_active(sb);
	struct next3_inode_info *ei = NEXT3_I(inode);
	int i, err, ret;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_INIT
	int count, nind;
	const long double_blocks = (1 << (2 * SNAPSHOT_ADDR_PER_BLOCK_BITS));
	struct buffer_head *bh = NULL;
	struct next3_group_desc *desc;
	unsigned long ino;
	struct next3_iloc iloc;
	next3_fsblk_t bmap_blk = 0, imap_blk = 0, inode_blk = 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	next3_fsblk_t prev_inode_blk = 0;
#endif
#endif
	next3_fsblk_t snapshot_blocks = le32_to_cpu(sbi->s_es->s_blocks_count);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *l, *list = &sbi->s_snapshot_list;

	if (!list_empty(list)) {
		struct inode *last_snapshot =
			&list_first_entry(list, struct next3_inode_info,
					  i_snaplist)->vfs_inode;
		if (active_snapshot != last_snapshot) {
			snapshot_debug(1, "failed to add snapshot because last"
				       " snapshot (%u) is not active\n",
				       last_snapshot->i_generation);
			return -EINVAL;
		}
	}
#else
	if (active_snapshot) {
		snapshot_debug(1, "failed to add snapshot because active "
			       "snapshot (%u) has to be deleted first\n",
			       active_snapshot->i_generation);
		return -EINVAL;
	}
#endif

	/* prevent take of unlinked snapshot file */
	if (!inode->i_nlink) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it has 0 nlink count\n",
				inode->i_ino);
		return -EINVAL;
	}

	/* prevent recycling of old snapshot files */
	if ((ei->i_flags & NEXT3_FL_SNAPSHOT_MASK) != NEXT3_SNAPFILE_FL) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it has snapshot flags (0x%x)\n",
				inode->i_ino,
				inode->i_flags & NEXT3_FL_SNAPSHOT_MASK);
		return -EINVAL;
	}

	/* verify that no inode blocks are allocated */
	for (i = 0; i < NEXT3_N_BLOCKS; i++) {
		if (ei->i_data[i])
			break;
	}
	/* Don't need i_size_read because we hold i_mutex */
	if (i != NEXT3_N_BLOCKS ||
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
	 * next3_ioctl() We will extend or restart this transaction as we go
	 * along.  journal_start(n > 1) would not have increase the buffer
	 * credits.
	 */
	handle = next3_journal_start(inode, 1);

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

	if (!NEXT3_HAS_RO_COMPAT_FEATURE(sb,
		NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT))
		/* set the 'has_snapshot' feature */
		NEXT3_SET_RO_COMPAT_FEATURE(sb,
			NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
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
	err = next3_inode_list_add(handle, inode, &NEXT_SNAPSHOT(inode),
			&sbi->s_es->s_snapshot_list,
			list, "snapshot");
	/* add snapshot list reference */
	if (err) {
		snapshot_debug(1, "failed to add snapshot (%u) to list\n",
			       inode->i_generation);
		iput(inode);
		goto out_handle;
	}
	l = list->next;
#else
	lock_super(sb);
	err = next3_journal_get_write_access(handle, sbi->s_sbh);
	sbi->s_es->s_snapshot_list = cpu_to_le32(inode->i_ino);
	if (!err)
		err = next3_journal_dirty_metadata(handle, sbi->s_sbh);
	unlock_super(sb);
	if (err)
		goto out_handle;
#endif

	err = next3_mark_inode_dirty(handle, inode);
	if (err)
		goto out_handle;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_INIT
	/* small filesystems can be mapped with just 1 double indirect block */
	nind = 1;
	if (snapshot_blocks > double_blocks)
		/* add up to 4 triple indirect blocks to map 2^32 blocks */
		nind += ((snapshot_blocks - double_blocks) >>
			(3 * SNAPSHOT_ADDR_PER_BLOCK_BITS)) + 1;
	if (nind > NEXT3_SNAPSHOT_NTIND_BLOCKS + 1) {
		snapshot_debug(1, "need too many [d,t]ind blocks (%d) "
				"for snapshot (%u)\n",
				nind, inode->i_generation);
		err = -EFBIG;
		goto out_handle;
	}

	err = extend_or_restart_transaction_inode(handle, inode,
			nind * NEXT3_DATA_TRANS_BLOCKS(sb));
	if (err)
		goto out_handle;

	/* pre-allocate and zero out [d,t]ind blocks */
	for (i = 0; i < nind; i++) {
		brelse(bh);
		bh = next3_getblk(handle, inode, i, SNAPMAP_WRITE, &err);
		if (!bh || err)
			break;
		/* zero out indirect block and journal as dirty metadata */
		err = next3_journal_get_write_access(handle, bh);
		if (err)
			break;
		lock_buffer(bh);
		memset(bh->b_data, 0, bh->b_size);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		err = next3_journal_dirty_metadata(handle, bh);
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
	err = next3_snapshot_shift_blocks(ei, 0, NEXT3_DIND_BLOCK, nind);
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
				NEXT3_DATA_TRANS_BLOCKS(sb));
		if (err)
			goto out_handle;
		err = next3_snapshot_map_blocks(handle, inode, i, count - i,
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

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	/* start with root inode and continue with snapshot list */
	ino = NEXT3_ROOT_INO;
alloc_inode_blocks:
#else
	ino = inode->i_ino;
#endif
	/*
	 * pre-allocate the following blocks in the new snapshot:
	 * - block and inode bitmap blocks of ino's block group
	 * - inode table block that contains ino
	 */
	err = extend_or_restart_transaction_inode(handle, inode,
			3 * NEXT3_DATA_TRANS_BLOCKS(sb));
	if (err)
		goto out_handle;

	iloc.block_group = 0;
	inode_blk = next3_get_inode_block(sb, ino, &iloc);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	if (!inode_blk || inode_blk == prev_inode_blk)
		goto next_snapshot;

	/* not same inode and bitmap blocks as prev snapshot */
	prev_inode_blk = inode_blk;
#endif
	bmap_blk = 0;
	imap_blk = 0;
	desc = next3_get_group_desc(sb, iloc.block_group, NULL);
	if (!desc)
		goto next_snapshot;

	bmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	imap_blk = le32_to_cpu(desc->bg_inode_bitmap);
	if (!bmap_blk || !imap_blk)
		goto next_snapshot;

	count = 1;
	if (imap_blk == bmap_blk + 1)
		count++;
	if (inode_blk == imap_blk + 1)
		count++;
	/* try to allocate all blocks at once */
	err = next3_snapshot_map_blocks(handle, inode,
			bmap_blk, count,
			NULL, SNAPMAP_WRITE);
	count = err;
	/* allocate remaining blocks one by one */
	if (err > 0 && count < 2)
		err = next3_snapshot_map_blocks(handle, inode,
				imap_blk, 1,
				NULL,
				SNAPMAP_WRITE);
	if (err > 0 && count < 3)
		err = next3_snapshot_map_blocks(handle, inode,
				inode_blk, 1,
				NULL,
				SNAPMAP_WRITE);
next_snapshot:
	if (!bmap_blk || !imap_blk || !inode_blk || err < 0) {
#ifdef CONFIG_NEXT3_FS_DEBUG
		next3_fsblk_t blk0 = iloc.block_group *
			NEXT3_BLOCKS_PER_GROUP(sb);
		snapshot_debug(1, "failed to allocate block/inode bitmap "
				"or inode table block of inode (%lu) "
				"(%lu,%lu,%lu/%lu) for snapshot (%u)\n",
				ino, bmap_blk - blk0,
				imap_blk - blk0, inode_blk - blk0,
				iloc.block_group, inode->i_generation);
#endif
		if (!err)
			err = -EIO;
		goto out_handle;
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (l != list) {
		ino = list_entry(l, struct next3_inode_info,
				i_snaplist)->vfs_inode.i_ino;
		l = l->next;
		goto alloc_inode_blocks;
	}
#else
	if (ino == NEXT3_ROOT_INO) {
		ino = inode->i_ino;
		goto alloc_inode_blocks;
	}
#endif
#endif
#endif

	snapshot_debug(1, "snapshot (%u) created\n", inode->i_generation);
	err = 0;
out_handle:
	ret = next3_journal_stop(handle);
	if (!err)
		err = ret;
	return err;
}

/*
 * If we call next3_getblk() with NULL handle we will get read through access
 * to snapshot inode.  We don't want read through access in snapshot_take(),
 * so we call next3_getblk() with this dummy handle and since we are not
 * allocating snapshot block here the handle will not be used anyway.
 */
static handle_t dummy_handle;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_INIT
/*
 * next3_snapshot_copy_block() - copy block to new snapshot
 * @snapshot:	new snapshot to copy block to
 * @bh:		source buffer to be copied
 * @mask:	if not NULL, mask buffer data before copying to snapshot
 * 		(used to mask block bitmap with exclude bitmap)
 * @name:	name of copied block to print
 * @idx:	index of copied block to print
 *
 * Called from next3_snapshot_take() under journal_lock_updates()
 * Returns snapshot buffer on success, NULL on error
 */
static struct buffer_head *next3_snapshot_copy_block(struct inode *snapshot,
		struct buffer_head *bh, const char *mask,
		const char *name, unsigned long idx)
{
	struct buffer_head *sbh = NULL;
	int err;

	if (!bh)
		return NULL;

	sbh = next3_getblk(&dummy_handle, snapshot,
			SNAPSHOT_IBLOCK(bh->b_blocknr),
			SNAPMAP_READ, &err);

	if (err || !sbh || sbh->b_blocknr == bh->b_blocknr) {
		snapshot_debug(1, "failed to copy %s (%lu) "
				"block [%lu/%lu] to snapshot (%u)\n",
				name, idx,
				SNAPSHOT_BLOCK_TUPLE(bh->b_blocknr),
				snapshot->i_generation);
		brelse(sbh);
		return NULL;
	}

	next3_snapshot_copy_buffer(sbh, bh, mask);

	snapshot_debug(4, "copied %s (%lu) block [%lu/%lu] "
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
#endif

/*
 * next3_snapshot_take() makes a new snapshot file
 * into the active snapshot
 *
 * this function calls journal_lock_updates()
 * and should not be called during a journal transaction
 * Called from next3_ioctl() under i_mutex and snapshot_mutex
 */
int next3_snapshot_take(struct inode *inode)
{
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *list = &NEXT3_SB(inode->i_sb)->s_snapshot_list;
	struct list_head *l = list->next;
#endif
	struct super_block *sb = inode->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_super_block *es = NULL;
	struct buffer_head *es_bh = NULL;
	struct buffer_head *sbh = NULL;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	struct buffer_head *exclude_bitmap_bh = NULL;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_INIT
	struct buffer_head *bhs[COPY_INODE_BLOCKS_NUM] = { NULL };
	const char *mask = NULL;
	struct inode *curr_inode;
	struct next3_iloc iloc;
	struct next3_group_desc *desc;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	next3_fsblk_t prev_inode_blk = 0;
	struct next3_inode *raw_inode;
	blkcnt_t excluded_blocks = 0;
	int fixing = 0;
#endif
	int i;
#endif
	int err = -EIO;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_RESERVE
	u64 snapshot_r_blocks;
	struct kstatfs statfs;
#endif

	if (!sbi->s_sbh)
		goto out_err;
	else if (sbi->s_sbh->b_blocknr != 0) {
		snapshot_debug(1, "warning: unexpected super block at block "
			"(%lld:%d)!\n", (long long)sbi->s_sbh->b_blocknr,
			(int)((char *)sbi->s_es - (char *)sbi->s_sbh->b_data));
	} else if (sbi->s_es->s_magic != cpu_to_le16(NEXT3_SUPER_MAGIC)) {
		snapshot_debug(1, "warning: super block of snapshot (%u) is "
			       "broken!\n", inode->i_generation);
	} else
		es_bh = next3_getblk(&dummy_handle, inode, SNAPSHOT_IBLOCK(0),
				   SNAPMAP_READ, &err);

	if (!es_bh || es_bh->b_blocknr == 0) {
		snapshot_debug(1, "warning: super block of snapshot (%u) not "
			       "allocated\n", inode->i_generation);
		goto out_err;
	} else {
		snapshot_debug(4, "super block of snapshot (%u) mapped to "
			       "block (%lld)\n", inode->i_generation,
			       (long long)es_bh->b_blocknr);
		es = (struct next3_super_block *)(es_bh->b_data +
						  ((char *)sbi->s_es -
						   sbi->s_sbh->b_data));
	}

	err = -EIO;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_RESERVE
	/* update fs statistics to calculate snapshot reserved space */
	if (next3_statfs_sb(sb, &statfs)) {
		snapshot_debug(1, "failed to statfs before snapshot (%u) "
			       "take\n", inode->i_generation);
		goto out_err;
	}
	/*
	 * Calculate maximum disk space for snapshot file metadata based on:
	 * 1 indirect block per 1K fs blocks (to map moved data blocks)
	 * +1 data block per 1K fs blocks (to copy indirect blocks)
	 * +1 data block per fs meta block (to copy meta blocks)
	 * +1 data block per directory (to copy small directory index blocks)
	 * +1 data block per 64 inodes (to copy large directory index blocks)
	 * XXX: reserved space may be too small in data jounaling mode,
	 *      which is currently not supported.
	 */
	snapshot_r_blocks = 2 * (statfs.f_blocks >>
				 SNAPSHOT_ADDR_PER_BLOCK_BITS) +
		statfs.f_spare[0] + statfs.f_spare[1] +
		(statfs.f_files - statfs.f_ffree) / 64;

	/* verify enough free space before taking the snapshot */
	if (statfs.f_bfree < snapshot_r_blocks) {
		err = -ENOSPC;
		goto out_err;
	}
#endif

	/*
	 * flush journal to disk and clear the RECOVER flag
	 * before taking the snapshot
	 */
	sb->s_op->freeze_fs(sb);
	lock_super(sb);

#ifdef CONFIG_NEXT3_FS_DEBUG
	if (snapshot_enable_test[SNAPTEST_TAKE]) {
		snapshot_debug(1, "taking snapshot (%u) ...\n",
				inode->i_generation);
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_TAKE);
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_INIT
	/*
	 * copy group descriptors to snapshot
	 */
	for (i = 0; i < sbi->s_gdb_count; i++) {
		brelse(sbh);
		sbh = next3_snapshot_copy_block(inode,
				sbi->s_group_desc[i], NULL,
				"GDT", i);
		if (!sbh)
			goto out_unlockfs;
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	/* start with root inode and continue with snapshot list */
	curr_inode = sb->s_root->d_inode;
copy_inode_blocks:
#else
	curr_inode = inode;
#endif
	/*
	 * copy the following blocks to the new snapshot:
	 * - block and inode bitmap blocks of curr_inode block group
	 * - inode table block that contains curr_inode
	 */
	iloc.block_group = 0;
	err = next3_get_inode_loc(curr_inode, &iloc);
	desc = next3_get_group_desc(sb, iloc.block_group, NULL);
	if (err || !desc) {
		snapshot_debug(1, "failed to read inode and bitmap blocks "
			       "of inode (%lu)\n", curr_inode->i_ino);
		err = err ? : -EIO;
		goto out_unlockfs;
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	if (fixing)
		goto fix_inode_copy;
	if (iloc.bh->b_blocknr == prev_inode_blk)
		goto next_inode;
	prev_inode_blk = iloc.bh->b_blocknr;
#endif
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++)
		brelse(bhs[i]);
	bhs[COPY_BLOCK_BITMAP] = sb_bread(sb,
			le32_to_cpu(desc->bg_block_bitmap));
	bhs[COPY_INODE_BITMAP] = sb_bread(sb,
			le32_to_cpu(desc->bg_inode_bitmap));
	bhs[COPY_INODE_TABLE] = iloc.bh;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
	exclude_bitmap_bh = read_exclude_bitmap(sb, iloc.block_group);
	if (exclude_bitmap_bh)
		/* mask block bitmap with exclude bitmap */
		mask = exclude_bitmap_bh->b_data;
#endif
	err = -EIO;
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++) {
		brelse(sbh);
		sbh = next3_snapshot_copy_block(inode, bhs[i], mask,
				copy_inode_block_name[i], curr_inode->i_ino);
		if (!sbh)
			goto out_unlockfs;
		mask = NULL;
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	/* this is the copy pass */
	goto next_inode;
fix_inode_copy:
	/* this is the fixing pass */
	/* get snapshot copy of raw inode */
	brelse(sbh);
	sbh = next3_getblk(&dummy_handle, inode,
			SNAPSHOT_IBLOCK(iloc.bh->b_blocknr),
			SNAPMAP_READ, &err);
	if (!sbh)
		goto out_unlockfs;
	iloc.bh = sbh;
	raw_inode = next3_raw_inode(&iloc);
	/*
	 * Snapshot inode blocks are excluded from COW bitmap,
	 * so they appear to be not allocated in the snapshot's
	 * block bitmap.  If we want the snapshot image to pass
	 * fsck with no errors, we need to detach those blocks
	 * from the copy of the snapshot inode, so we fix the
	 * snapshot inodes to appear as empty regular files.
	 */
	excluded_blocks += next3_inode_blocks(raw_inode,
			NEXT3_I(curr_inode)) >>
		(curr_inode->i_blkbits - 9);
	lock_buffer(sbh);
	raw_inode->i_size = 0;
	raw_inode->i_size_high = 0;
	raw_inode->i_blocks_lo = 0;
	raw_inode->i_blocks_high = 0;
	raw_inode->i_flags &= cpu_to_le32(~NEXT3_FL_SNAPSHOT_MASK);
	memset(raw_inode->i_block, 0, sizeof(raw_inode->i_block));
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);

next_inode:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (l == list && !fixing) {
		/* done with copy pass - start fixing pass */
		l = l->next;
		fixing = 1;
	}
	if (l != list) {
		curr_inode = &list_entry(l, struct next3_inode_info,
				       i_snaplist)->vfs_inode;
		l = l->next;
		goto copy_inode_blocks;
	}
#else
	if (curr_inode->i_ino == NEXT3_ROOT_INO) {
		curr_inode = inode;
		goto copy_inode_blocks;
	}
#endif
#endif

	/*
	 * copy super block to snapshot and fix it
	 */
	lock_buffer(es_bh);
	memcpy(es_bh->b_data, sbi->s_sbh->b_data, sb->s_blocksize);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_FIX
	/* remove the HAS_SNAPSHOT feature to disable next3 mount */
	es->s_feature_ro_compat &=
		~cpu_to_le32(NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
	/* set the IS_SNAPSHOT flag to signal fsck this is a snapshot */
	es->s_flags |= cpu_to_le32(NEXT3_FLAGS_IS_SNAPSHOT);
	/* reset snapshots list in snapshot's super block copy */
	es->s_snapshot_inum = 0;
	es->s_snapshot_list = 0;
	/* fix free blocks count after clearing old snapshot inode blocks */
	es->s_free_blocks_count = cpu_to_le32(
			le32_to_cpu(es->s_free_blocks_count) +
			excluded_blocks);
#endif
	set_buffer_uptodate(es_bh);
	unlock_buffer(es_bh);
	mark_buffer_dirty(es_bh);
	sync_dirty_buffer(es_bh);

#endif

	/* reset i_size and invalidate page cache */
	SNAPSHOT_SET_DISABLED(inode);
	/* reset COW bitmap cache */
	err = next3_snapshot_reset_bitmap_cache(sb, 0);
	if (err)
		goto out_unlockfs;
	/* set as in-memory active snapshot */
	err = next3_snapshot_set_active(sb, inode);
	if (err)
		goto out_unlockfs;

	/* set as on-disk active snapshot */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_RESERVE
	sbi->s_es->s_snapshot_r_blocks_count = cpu_to_le64(snapshot_r_blocks);
#endif
	sbi->s_es->s_snapshot_id =
		cpu_to_le32(le32_to_cpu(sbi->s_es->s_snapshot_id)+1);
	if (sbi->s_es->s_snapshot_id == 0)
		/* 0 is not a valid snapshot id */
		sbi->s_es->s_snapshot_id = cpu_to_le32(1);
	sbi->s_es->s_snapshot_inum = cpu_to_le32(inode->i_ino);

	err = 0;
out_unlockfs:
	unlock_super(sb);
	sb->s_op->unfreeze_fs(sb);

	if (err)
		goto out_err;

	snapshot_debug(1, "snapshot (%u) has been taken\n",
			inode->i_generation);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_DUMP
	next3_snapshot_dump(5, inode);
#endif

out_err:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
#endif
	brelse(es_bh);
	brelse(sbh);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_INIT
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++)
		brelse(bhs[i]);
#endif
	return err;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
/*
 * next3_snapshot_clean() "cleans" snapshot file blocks in 1 of 2 ways:
 * 1. from next3_snapshot_remove() with @cleanup=1 to free snapshot file
 *    blocks, before removing snapshot file from snapshots list.
 * 2. from next3_snapshot_exclude() with @cleanup=0 to mark snapshot file
 *    blocks in exclude bitmap.
 * Called under snapshot_mutex.
 *
 * Return values:
 * > 0 - no. of blocks in snapshot file (@cleanup=0)
 * = 0 - successful cleanup (@cleanup=1)
 * < 0 - error
 */
static int next3_snapshot_clean(handle_t *handle, struct inode *inode,
		int cleanup)
{
	struct next3_inode_info *ei = NEXT3_I(inode);
	int i, nblocks = 0;
	int *pblocks = cleanup ? NULL : &nblocks;

	if (!next3_snapshot_list(inode)) {
		snapshot_debug(1, "next3_snapshot_clean() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & NEXT3_SNAPFILE_ACTIVE_FL) {
		snapshot_debug(1, "clean of active snapshot (%u) "
			       "is not allowed.\n",
			       inode->i_generation);
		return -EPERM;
	}

	/*
	 * A very simplified version of next3_truncate() for snapshot files.
	 * A non-active snapshot file never allocates new blocks and only frees
	 * blocks under snapshot_mutex, so no need to take truncate_mutex here.
	 * No need to add inode to orphan list for post crash truncate, because
	 * snapshot is still on the snapshot list and marked for deletion.
	 * Free DIND branch last, to keep snapshot's super block around longer.
	 */
	for (i = NEXT3_SNAPSHOT_N_BLOCKS - 1; i >= NEXT3_DIND_BLOCK; i--) {
		int depth = (i == NEXT3_DIND_BLOCK ? 2 : 3);
		
		if (!ei->i_data[i])
			continue;
		next3_free_branches_cow(handle, inode, NULL,
				ei->i_data+i, ei->i_data+i+1, depth, pblocks);
		if (cleanup)
			ei->i_data[i] = 0;
	}
	return nblocks;
}

/*
 * next3_snapshot_exclude() marks snapshot file blocks in exclude bitmap.
 * Snapshot file blocks should already be excluded if everything works properly.
 * This function is used only to verify the correctness of exclude bitmap.
 * Called under i_mutex and snapshot_mutex.
 */
static int next3_snapshot_exclude(handle_t *handle, struct inode *inode)
{
	int err;

	/* extend small transaction started in next3_ioctl() */
	err = extend_or_restart_transaction(handle, NEXT3_MAX_TRANS_DATA);
	if (err)
		return err;

	err = next3_snapshot_clean(handle, inode, 0);
	if (err < 0)
		return err;

	snapshot_debug(1, "snapshot (%u) is clean (%d blocks)\n",
			inode->i_generation, err);
	return 0;
}
#endif

/*
 * next3_snapshot_enable() enables snapshot mount
 * sets the in-use flag and the active snapshot
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_enable(struct inode *inode)
{
	struct next3_inode_info *ei = NEXT3_I(inode);

	if (!next3_snapshot_list(inode)) {
		snapshot_debug(1, "next3_snapshot_enable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & NEXT3_SNAPFILE_DELETED_FL) {
		snapshot_debug(1, "enable of deleted snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to block device size to enable loop device mount
	 */
	SNAPSHOT_SET_ENABLED(inode);
	ei->i_flags |= NEXT3_SNAPFILE_ENABLED_FL;

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) enabled\n", inode->i_generation);
	return 0;
}

/*
 * next3_snapshot_disable() disables snapshot mount
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_disable(struct inode *inode)
{
	struct next3_inode_info *ei = NEXT3_I(inode);

	if (!next3_snapshot_list(inode)) {
		snapshot_debug(1, "next3_snapshot_disable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & NEXT3_SNAPFILE_OPEN_FL) {
		snapshot_debug(1, "disable of mounted snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/* reset i_size and invalidate page cache */
	SNAPSHOT_SET_DISABLED(inode);
	ei->i_flags &= ~NEXT3_SNAPFILE_ENABLED_FL;

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) disabled\n", inode->i_generation);
	return 0;
}

/*
 * next3_snapshot_delete() marks snapshot for deletion
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_delete(struct inode *inode)
{
	struct next3_inode_info *ei = NEXT3_I(inode);

	if (!next3_snapshot_list(inode)) {
		snapshot_debug(1, "next3_snapshot_delete() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & NEXT3_SNAPFILE_ENABLED_FL) {
		snapshot_debug(1, "delete of enabled snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/* mark deleted for later cleanup to finish the job */
	ei->i_flags |= NEXT3_SNAPFILE_DELETED_FL;
	snapshot_debug(1, "snapshot (%u) marked for deletion\n",
			inode->i_generation);
	return 0;
}

/*
 * next3_snapshot_remove - removes a snapshot from the list
 * @inode: snapshot inode
 *
 * Removed the snapshot inode from in-memory and on-disk snapshots list of
 * and truncates the snapshot inode.
 * Called from next3_snapshot_update/cleanup/merge() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_remove(struct inode *inode)
{
	handle_t *handle;
	struct next3_sb_info *sbi;
	struct next3_inode_info *ei = NEXT3_I(inode);
	int err = 0, ret;

	/* elevate ref count until final cleanup */
	if (!igrab(inode))
		return -EIO;

	if (ei->i_flags & (NEXT3_SNAPFILE_ENABLED_FL | NEXT3_SNAPFILE_INUSE_FL
			   | NEXT3_SNAPFILE_ACTIVE_FL)) {
		snapshot_debug(4, "deferred delete of %s snapshot (%u)\n",
				(ei->i_flags & NEXT3_SNAPFILE_ACTIVE_FL) ?
				"active" :
				((ei->i_flags & NEXT3_SNAPFILE_ENABLED_FL) ?
				"enabled" : "referenced"),
			       inode->i_generation);
		goto out_err;
	}

	/* start large truncate transaction that will be extended/restarted */
	handle = next3_journal_start(inode, NEXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_err;
	}
	sbi = NEXT3_SB(inode->i_sb);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
	/* free snapshot inode blocks */
	err = next3_snapshot_clean(handle, inode, 1);
	if (err)
		goto out_handle;

	/* reset i_size and i_disksize and invalidate page cache */
	SNAPSHOT_SET_REMOVED(inode);

	err = next3_mark_inode_dirty(handle, inode);
	if (err)
		goto out_handle;
#endif

	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	err = next3_inode_list_del(handle, inode, &NEXT_SNAPSHOT(inode),
			&sbi->s_es->s_snapshot_list,
			&NEXT3_SB(inode->i_sb)->s_snapshot_list,
			"snapshot");
	if (err)
		goto out_handle;
	/* remove snapshot list reference - taken on snapshot_create() */
	iput(inode);
#else
	lock_super(inode->i_sb);
	err = next3_journal_get_write_access(handle, sbi->s_sbh);
	sbi->s_es->s_snapshot_list = 0;
	if (!err)
		err = next3_journal_dirty_metadata(handle, sbi->s_sbh);
	unlock_super(inode->i_sb);
	if (err)
		goto out_handle;
#endif
	/*
	 * At this point, this snapshot is empty and not on the snapshots list.
	 * As long as it was on the list it had to have the LIST flag to prevent
	 * truncate/unlink.  Now that it is removed from the list, the LIST flag
	 * and other snapshot status flags should be cleared.  It will still
	 * have the SNAPFILE and DELETED flag to indicate this is a deleted
	 * snapshot that should not be recycled.  There is no need to mark the
	 * inode dirty, because the 'dynamic' status flags are not persistent.
	 */
	ei->i_flags &= ~NEXT3_FL_SNAPSHOT_DYN_MASK;

out_handle:
	ret = next3_journal_stop(handle);
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

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP_SHRINK
/*
 * next3_snapshot_shrink_range - free unused blocks from deleted snapshots
 * @handle: JBD handle for this transaction
 * @start:	latest non-deleted snapshot before deleted snapshots group
 * @end:	first non-deleted snapshot after deleted snapshots group
 * @iblock:	inode offset to first data block to shrink
 * @maxblocks:	inode range of data blocks to shrink
 * @cow_bh:	buffer head to map the COW bitmap block of snapshot @start
 *		if NULL, don't look for COW bitmap block
 *
 * Shrinks @maxblocks blocks starting at inode offset @iblock in a group of
 * subsequent deleted snapshots starting after @start and ending before @end.
 * Shrinking is done by finding a range of mapped blocks in @start snapshot
 * or in one of the deleted snapshots, where no other blocks are mapped in the
 * same range in @start snapshot or in snapshots between them.
 * The blocks in the found range may be 'in-use' by @start snapshot, so only
 * blocks which are not set in the COW bitmap are freed.
 * All mapped blocks of other deleted snapshots in the same range are freed.
 *
 * Called from next3_snapshot_shrink() under snapshot_mutex.
 * Returns the shrunk blocks range and <0 on error.
 */
static int next3_snapshot_shrink_range(handle_t *handle,
		struct inode *start, struct inode *end,
		sector_t iblock, unsigned long maxblocks,
		struct buffer_head *cow_bh)
{
	struct next3_sb_info *sbi = NEXT3_SB(start->i_sb);
	struct list_head *l;
	struct inode *inode = start;
	/* start with @maxblocks range and narrow it down */
	int err, count = maxblocks;
	/* @start snapshot blocks should not be freed only counted */
	int mapped, shrink = 0;

	/* iterate on (@start <= snapshot < @end) */
	list_for_each_prev(l, &NEXT3_I(start)->i_snaplist) {
		err = next3_snapshot_shrink_blocks(handle, inode,
				iblock, count, cow_bh, shrink, &mapped);
		if (err < 0)
			return err;

		/* 0 < new range <= old range */
		BUG_ON(!err || err > count);
		count = err;
		cond_resched();

		/*
		 * shrink mode state transitions:
		 * 1. on @start, shrink is set to 0 ('don't free' mode).
		 * 2. after @start, shrink is incremented until mapped blocks
		 *    are found in the shrunk range ('free unused' mode).
		 * 3. after mapped block were found, or if cow_bh is NULL,
		 *    shrink is set to -1 and decremented until the end of
		 *    the deleted snapshots group ('free all' mode).
		 */
		if (shrink < 0)
			/* stay in 'free all' mode */
			shrink--;
		else if (!cow_bh)
			/* no COW bitmap - enter 'free all' mode */
			shrink = -1;
		else if (mapped)
			/* found mapped blocks - enter 'free all' mode */
			shrink = -1;
		else
			/* enter/stay in 'free unused' mode */
			shrink++;

		if (l == &sbi->s_snapshot_list)
			/* didn't reach @end */
			return -EINVAL;
		inode = &list_entry(l, struct next3_inode_info,
						  i_snaplist)->vfs_inode;
		if (inode == end)
			break;
		/* indicate shrink progress via i_size */
		SNAPSHOT_SET_PROGRESS(inode, SNAPSHOT_BLOCK(iblock));
	}
	return count;
}

/*
 * next3_snapshot_shrink - free unused blocks from deleted snapshot files
 * @handle: JBD handle for this transaction
 * @start:	latest non-deleted snapshot before deleted snapshots group
 * @end:	first non-deleted snapshot after deleted snapshots group
 * @need_shrink: no. of deleted snapshots in the group
 *
 * Frees all blocks in subsequent deleted snapshots starting after @start and
 * ending before @end, except for blocks which are 'in-use' by @start snapshot.
 * (blocks 'in-use' are set in snapshot COW bitmap and not copied to snapshot).
 * Called from next3_snapshot_update() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_shrink(struct inode *start, struct inode *end,
				 int need_shrink)
{
	struct list_head *l;
	handle_t *handle;
	struct buffer_head cow_bitmap, *cow_bh = NULL;
	next3_fsblk_t block = 1; /* skip super block */
	struct next3_sb_info *sbi = NEXT3_SB(start->i_sb);
	/* blocks beyond the size of @start are not in-use by @start */
	next3_fsblk_t snapshot_blocks = SNAPSHOT_BLOCKS(start);
	unsigned long count = le32_to_cpu(sbi->s_es->s_blocks_count) - block;
	long block_group = -1;
	next3_fsblk_t bg_boundary = 0;
	int err, ret;

	snapshot_debug(3, "snapshot (%u-%u) shrink: "
			"count = 0x%lx, need_shrink = %d\n",
			start->i_generation, end->i_generation,
			count, need_shrink);

	/* start large truncate transaction that will be extended/restarted */
	handle = next3_journal_start(start, NEXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	while (count > 0) {
		while (block >= bg_boundary) {
			/* reset COW bitmap cache */
			cow_bitmap.b_state = 0;
			cow_bitmap.b_blocknr = 0;
			cow_bh = &cow_bitmap;
			bg_boundary += SNAPSHOT_BLOCKS_PER_GROUP;
			block_group++;
			if (block >= snapshot_blocks)
				/*
				 * Past last snapshot block group - pass NULL
				 * cow_bh to next3_snapshot_shrink_range().
				 * This will cause snapshots after resize to
				 * shrink to the size of @start snapshot.
				 */
				cow_bh = NULL;
			cond_resched();
		}

		err = extend_or_restart_transaction(handle,
						    NEXT3_MAX_TRANS_DATA);
		if (err)
			goto out_err;

		err = next3_snapshot_shrink_range(handle, start, end,
					      SNAPSHOT_IBLOCK(block), count,
					      cow_bh);

		snapshot_debug(3, "snapshot (%u-%u) shrink: "
				"block = 0x%lx, count = 0x%lx, err = 0x%x\n",
				start->i_generation, end->i_generation,
				block, count, err);

		if (buffer_mapped(&cow_bitmap) && buffer_new(&cow_bitmap)) {
			snapshot_debug(2, "snapshot (%u-%u) shrink: "
				"block group = %ld/%lu, "
				"COW bitmap = [%lu/%lu]\n",
				start->i_generation, end->i_generation,
				block_group, sbi->s_groups_count,
				SNAPSHOT_BLOCK_TUPLE(cow_bitmap.b_blocknr));
			clear_buffer_new(&cow_bitmap);
		}

		if (err <= 0)
			goto out_err;

		block += err;
		count -= err;
	}

	/* marks need_shrink snapshots shrunk */
	err = extend_or_restart_transaction(handle, need_shrink);
	if (err)
		goto out_err;

	/* iterate on (@start < snapshot < @end) */
	list_for_each_prev(l, &NEXT3_I(start)->i_snaplist) {
		struct next3_inode_info *ei;
		struct next3_iloc iloc;
		if (l == &sbi->s_snapshot_list)
			break;
		ei = list_entry(l, struct next3_inode_info, i_snaplist);
		if (&ei->vfs_inode == end)
			break;
		/* reset i_size that was used as progress indicator */
		SNAPSHOT_SET_DISABLED(&ei->vfs_inode);
		if (ei->i_flags & NEXT3_SNAPFILE_DELETED_FL &&
			!(ei->i_flags &
			(NEXT3_SNAPFILE_SHRUNK_FL|NEXT3_SNAPFILE_ACTIVE_FL))) {
			/* mark snapshot shrunk */
			err = next3_reserve_inode_write(handle, &ei->vfs_inode,
							&iloc);
			ei->i_flags |= NEXT3_SNAPFILE_SHRUNK_FL;
			if (!err)
				next3_mark_iloc_dirty(handle, &ei->vfs_inode,
						      &iloc);
			if (--need_shrink <= 0)
				break;
		}
	}

	err = 0;
out_err:
	ret = next3_journal_stop(handle);
	if (!err)
		err = ret;
	if (need_shrink)
		snapshot_debug(1, "snapshot (%u-%u) shrink: "
			       "need_shrink=%d(>0!), err=%d\n",
			       start->i_generation, end->i_generation,
			       need_shrink, err);
	return err;
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP_MERGE
/*
 * next3_snapshot_merge - merge deleted snapshots
 * @handle: JBD handle for this transaction
 * @start:	latest non-deleted snapshot before deleted snapshots group
 * @end:	first non-deleted snapshot after deleted snapshots group
 * @need_merge: no. of deleted snapshots in the group
 *
 * Move all blocks from deleted snapshots group starting after @start and
 * ending before @end to @start snapshot.  All moved blocks are 'in-use' by
 * @start snapshot, because these deleted snapshots have already been shrunk
 * (blocks 'in-use' are set in snapshot COW bitmap and not copied to snapshot).
 * Called from next3_snapshot_update() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_merge(struct inode *start, struct inode *end,
				int need_merge)
{
	struct list_head *l, *n;
	handle_t *handle = NULL;
	struct next3_sb_info *sbi = NEXT3_SB(start->i_sb);
	int err, ret;

	snapshot_debug(3, "snapshot (%u-%u) merge: need_merge=%d\n",
			start->i_generation, end->i_generation, need_merge);

	/* iterate safe on (@start < snapshot < @end) */
	list_for_each_prev_safe(l, n, &NEXT3_I(start)->i_snaplist) {
		struct next3_inode_info *ei = list_entry(l,
						 struct next3_inode_info,
						 i_snaplist);
		struct inode *inode = &ei->vfs_inode;
		next3_fsblk_t block = 1; /* skip super block */
		/* blocks beyond the size of @start are not in-use by @start */
		int count = SNAPSHOT_BLOCKS(start) - block;

		if (n == &sbi->s_snapshot_list || inode == end ||
			!(ei->i_flags & NEXT3_SNAPFILE_SHRUNK_FL))
			break;

		/* start large transaction that will be extended/restarted */
		handle = next3_journal_start(inode, NEXT3_MAX_TRANS_DATA);
		if (IS_ERR(handle))
			return PTR_ERR(handle);

		while (count > 0) {
			/* we modify one indirect block and the inode itself
			 * for both the source and destination inodes */
			err = extend_or_restart_transaction(handle, 4);
			if (err)
				goto out_err;

			err = next3_snapshot_merge_blocks(handle, inode, start,
						 SNAPSHOT_IBLOCK(block), count);

			snapshot_debug(3, "snapshot (%u) -> snapshot (%u) "
				       "merge: block = 0x%lx, count = 0x%x, "
				       "err = 0x%x\n", inode->i_generation,
				       start->i_generation, block, count, err);

			if (err <= 0)
				goto out_err;

			block += err;
			count -= err;
			/* indicate merge progress via i_size */
			SNAPSHOT_SET_PROGRESS(inode, block);
			cond_resched();
		}

		/* reset i_size that was used as progress indicator */
		SNAPSHOT_SET_DISABLED(inode);

		err = next3_journal_stop(handle);
		handle = NULL;
		if (err)
			goto out_err;

		/* we finished moving all blocks of interest from 'inode'
		 * into 'start' so it is now safe to remove 'inode' from the
		 * snapshots list forever */
		err = next3_snapshot_remove(inode);
		if (err)
			goto out_err;

		if (--need_merge <= 0)
			break;
	}

	err = 0;
out_err:
	if (handle) {
		ret = next3_journal_stop(handle);
		if (!err)
			err = ret;
	}
	if (need_merge)
		snapshot_debug(1, "snapshot (%u-%u) merge: need_merge=%d(>0!), "
			       "err=%d\n", start->i_generation,
			       end->i_generation, need_merge, err);
	return err;
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
/*
 * next3_snapshot_cleanup - shrink/merge/remove snapshot marked for deletion
 * @inode - inode in question
 * @used_by - latest non-deleted snapshot
 * @deleted - true if snapshot is marked for deletion and not active
 * @need_shrink - counter of deleted snapshots to shrink
 * @need_merge - counter of deleted snapshots to merge
 *
 * Deleted snapshot with no older non-deleted snapshot - remove from list
 * Deleted snapshot with no older enabled snapshot - add to merge count
 * Deleted snapshot with older enabled snapshot - add to shrink count
 * Non-deleted snapshot - shrink and merge deleted snapshots group
 *
 * Called from next3_snapshot_update() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_cleanup(struct inode *inode, struct inode *used_by,
		int deleted, int *need_shrink, int *need_merge)
{
	int err = 0;

	if (deleted && !used_by)
		/* remove permanently unused deleted snapshot */
		return next3_snapshot_remove(inode);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP_SHRINK
	if (deleted) {
		/* deleted (non-active) snapshot file */
		if (!(NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_SHRUNK_FL))
			/* deleted snapshot needs shrinking */
			(*need_shrink)++;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP_MERGE
		if (!(NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_INUSE_FL))
			/* temporarily unused deleted
			 * snapshot needs merging */
			(*need_merge)++;
#endif
		return 0;
	}

	/* non-deleted (or active) snapshot file */
	if (*need_shrink) {
		/* pass 1: shrink all deleted snapshots
		 * between 'used_by' and 'inode' */
		err = next3_snapshot_shrink(used_by, inode, *need_shrink);
		if (err)
			return err;
		*need_shrink = 0;
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP_MERGE
	if (*need_merge) {
		/* pass 2: merge all shrunk snapshots
		 * between 'used_by' and 'inode' */
		err = next3_snapshot_merge(used_by, inode, *need_merge);
		if (err)
			return err;
		*need_merge = 0;
	}
#endif
#endif
	return 0;
}
#endif
#endif

/*
 * Snapshot constructor/destructor
 */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
/*
 * next3_exclude_inode_bread - read indirect block from exclude inode
 * @handle:	JBD handle (NULL is !@create)
 * @inode:	exclude inode
 * @grp:	block group
 * @create:	if true, try to allocate missing indirect block
 *
 * Helper function for next3_snapshot_init_bitmap_cache().
 * Called under sb_lock and before snapshots are loaded, so changes made to
 * exclude inode are not COWed.
 *
 * Returns indirect block buffer or NULL if not allocated.
 */
static struct buffer_head *next3_exclude_inode_bread(handle_t *handle,
		struct inode *inode, int grp, int create)
{
	int dind_offset = grp / SNAPSHOT_ADDR_PER_BLOCK;
	struct buffer_head *ind_bh;
	int err;

	/* exclude bitmap blocks addresses are exposed on the IND branch */
	ind_bh = next3_bread(NULL, inode, NEXT3_IND_BLOCK + dind_offset,
						 0, &err);
	if (ind_bh)
		return ind_bh;

	snapshot_debug(1, "failed to read exclude inode indirect[%d] block\n",
			dind_offset);
	if (!create)
		return NULL;

	err = extend_or_restart_transaction(handle, NEXT3_RESERVE_TRANS_BLOCKS);
	if (err)
		return NULL;
	ind_bh = next3_bread(handle, inode, NEXT3_IND_BLOCK + dind_offset,
			create, &err);
	if (!ind_bh) {
		snapshot_debug(1, "failed to allocate exclude "
				"inode indirect[%d] block\n",
				dind_offset);
		return NULL;
	}
	snapshot_debug(2, "allocated exclude bitmap "
			"indirect[%d] block (%lld)\n",
			dind_offset, (long long)ind_bh->b_blocknr);
	return ind_bh;
}

/*
 * next3_exclude_inode_getblk - read address of exclude bitmap block
 * @handle:	JBD handle (NULL is !@create)
 * @inode:	exclude inode
 * @grp:	block group
 * @create:	if true, try to allocate missing blocks
  *
 * Helper function for next3_snapshot_init_bitmap_cache().
 * Called under sb_lock and before snapshots are loaded, so changes made to
 * exclude inode are not COWed.
 *
 * Returns exclude bitmap block address (little endian) or 0 if not allocated.
 */
static __le32 next3_exclude_inode_getblk(handle_t *handle,
		struct inode *inode, int grp, int create)
{
	int ind_offset = grp % SNAPSHOT_ADDR_PER_BLOCK;
	struct buffer_head *bh, *ind_bh = NULL;
	__le32 exclude_bitmap = 0;
	int err = 0;

	/* read exclude inode indirect block */
	ind_bh = next3_exclude_inode_bread(handle, inode, grp, create);
	if (!ind_bh)
		return 0;

	if (grp >= NEXT3_SB(inode->i_sb)->s_groups_count)
		/* past last block group - just allocating indirect blocks */
		goto out;

	exclude_bitmap = ((__le32 *)ind_bh->b_data)[ind_offset];
	if (exclude_bitmap)
		goto out;
	if (!create)
		goto alloc_out;

	/* try to allocate missing exclude bitmap(+ind+dind) block */
	err = extend_or_restart_transaction(handle,
			NEXT3_RESERVE_TRANS_BLOCKS);
	if (err)
		goto alloc_out;

	/* exclude bitmap blocks are mapped on the DIND branch */
	bh = next3_getblk(handle, inode, SNAPSHOT_IBLOCK(grp), create, &err);
	if (!bh)
		goto alloc_out;
	brelse(bh);
	exclude_bitmap = ((__le32 *)ind_bh->b_data)[ind_offset];
alloc_out:
	if (exclude_bitmap)
		snapshot_debug(2, "allocated exclude bitmap #%d block "
				"(%u)\n", grp,
				le32_to_cpu(exclude_bitmap));
	else
		snapshot_debug(1, "failed to allocate exclude "
				"bitmap #%d block (err = %d)\n",
				grp, err);
out:
	brelse(ind_bh);
	return exclude_bitmap;
}

/*
 * next3_snapshot_init_bitmap_cache():
 *
 * Init the COW/exclude bitmap cache for all block groups.
 * COW bitmap cache is set to 0 (lazy init on first access to block group).
 * Read exclude bitmap blocks addresses from exclude inode and store them
 * in block group descriptor.  If @create is true, Try to allocate missing
 * exclude bitmap blocks.
 *
 * Called from snapshot_load() under sb_lock during mount time.
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_init_bitmap_cache(struct super_block *sb, int create)
{
	struct next3_group_info *gi = NEXT3_SB(sb)->s_group_info;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	handle_t *handle = NULL;
	struct inode *inode;
	__le32 exclude_bitmap = 0;
	int grp, max_groups = sbi->s_groups_count;
	int err = 0, ret;
	loff_t i_size;

	/* reset COW/exclude bitmap cache */
	err = next3_snapshot_reset_bitmap_cache(sb, 1);
	if (err)
		return err;

	if (!NEXT3_HAS_COMPAT_FEATURE(sb,
				      NEXT3_FEATURE_COMPAT_EXCLUDE_INODE)) {
		/* exclude inode is a recommended feature - don't force it */
		snapshot_debug(1, "warning: exclude_inode feature not set - "
			       "snapshot merge might not free all unused "
			       "blocks!\n");
		return 0;
	}

	inode = next3_iget(sb, NEXT3_EXCLUDE_INO);
	if (IS_ERR(inode)) {
		snapshot_debug(1, "warning: bad exclude inode - "
				"no exclude bitmap!\n");
		return PTR_ERR(inode);
	}

	if (create) {
		/* start large transaction that will be extended/restarted */
		handle = next3_journal_start(inode, NEXT3_MAX_TRANS_DATA);
		if (IS_ERR(handle))
			return PTR_ERR(handle);
		/* number of groups the filesystem can grow to */
		max_groups = sbi->s_gdb_count +
			le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks);
		max_groups *= NEXT3_DESC_PER_BLOCK(sb);
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE_OLD
	if (create && NEXT3_HAS_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_COMPAT_EXCLUDE_INODE_OLD)) {
		/* journal exclude inode migration done inside next3_iget */
		err = next3_journal_get_write_access(handle, sbi->s_sbh);
		NEXT3_CLEAR_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_COMPAT_EXCLUDE_INODE_OLD);
		if (!err)
			err = next3_journal_dirty_metadata(handle, sbi->s_sbh);
		if (!err)
			err = next3_mark_inode_dirty(handle, inode);
	}

#endif
	/*
	 * Init exclude bitmap blocks for all existing block groups and
	 * allocate indirect blocks for all reserved block groups.
	 */
	err = -EIO;
	for (grp = 0; grp < max_groups; grp++, gi++) {
		exclude_bitmap = next3_exclude_inode_getblk(handle, inode, grp,
				create);
		cond_resched();
		if (create && grp >= sbi->s_groups_count)
			/* only allocating indirect blocks with getblk above */
			continue;

		if (create && !exclude_bitmap)
			goto out;

		gi->bg_exclude_bitmap = le32_to_cpu(exclude_bitmap);
		snapshot_debug(2, "update exclude bitmap #%d cache "
			       "(block=%lu)\n", grp,
			       gi->bg_exclude_bitmap);
	}

	err = 0;
	if (!create)
		goto out;

	i_size = SNAPSHOT_IBLOCK(max_groups) << SNAPSHOT_BLOCK_SIZE_BITS;
	if (NEXT3_I(inode)->i_disksize >= i_size)
		goto out;
	i_size_write(inode, i_size);
	NEXT3_I(inode)->i_disksize = i_size;
	err = next3_mark_inode_dirty(handle, inode);
out:
	if (handle) {
		ret = next3_journal_stop(handle);
		if (!err)
			err = ret;
	}
	iput(inode);
	return err;
}

#else
/* with no exclude inode, exclude bitmap is reset to 0 */
#define next3_snapshot_init_bitmap_cache(sb, create)	\
		next3_snapshot_reset_bitmap_cache(sb, 1)
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
/*
 * next3_snapshot_load - load the on-disk snapshot list to memory.
 * Start with last (or active) snapshot and continue to older snapshots.
 * If snapshot load fails before active snapshot, force read-only mount.
 * If snapshot load fails after active snapshot, allow read-write mount.
 * Called from next3_fill_super() under sb_lock during mount time.
 *
 * Return values:
 * = 0 - on-disk snapshot list is empty or active snapshot loaded
 * < 0 - error loading active snapshot
 */
int next3_snapshot_load(struct super_block *sb, struct next3_super_block *es,
		int read_only)
{
	__u32 active_ino = le32_to_cpu(es->s_snapshot_inum);
	__u32 load_ino = le32_to_cpu(es->s_snapshot_list);
	int err, num = 0, snapshot_id = 0;
	int has_active = 0;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (!list_empty(&NEXT3_SB(sb)->s_snapshot_list)) {
		snapshot_debug(1, "warning: snapshots already loaded!\n");
		return -EINVAL;
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_OLD
	/* Migrate super block on-disk format */
	if (NEXT3_HAS_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT_OLD) &&
			!NEXT3_HAS_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT)) {
		u64 snapshot_r_blocks;

		/* Copy snapshot fields to new positions */
		es->s_snapshot_inum = es->s_snapshot_inum_old;
		active_ino = le32_to_cpu(es->s_snapshot_inum);
		es->s_snapshot_id = es->s_snapshot_id_old;
		snapshot_r_blocks = le32_to_cpu(es->s_snapshot_r_blocks_old);
		es->s_snapshot_r_blocks_count = cpu_to_le64(snapshot_r_blocks);
		es->s_snapshot_list = es->s_snapshot_list_old;
		/* Clear old snapshot fields */
		es->s_snapshot_inum_old = 0;
		es->s_snapshot_id_old = 0;
		es->s_snapshot_r_blocks_old = 0;
		es->s_snapshot_list_old = 0;
		/* Copy snapshot flags to new positions */
		NEXT3_SET_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
		if (NEXT3_HAS_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_COMPAT_EXCLUDE_INODE_OLD))
			NEXT3_SET_COMPAT_FEATURE(sb,
					NEXT3_FEATURE_COMPAT_EXCLUDE_INODE);
		if (NEXT3_HAS_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_FIX_SNAPSHOT_OLD))
			NEXT3_SET_FLAGS(sb, NEXT3_FLAGS_FIX_SNAPSHOT);
		if (NEXT3_HAS_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_FIX_EXCLUDE_OLD))
			NEXT3_SET_FLAGS(sb, NEXT3_FLAGS_FIX_EXCLUDE);
		/* Clear old snapshot flags */
		NEXT3_CLEAR_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT_OLD|
				NEXT3_FEATURE_RO_COMPAT_IS_SNAPSHOT_OLD|
				NEXT3_FEATURE_RO_COMPAT_FIX_SNAPSHOT_OLD|
				NEXT3_FEATURE_RO_COMPAT_FIX_EXCLUDE_OLD);
		/* Clear deprecated big journal flag */
		NEXT3_CLEAR_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_COMPAT_BIG_JOURNAL_OLD);
		NEXT3_CLEAR_FLAGS(sb, NEXT3_FLAGS_BIG_JOURNAL);
		/* Keep old exclude inode flag b/c inode was not moved yet */
	}

#endif
	/* init COW bitmap and exclude bitmap cache */
	err = next3_snapshot_init_bitmap_cache(sb, !read_only);
	if (err)
		return err;

	if (!load_ino && active_ino) {
		/* snapshots list is empty and active snapshot exists */
		if (!read_only)
			/* reset list head to active snapshot */
			es->s_snapshot_list = es->s_snapshot_inum;
		/* try to load active snapshot */
		load_ino = le32_to_cpu(es->s_snapshot_inum);
	}

	if (!NEXT3_HAS_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT)) {
		/*
		 * When mounting an ext3 formatted volume as next3, the
		 * HAS_SNAPSHOT flag is set on first snapshot_take()
		 * and after that the volume can no longer be mounted
		 * as rw ext3 (only rw next3 or ro ext3/ext2).
		 * If we find a non-zero last_snapshot or snapshot_inum
		 * and the HAS_SNAPSHOT flag is not set, we ignore them.
		 */
		if (load_ino)
			snapshot_debug(1, "warning: has_snapshot feature not "
					"set and last snapshot found (%u).\n",
					load_ino);
		return 0;
	}

	while (load_ino) {
		struct inode *inode;

		inode = next3_orphan_get(sb, load_ino);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
		} else if (!next3_snapshot_file(inode)) {
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
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL_DUMP
		next3_snapshot_dump(5, inode);
#endif

		if (!has_active && load_ino == active_ino) {
			/* active snapshot was loaded */
			err = next3_snapshot_set_active(sb, inode);
			if (err)
				break;
			has_active = 1;
		}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
		list_add_tail(&NEXT3_I(inode)->i_snaplist,
			      &NEXT3_SB(sb)->s_snapshot_list);
		load_ino = NEXT_SNAPSHOT(inode);
		/* keep snapshot list reference */
#else
		iput(inode);
		break;
#endif
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
		err = next3_snapshot_update(sb, 0, read_only);
		snapshot_debug(1, "%d snapshots loaded\n", num);
	}
	return err;
}

/*
 * next3_snapshot_destroy() releases the in-memory snapshot list
 * Called from next3_put_super() under sb_lock during umount time.
 * This function cannot fail.
 */
void next3_snapshot_destroy(struct super_block *sb)
{
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *l, *n;
	/* iterate safe because we are deleting from list and freeing the
	 * inodes */
	list_for_each_safe(l, n, &NEXT3_SB(sb)->s_snapshot_list) {
		struct inode *inode = &list_entry(l, struct next3_inode_info,
						  i_snaplist)->vfs_inode;
		list_del_init(&NEXT3_I(inode)->i_snaplist);
		/* remove snapshot list reference */
		iput(inode);
	}
#endif
	/* deactivate in-memory active snapshot - cannot fail */
	(void) next3_snapshot_set_active(sb, NULL);
}

/*
 * next3_snapshot_update - iterate snapshot list and update snapshots status.
 * @sb: handle to file system super block.
 * @cleanup: if true, shrink/merge/cleanup all snapshots marked for deletion.
 * @read_only: if true, don't remove snapshot after failed take.
 *
 * Called from next3_ioctl() under snapshot_mutex.
 * Called from snapshot_load() under sb_lock with @cleanup=0.
 * Returns 0 on success and <0 on error.
 */
int next3_snapshot_update(struct super_block *sb, int cleanup, int read_only)
{
	struct inode *active_snapshot = next3_snapshot_has_active(sb);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
	struct inode *used_by = NULL; /* last non-deleted snapshot found */
	int deleted;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct inode *inode;
	struct next3_inode_info *ei;
	int found_active = 0;
	int found_enabled = 0;
	struct list_head *prev;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
	int need_shrink = 0;
	int need_merge = 0;
#endif
	int err = 0;

	BUG_ON(read_only && cleanup);
	if (active_snapshot)
		NEXT3_I(active_snapshot)->i_flags |=
			NEXT3_SNAPFILE_ACTIVE_FL|NEXT3_SNAPFILE_LIST_FL;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	/* iterate safe from oldest snapshot backwards */
	prev = NEXT3_SB(sb)->s_snapshot_list.prev;
	if (list_empty(prev))
		return 0;

update_snapshot:
	ei = list_entry(prev, struct next3_inode_info, i_snaplist);
	inode = &ei->vfs_inode;
	prev = ei->i_snaplist.prev;

	/* all snapshots on the list have the LIST flag */
	ei->i_flags |= NEXT3_SNAPFILE_LIST_FL;
	/* set the 'No_Dump' flag on all snapshots */
	ei->i_flags |= NEXT3_NODUMP_FL;

	/*
	 * snapshots later than active (failed take) should be removed.
	 * no active snapshot means failed first snapshot take.
	 */
	if (found_active || !active_snapshot) {
		if (!read_only)
			err = next3_snapshot_remove(inode);
		goto prev_snapshot;
	}

	deleted = ei->i_flags & NEXT3_SNAPFILE_DELETED_FL;
	if (!deleted && read_only)
		/* auto enable snapshots on readonly mount */
		next3_snapshot_enable(inode);

	/*
	 * after completion of a snapshot management operation,
	 * only the active snapshot can have the ACTIVE flag
	 */
	if (inode == active_snapshot) {
		ei->i_flags |= NEXT3_SNAPFILE_ACTIVE_FL;
		found_active = 1;
		deleted = 0;
	} else
		ei->i_flags &= ~NEXT3_SNAPFILE_ACTIVE_FL;

	if (found_enabled)
		/* snapshot is in use by an older enabled snapshot */
		ei->i_flags |= NEXT3_SNAPFILE_INUSE_FL;
	else
		/* snapshot is not in use by older enabled snapshots */
		ei->i_flags &= ~NEXT3_SNAPFILE_INUSE_FL;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
	if (cleanup)
		err = next3_snapshot_cleanup(inode, used_by, deleted,
				&need_shrink, &need_merge);
#else
	if (cleanup && deleted && !used_by)
		/* remove permanently unused deleted snapshot */
		err = next3_snapshot_remove(inode);
#endif

	if (!deleted) {
		if (!found_active)
			/* newer snapshots are potentially used by
			 * this snapshot (when it is enabled) */
			used_by = inode;
		if (ei->i_flags & NEXT3_SNAPFILE_ENABLED_FL)
			found_enabled = 1;
		else
			SNAPSHOT_SET_DISABLED(inode);
	} else
		SNAPSHOT_SET_DISABLED(inode);

prev_snapshot:
	if (err)
		return err;
	/* update prev snapshot */
	if (prev != &NEXT3_SB(sb)->s_snapshot_list)
		goto update_snapshot;
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
	if (!active_snapshot || !cleanup || used_by)
		return 0;

	/* if all snapshots are deleted - deactivate active snapshot */
	deleted = NEXT3_I(active_snapshot)->i_flags & NEXT3_SNAPFILE_DELETED_FL;
	if (deleted && igrab(active_snapshot)) {
		/* lock journal updates before deactivating snapshot */
		sb->s_op->freeze_fs(sb);
		lock_super(sb);
		/* deactivate in-memory active snapshot - cannot fail */
		(void) next3_snapshot_set_active(sb, NULL);
		/* clear on-disk active snapshot */
		NEXT3_SB(sb)->s_es->s_snapshot_inum = 0;
		unlock_super(sb);
		sb->s_op->unfreeze_fs(sb);
		/* remove unused deleted active snapshot */
		err = next3_snapshot_remove(active_snapshot);
		/* drop the refcount to 0 */
		iput(active_snapshot);
	}
#endif
	return err;
}
#else
int next3_snapshot_load(struct super_block *sb, struct next3_super_block *es,
		int read_only)
{
	return 0;
}

void next3_snapshot_destroy(struct super_block *sb)
{
}
#endif
