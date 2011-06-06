/*
 * linux/fs/ext4/snapshot.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots core functions.
 */

#include <linux/quotaops.h>
#include "snapshot.h"
#include "ext4.h"
#include "mballoc.h"

#define snapshot_debug_hl(n, f, a...) snapshot_debug_l(n, handle ? \
					IS_COWING(handle) : 0, f, ## a)

/*
 * ext4_snapshot_map_blocks() - helper function for
 * ext4_snapshot_test_and_cow().  Test if blocks are mapped in snapshot file.
 * If @block is not mapped and if @cmd is non zero, try to allocate @maxblocks.
 * Also used by ext4_snapshot_create() to pre-allocate snapshot blocks.
 *
 * Return values:
 * > 0 - no. of mapped blocks in snapshot file
 * = 0 - @block is not mapped in snapshot file
 * < 0 - error
 */
int ext4_snapshot_map_blocks(handle_t *handle, struct inode *inode,
			      ext4_snapblk_t block, unsigned long maxblocks,
			      ext4_fsblk_t *mapped, int cmd)
{
	int err;
	struct ext4_map_blocks map;

	map.m_lblk = SNAPSHOT_IBLOCK(block);
	map.m_len = maxblocks;

	err = ext4_map_blocks(handle, inode, &map, cmd);
	/*
	 * ext4_get_blocks_handle() returns number of blocks
	 * mapped. 0 in case of a HOLE.
	 */
	if (mapped && err > 0)
		*mapped = map.m_pblk;

	snapshot_debug_hl(4, "snapshot (%u) map_blocks "
			"[%lld/%lld] = [%lld/%lld] "
			"cmd=%d, maxblocks=%lu, mapped=%d\n",
			inode->i_generation,
			SNAPSHOT_BLOCK_TUPLE(block),
			SNAPSHOT_BLOCK_TUPLE(map.m_pblk),
			cmd, maxblocks, err);
	return err;
}

/*
 * COW helper functions
 */

/*
 * copy buffer @bh to (locked) snapshot buffer @sbh and mark it uptodate
 */
static inline void
__ext4_snapshot_copy_buffer(struct buffer_head *sbh,
		struct buffer_head *bh)
{
	memcpy(sbh->b_data, bh->b_data, SNAPSHOT_BLOCK_SIZE);
	set_buffer_uptodate(sbh);
}

/*
 * use @mask to clear exclude bitmap bits from block bitmap
 * when creating COW bitmap and mark snapshot buffer @sbh uptodate
 */
static inline void
__ext4_snapshot_copy_bitmap(struct buffer_head *sbh,
		char *dst, const char *src, const char *mask)
{
	const u32 *ps = (const u32 *)src, *pm = (const u32 *)mask;
	u32 *pd = (u32 *)dst;
	int i;

	if (mask) {
		for (i = 0; i < SNAPSHOT_ADDR_PER_BLOCK; i++)
			*pd++ = *ps++ & ~*pm++;
	} else
		memcpy(dst, src, SNAPSHOT_BLOCK_SIZE);

	set_buffer_uptodate(sbh);
}

/*
 * ext4_snapshot_complete_cow()
 * Unlock a newly COWed snapshot buffer and complete the COW operation.
 * Optionally, sync the buffer to disk or add it to the current transaction
 * as dirty data.
 */
static inline int
ext4_snapshot_complete_cow(handle_t *handle, struct inode *snapshot,
		struct buffer_head *sbh, struct buffer_head *bh, int sync)
{
	int err = 0;

	unlock_buffer(sbh);
	err = ext4_jbd2_file_inode(handle, snapshot);
	if (err)
		goto out;
	mark_buffer_dirty(sbh);
	if (sync)
		sync_dirty_buffer(sbh);
out:
	return err;
}

/*
 * ext4_snapshot_copy_buffer_cow()
 * helper function for ext4_snapshot_test_and_cow()
 * copy COWed buffer to new allocated (locked) snapshot buffer
 * add complete the COW operation
 */
static inline int
ext4_snapshot_copy_buffer_cow(handle_t *handle, struct inode *snapshot,
				   struct buffer_head *sbh,
				   struct buffer_head *bh)
{
	__ext4_snapshot_copy_buffer(sbh, bh);
	return ext4_snapshot_complete_cow(handle, snapshot, sbh, bh, 0);
}

/*
 * ext4_snapshot_copy_buffer()
 * helper function for ext4_snapshot_take()
 * used for initializing pre-allocated snapshot blocks
 * copy buffer to snapshot buffer and sync to disk
 * 'mask' block bitmap with exclude bitmap before copying to snapshot.
 */
void ext4_snapshot_copy_buffer(struct buffer_head *sbh,
		struct buffer_head *bh, const char *mask)
{
	lock_buffer(sbh);
	if (mask)
		__ext4_snapshot_copy_bitmap(sbh,
				sbh->b_data, bh->b_data, mask);
	else
		__ext4_snapshot_copy_buffer(sbh, bh);
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);
}

/*
 * COW bitmap functions
 */

/*
 * ext4_snapshot_init_cow_bitmap() init a new allocated (locked) COW bitmap
 * buffer on first time block group access after snapshot take.
 * COW bitmap is created by masking the block bitmap with exclude bitmap.
 */
static int
ext4_snapshot_init_cow_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *cow_bh)
{
	struct buffer_head *bitmap_bh;
	char *dst, *src, *mask = NULL;

	bitmap_bh = ext4_read_block_bitmap(sb, block_group);
	if (!bitmap_bh)
		return -EIO;

	src = bitmap_bh->b_data;
	/*
	 * Another COWing task may be changing this block bitmap
	 * (allocating active snapshot blocks) while we are trying
	 * to copy it.  At this point we are guaranteed that the only
	 * changes to block bitmap are the new active snapshot blocks,
	 * because before allocating/freeing any other blocks a task
	 * must first get_write_access() on the bitmap and get here.
	 */
	ext4_lock_group(sb, block_group);

	/*
	 * in the path coming from ext4_snapshot_read_block_bitmap(),
	 * cow_bh is a user page buffer so it has to be kmapped.
	 */
	dst = kmap_atomic(cow_bh->b_page, KM_USER0);
	__ext4_snapshot_copy_bitmap(cow_bh, dst, src, mask);
	kunmap_atomic(dst, KM_USER0);

	ext4_unlock_group(sb, block_group);

	brelse(bitmap_bh);
	return 0;
}

/*
 * ext4_snapshot_read_block_bitmap()
 * helper function for ext4_snapshot_get_block()
 * used for fixing the block bitmap user page buffer when
 * reading through to block device.
 */
int ext4_snapshot_read_block_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *bitmap_bh)
{
	int err;

	lock_buffer(bitmap_bh);
	err = ext4_snapshot_init_cow_bitmap(sb, block_group, bitmap_bh);
	unlock_buffer(bitmap_bh);
	return err;
}

/*
 * ext4_snapshot_read_cow_bitmap - read COW bitmap from active snapshot
 * @handle:	JBD handle
 * @snapshot:	active snapshot
 * @block_group: block group
 *
 * Reads the COW bitmap block (i.e., the active snapshot copy of block bitmap).
 * Creates the COW bitmap on first access to @block_group after snapshot take.
 * COW bitmap cache is non-persistent, so no need to mark the group descriptor
 * block dirty.  COW bitmap races are handled internally, so no locks are
 * required when calling this function, only a valid @handle.
 *
 * Return COW bitmap buffer on success or NULL in case of failure.
 */
static struct buffer_head *
ext4_snapshot_read_cow_bitmap(handle_t *handle, struct inode *snapshot,
			       unsigned int block_group)
{
	struct super_block *sb = snapshot->i_sb;
	struct ext4_group_info *grp = ext4_get_group_info(sb, block_group);
	struct ext4_group_desc *desc;
	struct buffer_head *cow_bh;
	ext4_fsblk_t bitmap_blk;
	ext4_fsblk_t cow_bitmap_blk;
	int err = 0;

	desc = ext4_get_group_desc(sb, block_group, NULL);
	if (!desc)
		return NULL;

	bitmap_blk = ext4_block_bitmap(sb, desc);

	ext4_lock_group(sb, block_group);
	cow_bitmap_blk = grp->bg_cow_bitmap;
	ext4_unlock_group(sb, block_group);
	if (cow_bitmap_blk)
		return sb_bread(sb, cow_bitmap_blk);

	/*
	 * Try to read cow bitmap block from snapshot file.  If COW bitmap
	 * is not yet allocated, create the new COW bitmap block.
	 */
	cow_bh = ext4_bread(handle, snapshot, SNAPSHOT_IBLOCK(bitmap_blk),
				SNAPMAP_READ, &err);
	if (cow_bh)
		goto out;

	/* allocate snapshot block for COW bitmap */
	cow_bh = ext4_getblk(handle, snapshot, SNAPSHOT_IBLOCK(bitmap_blk),
			     SNAPMAP_BITMAP, &err);
	if (!cow_bh)
		goto out;
	if (!err) {
		/*
		 * err should be 1 to indicate new allocated (locked) buffer.
		 * if err is 0, it means that someone mapped this block
		 * before us, while we are updating the COW bitmap cache.
		 * the pending COW bitmap code should prevent that.
		 */
		WARN_ON(1);
		err = -EIO;
		goto out;
	}

	err = ext4_snapshot_init_cow_bitmap(sb, block_group, cow_bh);
	if (err)
		goto out;
	/*
	 * complete pending COW operation. no need to wait for tracked reads
	 * of block bitmap, because it is copied directly to page buffer by
	 * ext4_snapshot_read_block_bitmap()
	 */
	err = ext4_snapshot_complete_cow(handle, snapshot, cow_bh, NULL, 1);
	if (err)
		goto out;

	trace_cow_inc(handle, bitmaps);
out:
	if (!err && cow_bh) {
		/* initialized COW bitmap block */
		cow_bitmap_blk = cow_bh->b_blocknr;
		snapshot_debug(3, "COW bitmap #%u of snapshot (%u) "
				"mapped to block [%lld/%lld]\n",
				block_group, snapshot->i_generation,
				SNAPSHOT_BLOCK_TUPLE(cow_bitmap_blk));
	} else {
		/* uninitialized COW bitmap block */
		cow_bitmap_blk = 0;
		snapshot_debug(1, "failed to read COW bitmap #%u of snapshot "
				"(%u)\n", block_group, snapshot->i_generation);
		brelse(cow_bh);
		cow_bh = NULL;
	}

	/* update or reset COW bitmap cache */
	ext4_lock_group(sb, block_group);
	grp->bg_cow_bitmap = cow_bitmap_blk;
	ext4_unlock_group(sb, block_group);

	return cow_bh;
}

/*
 * ext4_snapshot_test_cow_bitmap - test if blocks are in use by snapshot
 * @handle:	JBD handle
 * @snapshot:	active snapshot
 * @block:	address of block
 * @maxblocks:	max no. of blocks to be tested
 * @excluded:	if not NULL, blocks belong to this excluded inode
 *
 * If the block bit is set in the COW bitmap, than it was allocated at the time
 * that the active snapshot was taken and is therefore "in use" by the snapshot.
 *
 * Return values:
 * > 0 - blocks are in use by snapshot
 * = 0 - @blocks are not in use by snapshot
 * < 0 - error
 */
static int
ext4_snapshot_test_cow_bitmap(handle_t *handle, struct inode *snapshot,
		ext4_fsblk_t block, int *maxblocks, struct inode *excluded)
{
	struct buffer_head *cow_bh;
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(block);
	ext4_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(block);
	ext4_fsblk_t snapshot_blocks = SNAPSHOT_BLOCKS(snapshot);
	int ret;

	if (block >= snapshot_blocks)
		/*
		 * Block is not is use by snapshot because it is past the
		 * last f/s block at the time that the snapshot was taken.
		 * (suggests that f/s was resized after snapshot take)
		 */
		return 0;

	cow_bh = ext4_snapshot_read_cow_bitmap(handle, snapshot, block_group);
	if (!cow_bh)
		return -EIO;
	/*
	 * if the bit is set in the COW bitmap,
	 * then the block is in use by snapshot
	 */

	ret = ext4_mb_test_bit_range(bit, cow_bh->b_data, maxblocks);

	brelse(cow_bh);
	return ret;
}
/*
 * COW functions
 */

#ifdef CONFIG_EXT4_DEBUG
static void
__ext4_snapshot_trace_cow(const char *where, handle_t *handle,
		struct super_block *sb, struct inode *inode,
		struct buffer_head *bh, ext4_fsblk_t block,
		int count, int cmd)
{
	unsigned long inode_group = 0;
	ext4_grpblk_t inode_offset = 0;

	if (inode) {
		inode_group = (inode->i_ino - 1) /
			EXT4_INODES_PER_GROUP(sb);
		inode_offset = (inode->i_ino - 1) %
			EXT4_INODES_PER_GROUP(sb);
	}
	snapshot_debug_hl(4, "%s(i:%d/%ld, b:%lld/%lld) "
			"count=%d, h_ref=%d, cmd=%d\n",
			where, inode_offset, inode_group,
			SNAPSHOT_BLOCK_TUPLE(block),
			count, handle->h_ref, cmd);
}

#define ext4_snapshot_trace_cow(where, handle, sb, inode, bh, blk, cnt, cmd) \
	if (snapshot_enable_debug >= 4)					\
		__ext4_snapshot_trace_cow(where, handle, sb, inode,	\
				bh, block, count, cmd)
#else
#define ext4_snapshot_trace_cow(where, handle, sb, inode, bh, blk, cnt, cmd)
#endif
/*
 * The last transaction ID during which the buffer has been COWed is stored in
 * the b_cow_tid field of the journal_head struct.  If we know that the buffer
 * was COWed during the current transaction, we don't need to COW it again.
 * [jbd_lock_bh_state()]
 */

void init_ext4_snapshot_cow_cache(void)
{
#ifdef CONFIG_EXT4_DEBUG
	cow_cache_enabled = 1;
#endif
}

#ifdef CONFIG_EXT4_DEBUG
#define cow_cache_enabled()	(cow_cache_enabled)
#else
#define cow_cache_enabled()	(1)
#endif

#define test_cow_tid(jh, handle)	\
	((jh)->b_cow_tid == (handle)->h_transaction->t_tid)
#define set_cow_tid(jh, handle)		\
	((jh)->b_cow_tid = (handle)->h_transaction->t_tid)

/*
 * Journal COW cache functions.
 * a block can only be COWed once per snapshot,
 * so a block can only be COWed once per transaction,
 * so a buffer that was COWed in the current transaction,
 * doesn't need to be COWed.
 *
 * Return values:
 * 1 - block was COWed in current transaction
 * 0 - block wasn't COWed in current transaction
 */
static int
ext4_snapshot_test_cowed(handle_t *handle, struct buffer_head *bh)
{
	struct journal_head *jh;

	if (!cow_cache_enabled())
		return 0;

	/* check the COW tid in the journal head */
	if (bh && buffer_jbd(bh)) {
		jbd_lock_bh_state(bh);
		jh = bh2jh(bh);
		if (jh && !test_cow_tid(jh, handle))
			jh = NULL;
		jbd_unlock_bh_state(bh);
		if (jh)
			/*
			 * Block was already COWed in the running transaction,
			 * so we don't need to COW it again.
			 */
			return 1;
	}
	return 0;
}

static void
ext4_snapshot_mark_cowed(handle_t *handle, struct buffer_head *bh)
{
	struct journal_head *jh;

	if (!cow_cache_enabled())
		return;

	if (bh && buffer_jbd(bh)) {
		jbd_lock_bh_state(bh);
		jh = bh2jh(bh);
		if (jh && !test_cow_tid(jh, handle))
			/*
			 * this is the first time this block was COWed
			 * in the running transaction.
			 * update the COW tid in the journal head
			 * to mark that this block doesn't need to be COWed.
			 */
			set_cow_tid(jh, handle);
		jbd_unlock_bh_state(bh);
	}
}

/*
 * Begin COW or move operation.
 * No locks needed here, because @handle is a per-task struct.
 */
static inline void ext4_snapshot_cow_begin(handle_t *handle)
{
	if (!ext4_handle_has_enough_credits(handle, 1)) {
		/*
		 * The test above is based on lower limit heuristics of
		 * user_credits/buffer_credits, which is not always accurate,
		 * so it is possible that there is no bug here, just another
		 * false alarm.
		 */
		snapshot_debug_hl(1, "warning: insufficient buffer/user "
				  "credits (%d/%d) for COW operation?\n",
				  handle->h_buffer_credits,
				  handle->h_user_credits);
	}
	snapshot_debug_hl(4, "{\n");
	handle->h_cowing = 1;
}

/*
 * End COW or move operation.
 * No locks needed here, because @handle is a per-task struct.
 */
static inline void ext4_snapshot_cow_end(const char *where,
		handle_t *handle, ext4_fsblk_t block, int err)
{
	handle->h_cowing = 0;
	snapshot_debug_hl(4, "} = %d\n", err);
	snapshot_debug_hl(4, ".\n");
	if (err < 0)
		snapshot_debug(1, "%s(b:%lld/%lld) failed!"
				" h_ref=%d, err=%d\n", where,
				SNAPSHOT_BLOCK_TUPLE(block),
				handle->h_ref, err);
}

/*
 * ext4_snapshot_test_and_cow - COW metadata block
 * @where:	name of caller function
 * @handle:	JBD handle
 * @inode:	owner of blocks (NULL for global metadata blocks)
 * @block:	address of metadata block
 * @bh:		buffer head of metadata block
 * @cow:	if false, return 1 if block needs to be COWed
 *
 * Return values:
 * = 1 - @block needs to be COWed
 * = 0 - @block was COWed or doesn't need to be COWed
 * < 0 - error
 */
int ext4_snapshot_test_and_cow(const char *where, handle_t *handle,
		struct inode *inode, ext4_fsblk_t block,
		struct buffer_head *bh, int cow)
{
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	struct buffer_head *sbh = NULL;
	ext4_fsblk_t blk = 0;
	int err = 0, clear = 0, count = 1;

	if (!active_snapshot)
		/* no active snapshot - no need to COW */
		return 0;

	ext4_snapshot_trace_cow(where, handle, sb, inode, bh, block, 1, cow);

	if (IS_COWING(handle)) {
		/* avoid recursion on active snapshot updates */
		WARN_ON(inode && inode != active_snapshot);
		snapshot_debug_hl(4, "active snapshot update - "
				  "skip block cow!\n");
		return 0;
	} else if (inode == active_snapshot) {
		/* active snapshot may only be modified during COW */
		snapshot_debug_hl(4, "active snapshot access denied!\n");
		return -EPERM;
	}
	/* check if the buffer was COWed in the current transaction */
	if (ext4_snapshot_test_cowed(handle, bh)) {
		snapshot_debug_hl(4, "buffer found in COW cache - "
				  "skip block cow!\n");
		trace_cow_inc(handle, ok_jh);
		return 0;
	}

	/* BEGIN COWing */
	ext4_snapshot_cow_begin(handle);

	if (inode)
		clear = ext4_snapshot_excluded(inode);
	if (clear < 0) {
		/*
		 * excluded file block access - don't COW and
		 * mark block in exclude bitmap
		 */
		snapshot_debug_hl(4, "file (%lu) excluded from snapshot - "
				"mark block (%lld) in exclude bitmap\n",
				inode->i_ino, block);
		cow = 0;
	}

	/* get the COW bitmap and test if blocks are in use by snapshot */
	err = ext4_snapshot_test_cow_bitmap(handle, active_snapshot,
			block, &count, clear < 0 ? inode : NULL);
	if (err < 0)
		goto out;
	if (!err) {
		trace_cow_inc(handle, ok_bitmap);
		goto cowed;
	}

	/* block is in use by snapshot - check if it is mapped */
	err = ext4_snapshot_map_blocks(handle, active_snapshot, block, 1, &blk,
					SNAPMAP_READ);
	if (err < 0)
		goto out;
	if (err > 0) {
		sbh = sb_find_get_block(sb, blk);
		trace_cow_inc(handle, ok_mapped);
		err = 0;
		goto test_pending_cow;
	}

	/* block needs to be COWed */
	err = 1;
	if (!cow)
		/* don't COW - we were just checking */
		goto out;

	err = -EIO;
	/* make sure we hold an uptodate source buffer */
	if (!bh || !buffer_mapped(bh))
		goto out;
	if (!buffer_uptodate(bh)) {
		snapshot_debug(1, "warning: non uptodate buffer (%lld)"
				" needs to be copied to active snapshot!\n",
				block);
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh))
			goto out;
	}

	/* try to allocate snapshot block to make a backup copy */
	sbh = ext4_getblk(handle, active_snapshot, SNAPSHOT_IBLOCK(block),
			   SNAPMAP_COW, &err);
	if (!sbh)
		goto out;

	blk = sbh->b_blocknr;
	if (!err) {
		/*
		 * we didn't allocate this block -
		 * another COWing task must have allocated it
		 */
		trace_cow_inc(handle, ok_mapped);
		goto test_pending_cow;
	}

	/*
	 * we allocated this block -
	 * copy block data to snapshot and complete COW operation
	 */
	err = ext4_snapshot_copy_buffer_cow(handle, active_snapshot,
			sbh, bh);
	if (err)
		goto out;
	snapshot_debug(3, "block [%lld/%lld] of snapshot (%u) "
			"mapped to block [%lld/%lld]\n",
			SNAPSHOT_BLOCK_TUPLE(block),
			active_snapshot->i_generation,
			SNAPSHOT_BLOCK_TUPLE(sbh->b_blocknr));

	trace_cow_inc(handle, copied);
test_pending_cow:

cowed:
	/* mark the buffer COWed in the current transaction */
	ext4_snapshot_mark_cowed(handle, bh);
out:
	brelse(sbh);
	/* END COWing */
	ext4_snapshot_cow_end(where, handle, block, err);
	return err;
}

/*
 * ext4_snapshot_test_and_move - move blocks to active snapshot
 * @where:	name of caller function
 * @handle:	JBD handle
 * @inode:	owner of blocks (NULL for global metadata blocks)
 * @block:	address of first block to move
 * @maxblocks:	max. blocks to move
 * @move:	if false, only test if @block needs to be moved
 *
 * Return values:
 * > 0 - blocks a) were moved to snapshot for @move = 1;
 *		b) needs to be moved for @move = 0
 * = 0 - blocks dont need to be moved
 * < 0 - error
 */
int ext4_snapshot_test_and_move(const char *where, handle_t *handle,
	struct inode *inode, ext4_fsblk_t block, int *maxblocks, int move)
{
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	ext4_fsblk_t blk = 0;
	int err = 0, count = *maxblocks;
	int moved_blks = 0;
	int excluded = 0;

	if (!active_snapshot)
		/* no active snapshot - no need to move */
		return 0;

	ext4_snapshot_trace_cow(where, handle, sb, inode, NULL, block, count,
				move);

	BUG_ON(IS_COWING(handle) || inode == active_snapshot);

	/* BEGIN moving */
	ext4_snapshot_cow_begin(handle);

	if (inode)
		excluded = ext4_snapshot_excluded(inode);
	if (excluded) {
		/* don't move excluded file block to snapshot */
		snapshot_debug_hl(4, "file (%lu) excluded from snapshot\n",
				inode->i_ino);
		move = 0;
	}

	/* get the COW bitmap and test if blocks are in use by snapshot */
	err = ext4_snapshot_test_cow_bitmap(handle, active_snapshot,
			block, &count, excluded ? inode : NULL);
	if (err < 0)
		goto out;
	if (!err) {
		/* block not in COW bitmap - no need to move */
		trace_cow_add(handle, ok_bitmap, count);
		goto out;
	}

#ifdef CONFIG_EXT4_DEBUG
	if (inode == NULL &&
		!(EXT4_I(active_snapshot)->i_flags & EXT4_UNRM_FL)) {
		/*
		 * This is ext4_group_extend() "freeing" the blocks that
		 * were added to the block group.  These block should not be
		 * moved to snapshot, unless the snapshot is marked with the
		 * UNRM flag for large snapshot creation test.
		 */
		trace_cow_add(handle, ok_bitmap, count);
		err = 0;
		goto out;
	}
#endif

	/* count blocks are in use by snapshot - check if @block is mapped */
	err = ext4_snapshot_map_blocks(handle, active_snapshot, block, count,
					&blk, SNAPMAP_READ);
	if (err < 0)
		goto out;
	if (err > 0) {
		/* blocks already mapped in snapshot - no need to move */
		count = err;
		trace_cow_add(handle, ok_mapped, count);
		err = 0;
		goto out;
	}

	/* @count blocks need to be moved */
	err = count;
	if (!move)
		/* don't move - we were just checking */
		goto out;

	/* try to move @count blocks from inode to snapshot.
	 * @count blocks may cross block boundry.
	 * TODO: if moving fails after some blocks has been moved,
	 * maybe we need a blockbitmap fsck.
	 */
	blk = block;
	while (count) {
		err = ext4_snapshot_map_blocks(handle, active_snapshot, blk,
					count, NULL, SNAPMAP_MOVE);
		if (err <= 0)
			goto out;
		moved_blks += err;
		blk += err;
		count -= err;
	}
	count = moved_blks;
	err = moved_blks;
	/*
	 * User should no longer be charged for these blocks.
	 * Snapshot file owner was charged for these blocks
	 * when they were mapped to snapshot file.
	 */
	if (inode)
		dquot_free_block(inode, count);
	trace_cow_add(handle, moved, count);
out:
	/* END moving */
	ext4_snapshot_cow_end(where, handle, block, err);
	*maxblocks = count;
	return err;
}

