/*
 * Support for the Williams Ugly File System directory operations.
 * (c) 2011, 2015 duane a. bailey
 * (c) 1991, 1992 linus torvalds
 */
#include "wufs.h"

/*
 * (All entrypoints are exported indirectly through callback functions.)
 */
/*
 * Local routines
 */
static int            add_nondir(struct dentry *dentry, struct inode *inode);
static int            wufs_create(struct inode *dir, struct dentry *dentry,
				  int mode, struct nameidata *nd);
static int            wufs_link(struct dentry *old_dentry, struct inode *dir,
				struct dentry *dentry);
static struct dentry *wufs_lookup(struct inode *dir, struct dentry *dentry,
				  struct nameidata *nd);
static int            wufs_mkdir(struct inode *dir, struct dentry *dentry,
				 int mode);
static int            wufs_mknod(struct inode *dir, struct dentry *dentry,
				 int mode, dev_t rdev);
static int            wufs_rmdir(struct inode *dir, struct dentry *dentry);
static int            wufs_rename(struct inode *old_dir,
				  struct dentry *old_dentry,
				  struct inode *new_dir,
				  struct dentry *new_dentry);
static int            wufs_symlink(struct inode *dir, struct dentry *dentry,
				   const char *symname);
static int            wufs_unlink(struct inode *dir, struct dentry *dentry);

/*
 * Global variables.
 */
/**
 * wufs_dir_inode_operations:
 * These functions are called when operations are performed on directory
 * inodes.
 */
const struct inode_operations wufs_dir_inode_operations = {
  .create	= wufs_create,
  .lookup	= wufs_lookup,
  .link		= wufs_link,
  .unlink	= wufs_unlink,
  .symlink	= wufs_symlink,
  .mkdir	= wufs_mkdir,
  .rmdir	= wufs_rmdir,
  .mknod	= wufs_mknod,
  .rename	= wufs_rename,
  .getattr	= wufs_getattr, /* (see file.c) */
};

/*
 * Code.
 */
/**
 * wufs_create: (vfs directory inode operation)
 * Create a directory.
 */
static int wufs_create(struct inode *dir, struct dentry *dentry, int mode,
		       struct nameidata *nd)
{
  /*
   * Make a new node file node (see mknod(2) for details)
   * Note that the mode, here, is S_ISDIR, indicating a directory.
   */
  return wufs_mknod(dir, dentry, mode, 0);
}

/**
 * wufs_lookup: (vfs directory inode operation)
 * This routine is called when VFS is looking for an inode in a parent
 * directory (referenced by dir).  The name associated with the inode
 * is found in dentry (which is otherwise incomplete).  When the inode 
 * is found, this routine must call d_add to insert the inode information into
 * dentry, and the i_count field must be incremented.
 * (See linux/Documentation/filesystems/vfs.txt for more details.)
 */
static struct dentry *wufs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
  struct super_block *sb = dir->i_sb;
  struct wufs_sb_info *sbi = wufs_sb(sb);
  struct inode *inode = NULL;
  ino_t ino;

  /* grab the directory operations structure */
  dentry->d_op = sb->s_root->d_op;

  /* check to see if name of the file to be created is too long */
  if (dentry->d_name.len > sbi->sbi_namelen)
    return ERR_PTR(-ENAMETOOLONG);

  /* given the name, find it */
  ino = wufs_inode_by_name(dentry);
  if (ino) {
    /* fetch the vfs inode associated with inode number */
    inode = wufs_iget(sb, ino);
    if (IS_ERR(inode))
      return ERR_CAST(inode);
  }
  /* add the inode to the vfs entry */
  d_add(dentry, inode);
  return NULL;
}

/**
 * wufs_link: (vfs directory inode operation)
 * Create a new hard link refering to file specified by old_dentry
 * in directory dir.  Name of the link is specified in dentry.
 */
static int wufs_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *dentry)
{
  /* get the to-be-linked inode */
  struct inode *inode = old_dentry->d_inode;

  /* make sure we're not adding too many links */
  if (inode->i_nlink >= wufs_sb(inode->i_sb)->sbi_link_max)
    return -EMLINK;

  /* update the create time; may have no effect in WUFS */
  inode->i_ctime = CURRENT_TIME_SEC;

  /* increment inode link count */
  inode_inc_link_count(inode);
  /* increment the *usage counter* associated with the inode
   * if positive, these inodes are in use and must be synchronized */
  atomic_inc(&inode->i_count);

  /* add a (nondirectory) reference described by dentry to inode */
  return add_nondir(dentry, inode);
}

/**
 * wufs_unlink: (vfs directory inode operation)
 * Remove the hard link described by dentry from dir.
 */
static int wufs_unlink(struct inode *dir, struct dentry *dentry)
{
  int err = -ENOENT;

  /* get the to-be-unlinked-from inode */
  struct inode *inode = dentry->d_inode;
  struct page *page;
  struct wufs_dirent *de;

  /* find the entry in the directory, getting mapped page */
  de = wufs_find_entry(dentry, &page);
  if (!de) /* doesn't exist */
    goto end_unlink;

  /* remove the specific entry from the directory on page */
  err = wufs_delete_entry(de, page);
  if (err)
    goto end_unlink;

  /* delete entry set directory modification time; copy to inode */
  inode->i_ctime = dir->i_ctime;

  /* now, decrement link count (child directory doesn't point here any more) */
  inode_dec_link_count(inode);
 end_unlink:
  return err;
}

/**
 * wufs_symlink: (vfs directory inode operation)
 * Creates a new inode for a symbolic link associated with dentry in
 * directory dir.  The expansion is symname.
 */
static int wufs_symlink(struct inode *dir, struct dentry *dentry,
			const char * symname)
{
  int err = -ENAMETOOLONG;
  /* the (zero terminated) symbolic name cannot exceed one block in length */
  int i = strlen(symname)+1;
  struct inode *inode;

  /* name is too long */
  if (i > dir->i_sb->s_blocksize)
    goto out;

  /* progressing: create an inode in directory to hold the link */
  inode = wufs_new_inode(dir, &err);
  if (!inode)
    goto out;

  /* establish the link type; access is always 0777 (ignored) */
  inode->i_mode = S_IFLNK | 0777;

  /* set inode operations appropriately, no associated device */
  wufs_set_inode(inode, 0);
  
  /* create the symbolic link associated with length i symname */
  err = page_symlink(inode, symname, i);
  if (err)
    goto out_fail;

  /* add vfs inode into vfs dentry */
  err = add_nondir(dentry, inode);
 out:
  return err;

 out_fail:
  /* back out of link: cut down reference count (to zero) */
  inode_dec_link_count(inode);
  /* flush changes */
  iput(inode);
  goto out;
}

/**
 * wufs_mkdir: (vfs directory inode operation)
 * Create a new inode for a directory, patterned after dentry, in a directory.
 *   - dir is the inode of the containing directory
 *   - dentry provides the template for the link
 *   - mode is the access permission
 */
static int wufs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
  struct inode *inode;
  int err = -EMLINK;

  /* the new directory should not have too many links
   * remember: subdirectories will link upward with ..
   */
  if (dir->i_nlink >= wufs_sb(dir->i_sb)->sbi_link_max)
    goto out;

  /* increment number references in parent directory */
  inode_inc_link_count(dir);

  /* allocate a new inode to hold the directory */
  inode = wufs_new_inode(dir, &err);
  if (!inode)
    goto out_dir;

  /* this new inode is a directory, with the appropriate mode */
  inode->i_mode = S_IFDIR | mode;

  /* if "set group id" is set on the parent, inherit that in the child */
  if (dir->i_mode & S_ISGID)
    inode->i_mode |= S_ISGID;

  /* set the operations on the new directory to directory inode operations */
  wufs_set_inode(inode, 0);

  /* increment the number of links to inode (should now be...1) */
  inode_inc_link_count(inode);
  
  /* create the "." and ".." files in the directory */
  err = wufs_make_empty(inode, dir);
  if (err)
    goto out_fail;

  /* add a link to the directory from dentry */
  err = wufs_add_link(dentry, inode);
  if (err)
    goto out_fail;

  /* now, create a dentry cache entry */
  d_instantiate(dentry, inode);
 out:
  return err;

 out_fail:
  /* backout from directory and "." linking */
  inode_dec_link_count(inode);
  inode_dec_link_count(inode);
  iput(inode);
 out_dir:
  /* backout from subdirectory ".." linking addition */
  inode_dec_link_count(dir);
  goto out;
}

/**
 * wufs_rmdir: (vfs directory inode operation)
 * Remove, from dir, a subdirectory dentry.
 */
static int wufs_rmdir(struct inode *dir, struct dentry *dentry)
{
  struct inode *inode = dentry->d_inode;
  int err = -ENOTEMPTY;

  /* check for an empty directory (necessary before removal possible) */
  if (wufs_empty_dir(inode)) {
    /* remove the link from dir to dentry */
    err = wufs_unlink(dir, dentry);
    if (!err) {
      /* decrement the number of entries in the directory */
      inode_dec_link_count(dir);
      /* decrement the reference count to the subdirectory */
      inode_dec_link_count(inode);
    }
  }
  return err;
}

/**
 * wufs_mknod: (vfs directory inode operation)
 * Create a new disk inode for a special file associated with dentry in dir.
 * mode is the mode, rdev is (major,minor) device number.
 */
static int wufs_mknod(struct inode *dir, struct dentry *dentry, int mode,
		      dev_t rdev)
{
  int error;
  struct inode *inode;

  /* validate the specified device */
  if (!old_valid_dev(rdev))
    return -EINVAL;

  /* allocate a new inode */
  inode = wufs_new_inode(dir, &error);

  if (inode) { /* valid inode found */
    /* set mode (including the file type) */
    inode->i_mode = mode;
    /* set file-specific inode operations */
    wufs_set_inode(inode, rdev);
    /* mark and flush inode */
    mark_inode_dirty(inode);
    /* add inode to dentry */
    error = add_nondir(dentry, inode);
  }
  return error;
}

/**
 * wufs_rename: (vfs directory inode operation)
 * Move a file old_dentry in old_dir to new_dentry in new_dir.
 */
static int wufs_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry)
{
  /* get file system information */
  struct wufs_sb_info * info = wufs_sb(old_dir->i_sb);

  /* get inodes associated with old and new file entries */
  struct inode *old_inode = old_dentry->d_inode;
  struct inode *new_inode = new_dentry->d_inode;
  struct page *dir_page = NULL;
  struct wufs_dirent * dir_de = NULL;
  struct page *old_page;
  struct wufs_dirent * old_de;
  int err = -ENOENT;

  /* find the WUFS "raw" dentry for the old file */
  old_de = wufs_find_entry(old_dentry, &old_page);
  if (!old_de)
    goto out;

  /* if it is a directory... */
  if (S_ISDIR(old_inode->i_mode)) {
    err = -EIO;
    /* get directory entry of ".." in the source directory (must be changed) */
    dir_de = wufs_dotdot(old_inode, &dir_page);
    if (!dir_de)
      goto out_old;
  }

  /*
   * If there's an inode associated with the destination...
   */
  if (new_inode) {
    struct page *new_page;
    struct wufs_dirent *new_de;

    /* if source is a directory and the destination is not empty, fail */
    err = -ENOTEMPTY;
    if (dir_de && !wufs_empty_dir(new_inode))
      goto out_dir;

    /* get raw WUFS dirent for the destination */
    err = -ENOENT;
    new_de = wufs_find_entry(new_dentry, &new_page);
    if (!new_de)
      goto out_dir;

    /* form a hard link to old inode */
    inode_inc_link_count(old_inode);
    wufs_set_link(new_de, new_page, old_inode);
    new_inode->i_ctime = CURRENT_TIME_SEC;
    /* decrement references to what new dentry used to point to */
    if (dir_de)
      drop_nlink(new_inode);
    inode_dec_link_count(new_inode);
  } else {
    /* we're moving to a non-directory */
    if (dir_de) {
      err = -EMLINK;
      /* check to make sure destination can accept another link (..) */
      if (new_dir->i_nlink >= info->sbi_link_max)
	goto out_dir;
    }
    /* increment the link count to the old inode (the new directory) */
    inode_inc_link_count(old_inode);
    /* link destination to old inode */
    err = wufs_add_link(new_dentry, old_inode);
    if (err) {
      inode_dec_link_count(old_inode);
      goto out_dir;
    }
    /* increment the link count to the new directory (..) */
    if (dir_de) /* source had a dot-dot */
      inode_inc_link_count(new_dir);
  }

  /* unlink old file entry */
  wufs_delete_entry(old_de, old_page);
  /* decrement the link count to the old inode */
  inode_dec_link_count(old_inode);

  if (dir_de) { /* source had a dot-dot */
    /* source dot-dot points to new directory */
    wufs_set_link(dir_de, dir_page, new_dir);
    /* remove link from source directory */
    inode_dec_link_count(old_dir);
  }
  return 0;

 out_dir:
  if (dir_de) {
    kunmap(dir_page);
    page_cache_release(dir_page);
  }
 out_old:
  /* back out of page references */
  kunmap(old_page);
  page_cache_release(old_page);
 out:
  return err;
}

/**
 * add_nondir: (utility routine)
 * Link directory entry to an inode.
 */
static int add_nondir(struct dentry *dentry, struct inode *inode)
{
  /* perform the link */
  int err = wufs_add_link(dentry, inode);
  if (!err) {
    /* follow up by adding dentry to the dcache */
    d_instantiate(dentry, inode);
    return 0;
  }
  /* back out: decrement link count */
  inode_dec_link_count(inode);
  /* possibly release target inode */
  iput(inode);
  return err;
}
