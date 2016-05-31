/*
 * linux/fs/next3/xattr_security.c
 * Handler for storing security labels as extended attributes.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include "next3_jbd.h"
#include "next3.h"
#include <linux/security.h>
#include "xattr.h"

static size_t
next3_xattr_security_list(struct dentry *dentry, char *list, size_t list_size,
			 const char *name, size_t name_len, int type)
{
	const size_t prefix_len = XATTR_SECURITY_PREFIX_LEN;
	const size_t total_len = prefix_len + name_len + 1;


	if (list && total_len <= list_size) {
		memcpy(list, XATTR_SECURITY_PREFIX, prefix_len);
		memcpy(list+prefix_len, name, name_len);
		list[prefix_len + name_len] = '\0';
	}
	return total_len;
}

static int
next3_xattr_security_get(struct dentry *dentry, const char *name,
		void *buffer, size_t size, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	return next3_xattr_get(dentry->d_inode, NEXT3_XATTR_INDEX_SECURITY,
			      name, buffer, size);
}

static int
next3_xattr_security_set(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	return next3_xattr_set(dentry->d_inode, NEXT3_XATTR_INDEX_SECURITY,
			      name, value, size, flags);
}

int next3_initxattrs(struct inode *inode, const struct xattr *xattr_array,
		    void *fs_info)
{
	const struct xattr *xattr;
	handle_t *handle = fs_info;
	int err = 0;

	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		err = next3_xattr_set_handle(handle, inode,
					    NEXT3_XATTR_INDEX_SECURITY,
					    xattr->name, xattr->value,
					    xattr->value_len, 0);
		if (err < 0)
			break;
	}
	return err;
}

int
next3_init_security(handle_t *handle, struct inode *inode, struct inode *dir,
		   const struct qstr *qstr)
{
	return security_inode_init_security(inode, dir, qstr,
					    &next3_initxattrs, handle);
}

const struct xattr_handler next3_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.list	= next3_xattr_security_list,
	.get	= next3_xattr_security_get,
	.set	= next3_xattr_security_set,
};
