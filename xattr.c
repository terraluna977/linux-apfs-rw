// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/xattr.h>
#include <linux/blk_types.h>
#include "apfs.h"

/**
 * apfs_xattr_from_query - Read the xattr record found by a successful query
 * @query:	the query that found the record
 * @xattr:	Return parameter.  The xattr record found.
 *
 * Reads the xattr record into @xattr and performs some basic sanity checks
 * as a protection against crafted filesystems.  Returns 0 on success or
 * -EFSCORRUPTED otherwise.
 *
 * The caller must not free @query while @xattr is in use, because @xattr->name
 * and @xattr->xdata point to data on disk.
 */
static int apfs_xattr_from_query(struct apfs_query *query,
				 struct apfs_xattr *xattr)
{
	struct apfs_xattr_val *xattr_val;
	struct apfs_xattr_key *xattr_key;
	char *raw = query->node->object.data;
	int datalen = query->len - sizeof(*xattr_val);
	int namelen = query->key_len - sizeof(*xattr_key);

	if (namelen < 1 || datalen < 0)
		return -EFSCORRUPTED;

	xattr_val = (struct apfs_xattr_val *)(raw + query->off);
	xattr_key = (struct apfs_xattr_key *)(raw + query->key_off);

	if (namelen != le16_to_cpu(xattr_key->name_len))
		return -EFSCORRUPTED;

	/* The xattr name must be NULL-terminated */
	if (xattr_key->name[namelen - 1] != 0)
		return -EFSCORRUPTED;

	xattr->has_dstream = le16_to_cpu(xattr_val->flags) &
			     APFS_XATTR_DATA_STREAM;

	if (xattr->has_dstream && datalen != sizeof(struct apfs_xattr_dstream))
		return -EFSCORRUPTED;
	if (!xattr->has_dstream && datalen != le16_to_cpu(xattr_val->xdata_len))
		return -EFSCORRUPTED;

	xattr->name = xattr_key->name;
	xattr->name_len = namelen - 1; /* Don't count the NULL termination */
	xattr->xdata = xattr_val->xdata;
	xattr->xdata_len = datalen;
	return 0;
}

/**
 * apfs_dstream_from_xattr - Get the dstream info for a dstream xattr
 * @sb:		filesystem superblock
 * @xattr:	in-memory xattr record (already sanity-checked)
 * @dstream:	on return, the data stream info
 */
static void apfs_dstream_from_xattr(struct super_block *sb, struct apfs_xattr *xattr, struct apfs_dstream_info *dstream)
{
	struct apfs_xattr_dstream *xdata = (void *)xattr->xdata;

	dstream->ds_sb = sb;
	dstream->ds_id = le64_to_cpu(xdata->xattr_obj_id);
	dstream->ds_size = le64_to_cpu(xdata->dstream.size);
	dstream->ds_sparse_bytes = 0; /* Irrelevant for xattrs */

	dstream->ds_cached_ext.len = 0;
	dstream->ds_ext_dirty = false;
	spin_lock_init(&dstream->ds_ext_lock);
}

/**
 * apfs_xattr_extents_read - Read the value of a xattr from its extents
 * @parent:	inode the attribute belongs to
 * @xattr:	the xattr structure
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 * @only_whole:	are partial reads banned?
 *
 * Copies the value of @xattr to @buffer, if provided. If @buffer is NULL, just
 * computes the size of the buffer required.
 *
 * Returns the number of bytes used/required, or a negative error code in case
 * of failure.
 */
static int apfs_xattr_extents_read(struct inode *parent,
				   struct apfs_xattr *xattr,
				   void *buffer, size_t size, bool only_whole)
{
	struct super_block *sb = parent->i_sb;
	struct apfs_dstream_info *dstream;
	int length, blkcnt, i;
	struct buffer_head **bhs = NULL;
	int ret;

	dstream = kzalloc(sizeof(*dstream), GFP_KERNEL);
	if (!dstream)
		return -ENOMEM;
	apfs_dstream_from_xattr(sb, xattr, dstream);

	length = dstream->ds_size;
	if (length < 0 || length < dstream->ds_size) {
		/* TODO: avoid overflow here for huge compressed files */
		return -E2BIG;
	}

	if (!buffer) {
		/* All we want is the length */
		ret = length;
		goto out;
	}

	if (only_whole) {
		if (length > size) {
			/* xattr won't fit in the buffer */
			ret = -ERANGE;
			goto out;
		}
	} else {
		if (length > size)
			length = size;
	}

	blkcnt = (length + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	bhs = kcalloc(blkcnt, sizeof(*bhs), GFP_KERNEL);
	for (i = 0; i < blkcnt; i++) {
		struct buffer_head *bh = NULL;
		u64 bno = 0;

		ret = apfs_logic_to_phys_bno(dstream, i, &bno);
		if (ret)
			goto out;
		if (bno == 0) {
			/* No holes in xattr dstreams, I believe */
			ret = -EFSCORRUPTED;
			goto out;
		}

		bhs[i] = __getblk_gfp(APFS_NXI(sb)->nx_bdev, bno, sb->s_blocksize, __GFP_MOVABLE);
		if (!bhs[i]) {
			ret = -EIO;
			goto out;
		}

		bh = bhs[i];
		if (!buffer_uptodate(bh)) {
			get_bh(bh);
			lock_buffer(bh);
			bh->b_end_io = end_buffer_read_sync;
			apfs_submit_bh(REQ_OP_READ, 0, bh);
		}
	}
	for (i = 0; i < blkcnt; i++) {
		int off, tocopy;

		wait_on_buffer(bhs[i]);
		if (!buffer_uptodate(bhs[i])) {
			ret = -EIO;
			goto out;
		}

		off = i << sb->s_blocksize_bits;
		tocopy = min(sb->s_blocksize, (unsigned long)(length - off));
		memcpy(buffer + off, bhs[i]->b_data, tocopy);
	}
	ret = length;

out:
	if (bhs) {
		for (i = 0; i < blkcnt; i++)
			brelse(bhs[i]);
		kfree(bhs);
	}
	kfree(dstream);
	return ret;
}

/**
 * apfs_xattr_inline_read - Read the value of an inline xattr
 * @xattr:	the xattr structure
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 * @only_whole:	are partial reads banned?
 *
 * Copies the inline value of @xattr to @buffer, if provided. If @buffer is
 * NULL, just computes the size of the buffer required.
 *
 * Returns the number of bytes used/required, or a negative error code in case
 * of failure.
 */
static int apfs_xattr_inline_read(struct apfs_xattr *xattr, void *buffer, size_t size, bool only_whole)
{
	int length = xattr->xdata_len;

	if (!buffer) /* All we want is the length */
		return length;
	if (only_whole) {
		if (length > size) /* xattr won't fit in the buffer */
			return -ERANGE;
	} else {
		if (length > size)
			length = size;
	}
	memcpy(buffer, xattr->xdata, length);
	return length;
}

/**
 * __apfs_xattr_get - Find and read a named attribute
 * @inode:	inode the attribute belongs to
 * @name:	name of the attribute
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 *
 * This does the same as apfs_xattr_get(), but without taking any locks.
 */
int __apfs_xattr_get(struct inode *inode, const char *name, void *buffer,
		     size_t size)
{
	return ____apfs_xattr_get(inode, name, buffer, size, true /* only_whole */);
}

/**
 * ____apfs_xattr_get - Find and read a named attribute, optionally header only
 * @inode:	inode the attribute belongs to
 * @name:	name of the attribute
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 * @only_whole:	must read complete (no partial header read allowed)
 */
int ____apfs_xattr_get(struct inode *inode, const char *name, void *buffer,
		       size_t size, bool only_whole)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_xattr xattr;
	u64 cnid = apfs_ino(inode);
	int ret;

	apfs_init_xattr_key(cnid, name, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto done;

	ret = apfs_xattr_from_query(query, &xattr);
	if (ret) {
		apfs_alert(sb, "bad xattr record in inode 0x%llx", cnid);
		goto done;
	}

	if (xattr.has_dstream)
		ret = apfs_xattr_extents_read(inode, &xattr, buffer, size, only_whole);
	else
		ret = apfs_xattr_inline_read(&xattr, buffer, size, only_whole);

done:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_xattr_get - Find and read a named attribute
 * @inode:	inode the attribute belongs to
 * @name:	name of the attribute
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 *
 * Finds an extended attribute and copies its value to @buffer, if provided. If
 * @buffer is NULL, just computes the size of the buffer required.
 *
 * Returns the number of bytes used/required, or a negative error code in case
 * of failure.
 */
static int apfs_xattr_get(struct inode *inode, const char *name, void *buffer, size_t size)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(inode->i_sb);
	int ret;

	down_read(&nxi->nx_big_sem);
	ret = __apfs_xattr_get(inode, name, buffer, size);
	up_read(&nxi->nx_big_sem);
	if (ret > XATTR_SIZE_MAX)
		return -E2BIG;
	return ret;
}

static int apfs_xattr_osx_get(const struct xattr_handler *handler,
				struct dentry *unused, struct inode *inode,
				const char *name, void *buffer, size_t size)
{
	/* Ignore the fake 'osx' prefix */
	return apfs_xattr_get(inode, name, buffer, size);
}

/**
 * apfs_delete_xattr - Delete an extended attribute
 * @query:	successful query pointing to the xattr to delete
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_delete_xattr(struct apfs_query *query)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_xattr xattr;
	struct apfs_dstream_info *dstream;
	int err;

	err = apfs_xattr_from_query(query, &xattr);
	if (err)
		return err;

	if (!xattr.has_dstream)
		return apfs_btree_remove(query);

	dstream = kzalloc(sizeof(*dstream), GFP_KERNEL);
	if (!dstream)
		return -ENOMEM;
	apfs_dstream_from_xattr(sb, &xattr, dstream);

	/*
	 * Remove the xattr record before truncation, because truncation creates
	 * new queries and makes ours invalid. This stuff is all too subtle, I
	 * really need to add some assertions (TODO).
	 */
	err = apfs_btree_remove(query);
	if (err)
		goto fail;
	err = apfs_truncate(dstream, 0);

fail:
	kfree(dstream);
	return err;
}

/**
 * apfs_delete_any_xattr - Delete any single xattr for a given inode
 * @inode: the vfs inode
 *
 * Intended to be called repeatedly, to delete all the xattrs one by one.
 * Returns -EAGAIN on success until the process is complete, then it returns
 * 0. Returns other negative error codes in case of failure.
 */
static int apfs_delete_any_xattr(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	int ret;

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_xattr_key(apfs_ino(inode), NULL /* name */, &key);
	query->key = &key;
	query->flags = APFS_QUERY_CAT | APFS_QUERY_ANY_NAME | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		if (ret == -ENODATA)
			ret = 0; /* No more xattrs, we are done */
		goto out;
	}

	ret = apfs_delete_xattr(query);
	if (!ret)
		ret = -EAGAIN;

out:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_delete_all_xattrs - Delete all xattrs for a given inode
 * @inode: the vfs inode
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_delete_all_xattrs(struct inode *inode)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(inode->i_sb);
	int ret;

	lockdep_assert_held_write(&nxi->nx_big_sem);

	do {
		ret = apfs_delete_any_xattr(inode);
	} while (ret == -EAGAIN);

	return ret;
}

/**
 * apfs_build_xattr_key - Allocate and initialize the key for a xattr record
 * @name:	xattr name
 * @ino:	inode number for xattr's owner
 * @key_p:	on return, a pointer to the new on-disk key structure
 *
 * Returns the length of the key, or a negative error code in case of failure.
 */
static int apfs_build_xattr_key(const char *name, u64 ino, struct apfs_xattr_key **key_p)
{
	struct apfs_xattr_key *key;
	u16 namelen = strlen(name) + 1; /* We count the null-termination */
	int key_len;

	key_len = sizeof(*key) + namelen;
	key = kmalloc(key_len, GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	apfs_key_set_hdr(APFS_TYPE_XATTR, ino, key);
	key->name_len = cpu_to_le16(namelen);
	strcpy(key->name, name);

	*key_p = key;
	return key_len;
}

/**
 * apfs_build_dstream_xattr_val - Allocate and init value for a dstream xattr
 * @dstream:	data stream info
 * @val_p:	on return, a pointer to the new on-disk value structure
 *
 * Returns the length of the value, or a negative error code in case of failure.
 */
static int apfs_build_dstream_xattr_val(struct apfs_dstream_info *dstream, struct apfs_xattr_val **val_p)
{
	struct apfs_xattr_val *val;
	struct apfs_xattr_dstream *dstream_raw;
	int val_len;

	val_len = sizeof(*val) + sizeof(*dstream_raw);
	val = kzalloc(val_len, GFP_KERNEL);
	if (!val)
		return -ENOMEM;

	val->flags = cpu_to_le16(APFS_XATTR_DATA_STREAM);
	val->xdata_len = cpu_to_le16(sizeof(*dstream_raw));

	dstream_raw = (void *)val->xdata;
	dstream_raw->xattr_obj_id = cpu_to_le64(dstream->ds_id);
	dstream_raw->dstream.size = cpu_to_le64(dstream->ds_size);
	dstream_raw->dstream.alloced_size = cpu_to_le64(apfs_alloced_size(dstream));
	if (apfs_vol_is_encrypted(dstream->ds_sb))
		dstream_raw->dstream.default_crypto_id = cpu_to_le64(dstream->ds_id);

	*val_p = val;
	return val_len;
}

/**
 * apfs_build_inline_xattr_val - Allocate and init value for an inline xattr
 * @value:	content of the xattr
 * @size:	size of @value
 * @val_p:	on return, a pointer to the new on-disk value structure
 *
 * Returns the length of the value, or a negative error code in case of failure.
 */
static int apfs_build_inline_xattr_val(const void *value, size_t size, struct apfs_xattr_val **val_p)
{
	struct apfs_xattr_val *val;
	int val_len;

	val_len = sizeof(*val) + size;
	val = kmalloc(val_len, GFP_KERNEL);
	if (!val)
		return -ENOMEM;

	val->flags = cpu_to_le16(APFS_XATTR_DATA_EMBEDDED);
	val->xdata_len = cpu_to_le16(size);
	memcpy(val->xdata, value, size);

	*val_p = val;
	return val_len;
}

/**
 * apfs_create_xattr_dstream - Create the extents for a dstream xattr
 * @sb:		filesystem superblock
 * @value:	value for the attribute
 * @size:	sizeo of @value
 *
 * Returns the info for the created data stream, or an error pointer in case
 * of failure.
 */
static struct apfs_dstream_info *apfs_create_xattr_dstream(struct super_block *sb, const void *value, size_t size)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_dstream_info *dstream;
	int blkcnt, i;
	int err;

	dstream = kzalloc(sizeof(*dstream), GFP_KERNEL);
	if (!dstream)
		return ERR_PTR(-ENOMEM);
	dstream->ds_sb = sb;
	spin_lock_init(&dstream->ds_ext_lock);

	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	dstream->ds_id = le64_to_cpu(vsb_raw->apfs_next_obj_id);
	le64_add_cpu(&vsb_raw->apfs_next_obj_id, 1);

	blkcnt = (size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	for (i = 0; i < blkcnt; i++) {
		struct buffer_head *bh;
		int off, tocopy;
		u64 bno;

		err = apfs_dstream_get_new_bno(dstream, i, &bno);
		if (err)
			goto fail;
		bh = apfs_sb_bread(sb, bno);
		if (!bh) {
			err = -EIO;
			goto fail;
		}

		err = apfs_transaction_join(sb, bh);
		if (err) {
			brelse(bh);
			goto fail;
		}

		off = i << sb->s_blocksize_bits;
		tocopy = min(sb->s_blocksize, (unsigned long)(size - off));
		memcpy(bh->b_data, value + off, tocopy);
		if (tocopy < sb->s_blocksize)
			memset(bh->b_data + tocopy, 0, sb->s_blocksize - tocopy);
		brelse(bh);

		dstream->ds_size += tocopy;
	}

	err = apfs_flush_extent_cache(dstream);
	if (err)
		goto fail;
	return dstream;

fail:
	kfree(dstream);
	return ERR_PTR(err);
}

/**
 * apfs_xattr_dstream_from_query - Extract the dstream from a xattr record
 * @query:	the query that found the record
 * @dstream_p:	on return, the newly allocated dstream info (or NULL if none)
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_xattr_dstream_from_query(struct apfs_query *query, struct apfs_dstream_info **dstream_p)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_dstream_info *dstream = NULL;
	struct apfs_xattr xattr;
	int err;

	err = apfs_xattr_from_query(query, &xattr);
	if (err)
		return err;

	if (xattr.has_dstream) {
		dstream = kzalloc(sizeof(*dstream), GFP_KERNEL);
		if (!dstream)
			return -ENOMEM;
		apfs_dstream_from_xattr(sb, &xattr, dstream);
	}
	*dstream_p = dstream;
	return 0;
}

/**
 * apfs_xattr_set - Write a named attribute
 * @inode:	inode the attribute will belong to
 * @name:	name for the attribute
 * @value:	value for the attribute
 * @size:	size of @value
 * @flags:	XATTR_REPLACE and XATTR_CREATE
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_xattr_set(struct inode *inode, const char *name, const void *value,
		   size_t size, int flags)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query = NULL;
	u64 cnid = apfs_ino(inode);
	int key_len, val_len;
	struct apfs_xattr_key *raw_key = NULL;
	struct apfs_xattr_val *raw_val = NULL;
	struct apfs_dstream_info *dstream = NULL;
	struct apfs_dstream_info *old_dstream = NULL;
	int ret;

	if (size > APFS_XATTR_MAX_EMBEDDED_SIZE) {
		dstream = apfs_create_xattr_dstream(sb, value, size);
		if (IS_ERR(dstream))
			return PTR_ERR(dstream);
	}

	apfs_init_xattr_key(cnid, name, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto done;
	}
	query->key = &key;
	query->flags = APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		if (ret != -ENODATA)
			goto done;
		else if (flags & XATTR_REPLACE)
			goto done;
	} else if (flags & XATTR_CREATE) {
		ret = -EEXIST;
		goto done;
	} else if (!value) {
		ret = apfs_delete_xattr(query);
		goto done;
	} else {
		/* Remember the old dstream to clean it up later */
		ret = apfs_xattr_dstream_from_query(query, &old_dstream);
		if (ret)
			goto done;
	}

	key_len = apfs_build_xattr_key(name, cnid, &raw_key);
	if (key_len < 0) {
		ret = key_len;
		goto done;
	}

	if (dstream)
		val_len = apfs_build_dstream_xattr_val(dstream, &raw_val);
	else
		val_len = apfs_build_inline_xattr_val(value, size, &raw_val);
	if (val_len < 0) {
		ret = val_len;
		goto done;
	}

	/* For now this is the only system xattr we support */
	if (strcmp(name, APFS_XATTR_NAME_SYMLINK) == 0)
		raw_val->flags |= cpu_to_le16(APFS_XATTR_FILE_SYSTEM_OWNED);

	if (ret)
		ret = apfs_btree_insert(query, raw_key, key_len, raw_val, val_len);
	else
		ret = apfs_btree_replace(query, raw_key, key_len, raw_val, val_len);
	if (ret)
		goto done;

	if (old_dstream)
		ret = apfs_truncate(old_dstream, 0);

done:
	kfree(dstream);
	kfree(old_dstream);
	kfree(raw_val);
	kfree(raw_key);
	apfs_free_query(query);
	return ret;
}
int APFS_XATTR_SET_MAXOPS(void)
{
	return 1;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
static int apfs_xattr_osx_set(const struct xattr_handler *handler,
	      struct dentry *unused, struct inode *inode, const char *name,
	      const void *value, size_t size, int flags)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
static int apfs_xattr_osx_set(const struct xattr_handler *handler,
		  struct user_namespace *mnt_userns, struct dentry *unused,
		  struct inode *inode, const char *name, const void *value,
		  size_t size, int flags)
#else
static int apfs_xattr_osx_set(const struct xattr_handler *handler,
		  struct mnt_idmap *idmap, struct dentry *unused,
		  struct inode *inode, const char *name, const void *value,
		  size_t size, int flags)
#endif
{
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	int err;

	maxops.cat = APFS_XATTR_SET_MAXOPS();
	maxops.blks = 0;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;

	/* Ignore the fake 'osx' prefix */
	err = apfs_xattr_set(inode, name, value, size, flags);
	if (err)
		goto fail;

	err = apfs_transaction_commit(sb);
	if (!err)
		return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

static const struct xattr_handler apfs_xattr_osx_handler = {
	.prefix	= XATTR_MAC_OSX_PREFIX,
	.get	= apfs_xattr_osx_get,
	.set	= apfs_xattr_osx_set,
};

/* On-disk xattrs have no namespace; use a fake 'osx' prefix in the kernel */
const struct xattr_handler *apfs_xattr_handlers[] = {
	&apfs_xattr_osx_handler,
	NULL
};

ssize_t apfs_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_key key;
	struct apfs_query *query;
	u64 cnid = apfs_ino(inode);
	size_t free = size;
	ssize_t ret;

	down_read(&nxi->nx_big_sem);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto fail;
	}

	/* We want all the xattrs for the cnid, regardless of the name */
	apfs_init_xattr_key(cnid, NULL /* name */, &key);
	query->key = &key;
	query->flags = APFS_QUERY_CAT | APFS_QUERY_MULTIPLE | APFS_QUERY_EXACT;

	while (1) {
		struct apfs_xattr xattr;

		ret = apfs_btree_query(sb, &query);
		if (ret == -ENODATA) { /* Got all the xattrs */
			ret = size - free;
			break;
		}
		if (ret)
			break;

		ret = apfs_xattr_from_query(query, &xattr);
		if (ret) {
			apfs_alert(sb, "bad xattr key in inode %llx", cnid);
			break;
		}

		if (buffer) {
			/* Prepend the fake 'osx' prefix before listing */
			if (xattr.name_len + XATTR_MAC_OSX_PREFIX_LEN + 1 >
									free) {
				ret = -ERANGE;
				break;
			}
			memcpy(buffer, XATTR_MAC_OSX_PREFIX,
			       XATTR_MAC_OSX_PREFIX_LEN);
			buffer += XATTR_MAC_OSX_PREFIX_LEN;
			memcpy(buffer, xattr.name, xattr.name_len + 1);
			buffer += xattr.name_len + 1;
		}
		free -= xattr.name_len + XATTR_MAC_OSX_PREFIX_LEN + 1;
	}

fail:
	apfs_free_query(query);
	up_read(&nxi->nx_big_sem);
	return ret;
}
