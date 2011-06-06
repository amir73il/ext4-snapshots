/*
 * linux/fs/ext4/snapshot_inode.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots inode functions.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/jbd2.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/mpage.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include <linux/bio.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"
#include "ext4_extents.h"

#include <trace/events/ext4.h>
#include "snapshot.h"

/**
 * ext4_blks_to_skip - count the number blocks that can be skipped
 * @inode: inode in question
 * @i_block: start block number
 * @maxblocks: max number of data blocks to be skipped
 * @chain: chain of indirect blocks
 * @depth: length of chain from inode to data block
 * @offsets: array of offsets in chain blocks
 * @k: number of allocated blocks in the chain
 *
 * Counts the number of non-allocated data blocks (holes) at offset @i_block.
 * Called from ext4_snapshot_merge_blocks() and ext4_snapshot_shrink_blocks()
 * under snapshot_mutex.
 * Returns the total number of data blocks to be skipped.
 */

static int ext4_blks_to_skip(struct inode *inode, long i_block,
		unsigned long maxblocks, Indirect chain[4], int depth,
		int *offsets, int k)
{
	int ptrs = EXT4_ADDR_PER_BLOCK(inode->i_sb);
	int ptrs_bits = EXT4_ADDR_PER_BLOCK_BITS(inode->i_sb);
	const long direct_blocks = EXT4_NDIR_BLOCKS,
		indirect_blocks = ptrs,
		double_blocks = (1 << (ptrs_bits * 2));
	/* number of data blocks mapped with a single splice to the chain */
	int data_ptrs_bits = ptrs_bits * (depth - k - 1);
	int max_ptrs = maxblocks >> data_ptrs_bits;
	int final = 0;
	unsigned long count = 0;

	switch (depth) {
	case 4: /* tripple indirect */
		i_block -= double_blocks;
		/* fall through */
	case 3: /* double indirect */
		i_block -= indirect_blocks;
		/* fall through */
	case 2: /* indirect */
		i_block -= direct_blocks;
		final = (k == 0 ? 1 : ptrs);
		break;
	case 1: /* direct */
		final = direct_blocks;
		break;
	}
	/* offset of block from start of splice point */
	i_block &= ((1 << data_ptrs_bits) - 1);

	count++;
	while (count <= max_ptrs &&
		offsets[k] + count < final &&
		le32_to_cpu(*(chain[k].p + count)) == 0) {
		count++;
	}
	/* number of data blocks mapped by 'count' splice points */
	count <<= data_ptrs_bits;
	count -= i_block;
	return count < maxblocks ? count : maxblocks;
}

/*
 * ext4_snapshot_shrink_blocks - free unused blocks from deleted snapshot
 * @handle: JBD handle for this transaction
 * @inode:	inode we're shrinking
 * @iblock:	inode offset to first data block to shrink
 * @maxblocks:	inode range of data blocks to shrink
 * @cow_bh:	buffer head to map the COW bitmap block
 *		if NULL, don't look for COW bitmap block
 * @shrink:	shrink mode: 0 (don't free), >0 (free unused), <0 (free all)
 * @pmapped:	return no. of mapped blocks or 0 for skipped holes
 *
 * Frees @maxblocks blocks starting at offset @iblock in @inode, which are not
 * 'in-use' by non-deleted snapshots (blocks 'in-use' are set in COW bitmap).
 * If @shrink is false, just count mapped blocks and look for COW bitmap block.
 * The first time that a COW bitmap block is found in @inode, whether @inode is
 * deleted or not, it is stored in @cow_bh and is used in subsequent calls to
 * this function with other deleted snapshots within the block group boundaries.
 * Called from ext4_snapshot_shrink_blocks() under snapshot_mutex.
 *
 * Return values:
 * >= 0 - no. of shrunk blocks (*@pmapped ? mapped blocks : skipped holes)
 *  < 0 - error
 */
int ext4_snapshot_shrink_blocks(handle_t *handle, struct inode *inode,
		sector_t iblock, unsigned long maxblocks,
		struct buffer_head *cow_bh,
		int shrink, int *pmapped)
{
	int offsets[4];
	Indirect chain[4], *partial;
	int err, blocks_to_boundary, depth, count;
	struct buffer_head *sbh = NULL;
	struct ext4_group_desc *desc = NULL;
	ext4_snapblk_t block_bitmap, block = SNAPSHOT_BLOCK(iblock);
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(block);
	int mapped_blocks = 0, freed_blocks = 0;
	const char *cow_bitmap;

	BUG_ON(shrink &&
	       (!(ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED)) ||
		ext4_snapshot_is_active(inode)));

	depth = ext4_block_to_path(inode, iblock, offsets,
			&blocks_to_boundary);
	if (depth == 0)
		return -EIO;

	desc = ext4_get_group_desc(inode->i_sb, block_group, NULL);
	if (!desc)
		return -EIO;
	block_bitmap = ext4_block_bitmap(inode->i_sb, desc);
	partial = ext4_get_branch(inode, depth, offsets, chain, &err);
	if (err)
		return err;

	if (partial) {
		/* block not mapped (hole) - count the number of holes to
		 * skip */
		count = ext4_blks_to_skip(inode, iblock, maxblocks, chain,
					   depth, offsets, (partial - chain));
		snapshot_debug(3, "skipping snapshot (%u) blocks: block=0x%llx"
			       ", count=0x%x\n", inode->i_generation,
			       block, count);
		goto shrink_indirect_blocks;
	}

	/* data block mapped - check if data blocks should be freed */
	partial = chain + depth - 1;
	/* scan all blocks upto maxblocks/boundary */
	count = 0;
	while (count < maxblocks && count <= blocks_to_boundary) {
		ext4_fsblk_t blk = le32_to_cpu(*(partial->p + count));
		if (blk && block + count == block_bitmap &&
			cow_bh && !buffer_mapped(cow_bh)) {
			/*
			 * 'blk' is the COW bitmap physical block -
			 * store it in cow_bh for subsequent calls
			 * FIXME: for non-first flex_bg group,
			 * we will not find COW bitmap like this.
			 */
			map_bh(cow_bh, inode->i_sb, blk);
			set_buffer_new(cow_bh);
			snapshot_debug(3, "COW bitmap #%lu: snapshot "
				"(%u), bitmap_blk=(+%lld)\n",
				block_group, inode->i_generation,
				SNAPSHOT_BLOCK_GROUP_OFFSET(block_bitmap));
		}
		if (blk)
			/* count mapped blocks in range */
			mapped_blocks++;
		else if (shrink >= 0)
			/*
			 * Unless we are freeing all block in range,
			 * we cannot have holes inside mapped range
			 */
			break;
		/* count size of range */
		count++;
	}

	if (!shrink)
		goto done_shrinking;

	cow_bitmap = NULL;
	if (shrink > 0 && cow_bh && buffer_mapped(cow_bh)) {
		/* we found COW bitmap - consult it when shrinking */
		sbh = sb_bread(inode->i_sb, cow_bh->b_blocknr);
		if (!sbh) {
			err = -EIO;
			goto cleanup;
		}
		cow_bitmap = sbh->b_data;
	}
	if (shrink < 0 || cow_bitmap) {
		int bit = SNAPSHOT_BLOCK_GROUP_OFFSET(block);

		BUG_ON(bit + count > SNAPSHOT_BLOCKS_PER_GROUP);
		/* free blocks with or without consulting COW bitmap */
		ext4_free_data_cow(handle, inode, partial->bh,
				partial->p, partial->p + count,
				cow_bitmap, bit, &freed_blocks);
	}

shrink_indirect_blocks:
	/* check if the indirect block should be freed */
	if (shrink && partial == chain + depth - 1) {
		Indirect *ind = partial - 1;
		__le32 *p = NULL;
		if (freed_blocks == mapped_blocks &&
		    count > blocks_to_boundary) {
			for (p = (__le32 *)(partial->bh->b_data);
			     !*p && p < partial->p; p++)
				;
		}
		if (p == partial->p)
			/* indirect block maps zero data blocks - free it */
			ext4_free_branches(handle, inode, ind->bh, ind->p,
					ind->p+1, 1);
	}

done_shrinking:
	snapshot_debug(3, "shrinking snapshot (%u) blocks: shrink=%d, "
			"block=0x%llx, count=0x%x, mapped=0x%x, freed=0x%x\n",
			inode->i_generation, shrink, block, count,
			mapped_blocks, freed_blocks);

	if (pmapped)
		*pmapped = mapped_blocks;
	err = count;
cleanup:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	brelse(sbh);
return err;
}

/*
 * ext4_snapshot_count_blocks - count blocks and verify that
 * snapshot blocks are excluded.
 *	@inode:	snapshot we are merging
 *	@block:	first block to test
 *	@count:	no. of blocks to test
 *	@pblocks: pointer to counter of blocks
 *
 * Return <0 on error or if blocks are not excluded.
 */
static int ext4_snapshot_count_blocks(struct inode *inode, ext4_fsblk_t block,
				      unsigned long count, int *pblocks)
{
	int err;

	/* test that blocks are excluded and update blocks counter */
	err = ext4_snapshot_test_excluded(inode, block, count);
	if (err)
		return err;
	*pblocks += count;
	return 0;
}

/**
 * ext4_snapshot_count_data - count blocks on an array of data blocks
 * and verify that snapshot blocks are excluded.
 *	@inode:	snapshot we are merging
 *	@first:	array of block numbers
 *	@last:	points immediately past the end of array
 *	@pblocks: pointer to counter of branch blocks
 *
 * We accumulate contiguous runs of blocks to test they are excluded.
 *
 * Return <0 on error or if blocks are not excluded.
 */
static int ext4_snapshot_count_data(struct inode *inode,
				    __le32 *first, __le32 *last,
				    int *pblocks)
{
	ext4_fsblk_t block = 0;		/* Starting block # of a run */
	unsigned long count = 0;	/* Number of blocks in the run */
	__le32 *block_p = NULL;		/* Pointer into inode/ind
					   corresponding to block */
	ext4_fsblk_t nr;		/* Current block # */
	__le32 *p;			/* Pointer into inode/ind
					   for current block */
	int err = 0;

	for (p = first; p < last; p++) {
		nr = le32_to_cpu(*p);
		if (nr) {
			/* accumulate blocks to test if they're contiguous */
			if (count == 0) {
				block = nr;
				block_p = p;
				count = 1;
			} else if (nr == block + count) {
				count++;
			} else {
				err = ext4_snapshot_count_blocks(inode,
						block, count, pblocks);
				if (err)
					return err;
				block = nr;
				block_p = p;
				count = 1;
			}
		}
	}

	if (count > 0)
		err = ext4_snapshot_count_blocks(inode, block, count, pblocks);
	return err;
}

/**
 * ext4_snapshot_count_branches - count blocks on an array of branches
 * and verify that snapshot blocks are excluded.
 *	@inode:	snapshot we are merging
 *	@first:	array of block numbers
 *	@last:	pointer immediately past the end of array
 *	@depth:	depth of the branches to free
 *	@pblocks: pointer to counter of branch blocks
 *
 * Return <0 on error or if blocks are not excluded.
 */
static int ext4_snapshot_count_branches(struct inode *inode,
			       __le32 *first, __le32 *last, int depth,
			       int *pblocks)
{
	ext4_fsblk_t nr;
	__le32 *p;
	int err = 0;

	if (depth--) {
		struct buffer_head *bh;
		int addr_per_block = EXT4_ADDR_PER_BLOCK(inode->i_sb);
		p = last;
		while (--p >= first) {
			nr = le32_to_cpu(*p);
			if (!nr)
				continue;		/* A hole */

			if (!ext4_data_block_valid(EXT4_SB(inode->i_sb),
						   nr, 1)) {
				EXT4_ERROR_INODE(inode,
						 "invalid indirect mapped "
						 "block %lu (level %d)",
						 (unsigned long) nr, depth);
				break;
			}

			/* Go read the buffer for the next level down */
			bh = sb_bread(inode->i_sb, nr);

			/*
			 * A read failure? Report error and clear slot
			 * (should be rare).
			 */
			if (!bh) {
				EXT4_ERROR_INODE_BLOCK(inode, nr,
						       "Read failure");
				continue;
			}

			/* This counts the entire branch.  Bottom up. */
			BUFFER_TRACE(bh, "count child branches");
			err = ext4_snapshot_count_branches(inode,
					(__le32 *)bh->b_data,
					(__le32 *)bh->b_data + addr_per_block,
					depth, pblocks);
			if (err)
				break;
			/* Count the parent block */
			err = ext4_snapshot_count_blocks(inode, nr, 1, pblocks);
			if (err)
				break;
		}
	} else {
		/* We have reached the bottom of the tree. */
		BUFFER_TRACE(parent_bh, "count data blocks");
		err = ext4_snapshot_count_data(inode, first, last, pblocks);
	}
	return err;
}

/*
 * ext4_move_branches - move an array of branches
 * @handle: JBD handle for this transaction
 * @src:	inode we're moving blocks from
 * @ps:		array of src block numbers
 * @pd:		array of dst block numbers
 * @depth:	depth of the branches to move
 * @count:	max branches to move
 * @pmoved:	pointer to counter of moved blocks
 *
 * We move whole branches from src to dst, skipping the holes in src
 * and stopping at the first branch that needs to be merged at higher level.
 * Called from ext4_snapshot_merge_blocks() under snapshot_mutex.
 * Returns the number of merged branches.
 * Return <0 on error or if blocks are not excluded.
 */
static int ext4_move_branches(handle_t *handle, struct inode *src,
		__le32 *ps, __le32 *pd, int depth,
		int count, int *pmoved)
{
	int i, err;

	for (i = 0; i < count; i++, ps++, pd++) {
		__le32 s = *ps, d = *pd;
		if (s && d && depth)
			/* can't move or skip entire branch, need to merge
			   these 2 branches */
			break;
		if (!s || d)
			/* skip holes is src and mapped data blocks in dst */
			continue;

		/* count moved blocks (and verify they are excluded) */
		err = ext4_snapshot_count_branches(src, ps, ps+1, depth,
						   pmoved);
		if (err)
			return err;

		/* move the entire branch from src to dst inode */
		*pd = s;
		*ps = 0;
	}
	return i;
}

/*
 * ext4_snapshot_merge_blocks - merge blocks from @src to @dst inode
 * @handle: JBD handle for this transaction
 * @src:	inode we're merging blocks from
 * @dst:	inode we're merging blocks to
 * @iblock:	inode offset to first data block to merge
 * @maxblocks:	inode range of data blocks to merge
 *
 * Merges @maxblocks data blocks starting at @iblock and all the indirect
 * blocks that map them.
 * Called from ext4_snapshot_merge() under snapshot_mutex.
 * Returns the merged blocks range and <0 on error.
 */
int ext4_snapshot_merge_blocks(handle_t *handle,
		struct inode *src, struct inode *dst,
		sector_t iblock, unsigned long maxblocks)
{
	Indirect S[4], D[4], *pS, *pD;
	int offsets[4];
	int ks, kd, depth, count;
	int ptrs = EXT4_ADDR_PER_BLOCK(src->i_sb);
	int ptrs_bits = EXT4_ADDR_PER_BLOCK_BITS(src->i_sb);
	int data_ptrs_bits, data_ptrs_mask, max_ptrs;
	int moved = 0, err;

	depth = ext4_block_to_path(src, iblock, offsets, NULL);
	if (depth < 3)
		/* snapshot blocks are mapped with double and tripple
		   indirect blocks */
		return -1;

	memset(D, 0, sizeof(D));
	memset(S, 0, sizeof(S));
	pD = ext4_get_branch(dst, depth, offsets, D, &err);
	kd = (pD ? pD - D : depth - 1);
	if (err)
		goto out;
	pS = ext4_get_branch(src, depth, offsets, S, &err);
	ks = (pS ? pS - S : depth - 1);
	if (err)
		goto out;

	if (ks < 1 || kd < 1) {
		/* snapshot double and tripple tree roots are pre-allocated */
		err = -EIO;
		goto out;
	}

	if (ks < kd) {
		/* nothing to move from src to dst */
		count = ext4_blks_to_skip(src, iblock, maxblocks,
					S, depth, offsets, ks);
		snapshot_debug(3, "skipping src snapshot (%u) holes: "
			       "block=0x%llx, count=0x%x\n", src->i_generation,
			       SNAPSHOT_BLOCK(iblock), count);
		err = count;
		goto out;
	}

	/* move branches from level kd in src to dst */
	pS = S+kd;
	pD = D+kd;

	/* compute max branches that can be moved */
	data_ptrs_bits = ptrs_bits * (depth - kd - 1);
	data_ptrs_mask = (1 << data_ptrs_bits) - 1;
	max_ptrs = (maxblocks >> data_ptrs_bits) + 1;
	if (max_ptrs > ptrs-offsets[kd])
		max_ptrs = ptrs-offsets[kd];

	/* get write access for the splice point */
	err = ext4_journal_get_write_access_inode(handle, src, pS->bh);
	if (err)
		goto out;
	err = ext4_journal_get_write_access_inode(handle, dst, pD->bh);
	if (err)
		goto out;

	/* move as many whole branches as possible */
	err = ext4_move_branches(handle, src, pS->p, pD->p, depth-1-kd,
			max_ptrs, &moved);
	if (err < 0)
		goto out;
	count = err;
	if (moved) {
		snapshot_debug(3, "moved snapshot (%u) -> snapshot (%d) "
			       "branches: block=0x%llx, count=0x%x, k=%d/%d, "
			       "moved_blocks=%d\n", src->i_generation,
			       dst->i_generation, SNAPSHOT_BLOCK(iblock),
			       count, kd, depth, moved);
		/* update src and dst inodes blocks usage */
		dquot_free_block(src, moved);
		dquot_alloc_block_nofail(dst, moved);
		err = ext4_handle_dirty_metadata(handle, NULL, pD->bh);
		if (err)
			goto out;
		err = ext4_handle_dirty_metadata(handle, NULL, pS->bh);
		if (err)
			goto out;
	}

	/* we merged at least 1 partial branch and optionally count-1 full
	   branches */
	err = (count << data_ptrs_bits) -
		(SNAPSHOT_BLOCK(iblock) & data_ptrs_mask);
out:
	/* count_branch_blocks may use the entire depth of S */
	for (ks = 1; ks < depth; ks++) {
		if (S[ks].bh)
			brelse(S[ks].bh);
		if (ks <= kd)
			brelse(D[ks].bh);
	}
	return err < maxblocks ? err : maxblocks;
}

/*
 * ext4_snapshot_get_block_access() - called from ext4_snapshot_read_through()
 * on snapshot file access.
 * return value <0 indicates access not granted
 * return value 0 indicates snapshot inode read through access
 * in which case 'prev_snapshot' is pointed to the previous snapshot
 * on the list or set to NULL to indicate read through to block device.
 */
/*
 * In-memory snapshot list manipulation is protected by snapshot_mutex.
 * In this function we read the in-memory snapshot list without holding
 * snapshot_mutex, because we don't want to slow down snapshot read performance.
 * Following is a proof, that even though we don't hold snapshot_mutex here,
 * reading the list is safe from races with snapshot list delete and add (take).
 *
 * Proof of no race with snapshot delete:
 * --------------------------------------
 * We get here only when reading from an enabled snapshot or when reading
 * through from an enabled snapshot to a newer snapshot.  Snapshot delete
 * operation is only allowed for a disabled snapshot, when no older enabled
 * snapshot exists (i.e., the deleted snapshot in not 'in-use').  Hence,
 * read through is safe from races with snapshot list delete operations.
 *
 * Proof of no race with snapshot take:
 * ------------------------------------
 * Snapshot B take is composed of the following steps:
 * ext4_snapshot_create():
 * - Add snapshot B to head of list (active_snapshot is A).
 * - Allocate and copy snapshot B initial blocks.
 * ext4_snapshot_take():
 * - Freeze FS
 * - Clear snapshot A 'active' flag.
 * - Set snapshot B 'list'+'active' flags.
 * - Set snapshot B as active snapshot (active_snapshot=B).
 * - Unfreeze FS
 *
 * Note that we do not need to rely on correct order of instructions within
 * each of the functions above, but we can assume that Freeze FS will provide
 * a strong barrier between adding B to list and the ops inside snapshot_take.
 *
 * When reading from snapshot A during snapshot B take, we have 2 cases:
 * 1. is_active(A) is tested before setting active_snapshot=B -
 *    read through from A to block device.
 * 2. is_active(A) is tested after setting active_snapshot=B -
 *    read through from A to B.
 *
 * When reading from snapshot B during snapshot B take, we have 2 cases:
 * 1. B->flags and B->prev are read before adding B to list
 *    AND/OR before setting the 'list'+'active' flags -
 *    access to B denied.
 * 2. is_active(B) is tested after setting active_snapshot=B
 *    AND/OR after setting the 'list'+'active' flags -
 *    read through from B to block device.
 */
static int ext4_snapshot_get_block_access(struct inode *inode,
		struct inode **prev_snapshot)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	unsigned long flags = ext4_get_snapstate_flags(inode);
	struct list_head *prev = ei->i_snaplist.prev;

	if (!(flags & 1UL<<EXT4_SNAPSTATE_LIST))
		/* snapshot not on the list - read/write access denied */
		return -EPERM;

	*prev_snapshot = NULL;
	if (ext4_snapshot_is_active(inode) ||
			(flags & 1UL<<EXT4_SNAPSTATE_ACTIVE))
		/* read through from active snapshot to block device */
		return 0;

	if (prev == &ei->i_snaplist)
		/* not on snapshots list? */
		return -EIO;

	if (prev == &EXT4_SB(inode->i_sb)->s_snapshot_list)
		/* active snapshot not found on list? */
		return -EIO;

	/* read through to prev snapshot on the list */
	ei = list_entry(prev, struct ext4_inode_info, i_snaplist);
	*prev_snapshot = &ei->vfs_inode;

	if (!ext4_snapshot_file(*prev_snapshot))
		/* non snapshot file on the list? */
		return -EIO;

	return 0;
}

#ifdef CONFIG_EXT4_DEBUG
/*
 * ext4_snapshot_get_blockdev_access - get read through access to block device.
 * Sanity test to verify that the read block is allocated and not excluded.
 * This test has performance penalty and is only called if SNAPTEST_READ
 * is enabled.  An attempt to read through to block device of a non allocated
 * or excluded block may indicate a corrupted filesystem, corrupted snapshot
 * or corrupted exclude bitmap.  However, it may also be a read-ahead, which
 * was not implicitly requested by the user, so be sure to disable read-ahead
 * on block device (blockdev --setra 0 <bdev>) before enabling SNAPTEST_READ.
 *
 * Return values:
 * = 0 - block is allocated and not excluded
 * < 0 - error (or block is not allocated or excluded)
 */
static int ext4_snapshot_get_blockdev_access(struct super_block *sb,
				   struct buffer_head *bh)
{
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(bh->b_blocknr);
	ext4_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr);
	struct buffer_head *bitmap_bh;
	struct buffer_head *exclude_bitmap_bh = NULL;
	int err = 0;

	if (PageReadahead(bh->b_page))
		return 0;

	bitmap_bh = ext4_read_block_bitmap(sb, block_group);
	if (!bitmap_bh)
		return -EIO;

	if (!ext4_test_bit(bit, bitmap_bh->b_data)) {
		snapshot_debug(2, "warning: attempt to read through to "
				"non-allocated block [%d/%lu] - read ahead?\n",
				bit, block_group);
		brelse(bitmap_bh);
		return -EIO;
	}

	exclude_bitmap_bh = ext4_read_exclude_bitmap(sb, block_group);
	if (exclude_bitmap_bh &&
		ext4_test_bit(bit, exclude_bitmap_bh->b_data)) {
		snapshot_debug(2, "warning: attempt to read through to "
				"excluded block [%d/%lu] - read ahead?\n",
				bit, block_group);
		err = -EIO;
	}

	brelse(exclude_bitmap_bh);
	brelse(bitmap_bh);
	return err;
}
#endif

/*
 * ext4_snapshot_read_through - get snapshot image block.
 * On read of snapshot file, an unmapped block is a peephole to prev snapshot.
 * On read of active snapshot, an unmapped block is a peephole to the block
 * device.  On first block write, the peephole is filled forever.
 */
static int ext4_snapshot_read_through(struct inode *inode, sector_t iblock,
				      struct buffer_head *bh_result)
{
	int err;
	struct ext4_map_blocks map;
	struct inode *prev_snapshot;
	struct buffer_head *sbh = NULL;

	map.m_lblk = iblock;
	map.m_pblk = 0;
	map.m_len = bh_result->b_size >> inode->i_blkbits;

get_block:
	prev_snapshot = NULL;
	/* request snapshot file read access */
	err = ext4_snapshot_get_block_access(inode, &prev_snapshot);
	if (err < 0)
		return err;
	if (!prev_snapshot) {
		/*
		 * Possible read through to block device.
		 * Start tracked read before checking if block is mapped to
		 * avoid race condition with COW that maps the block after
		 * we checked if the block is mapped.  If we find that the
		 * block is mapped, we will cancel the tracked read before
		 * returning from this function.
		 */
		map_bh(bh_result, inode->i_sb, SNAPSHOT_BLOCK(iblock));
		err = start_buffer_tracked_read(bh_result);
		if (err < 0) {
			snapshot_debug(1,
					"snapshot (%u) failed to start "
					"tracked read on block (%lld) "
					"(err=%d)\n", inode->i_generation,
					(long long)bh_result->b_blocknr, err);
			return err;
		}
	}
	err = ext4_map_blocks(NULL, inode, &map, 0);
	snapshot_debug(4, "ext4_snapshot_read_through(%lld): block = "
		       "(%lld), err = %d\n prev_snapshot = %u",
		       (long long)iblock, map.m_pblk, err,
		       prev_snapshot ? prev_snapshot->i_generation : 0);
	/* if it's not a hole - cancel tracked read before we deadlock
	 * on pending COW */
	if (err && buffer_tracked_read(bh_result))
		cancel_buffer_tracked_read(bh_result);
	if (err < 0)
		return err;
	if (!err && prev_snapshot) {
		/* hole in snapshot - check again with prev snapshot */
		inode = prev_snapshot;
		goto get_block;
	}
	if (!err)
		/* hole in active snapshot - read though to block device */
		return 0;

	map_bh(bh_result, inode->i_sb, map.m_pblk);
	bh_result->b_state = (bh_result->b_state & ~EXT4_MAP_FLAGS) |
		map.m_flags;

	/*
	 * On read of active snapshot, a mapped block may belong to a non
	 * completed COW operation.  Use the buffer cache to test this
	 * condition.  if (bh_result->b_blocknr == SNAPSHOT_BLOCK(iblock)),
	 * then this is either read through to block device or moved block.
	 * Either way, it is not a COWed block, so it cannot be pending COW.
	 */
	if (ext4_snapshot_is_active(inode) &&
	    bh_result->b_blocknr != SNAPSHOT_BLOCK(iblock))
		sbh = sb_find_get_block(inode->i_sb, bh_result->b_blocknr);
	if (!sbh)
		return 0;
	/* wait for pending COW to complete */
	ext4_snapshot_test_pending_cow(sbh, SNAPSHOT_BLOCK(iblock));
	lock_buffer(sbh);
	if (buffer_uptodate(sbh)) {
		/*
		 * Avoid disk I/O and copy out snapshot page directly
		 * from block device page when possible.
		 */
		BUG_ON(!sbh->b_page);
		BUG_ON(!bh_result->b_page);
		lock_buffer(bh_result);
		copy_highpage(bh_result->b_page, sbh->b_page);
		set_buffer_uptodate(bh_result);
		unlock_buffer(bh_result);
	} else if (buffer_dirty(sbh)) {
		/*
		 * If snapshot data buffer is dirty (just been COWed),
		 * then it is not safe to read it from disk yet.
		 * We shouldn't get here because snapshot data buffer
		 * only becomes dirty during COW and because we waited
		 * for pending COW to complete, which means that a
		 * dirty snapshot data buffer should be uptodate.
		 */
		WARN_ON(1);
	}
	unlock_buffer(sbh);
	brelse(sbh);
	return 0;
}

/*
 * Check if @block is a bitmap block of @group.
 * if @block is found to be a block/inode/exclude bitmap block, the return
 * value is one of the non-zero values below:
 */
#define BLOCK_BITMAP	1
#define INODE_BITMAP	2
#define EXCLUDE_BITMAP	3

static int ext4_snapshot_is_group_bitmap(struct super_block *sb,
		ext4_fsblk_t block, ext4_group_t group)
{
	struct ext4_group_desc *gdp;
	struct ext4_group_info *grp;
	ext4_fsblk_t bitmap_blk;

	gdp = ext4_get_group_desc(sb, group, NULL);
	grp = ext4_get_group_info(sb, group);
	if (!gdp || !grp)
		return 0;

	bitmap_blk = ext4_block_bitmap(sb, gdp);
	if (bitmap_blk == block)
		return BLOCK_BITMAP;
	bitmap_blk = ext4_inode_bitmap(sb, gdp);
	if (bitmap_blk == block)
		return INODE_BITMAP;
	bitmap_blk = ext4_exclude_bitmap(sb, gdp);
	if (bitmap_blk == block)
		return EXCLUDE_BITMAP;
	return 0;
}

/*
 * Check if @block is a bitmap block and of any block group.
 * if @block is found to be a bitmap block, @bitmap_group is set to the
 * block group described by the bitmap block.
 */
static int ext4_snapshot_is_bitmap(struct super_block *sb,
		ext4_fsblk_t block, ext4_group_t *bitmap_group)
{
	ext4_group_t group = SNAPSHOT_BLOCK_GROUP(block);
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	int flex_groups = ext4_flex_bg_size(EXT4_SB(sb));
	int i, is_bitmap = 0;

	/*
	 * When block is in the first block group of a flex group, we need to
	 * check all group desc of the flex group.
	 * The exclude bitmap can potentially be allocated from any group, if
	 * exclude inode was added not on mkfs.  The worst case if we fail to
	 * identify a block as an exclude bitmap is that fsck sanity check of
	 * snapshots will fail, becasue exclude bitmap inside snapshot will
	 * not be all zeros.
	 */
	if (!EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FLEX_BG) ||
			(group % flex_groups) != 0)
		flex_groups = 1;

	for (i = 0; i < flex_groups && group < ngroups; i++, group++) {
		is_bitmap = ext4_snapshot_is_group_bitmap(sb, block, group);
		if (is_bitmap)
			break;
	}
	*bitmap_group = group;
	return is_bitmap;
}

static int ext4_snapshot_get_block(struct inode *inode, sector_t iblock,
			struct buffer_head *bh_result, int create)
{
	ext4_group_t block_group;
	int err, is_bitmap;

	BUG_ON(create != 0);
	BUG_ON(buffer_tracked_read(bh_result));

	err = ext4_snapshot_read_through(inode, SNAPSHOT_IBLOCK(iblock),
					 bh_result);
	snapshot_debug(4, "ext4_snapshot_get_block(%lld): block = (%lld), "
		       "err = %d\n",
		       (long long)iblock, buffer_mapped(bh_result) ?
		       (long long)bh_result->b_blocknr : 0, err);
	if (err < 0)
		return err;

	if (!buffer_tracked_read(bh_result))
		return 0;

	/* Check for read through to block or exclude bitmap block */
	is_bitmap = ext4_snapshot_is_bitmap(inode->i_sb, bh_result->b_blocknr,
			&block_group);
	if (is_bitmap == BLOCK_BITMAP) {
		/* copy fixed block bitmap directly to page buffer */
		cancel_buffer_tracked_read(bh_result);
		/* cancel_buffer_tracked_read() clears mapped flag */
		set_buffer_mapped(bh_result);
		snapshot_debug(2, "fixing snapshot block bitmap #%u\n",
			       block_group);
		/*
		 * XXX: if we return unmapped buffer, the page will be zeroed
		 * but if we return mapped to block device and uptodate buffer
		 * next readpage may read directly from block device without
		 * fixing block bitmap.  This only affects fsck of snapshots.
		 */
		return ext4_snapshot_read_block_bitmap(inode->i_sb,
						       block_group, bh_result);
	} else if (is_bitmap == EXCLUDE_BITMAP) {
		/* return unmapped buffer to zero out page */
		cancel_buffer_tracked_read(bh_result);
		/* cancel_buffer_tracked_read() clears mapped flag */
		snapshot_debug(2, "zeroing snapshot exclude bitmap #%u\n",
			       block_group);
		return 0;
	}

#ifdef CONFIG_EXT4_DEBUG
	snapshot_debug(3, "started tracked read: block = [%llu/%llu]\n",
		       SNAPSHOT_BLOCK_TUPLE(bh_result->b_blocknr));
	if (snapshot_enable_test[SNAPTEST_READ]) {
		err = ext4_snapshot_get_blockdev_access(inode->i_sb,
						    bh_result);
		if (err) {
			/* read through access denied */
			cancel_buffer_tracked_read(bh_result);
			return err;
		}
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_READ);
	}
#endif
	return 0;
}

int ext4_snapshot_readpage(struct file *file, struct page *page)
{
	/* do read I/O with buffer heads to enable tracked reads */
	return ext4_read_full_page(page, ext4_snapshot_get_block);
}
