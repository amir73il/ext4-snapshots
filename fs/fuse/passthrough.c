// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE passthrough to backing file.
 *
 * Copyright (c) 2023 CTERA Networks.
 */

#include "fuse_i.h"

#include <linux/file.h>
#include <linux/backing-file.h>
#include <linux/splice.h>

static void fuse_file_accessed(struct file *file)
{
	struct inode *inode = file_inode(file);

	fuse_invalidate_atime(inode);
}

static void fuse_passthrough_end_write(struct kiocb *iocb, ssize_t ret)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	fuse_write_update_attr(inode, iocb->ki_pos, ret);
}

ssize_t fuse_passthrough_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;
	struct file *backing_file = fuse_file_passthrough(ff);
	size_t count = iov_iter_count(iter);
	ssize_t ret;
	struct backing_file_ctx ctx = {
		.cred = ff->cred,
		.accessed = fuse_file_accessed,
	};


	pr_debug("%s: backing_file=0x%p, pos=%lld, len=%zu\n", __func__,
		 backing_file, iocb->ki_pos, count);

	if (!count)
		return 0;

	ret = backing_file_read_iter(backing_file, iter, iocb, iocb->ki_flags,
				     &ctx);

	return ret;
}

ssize_t fuse_passthrough_write_iter(struct kiocb *iocb,
				    struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct fuse_file *ff = file->private_data;
	struct file *backing_file = fuse_file_passthrough(ff);
	size_t count = iov_iter_count(iter);
	ssize_t ret;
	struct backing_file_ctx ctx = {
		.cred = ff->cred,
		.end_write = fuse_passthrough_end_write,
	};

	pr_debug("%s: backing_file=0x%p, pos=%lld, len=%zu\n", __func__,
		 backing_file, iocb->ki_pos, count);

	if (!count)
		return 0;

	inode_lock(inode);
	ret = backing_file_write_iter(backing_file, iter, iocb, iocb->ki_flags,
				      &ctx);
	inode_unlock(inode);

	return ret;
}

ssize_t fuse_passthrough_splice_read(struct file *in, loff_t *ppos,
				     struct pipe_inode_info *pipe,
				     size_t len, unsigned int flags)
{
	struct fuse_file *ff = in->private_data;
	struct file *backing_file = fuse_file_passthrough(ff);
	struct backing_file_ctx ctx = {
		.cred = ff->cred,
		.accessed = fuse_file_accessed,
	};
	struct kiocb iocb;
	ssize_t ret;

	pr_debug("%s: backing_file=0x%p, pos=%lld, len=%zu, flags=0x%x\n", __func__,
		 backing_file, *ppos, len, flags);

	init_sync_kiocb(&iocb, in);
	iocb.ki_pos = *ppos;
	ret = backing_file_splice_read(backing_file, &iocb, pipe, len, flags, &ctx);
	*ppos = iocb.ki_pos;

	return ret;
}

ssize_t fuse_passthrough_splice_write(struct pipe_inode_info *pipe,
				      struct file *out, loff_t *ppos,
				      size_t len, unsigned int flags)
{
	struct fuse_file *ff = out->private_data;
	struct file *backing_file = fuse_file_passthrough(ff);
	struct inode *inode = file_inode(out);
	ssize_t ret;
	struct backing_file_ctx ctx = {
		.cred = ff->cred,
		.end_write = fuse_passthrough_end_write,
	};
	struct kiocb iocb;

	pr_debug("%s: backing_file=0x%p, pos=%lld, len=%zu, flags=0x%x\n", __func__,
		 backing_file, *ppos, len, flags);

	inode_lock(inode);
	init_sync_kiocb(&iocb, out);
	iocb.ki_pos = *ppos;
	ret = backing_file_splice_write(pipe, backing_file, &iocb, len, flags, &ctx);
	*ppos = iocb.ki_pos;
	inode_unlock(inode);

	return ret;
}

ssize_t fuse_passthrough_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct fuse_file *ff = file->private_data;
	struct file *backing_file = fuse_file_passthrough(ff);
	struct backing_file_ctx ctx = {
		.cred = ff->cred,
		.accessed = fuse_file_accessed,
	};

	pr_debug("%s: backing_file=0x%p, start=%lu, end=%lu\n", __func__,
		 backing_file, vma->vm_start, vma->vm_end);

	return backing_file_mmap(backing_file, vma, &ctx);
}

struct fuse_backing *fuse_backing_get(struct fuse_backing *fb)
{
	if (fb && refcount_inc_not_zero(&fb->count))
		return fb;
	return NULL;
}

static void fuse_backing_free(struct fuse_backing *fb)
{
	pr_debug("%s: fb=0x%p\n", __func__, fb);

	if (fb->file)
		fput(fb->file);
	put_cred(fb->cred);
	kfree_rcu(fb, rcu);
}

void fuse_backing_put(struct fuse_backing *fb)
{
	if (fb && refcount_dec_and_test(&fb->count))
		fuse_backing_free(fb);
}

void fuse_backing_files_init(struct fuse_conn *fc)
{
	idr_init(&fc->backing_files_map);
}

static int fuse_backing_id_alloc(struct fuse_conn *fc, struct fuse_backing *fb)
{
	int id;

	idr_preload(GFP_KERNEL);
	spin_lock(&fc->lock);
	/* FIXME: xarray might be space inefficient */
	id = idr_alloc_cyclic(&fc->backing_files_map, fb, 1, 0, GFP_ATOMIC);
	spin_unlock(&fc->lock);
	idr_preload_end();

	WARN_ON_ONCE(id == 0);
	return id;
}

static struct fuse_backing *fuse_backing_id_remove(struct fuse_conn *fc,
						   int id)
{
	struct fuse_backing *fb;

	spin_lock(&fc->lock);
	fb = idr_remove(&fc->backing_files_map, id);
	spin_unlock(&fc->lock);

	return fb;
}

static int fuse_backing_id_free(int id, void *p, void *data)
{
	struct fuse_backing *fb = p;

	WARN_ON_ONCE(refcount_read(&fb->count) != 1);
	fuse_backing_free(fb);
	return 0;
}

void fuse_backing_files_free(struct fuse_conn *fc)
{
	idr_for_each(&fc->backing_files_map, fuse_backing_id_free, NULL);
	idr_destroy(&fc->backing_files_map);
}

int fuse_backing_open(struct fuse_conn *fc, struct fuse_backing_map *map)
{
	struct file *file;
	struct super_block *backing_sb;
	struct fuse_backing *fb = NULL;
	int res;

	pr_debug("%s: fd=%d flags=0x%x ops_mask=0x%llx\n", __func__,
		 map->fd, map->flags, map->ops_mask);

	/* TODO: relax CAP_SYS_ADMIN once backing files are visible to lsof */
	res = -EPERM;
	if (!fc->passthrough || !capable(CAP_SYS_ADMIN))
		goto out;

	res = -EINVAL;
	if (map->flags || map->ops_mask & ~FUSE_BACKING_MAP_VALID_OPS)
		goto out;

	/* For now passthough inode operations requires FUSE_PASSTHROUGH_INO */
	if (!fc->passthrough_ino && map->ops_mask & FUSE_PASSTHROUGH_INODE_OPS)
		goto out;

	file = fget_raw(map->fd);
	res = -EBADF;
	if (!file)
		goto out;

	res = -EOPNOTSUPP;
	/*
	 * It is not a problem to use an O_PATH fd as a backing file, because
	 * fuse_passthrough_open() will anyway open a new backing file per fuse
	 * file from the backing file path, but if server explicitly declares
	 * using ops_mask that this fd is expected to be used for read/write
	 * check for sanity that the file implements the read/write iter ops.
	 */
	if ((FUSE_BACKING_MAP_OP(map, FUSE_READ) && !file->f_op->read_iter) ||
	    (FUSE_BACKING_MAP_OP(map, FUSE_WRITE) && !file->f_op->write_iter))
		goto out_fput;

	/* FUSE_STATX passthrough implies FUSE_GETATTR passthrough */
	if (FUSE_BACKING_MAP_OP(map, FUSE_STATX))
		map->ops_mask |= FUSE_PASSTHROUGH_OP_GETATTR;

	backing_sb = file_inode(file)->i_sb;
	res = -ELOOP;
	if (backing_sb->s_stack_depth >= fc->max_stack_depth)
		goto out_fput;

	fb = kmalloc(sizeof(struct fuse_backing), GFP_KERNEL);
	res = -ENOMEM;
	if (!fb)
		goto out_fput;

	fb->file = file;
	fb->cred = prepare_creds();
	fb->ops_mask = map->ops_mask;
	refcount_set(&fb->count, 1);

	res = fuse_backing_id_alloc(fc, fb);
	if (res < 0) {
		fuse_backing_free(fb);
		fb = NULL;
	}

out:
	pr_debug("%s: fb=0x%p, ret=%i\n", __func__, fb, res);

	return res;

out_fput:
	fput(file);
	goto out;
}

int fuse_backing_close(struct fuse_conn *fc, int backing_id)
{
	struct fuse_backing *fb = NULL;
	int err;

	pr_debug("%s: backing_id=%d\n", __func__, backing_id);

	/* TODO: relax CAP_SYS_ADMIN once backing files are visible to lsof */
	err = -EPERM;
	if (!fc->passthrough || !capable(CAP_SYS_ADMIN))
		goto out;

	err = -EINVAL;
	if (backing_id <= 0)
		goto out;

	err = -ENOENT;
	fb = fuse_backing_id_remove(fc, backing_id);
	if (!fb)
		goto out;

	fuse_backing_put(fb);
	err = 0;
out:
	pr_debug("%s: fb=0x%p, err=%i\n", __func__, fb, err);

	return err;
}

/*
 * Setup passthrough to a backing file.
 *
 * Returns an fb object with elevated refcount to be stored in fuse inode.
 */
struct fuse_backing *fuse_passthrough_open(struct file *file,
					   struct inode *inode,
					   int backing_id)
{
	struct fuse_file *ff = file->private_data;
	struct fuse_conn *fc = ff->fm->fc;
	struct fuse_backing *fb = NULL;
	struct file *backing_file;
	int err;

	err = -EINVAL;
	if (backing_id <= 0)
		goto out;

	rcu_read_lock();
	fb = idr_find(&fc->backing_files_map, backing_id);
	fb = fuse_backing_get(fb);
	rcu_read_unlock();

	err = -ENOENT;
	if (!fb)
		goto out;

	/* Allocate backing file per fuse file to store fuse path */
	backing_file = backing_file_open(&file->f_path, file->f_flags,
					 &fb->file->f_path, fb->cred);
	err = PTR_ERR(backing_file);
	if (IS_ERR(backing_file)) {
		fuse_backing_put(fb);
		goto out;
	}

	err = 0;
	ff->passthrough = backing_file;
	ff->cred = get_cred(fb->cred);
out:
	pr_debug("%s: backing_id=%d, fb=0x%p, backing_file=0x%p, err=%i\n", __func__,
		 backing_id, fb, ff->passthrough, err);

	return err ? ERR_PTR(err) : fb;
}

void fuse_passthrough_release(struct fuse_file *ff, struct fuse_backing *fb)
{
	pr_debug("%s: fb=0x%p, backing_file=0x%p\n", __func__,
		 fb, ff->passthrough);

	fput(ff->passthrough);
	ff->passthrough = NULL;
	put_cred(ff->cred);
	ff->cred = NULL;
}

/*
 * Inode passthrough operations for backing file attached on lookup.
 */

int fuse_passthrough_getattr(struct mnt_idmap *idmap, struct inode *inode,
			     struct kstat *stat, u32 request_mask,
			     unsigned int flags)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_backing *fb = fuse_inode_passthrough(fi);
	u64 attr_version = fuse_get_attr_version(fc);
	struct path *fb_path = &fb->file->f_path;
	const struct cred *old_cred;
	struct kstat backing_stat;
	struct fuse_attr attr;
	struct fuse_statx sx;
	int err;

	if (!stat)
		stat = &backing_stat;

	old_cred = override_creds(fb->cred);
	err = vfs_getattr(fb_path, stat, request_mask, flags);
	revert_creds(old_cred);
	if (err)
		return err;

	/* Always override st_dev with FUSE dev */
	stat->dev = inode->i_sb->s_dev;
	/* Fill fuse inode attrs from backing inode stat */
	fuse_kstat_to_attr(idmap, inode, stat, &attr, &sx);
	fuse_change_attributes(inode, &attr, &sx, 0, attr_version);

	return err;
}

ssize_t fuse_passthrough_getxattr(struct inode *inode, const char *name,
				  void *value, size_t size)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_backing *fb = fuse_inode_passthrough(fi);
	struct path *fb_path = &fb->file->f_path;
	const struct cred *old_cred;
	ssize_t res;

	old_cred = override_creds(fb->cred);
	res = vfs_getxattr(mnt_idmap(fb_path->mnt), fb_path->dentry, name,
			   value, size);
	revert_creds(old_cred);
	return res;
}

ssize_t fuse_passthrough_listxattr(struct dentry *entry, char *list,
				   size_t size)
{
	struct fuse_inode *fi = get_fuse_inode(d_inode(entry));
	struct fuse_backing *fb = fuse_inode_passthrough(fi);
	struct path *fb_path = &fb->file->f_path;
	const struct cred *old_cred;
	ssize_t res;

	old_cred = override_creds(fb->cred);
	res = vfs_listxattr(fb_path->dentry, list, size);
	revert_creds(old_cred);
	return res;
}
