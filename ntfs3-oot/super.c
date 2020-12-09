// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ntfs3/super.c
 *
 * Copyright (C) 2019-2020 Paragon Software GmbH, All rights reserved.
 *
 *
 *                 terminology
 *
 * vcn - virtual cluster number - offset inside the file in clusters
 * vbo - virtual byte offset    - offset inside the file in bytes
 * lcn - logical cluster number - 0 based cluster in clusters heap
 * lbo - logical byte offset    - absolute position inside volume
 *
 */

#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/module.h>
#include <linux/nls.h>
#include <linux/parser.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>

#include "debug.h"
#include "ntfs.h"
#include "ntfs_fs.h"

#ifdef CONFIG_PRINTK
/*
 * Trace warnings/notices/errors
 * Thanks Joe Perches <joe@perches.com> for implementation
 */
void ntfs_printk(const struct super_block *sb, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	int level;
	struct ntfs_sb_info *sbi = sb->s_fs_info;

	/*should we use different ratelimits for warnings/notices/errors? */
	if (!___ratelimit(&sbi->msg_ratelimit, "ntfs3"))
		return;

	va_start(args, fmt);

	level = printk_get_level(fmt);
	vaf.fmt = printk_skip_level(fmt);
	vaf.va = &args;
	printk("%c%cntfs3: %s: %pV\n", KERN_SOH_ASCII, level, sb->s_id, &vaf);

	va_end(args);
}

/* print warnings/notices/errors about inode using name or inode number */
void ntfs_inode_printk(struct inode *inode, const char *fmt, ...)
{
	struct super_block *sb = inode->i_sb;
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	struct dentry *dentry;
	char *name;
	va_list args;
	struct va_format vaf;
	int level;
	/*not necessary to print full inode name. just for information */
	const u32 name_len = 63;

	if (!___ratelimit(&sbi->msg_ratelimit, "ntfs3"))
		return;

	name = ntfs_alloc(name_len + 1, 0);
	if (!name)
		return;

	dentry = d_find_alias(inode);
	if (dentry) {
		/*we can get the name of inode*/
		spin_lock(&dentry->d_lock);
		snprintf(name, name_len, "%s", dentry->d_name.name);
		spin_unlock(&dentry->d_lock);
		dput(dentry);
		name[name_len] = 0; /* to be sure*/
	} else {
		/*use inode number as inode info*/
		snprintf(name, name_len, "ino=%lx", inode->i_ino);
	}

	va_start(args, fmt);

	level = printk_get_level(fmt);
	vaf.fmt = printk_skip_level(fmt);
	vaf.va = &args;

	printk("%c%cntfs3: %s: %s %pV\n", KERN_SOH_ASCII, level, sb->s_id, name,
	       &vaf);

	va_end(args);

	ntfs_free(name);
}
#endif

/*
 * Shared memory struct.
 *
 * on-disk ntfs's upcase table is created by ntfs formater
 * 'upcase' table is 128K bytes of memory
 * we should read it into memory when mounting
 * Several ntfs volumes likely use the same 'upcase' table
 * It is good idea to share in-memory 'upcase' table between different volumes
 * Unfortunately winxp/vista/win7 use different upcase tables
 */
static DEFINE_SPINLOCK(s_shared_lock);

static struct {
	void *ptr;
	u32 len;
	int cnt;
} s_shared[8];

/*
 * ntfs_set_shared
 *
 * Returns 'ptr' if pointer was saved in shared memory
 * Returns NULL if pointer was not shared
 */
void *ntfs_set_shared(void *ptr, u32 bytes)
{
	void *ret = NULL;
	int i, j = -1;

	spin_lock(&s_shared_lock);
	for (i = 0; i < ARRAY_SIZE(s_shared); i++) {
		if (!s_shared[i].cnt) {
			j = i;
		} else if (bytes == s_shared[i].len &&
			   !memcmp(s_shared[i].ptr, ptr, bytes)) {
			s_shared[i].cnt += 1;
			ret = s_shared[i].ptr;
			break;
		}
	}

	if (!ret && j != -1) {
		s_shared[j].ptr = ptr;
		s_shared[j].len = bytes;
		s_shared[j].cnt = 1;
		ret = ptr;
	}
	spin_unlock(&s_shared_lock);

	return ret;
}

/*
 * ntfs_put_shared
 *
 * Returns 'ptr' if pointer is not shared anymore
 * Returns NULL if pointer is still shared
 */
void *ntfs_put_shared(void *ptr)
{
	void *ret = ptr;
	int i;

	spin_lock(&s_shared_lock);
	for (i = 0; i < ARRAY_SIZE(s_shared); i++) {
		if (s_shared[i].cnt && s_shared[i].ptr == ptr) {
			if (--s_shared[i].cnt)
				ret = NULL;
			break;
		}
	}
	spin_unlock(&s_shared_lock);

	return ret;
}

static void clear_mount_options(struct ntfs_mount_options *options)
{
	static_assert(ARRAY_SIZE(options->nls) == 2);

	unload_nls(options->nls[0]);
	unload_nls(options->nls[1]);
}

enum Opt {
	Opt_uid,
	Opt_gid,
	Opt_umask,
	Opt_dmask,
	Opt_fmask,
	Opt_immutable,
	Opt_discard,
	Opt_force,
	Opt_sparse,
	Opt_nohidden,
	Opt_showmeta,
	Opt_acl,
	Opt_noatime,
	Opt_nls,
	Opt_nls_alt,
	Opt_prealloc,
	Opt_no_acs_rules,
	Opt_err,
};

static const match_table_t ntfs_tokens = {
	{ Opt_uid, "uid=%u" },
	{ Opt_gid, "gid=%u" },
	{ Opt_umask, "umask=%o" },
	{ Opt_dmask, "dmask=%o" },
	{ Opt_fmask, "fmask=%o" },
	{ Opt_immutable, "sys_immutable" },
	{ Opt_discard, "discard" },
	{ Opt_force, "force" },
	{ Opt_sparse, "sparse" },
	{ Opt_nohidden, "nohidden" },
	{ Opt_acl, "acl" },
	{ Opt_noatime, "noatime" },
	{ Opt_showmeta, "showmeta" },
	{ Opt_nls, "nls=%s" },
	{ Opt_nls_alt, "nls_alt=%s" },
	{ Opt_prealloc, "prealloc" },
	{ Opt_no_acs_rules, "no_acs_rules" },
	{ Opt_err, NULL },
};

static noinline int ntfs_parse_options(struct super_block *sb, char *options,
				       int silent,
				       struct ntfs_mount_options *opts)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int i, option;
	char nls_name[2][30];

	opts->fs_uid = current_uid();
	opts->fs_gid = current_gid();
	opts->fs_fmask_inv = opts->fs_dmask_inv = ~current_umask();
	nls_name[0][0] = 0;
	nls_name[1][0] = 0;

	if (!options)
		goto out;

	while ((p = strsep(&options, ","))) {
		int token;

		if (!*p)
			continue;

		token = match_token(p, ntfs_tokens, args);
		switch (token) {
		case Opt_immutable:
			opts->sys_immutable = 1;
			break;
		case Opt_uid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			opts->fs_uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(opts->fs_uid))
				return -EINVAL;
			opts->uid = 1;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			opts->fs_gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(opts->fs_gid))
				return -EINVAL;
			opts->gid = 1;
			break;
		case Opt_umask:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->fs_fmask_inv = opts->fs_dmask_inv = ~option;
			opts->fmask = opts->dmask = 1;
			break;
		case Opt_dmask:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->fs_dmask_inv = ~option;
			opts->dmask = 1;
			break;
		case Opt_fmask:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->fs_fmask_inv = ~option;
			opts->fmask = 1;
			break;
		case Opt_discard:
			opts->discard = 1;
			break;
		case Opt_force:
			opts->force = 1;
			break;
		case Opt_sparse:
			opts->sparse = 1;
			break;
		case Opt_nohidden:
			opts->nohidden = 1;
			break;
		case Opt_acl:
			sb->s_flags |= SB_POSIXACL;
			break;
		case Opt_noatime:
			sb->s_flags |= SB_NOATIME;
			break;
		case Opt_showmeta:
			opts->showmeta = 1;
			break;
		case Opt_nls:
		case Opt_nls_alt:
			match_strlcpy(nls_name[token == Opt_nls_alt], &args[0],
				      sizeof(nls_name[0]));
			break;
		case Opt_prealloc:
			opts->prealloc = 1;
			break;
		case Opt_no_acs_rules:
			opts->no_acs_rules = 1;
			break;
		default:
			if (!silent)
				ntfs_err(
					sb,
					"Unrecognized mount option \"%s\" or missing value",
					p);
			//return -EINVAL;
		}
	}

out:

	for (i = 0; i < ARRAY_SIZE(opts->nls); i++) {
		struct nls_table *nls;

		if (nls_name[i][0]) {
			nls = load_nls(nls_name[i]);
			if (!nls) {
				ntfs_err(sb, "failed to load \"%s\"",
					 nls_name[i]);
				return -EINVAL;
			}
		} else {
			nls = load_nls_default();
			if (!nls) {
				ntfs_err(sb, "failed to load default nls");
				return -EINVAL;
			}
		}

		opts->nls[i] = nls;
	}

	/* unload alternative nls if it equals primary one */
	if (!strcmp(opts->nls[0]->charset, opts->nls[1]->charset)) {
		unload_nls(opts->nls[1]);
		opts->nls[1] = NULL;
	}

	if (!strcmp(opts->nls[0]->charset, "utf8")) {
		/*use utf16s_to_utf8s/utf8s_to_utf16s instead of nls*/
		unload_nls(opts->nls[0]);
		opts->nls[0] = NULL;
	}

	return 0;
}

static int ntfs_remount(struct super_block *sb, int *flags, char *data)
{
	int err, ro_rw;
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	struct ntfs_mount_options old_opts;
	char *orig_data = kstrdup(data, GFP_KERNEL);

	if (data && !orig_data)
		return -ENOMEM;

	/* Store  original options */
	memcpy(&old_opts, &sbi->options, sizeof(old_opts));
	clear_mount_options(&sbi->options);
	memset(&sbi->options, 0, sizeof(sbi->options));

	err = ntfs_parse_options(sb, data, 0, &sbi->options);
	if (err)
		goto restore_opts;

	ro_rw = 0;
	if (sb_rdonly(sb) && !(*flags & SB_RDONLY)) {
		/* ro -> rw */
		ro_rw = 1;
		if (sbi->flags & NTFS_FLAGS_NEED_REPLAY) {
			ntfs_warn(
				sb,
				"Couldn't remount rw because journal is not replayed. Please umount/remount instead\n");
			err = -EINVAL;
			goto restore_opts;
		}
	}

	sync_filesystem(sb);

	if (ro_rw && (sbi->volume.flags & VOLUME_FLAG_DIRTY) &&
	    !sbi->options.force) {
		ntfs_warn(sb, "volume is dirty and \"force\" flag is not set!");
		err = -EINVAL;
		goto restore_opts;
	}

	clear_mount_options(&old_opts);

	*flags = (*flags & ~SB_LAZYTIME) | (sb->s_flags & SB_LAZYTIME) |
		 SB_NODIRATIME | SB_NOATIME;
	ntfs_info(sb, "re-mounted. Opts: %s", orig_data);
	err = 0;
	goto out;

restore_opts:
	clear_mount_options(&sbi->options);
	memcpy(&sbi->options, &old_opts, sizeof(old_opts));

out:
	kfree(orig_data);
	return err;
}

static struct kmem_cache *ntfs_inode_cachep;

static struct inode *ntfs_alloc_inode(struct super_block *sb)
{
	struct ntfs_inode *ni = kmem_cache_alloc(ntfs_inode_cachep, GFP_NOFS);

	if (!ni)
		return NULL;

	memset(ni, 0, offsetof(struct ntfs_inode, vfs_inode));

	mutex_init(&ni->ni_lock);

	return &ni->vfs_inode;
}

static void ntfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	struct ntfs_inode *ni = ntfs_i(inode);

	mutex_destroy(&ni->ni_lock);

	kmem_cache_free(ntfs_inode_cachep, ni);
}

static void ntfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, ntfs_i_callback);
}

static void init_once(void *foo)
{
	struct ntfs_inode *ni = foo;

	inode_init_once(&ni->vfs_inode);
}

/* noinline to reduce binary size*/
static noinline void put_ntfs(struct ntfs_sb_info *sbi)
{
	ntfs_free(sbi->new_rec);
	ntfs_free(ntfs_put_shared(sbi->upcase));
	ntfs_free(sbi->def_table);

	wnd_close(&sbi->mft.bitmap);
	wnd_close(&sbi->used.bitmap);

	if (sbi->mft.ni)
		iput(&sbi->mft.ni->vfs_inode);

	if (sbi->security.ni)
		iput(&sbi->security.ni->vfs_inode);

	if (sbi->reparse.ni)
		iput(&sbi->reparse.ni->vfs_inode);

	if (sbi->objid.ni)
		iput(&sbi->objid.ni->vfs_inode);

	if (sbi->volume.ni)
		iput(&sbi->volume.ni->vfs_inode);

	ntfs_update_mftmirr(sbi, 0);

	indx_clear(&sbi->security.index_sii);
	indx_clear(&sbi->security.index_sdh);
	indx_clear(&sbi->reparse.index_r);
	indx_clear(&sbi->objid.index_o);
	ntfs_free(sbi->compress.frame_unc);
	ntfs_free(sbi->compress.ctx);

	clear_mount_options(&sbi->options);

	ntfs_free(sbi);
}

static void ntfs_put_super(struct super_block *sb)
{
	struct ntfs_sb_info *sbi = sb->s_fs_info;

	/*mark rw ntfs as clear, if possible*/
	ntfs_set_state(sbi, NTFS_DIRTY_CLEAR);

	put_ntfs(sbi);

	sync_blockdev(sb->s_bdev);
}

static int ntfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	struct wnd_bitmap *wnd = &sbi->used.bitmap;

	buf->f_type = sb->s_magic;
	buf->f_bsize = sbi->cluster_size;
	buf->f_blocks = wnd->nbits;

	buf->f_bfree = buf->f_bavail = wnd_zeroes(wnd);
	buf->f_fsid.val[0] = (u32)sbi->volume.ser_num;
	buf->f_fsid.val[1] = (u32)(sbi->volume.ser_num >> 32);
	buf->f_namelen = NTFS_NAME_LEN;

	return 0;
}

static int ntfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct super_block *sb = root->d_sb;
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	struct ntfs_mount_options *opts = &sbi->options;

	if (opts->uid)
		seq_printf(m, ",uid=%u",
			   from_kuid_munged(&init_user_ns, opts->fs_uid));
	if (opts->gid)
		seq_printf(m, ",gid=%u",
			   from_kgid_munged(&init_user_ns, opts->fs_gid));
	if (opts->fmask)
		seq_printf(m, ",fmask=%04o", ~opts->fs_fmask_inv);
	if (opts->dmask)
		seq_printf(m, ",dmask=%04o", ~opts->fs_dmask_inv);
	if (opts->nls[0])
		seq_printf(m, ",nls=%s", opts->nls[0]->charset);
	else
		seq_puts(m, ",nls=utf8");
	if (opts->nls[1])
		seq_printf(m, ",nls_alt=%s", opts->nls[1]->charset);
	if (opts->sys_immutable)
		seq_puts(m, ",sys_immutable");
	if (opts->discard)
		seq_puts(m, ",discard");
	if (opts->sparse)
		seq_puts(m, ",sparse");
	if (opts->showmeta)
		seq_puts(m, ",showmeta");
	if (opts->nohidden)
		seq_puts(m, ",nohidden");
	if (opts->force)
		seq_puts(m, ",force");
	if (opts->no_acs_rules)
		seq_puts(m, ",no_acs_rules");
	if (opts->prealloc)
		seq_puts(m, ",prealloc");
	if (sb->s_flags & SB_POSIXACL)
		seq_puts(m, ",acl");
	if (sb->s_flags & SB_NOATIME)
		seq_puts(m, ",noatime");

	return 0;
}

/*super_operations::sync_fs*/
static int ntfs_sync_fs(struct super_block *sb, int wait)
{
	int err = 0, err2;
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	struct ntfs_inode *ni;
	struct inode *inode;

	ni = sbi->security.ni;
	if (ni) {
		inode = &ni->vfs_inode;
		err2 = _ni_write_inode(inode, wait);
		if (err2 && !err)
			err = err2;
	}

	ni = sbi->objid.ni;
	if (ni) {
		inode = &ni->vfs_inode;
		err2 = _ni_write_inode(inode, wait);
		if (err2 && !err)
			err = err2;
	}

	ni = sbi->reparse.ni;
	if (ni) {
		inode = &ni->vfs_inode;
		err2 = _ni_write_inode(inode, wait);
		if (err2 && !err)
			err = err2;
	}

	if (!err)
		ntfs_set_state(sbi, NTFS_DIRTY_CLEAR);

	ntfs_update_mftmirr(sbi, wait);

	return err;
}

static const struct super_operations ntfs_sops = {
	.alloc_inode = ntfs_alloc_inode,
	.destroy_inode = ntfs_destroy_inode,
	.evict_inode = ntfs_evict_inode,
	.put_super = ntfs_put_super,
	.statfs = ntfs_statfs,
	.show_options = ntfs_show_options,
	.sync_fs = ntfs_sync_fs,
	.remount_fs = ntfs_remount,
	.write_inode = ntfs_write_inode,
};

static struct inode *ntfs_export_get_inode(struct super_block *sb, u64 ino,
					   u32 generation)
{
	struct MFT_REF ref;
	struct inode *inode;

	ref.low = cpu_to_le32(ino);
#ifdef NTFS3_64BIT_CLUSTER
	ref.high = cpu_to_le16(ino >> 32);
#else
	ref.high = 0;
#endif
	ref.seq = cpu_to_le16(generation);

	inode = ntfs_iget5(sb, &ref, NULL);
	if (!IS_ERR(inode) && is_bad_inode(inode)) {
		iput(inode);
		inode = ERR_PTR(-ESTALE);
	}

	return inode;
}

static struct dentry *ntfs_fh_to_dentry(struct super_block *sb, struct fid *fid,
					int fh_len, int fh_type)
{
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				    ntfs_export_get_inode);
}

static struct dentry *ntfs_fh_to_parent(struct super_block *sb, struct fid *fid,
					int fh_len, int fh_type)
{
	return generic_fh_to_parent(sb, fid, fh_len, fh_type,
				    ntfs_export_get_inode);
}

/* TODO: == ntfs_sync_inode */
static int ntfs_nfs_commit_metadata(struct inode *inode)
{
	return _ni_write_inode(inode, 1);
}

static const struct export_operations ntfs_export_ops = {
	.fh_to_dentry = ntfs_fh_to_dentry,
	.fh_to_parent = ntfs_fh_to_parent,
	.get_parent = ntfs_get_parent,
	.commit_metadata = ntfs_nfs_commit_metadata,
};

/* Returns Gb,Mb to print with "%u.%02u Gb" */
static u32 format_size_gb(const u64 bytes, u32 *mb)
{
	/* Do simple right 30 bit shift of 64 bit value */
	u64 kbytes = bytes >> 10;
	u32 kbytes32 = (u32)kbytes;

	*mb = (100 * (kbytes32 & 0xfffff) + 0x7ffff) >> 20;
	if (*mb >= 100)
		*mb = 99;

	return (kbytes32 >> 20) | (((u32)(kbytes >> 32)) << 12);
}

static u32 true_sectors_per_clst(const struct NTFS_BOOT *boot)
{
	return boot->sectors_per_clusters <= 0x80 ?
		       boot->sectors_per_clusters :
		       (1u << (0 - boot->sectors_per_clusters));
}

/* inits internal info from on-disk boot sector*/
static int ntfs_init_from_boot(struct super_block *sb, u32 sector_size,
			       u64 dev_size)
{
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	int err;
	u32 mb, gb, boot_sector_size, sct_per_clst, record_size;
	u64 sectors, clusters, fs_size, mlcn, mlcn2;
	struct NTFS_BOOT *boot;
	struct buffer_head *bh;
	struct MFT_REC *rec;
	u16 fn, ao;

	sbi->volume.blocks = dev_size >> PAGE_SHIFT;

	bh = ntfs_bread(sb, 0);
	if (!bh)
		return -EIO;

	err = -EINVAL;
	boot = (struct NTFS_BOOT *)bh->b_data;

	if (memcmp(boot->system_id, "NTFS    ", sizeof("NTFS    ") - 1))
		goto out;

	/* 0x55AA is not mandaroty. Thanks Maxim Suhanov*/
	/*if (0x55 != boot->boot_magic[0] || 0xAA != boot->boot_magic[1])
	 *	goto out;
	 */

	boot_sector_size = (u32)boot->bytes_per_sector[1] << 8;
	if (boot->bytes_per_sector[0] || boot_sector_size < SECTOR_SIZE ||
	    !is_power_of2(boot_sector_size)) {
		goto out;
	}

	sct_per_clst = true_sectors_per_clst(boot);
	if (!is_power_of2(sct_per_clst))
		goto out;

	mlcn = le64_to_cpu(boot->mft_clst);
	mlcn2 = le64_to_cpu(boot->mft2_clst);
	sectors = le64_to_cpu(boot->sectors_per_volume);

	if (mlcn * sct_per_clst >= sectors)
		goto out;

	if (mlcn2 * sct_per_clst >= sectors)
		goto out;

	/* Check MFT record size */
	if ((boot->record_size < 0 &&
	     SECTOR_SIZE > (2U << (-boot->record_size))) ||
	    (boot->record_size >= 0 && !is_power_of2(boot->record_size))) {
		goto out;
	}

	/* Check index record size */
	if ((boot->index_size < 0 &&
	     SECTOR_SIZE > (2U << (-boot->index_size))) ||
	    (boot->index_size >= 0 && !is_power_of2(boot->index_size))) {
		goto out;
	}

	sbi->sector_size = boot_sector_size;
	sbi->sector_bits = blksize_bits(boot_sector_size);
	fs_size = (sectors + 1) << sbi->sector_bits;

	gb = format_size_gb(fs_size, &mb);

	/*
	 * - Volume formatted and mounted with the same sector size
	 * - Volume formatted 4K and mounted as 512
	 * - Volume formatted 512 and mounted as 4K
	 */
	if (sbi->sector_size != sector_size) {
		ntfs_warn(sb,
			  "Different NTFS' sector size and media sector size");
		dev_size += sector_size - 1;
	}

	sbi->cluster_size = boot_sector_size * sct_per_clst;
	sbi->cluster_bits = blksize_bits(sbi->cluster_size);

	sbi->mft.lbo = mlcn << sbi->cluster_bits;
	sbi->mft.lbo2 = mlcn2 << sbi->cluster_bits;

	if (sbi->cluster_size < sbi->sector_size)
		goto out;

	sbi->cluster_mask = sbi->cluster_size - 1;
	sbi->cluster_mask_inv = ~(u64)sbi->cluster_mask;
	sbi->record_size = record_size = boot->record_size < 0 ?
						 1 << (-boot->record_size) :
						 (u32)boot->record_size
							 << sbi->cluster_bits;

	if (record_size > MAXIMUM_BYTES_PER_MFT)
		goto out;

	sbi->record_bits = blksize_bits(record_size);
	sbi->attr_size_tr = (5 * record_size >> 4); // ~320 bytes

	sbi->max_bytes_per_attr =
		record_size - QuadAlign(MFTRECORD_FIXUP_OFFSET_1) -
		QuadAlign(((record_size >> SECTOR_SHIFT) * sizeof(short))) -
		QuadAlign(sizeof(enum ATTR_TYPE));

	sbi->index_size = boot->index_size < 0 ?
				  1u << (-boot->index_size) :
				  (u32)boot->index_size << sbi->cluster_bits;

	sbi->volume.ser_num = le64_to_cpu(boot->serial_num);
	sbi->volume.size = sectors << sbi->sector_bits;

	/* warning if RAW volume */
	if (dev_size < fs_size) {
		u32 mb0, gb0;

		gb0 = format_size_gb(dev_size, &mb0);
		ntfs_warn(
			sb,
			"RAW NTFS volume: Filesystem size %u.%02u Gb > volume size %u.%02u Gb. Mount in read-only",
			gb, mb, gb0, mb0);
		sb->s_flags |= SB_RDONLY;
	}

	clusters = sbi->volume.size >> sbi->cluster_bits;
#ifdef NTFS3_64BIT_CLUSTER
#if BITS_PER_LONG < 64
#error "NTFS3_64BIT_CLUSTER incompatible in 32 bit OS"
#endif
#else
	/* 32 bits per cluster */
	if (clusters >> 32) {
		ntfs_notice(
			sb,
			"NTFS %u.%02u Gb is too big to use 32 bits per cluster",
			gb, mb);
		goto out;
	}
#endif

	sbi->used.bitmap.nbits = clusters;

	rec = ntfs_alloc(record_size, 1);
	if (!rec) {
		err = -ENOMEM;
		goto out;
	}

	sbi->new_rec = rec;
	rec->rhdr.sign = NTFS_FILE_SIGNATURE;
	rec->rhdr.fix_off = cpu_to_le16(MFTRECORD_FIXUP_OFFSET_1);
	fn = (sbi->record_size >> SECTOR_SHIFT) + 1;
	rec->rhdr.fix_num = cpu_to_le16(fn);
	ao = QuadAlign(MFTRECORD_FIXUP_OFFSET_1 + sizeof(short) * fn);
	rec->attr_off = cpu_to_le16(ao);
	rec->used = cpu_to_le32(ao + QuadAlign(sizeof(enum ATTR_TYPE)));
	rec->total = cpu_to_le32(sbi->record_size);
	((struct ATTRIB *)Add2Ptr(rec, ao))->type = ATTR_END;

	if (sbi->cluster_size < PAGE_SIZE)
		sb_set_blocksize(sb, sbi->cluster_size);

	sbi->block_mask = sb->s_blocksize - 1;
	sbi->blocks_per_cluster = sbi->cluster_size >> sb->s_blocksize_bits;
	sbi->volume.blocks = sbi->volume.size >> sb->s_blocksize_bits;

	/* Maximum size for normal files */
	sbi->maxbytes = (clusters << sbi->cluster_bits) - 1;

#ifdef NTFS3_64BIT_CLUSTER
	if (clusters >= (1ull << (64 - sbi->cluster_bits)))
		sbi->maxbytes = -1;
	sbi->maxbytes_sparse = -1;
#else
	/* Maximum size for sparse file */
	sbi->maxbytes_sparse = (1ull << (sbi->cluster_bits + 32)) - 1;
#endif

	err = 0;

out:
	brelse(bh);

	return err;
}

/* try to mount*/
static int ntfs_fill_super(struct super_block *sb, void *data, int silent)
{
	int err;
	struct ntfs_sb_info *sbi;
	struct block_device *bdev = sb->s_bdev;
	struct inode *bd_inode = bdev->bd_inode;
	struct request_queue *rq = bdev_get_queue(bdev);
	struct inode *inode = NULL;
	struct ntfs_inode *ni;
	size_t i, tt;
	CLST vcn, lcn, len;
	struct ATTRIB *attr;
	const struct VOLUME_INFO *info;
	u32 idx, done, bytes;
	struct ATTR_DEF_ENTRY *t;
	u16 *upcase = NULL;
	u16 *shared;
	bool is_ro;
	struct MFT_REF ref;

	ref.high = 0;

	sbi = ntfs_alloc(sizeof(struct ntfs_sb_info), true);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	sbi->sb = sb;
	sb->s_flags |= SB_NODIRATIME;
	sb->s_magic = 0x7366746e; // "ntfs"
	sb->s_op = &ntfs_sops;
	sb->s_export_op = &ntfs_export_ops;
	sb->s_time_gran = NTFS_TIME_GRAN; // 100 nsec
	sb->s_xattr = ntfs_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	ratelimit_state_init(&sbi->msg_ratelimit, DEFAULT_RATELIMIT_INTERVAL,
			     DEFAULT_RATELIMIT_BURST);

	err = ntfs_parse_options(sb, data, silent, &sbi->options);
	if (err)
		goto out;

	if (!rq || !blk_queue_discard(rq) || !rq->limits.discard_granularity) {
		;
	} else {
		sbi->discard_granularity = rq->limits.discard_granularity;
		sbi->discard_granularity_mask_inv =
			~(u64)(sbi->discard_granularity - 1);
	}

	sb_set_blocksize(sb, PAGE_SIZE);

	/* parse boot */
	err = ntfs_init_from_boot(sb, rq ? queue_logical_block_size(rq) : 512,
				  bd_inode->i_size);
	if (err)
		goto out;

	spin_lock_init(&sbi->compress.lock);
	if (sbi->cluster_size <= NTFS_LZNT_MAX_CLUSTER) {
		u32 bytes_per_frame = sbi->cluster_size << NTFS_LZNT_CUNIT;

		sbi->compress.frame_unc = ntfs_alloc(bytes_per_frame, 0);
		if (!sbi->compress.frame_unc) {
			err = -ENOMEM;
			goto out;
		}

		sbi->compress.ctx = get_compression_ctx(true);
		if (!sbi->compress.ctx) {
			err = -ENOMEM;
			goto out;
		}
	}

	/*
	 * Load $Volume. This should be done before $LogFile
	 * 'cause 'sbi->volume.ni' is used 'ntfs_set_state'
	 */
	ref.low = cpu_to_le32(MFT_REC_VOL);
	ref.seq = cpu_to_le16(MFT_REC_VOL);
	inode = ntfs_iget5(sb, &ref, &NAME_VOLUME);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $Volume.");
		inode = NULL;
		goto out;
	}

	ni = ntfs_i(inode);

	/* Load and save label (not necessary) */
	attr = ni_find_attr(ni, NULL, NULL, ATTR_LABEL, NULL, 0, NULL, NULL);

	if (!attr) {
		;
	} else if (!attr->non_res && !is_attr_ext(attr)) {
		/* $AttrDef allows labels to be up to 128 symbols */
		err = utf16s_to_utf8s(resident_data(attr),
				      le32_to_cpu(attr->res.data_size) >> 1,
				      UTF16_LITTLE_ENDIAN, sbi->volume.label,
				      sizeof(sbi->volume.label));
		if (err < 0)
			sbi->volume.label[0] = 0;
	} else {
		/* should we break mounting here? */
		//err = -EINVAL;
		//goto out;
	}

	attr = ni_find_attr(ni, attr, NULL, ATTR_VOL_INFO, NULL, 0, NULL, NULL);
	if (!attr || is_attr_ext(attr)) {
		err = -EINVAL;
		goto out;
	}

	info = resident_data_ex(attr, SIZEOF_ATTRIBUTE_VOLUME_INFO);
	if (!info) {
		err = -EINVAL;
		goto out;
	}

	sbi->volume.major_ver = info->major_ver;
	sbi->volume.minor_ver = info->minor_ver;
	sbi->volume.flags = info->flags;

	sbi->volume.ni = ni;
	inode = NULL;

	/* Load $MFTMirr to estimate recs_mirr */
	ref.low = cpu_to_le32(MFT_REC_MIRR);
	ref.seq = cpu_to_le16(MFT_REC_MIRR);
	inode = ntfs_iget5(sb, &ref, &NAME_MIRROR);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $MFTMirr.");
		inode = NULL;
		goto out;
	}

	sbi->mft.recs_mirr =
		ntfs_up_cluster(sbi, inode->i_size) >> sbi->record_bits;

	iput(inode);

	/* Load $LogFile to replay */
	ref.low = cpu_to_le32(MFT_REC_LOG);
	ref.seq = cpu_to_le16(MFT_REC_LOG);
	inode = ntfs_iget5(sb, &ref, &NAME_LOGFILE);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $LogFile.");
		inode = NULL;
		goto out;
	}

	ni = ntfs_i(inode);

	err = ntfs_loadlog_and_replay(ni, sbi);
	if (err)
		goto out;

	iput(inode);
	inode = NULL;

	is_ro = sb_rdonly(sbi->sb);

	if (sbi->flags & NTFS_FLAGS_NEED_REPLAY) {
		if (!is_ro) {
			ntfs_warn(sb,
				  "failed to replay log file. Can't mount rw!");
			err = -EINVAL;
			goto out;
		}
	} else if (sbi->volume.flags & VOLUME_FLAG_DIRTY) {
		if (!is_ro && !sbi->options.force) {
			ntfs_warn(
				sb,
				"volume is dirty and \"force\" flag is not set!");
			err = -EINVAL;
			goto out;
		}
	}

	/* Load $MFT */
	ref.low = cpu_to_le32(MFT_REC_MFT);
	ref.seq = cpu_to_le16(1);

	inode = ntfs_iget5(sb, &ref, &NAME_MFT);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $MFT.");
		inode = NULL;
		goto out;
	}

	ni = ntfs_i(inode);

	sbi->mft.used = ni->i_valid >> sbi->record_bits;
	tt = inode->i_size >> sbi->record_bits;
	sbi->mft.next_free = MFT_REC_USER;

	err = wnd_init(&sbi->mft.bitmap, sb, tt);
	if (err)
		goto out;

	err = ni_load_all_mi(ni);
	if (err)
		goto out;

	sbi->mft.ni = ni;

	/* Load $BadClus */
	ref.low = cpu_to_le32(MFT_REC_BADCLUST);
	ref.seq = cpu_to_le16(MFT_REC_BADCLUST);
	inode = ntfs_iget5(sb, &ref, &NAME_BADCLUS);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $BadClus.");
		inode = NULL;
		goto out;
	}

	ni = ntfs_i(inode);

	for (i = 0; run_get_entry(&ni->file.run, i, &vcn, &lcn, &len); i++) {
		if (lcn == SPARSE_LCN)
			continue;

		if (!sbi->bad_clusters)
			ntfs_notice(sb, "Volume contains bad blocks");

		sbi->bad_clusters += len;
	}

	iput(inode);

	/* Load $Bitmap */
	ref.low = cpu_to_le32(MFT_REC_BITMAP);
	ref.seq = cpu_to_le16(MFT_REC_BITMAP);
	inode = ntfs_iget5(sb, &ref, &NAME_BITMAP);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $Bitmap.");
		inode = NULL;
		goto out;
	}

	ni = ntfs_i(inode);

#ifndef NTFS3_64BIT_CLUSTER
	if (inode->i_size >> 32) {
		err = -EINVAL;
		goto out;
	}
#endif

	/* Check bitmap boundary */
	tt = sbi->used.bitmap.nbits;
	if (inode->i_size < bitmap_size(tt)) {
		err = -EINVAL;
		goto out;
	}

	/* Not necessary */
	sbi->used.bitmap.set_tail = true;
	err = wnd_init(&sbi->used.bitmap, sbi->sb, tt);
	if (err)
		goto out;

	iput(inode);

	/* Compute the mft zone */
	err = ntfs_refresh_zone(sbi);
	if (err)
		goto out;

	/* Load $AttrDef */
	ref.low = cpu_to_le32(MFT_REC_ATTR);
	ref.seq = cpu_to_le16(MFT_REC_ATTR);
	inode = ntfs_iget5(sbi->sb, &ref, &NAME_ATTRDEF);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $AttrDef -> %d", err);
		inode = NULL;
		goto out;
	}

	if (inode->i_size < sizeof(struct ATTR_DEF_ENTRY)) {
		err = -EINVAL;
		goto out;
	}
	bytes = inode->i_size;
	sbi->def_table = t = ntfs_alloc(bytes, 0);
	if (!t) {
		err = -ENOMEM;
		goto out;
	}

	for (done = idx = 0; done < bytes; done += PAGE_SIZE, idx++) {
		unsigned long tail = bytes - done;
		struct page *page = ntfs_map_page(inode->i_mapping, idx);

		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			goto out;
		}
		memcpy(Add2Ptr(t, done), page_address(page),
		       min(PAGE_SIZE, tail));
		ntfs_unmap_page(page);

		if (!idx && ATTR_STD != t->type) {
			err = -EINVAL;
			goto out;
		}
	}

	t += 1;
	sbi->def_entries = 1;
	done = sizeof(struct ATTR_DEF_ENTRY);
	sbi->reparse.max_size = MAXIMUM_REPARSE_DATA_BUFFER_SIZE;

	while (done + sizeof(struct ATTR_DEF_ENTRY) <= bytes) {
		u32 t32 = le32_to_cpu(t->type);

		if ((t32 & 0xF) || le32_to_cpu(t[-1].type) >= t32)
			break;

		if (t->type == ATTR_REPARSE)
			sbi->reparse.max_size = le64_to_cpu(t->max_sz);

		done += sizeof(struct ATTR_DEF_ENTRY);
		t += 1;
		sbi->def_entries += 1;
	}
	iput(inode);

	/* Load $UpCase */
	ref.low = cpu_to_le32(MFT_REC_UPCASE);
	ref.seq = cpu_to_le16(MFT_REC_UPCASE);
	inode = ntfs_iget5(sb, &ref, &NAME_UPCASE);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load $LogFile.");
		inode = NULL;
		goto out;
	}

	ni = ntfs_i(inode);

	if (inode->i_size != 0x10000 * sizeof(short)) {
		err = -EINVAL;
		goto out;
	}

	sbi->upcase = upcase = ntfs_alloc(0x10000 * sizeof(short), 0);
	if (!upcase) {
		err = -ENOMEM;
		goto out;
	}

	for (idx = 0; idx < (0x10000 * sizeof(short) >> PAGE_SHIFT); idx++) {
		const u16 *src;
		u16 *dst = Add2Ptr(upcase, idx << PAGE_SHIFT);
		struct page *page = ntfs_map_page(inode->i_mapping, idx);

		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			goto out;
		}

		src = page_address(page);

#ifdef __BIG_ENDIAN
		for (i = 0; i < PAGE_SIZE / sizeof(u16); i++)
			*dst++ = le16_to_cpu(*src++);
#else
		memcpy(dst, src, PAGE_SIZE);
#endif
		ntfs_unmap_page(page);
	}

	shared = ntfs_set_shared(upcase, 0x10000 * sizeof(short));
	if (shared && upcase != shared) {
		sbi->upcase = shared;
		ntfs_free(upcase);
	}

	iput(inode);
	inode = NULL;

	if (is_ntfs3(sbi)) {
		/* Load $Secure */
		err = ntfs_security_init(sbi);
		if (err)
			goto out;

		/* Load $Extend */
		err = ntfs_extend_init(sbi);
		if (err)
			goto load_root;

		/* Load $Extend\$Reparse */
		err = ntfs_reparse_init(sbi);
		if (err)
			goto load_root;

		/* Load $Extend\$ObjId */
		err = ntfs_objid_init(sbi);
		if (err)
			goto load_root;
	}

load_root:

	/* Load root */
	ref.low = cpu_to_le32(MFT_REC_ROOT);
	ref.seq = cpu_to_le16(MFT_REC_ROOT);
	inode = ntfs_iget5(sb, &ref, &NAME_ROOT);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ntfs_err(sb, "Failed to load root.");
		inode = NULL;
		goto out;
	}

	ni = ntfs_i(inode);

	sb->s_root = d_make_root(inode);

	if (!sb->s_root) {
		err = -EINVAL;
		goto out;
	}

	return 0;

out:
	iput(inode);

	if (sb->s_root) {
		d_drop(sb->s_root);
		sb->s_root = NULL;
	}

	put_ntfs(sbi);

	sb->s_fs_info = NULL;
	return err;
}

void ntfs_unmap_meta(struct super_block *sb, CLST lcn, CLST len)
{
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	struct block_device *bdev = sb->s_bdev;
	sector_t devblock = (u64)lcn * sbi->blocks_per_cluster;
	unsigned long blocks = (u64)len * sbi->blocks_per_cluster;
	unsigned long cnt = 0;
	unsigned long limit = global_zone_page_state(NR_FREE_PAGES)
			      << (PAGE_SHIFT - sb->s_blocksize_bits);

	if (limit >= 0x2000)
		limit -= 0x1000;
	else if (limit < 32)
		limit = 32;
	else
		limit >>= 1;

	while (blocks--) {
		clean_bdev_aliases(bdev, devblock++, 1);
		if (cnt++ >= limit) {
			sync_blockdev(bdev);
			cnt = 0;
		}
	}
}

/*
 * ntfs_discard
 *
 * issue a discard request (trim for SSD)
 */
int ntfs_discard(struct ntfs_sb_info *sbi, CLST lcn, CLST len)
{
	int err;
	u64 lbo, bytes, start, end;
	struct super_block *sb;

	if (sbi->used.next_free_lcn == lcn + len)
		sbi->used.next_free_lcn = lcn;

	if (sbi->flags & NTFS_FLAGS_NODISCARD)
		return -EOPNOTSUPP;

	if (!sbi->options.discard)
		return -EOPNOTSUPP;

	lbo = (u64)lcn << sbi->cluster_bits;
	bytes = (u64)len << sbi->cluster_bits;

	/* Align up 'start' on discard_granularity */
	start = (lbo + sbi->discard_granularity - 1) &
		sbi->discard_granularity_mask_inv;
	/* Align down 'end' on discard_granularity */
	end = (lbo + bytes) & sbi->discard_granularity_mask_inv;

	sb = sbi->sb;
	if (start >= end)
		return 0;

	err = blkdev_issue_discard(sb->s_bdev, start >> 9, (end - start) >> 9,
				   GFP_NOFS, 0);

	if (err == -EOPNOTSUPP)
		sbi->flags |= NTFS_FLAGS_NODISCARD;

	return err;
}

static struct dentry *ntfs_mount(struct file_system_type *fs_type, int flags,
				 const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, ntfs_fill_super);
}

static struct file_system_type ntfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "ntfs3",
	.mount = ntfs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init init_ntfs_fs(void)
{
	int err;

#ifdef NTFS3_INDEX_BINARY_SEARCH
	pr_notice("ntfs3: +index binary search");
#endif

#ifdef NTFS3_CHECK_FREE_CLST
	pr_notice("ntfs3: +check free clusters");
#endif

#if NTFS_LINK_MAX < 0xffff
	pr_notice("ntfs3: max link count %u", NTFS_LINK_MAX);
#endif

#ifdef NTFS3_64BIT_CLUSTER
	pr_notice("ntfs3: 64 bits per cluster");
#else
	pr_notice("ntfs3: 32 bits per cluster");
#endif

	ntfs_inode_cachep = kmem_cache_create(
		"ntfs_inode_cache", sizeof(struct ntfs_inode), 0,
		(SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT),
		init_once);
	if (!ntfs_inode_cachep) {
		err = -ENOMEM;
		goto failed;
	}

	err = register_filesystem(&ntfs_fs_type);
	if (!err)
		return 0;

failed:
	return err;
}

static void __exit exit_ntfs_fs(void)
{
	if (ntfs_inode_cachep) {
		rcu_barrier();
		kmem_cache_destroy(ntfs_inode_cachep);
	}

	unregister_filesystem(&ntfs_fs_type);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ntfs3 filesystem");
MODULE_AUTHOR("Konstantin Komarov");
MODULE_ALIAS_FS("ntfs3");

module_init(init_ntfs_fs);
module_exit(exit_ntfs_fs);