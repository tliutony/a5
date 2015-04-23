/*
 * Operations supporting file/inode interface.
 * (c) 2011, 2015 duane a. bailey
 * (c) 1991, 1992 linus torvalds
 */
#include "wufs.h"

/*
 * Exported entrypoints.
 */
void          wufs_truncate_file(struct inode * inode);
int           wufs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			   struct kstat *stat);

/*
 * Global structures.
 */
/**
 * wufs_file_operations:
 * The virtual function table for file operations
 * (see linux/Documentation/filesystems/vfs.txt for details)
 * WUFS does not handle files in an unusual manner, so we fall back to
 * fairly generic functionality.
 */
const struct file_operations wufs_file_operations = {
  .llseek	= generic_file_llseek,
  .read		= do_sync_read,
  .aio_read	= generic_file_aio_read,
  .write	= do_sync_write,
  .aio_write	= generic_file_aio_write,
  .mmap		= generic_file_mmap,
  .fsync	= simple_fsync,
  .splice_read	= generic_file_splice_read,
};

/**
 * wufs_inode_operations:
 * The virtual function table for vfs file-inode class.
 * (see linux/Documentation/filesystems/vfs.txt for details)
 * Most operations fall back to the generic versions.
 */
const struct inode_operations wufs_file_inode_operations = {
  .truncate	= wufs_truncate_file,
  .getattr	= wufs_getattr,
};

/**
 * wufs_symlink_operations:
 * Virtual function table for symlink inodes.
 */
const struct inode_operations wufs_symlink_inode_operations = {
  .readlink	= generic_readlink,
  .follow_link	= page_follow_link_light,
  .put_link	= page_put_link,
  .getattr	= wufs_getattr,
};

/**
 * wufs_truncate_file:
 * The function that is called for file size change.
 */
void wufs_truncate_file(struct inode * inode)
{
  /* if file has no real data associated with it, skip out */
  if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) 
	|| S_ISLNK(inode->i_mode)))
    return;

  /* return pages associated with inode (see itree.c) */
  wufs_truncate(inode);
}

/**
 * wufs_getattr: (file-inode and symlink-inode operation)
 * 
 */
int wufs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
  /* fill out most of the file information as you normally would */
  struct inode *dir = dentry->d_parent->d_inode;
  struct super_block *sb = dir->i_sb;
  generic_fillattr(dentry->d_inode, stat);

  /* ...but report the block count in device blocks (not 512-byte blocks) */
  stat->blocks = (WUFS_BLOCKSIZE / 512) * wufs_blocks(stat->size, sb);
  stat->blksize = sb->s_blocksize;
  return 0;
}

