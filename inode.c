/*
 * Module support for the Williams Ultra-buntu File System
 * (c) 2011, 2015 duane a. bailey
 * (c) 1991, 1992 linus torvalds
 */

#include <linux/module.h>
#include "wufs.h"
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/highuid.h>
#include <linux/vfs.h>

/*
 * Global routines
 */
void          wufs_set_inode(struct inode *inode, dev_t rdev);
int         __wufs_write_begin(struct file *file, struct address_space *mapping,
			       loff_t pos, unsigned len, unsigned flags,
			       struct page **pagep, void **fsdata);
struct inode *wufs_iget(struct super_block *sb, unsigned long ino);

/*
 * Local routines.
 */
static void                init_once(void *foo);
static int                 init_inodecache(void);
static void                destroy_inodecache(void);

static sector_t 	   wufs_bmap(struct address_space *mapping,
				     sector_t block);
static struct inode       *wufs_alloc_inode(struct super_block *sb);
static void                wufs_delete_inode(struct inode *inode);
static void                wufs_destroy_inode(struct inode *inode);
static int		   wufs_fill_super(struct super_block *s, void *data, int silent);
static int                 wufs_get_sb(struct file_system_type *fs_type,
				       int flags, const char *dev_name,
				       void *data, struct vfsmount *mnt);
static struct inode       *wufs_iget0(struct inode *inode);
static void		   wufs_put_super(struct super_block *sb);
static int		   wufs_readpage(struct file *file, struct page *page);
static int                 wufs_remount (struct super_block * sb,
					 int * flags, char * data);
static int                 wufs_statfs(struct dentry *dentry,
				       struct kstatfs *buf);
static struct buffer_head *wufs_update_inode(struct inode * inode);
static int                 wufs_write_inode(struct inode * inode, int wait);
static int                 wufs_writepage(struct page *page,
					  struct writeback_control *wbc);

static int                 wufs_write_begin(struct file *file,
					    struct address_space *mapping,
					    loff_t pos, unsigned len,
					    unsigned flags, struct page **pagep,
					    void **fsdata);
/*
 * global variables
 */

/**
 * wufs_fs_type:
 * The file system descriptor.
 * This object is used by register_filesystem to link the WUFS driver into
 * the kernel's list of acceptable file systems.
 */
static struct file_system_type wufs_fs_type = {
  .owner	= THIS_MODULE,
  .name		= "wufs",	    /* woof. */
  .get_sb	= wufs_get_sb,      /* mount routine */
  .kill_sb	= kill_block_super, /* unmount routine (see fs/super.c)*/
  .fs_flags	= FS_REQUIRES_DEV,  /* this system requires a block device */
};

/**
 * wufs_inode_cachep:
 * The kernel slab cache that holds wufs inode info structures
 */
static struct kmem_cache *wufs_inode_cachep;

/**
 * wufs_sops: (superblock operations)
 * VFS operations supported by WUFS; superblock virtual function table.
 * (see linux/Documentation/filesystems/vfs.txt for overview)
 */
static const struct super_operations wufs_sops = {
  .alloc_inode	 = wufs_alloc_inode,
  .destroy_inode = wufs_destroy_inode,
  .write_inode	 = wufs_write_inode,
  .delete_inode	 = wufs_delete_inode,
  .put_super	 = wufs_put_super,
  .statfs	 = wufs_statfs,
  .remount_fs	 = wufs_remount,
};

/**
 * wufs_aops: (address space operations)
 * Virtual funtion table for managing memory pages for this driver.
 */
static const struct address_space_operations wufs_aops = {
  .readpage    = wufs_readpage,
  .writepage   = wufs_writepage,
  .sync_page   = block_sync_page,
  .write_begin = wufs_write_begin,
  .write_end   = generic_write_end,
  .bmap        = wufs_bmap
};



/***********************************************************************
 * inode methods.
 */

/**
 * init_wufs_fs:
 * This module initialization routine called when the module is first loaded:
 *  1. allocate a cache for inodes.
 *  2. register this module as supporting a new filesystem.
 * This method is identified by the __init keyword and a reference at the
 * bottom of this file.
 */
static int __init init_wufs_fs(void)
{
  /* allocate the cache */
  int err = init_inodecache();
  if (err) return err;

  /* register the filesystem */
  err = register_filesystem(&wufs_fs_type);
  if (err) {
    destroy_inodecache();
    return err;
  }
  printk("WUFS: filesystem module loaded.\n");
  return 0;
}

/**
 * exit_wufs_fs:
 * This routine is called when the module is unloaded.
 * Simply backwards of loading routine.
 */
static void __exit exit_wufs_fs(void)
{
  unregister_filesystem(&wufs_fs_type);
  destroy_inodecache();
  printk("WUFS: filesystem module unloaded.\n");
}

/**
 * init_inodecache:
 * Setup a kernel slab memory cache to hold inode_info structures.
 */
static int init_inodecache(void)
{
  /* point to a kernel cache of inode info blocks from the slab allocator
   * you can follow this cache in /proc/slabinfo
   */
  wufs_inode_cachep = kmem_cache_create("wufs_inode_cache",
					sizeof(struct wufs_inode_info),
					0, 
					(SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD),
					init_once);
  if (!wufs_inode_cachep) return -ENOMEM;
  return 0;
}

/**
 * destroy_inodecache:
 * Tear down the inode cache.
 */
static void destroy_inodecache(void)
{
  kmem_cache_destroy(wufs_inode_cachep);
}

/**
 * init_once:
 * This is the inode_info constructor; called on demand from the allocator.
 */
static void init_once(void *foo)
{
  /* inode info maintains a reference to the vfs inode, which must be inited */
  struct wufs_inode_info *ei = (struct wufs_inode_info *) foo;

  inode_init_once(&ei->ini_vfs_inode);
}

/**
 * wufs_get_sb:
 * This routine (pointed to by the file_system_type, above) is called when
 * there is a request to mount the file system.
 *   fs_type has been partially filled out by VFS
 *   flags encodes the mount flags
 *   dev_name is the name of the device being mounted
 *   mnt is a string describing the mount options
 */
static int wufs_get_sb(struct file_system_type *fs_type,
		       int flags, const char *dev_name, void *data,
		       struct vfsmount *mnt)
{
  /* call the generic vfs method (in super.c) with the help of wufs_fill_super */
  return get_sb_bdev(fs_type, flags, dev_name, data, wufs_fill_super, mnt);
}

/**
 * wufs_fill_super:
 * This helper routine fills out the fields of the vfs super_block structure
 * from information found in the on-disk WUFS superblock.
 * Since the WUFS superblock is fairly simple, some fields will have to be
 * constructed from whole cloth.
 *  s - the vfs superblock structure, off which hangs the information about
 *      the file system, including the wufs-specific superblock info struct
 */
static int wufs_fill_super(struct super_block *s, void *data, int silent)
{
  struct buffer_head *bh;
  struct buffer_head **map;
  struct wufs_super_block *ms;
  unsigned long i, block;
  struct inode *root_inode;
  struct wufs_sb_info *sbi;
  int ret = -EINVAL;

  /*
   * The sbi is device-specific "information" associated with the superblock.
   * We use this structure to keep track of information we would otherwise have
   * to find from the disk copy; it's a cache for whatever data we want
   * to access quickly.
   * See wufs.h for the definition.
   * Carefully check allocations in the kernel.
   */
  sbi = kzalloc(sizeof(struct wufs_sb_info), GFP_KERNEL);
  if (!sbi) { return -ENOMEM; }
  /* link it into the vfs superblock */
  s->s_fs_info = sbi;

  /* Set the optimal transfer size for the device.
   * Currently, BLOCK_SIZE is 1024 (see fs.h)
   */
  if (!sb_set_blocksize(s, BLOCK_SIZE)) goto out_bad_hblock;

  /* Read block 1 (the second block) from the disk; this contains the superblock */
  if (!(bh = sb_bread(s, 1))) goto out_bad_sb;

  /* This is the on-disk super block, as defined in wufs_fs.h */
  ms = (struct wufs_super_block *) bh->b_data;
  sbi->sbi_ms = ms;		/* the mounted super block */
  sbi->sbi_sbh = bh;		/* memory associated with the disk block */

  /* these fields are clones of fields in the on-disk super block */
  sbi->sbi_state = ms->sb_state;
  sbi->sbi_inodes = ms->sb_inodes;
  sbi->sbi_blocks = ms->sb_blocks;
  sbi->sbi_imap_bcnt = ms->sb_imap_bcnt;
  sbi->sbi_bmap_bcnt = ms->sb_bmap_bcnt; /* n.b. this maps *all* blocks on disk */
  sbi->sbi_first_block = ms->sb_first_block;
  sbi->sbi_max_fsize = ms->sb_max_fsize;
  sbi->sbi_max_fblks = (ms->sb_max_fsize+WUFS_BLOCKSIZE-1)/WUFS_BLOCKSIZE;
  s->s_magic = 0x0fff & ms->sb_magic;
  if (s->s_magic == WUFS_MAGIC) {
    sbi->sbi_version = (ms->sb_magic >> 12) & 0x000f;
    printk("WUFS: Version 0x%x file system detected.\n",sbi->sbi_version);
    
    /* you might make the following conditional, based on version: */
    sbi->sbi_dirsize = WUFS_DIRENTSIZE;
    sbi->sbi_namelen = WUFS_NAMELEN;

    sbi->sbi_link_max = WUFS_LINK_MAX; /* Maximum number of links to a single file */
  } else {
      printk("WUFS: Bad magic: was 0x%04x should be 0x*%03x.\n",ms->sb_magic,
	     WUFS_MAGIC);
    goto out_no_fs;
  }

  /*
   * Allocate the inode and disk map buffers.
   */
  if (sbi->sbi_imap_bcnt == 0 || sbi->sbi_bmap_bcnt == 0) goto out_illegal_sb;
  i = (sbi->sbi_imap_bcnt + sbi->sbi_bmap_bcnt) * sizeof(bh);
  map = kzalloc(i, GFP_KERNEL);
  if (!map) goto out_no_map;
  sbi->sbi_imap = map;
  sbi->sbi_bmap = map + sbi->sbi_imap_bcnt;

  /* now, begin reading map blocks.  inode map starts at block 2 */
  block=2;
  for (i=0; i < sbi->sbi_imap_bcnt; i++) {
    sbi->sbi_imap[i] = sb_bread(s, block);
    if (!sbi->sbi_imap[i]) goto out_no_bitmap;
    block++;
  }

  /* follow with the data block map */
  for (i=0 ; i < sbi->sbi_bmap_bcnt ; i++) {
    sbi->sbi_bmap[i] = sb_bread(s, block);
    if (!sbi->sbi_bmap[i]) goto out_no_bitmap;
    block++;
  }

  /*
   * We now begin filling out the vfs superblock.
   * Hook up the operations to bootstrap functionality of superblock routines.
   */
  s->s_op = &wufs_sops;

  /* read the root inode data */
  root_inode = wufs_iget(s, WUFS_ROOT_INODE);
  if (IS_ERR(root_inode)) {
    ret = PTR_ERR(root_inode);
    goto out_no_root;
  }

  /* allocate the root inode in the dcache (see dcache.c) */
  ret = -ENOMEM;
  s->s_root = d_alloc_root(root_inode);
  if (!s->s_root) goto out_iput;

  /*
   * If the file system is not mounted read-only, we're about to dirty it.
   */
  if (!(s->s_flags & MS_RDONLY)) {
    ms->sb_state &= ~WUFS_VALID_FS;
    mark_buffer_dirty(bh);
  }

  /*
   * If the file system as marked on disk was not valid or had errors, warn
   */  
  if (!(sbi->sbi_state & WUFS_VALID_FS)) {
    printk("WUFS: mounting unchecked file system, run fsck!\n");
  } else if (sbi->sbi_state & WUFS_ERROR_FS) {
    printk("WUFS: mounting file system with errors, run fsck!\n");
  }
  return 0;

 out_iput:
  /* unreference root_inode */
  iput(root_inode);
  goto out_freemap;

 out_no_root:
  if (!silent) printk("WUFS: get root inode failed\n");
  goto out_freemap;

 out_no_bitmap:
  printk("WUFS: bad superblock or unable to read bitmaps\n");

 out_freemap:
  /* release block buffers read from disk that hold bitmaps */
  for (i = 0; i < sbi->sbi_imap_bcnt; i++)
    brelse(sbi->sbi_imap[i]);
  for (i = 0; i < sbi->sbi_bmap_bcnt; i++)
    brelse(sbi->sbi_bmap[i]);
  kfree(sbi->sbi_imap);
  goto out_release;

 out_no_map:
  ret = -ENOMEM;
  if (!silent) printk("WUFS: can't allocate map\n");
  goto out_release;

 out_illegal_sb:
  if (!silent) printk("WUFS: bad superblock\n");
  goto out_release;

 out_no_fs:
  if (!silent) printk("VFS: Can't find a WUFS filesystem on device %s.\n",
		      s->s_id);
 out_release:
  /* release superblock buffer */
  brelse(bh);
  goto out;

 out_bad_hblock:
  printk("WUFS: blocksize too small for device\n");
  goto out;

 out_bad_sb:
  printk("WUFS: unable to read superblock\n");

 out:
  /* WUFS-specific release superblock information */
  s->s_fs_info = NULL;
  kfree(sbi);
  return ret;
}

/**
 * wufs_put_super:
 * File system is unmounting; free the superblock and associated info.
 */
static void wufs_put_super(struct super_block *sb)
{
  int i;
  struct wufs_sb_info *sbi = wufs_sb(sb);

  /* if this filesystem is read/write, we flush back the state in the sb */
  if (!(sb->s_flags & MS_RDONLY)) {
    /* write the state back to superblock disk buffer */
    sbi->sbi_ms->sb_state = sbi->sbi_state;
    /* target the buffer for flushing to disk */
    mark_buffer_dirty(sbi->sbi_sbh);
  }

  /* free the blocks used for holding bitmaps (they were marked dirty
   * (if necessary) in bitmap handling routines
   */
  for (i = 0; i < sbi->sbi_imap_bcnt; i++)
    brelse(sbi->sbi_imap[i]);
  for (i = 0; i < sbi->sbi_bmap_bcnt; i++)
    brelse(sbi->sbi_bmap[i]);

  /* free the superblock header */
  brelse (sbi->sbi_sbh);

  /* free the imap (and bmap; they're together; see above) map block array */
  kfree(sbi->sbi_imap);
  
  /* unlink the info from the superblock */
  sb->s_fs_info = NULL;

  /* free the information */
  kfree(sbi);
}

/**
 * wufs_iget: (WUFS internal inode get function)
 * The global function to read an inode.
 */
struct inode *wufs_iget(struct super_block *sb, unsigned long ino)
{
  struct inode *inode;

  inode = iget_locked(sb, ino);
  if (!inode) return ERR_PTR(-ENOMEM);
  if (!(inode->i_state & I_NEW)) return inode;

  /* fill in inode state from hard disk */
  return wufs_iget0(inode);
}

/**
 * wufs_iget0: (a helper function for wufs_iget)
 * The wufs function to fill out the fields of a vfs inode based on
 * actual inode values found on disk (the "raw" inode) data
 */
static struct inode *wufs_iget0(struct inode *inode)
{
  struct buffer_head *bh;
  struct wufs_inode *raw_inode;
  struct wufs_inode_info *wufs_inode = wufs_i(inode);
  int i;

  /* fetch the "raw" inode from the disk; point bh as the buffer used */
  raw_inode = wufs_raw_inode(inode->i_sb, inode->i_ino, &bh);
  if (!raw_inode) {
    iget_failed(inode);
    return ERR_PTR(-EIO);
  }

  /* we now copy over data from the WUFS inode to the VFS inode */
  inode->i_mode = raw_inode->in_mode; /* mode */
  inode->i_uid = raw_inode->in_uid;   /* owner */
  inode->i_gid = raw_inode->in_gid;   /* owning group */
  inode->i_nlink = raw_inode->in_nlinks; /* count of hard links */
  inode->i_size = raw_inode->in_size;	 /* size (in bytes) */

  /*
   * the U in WUFS stands for underpowered
   * the time field has the granularity of one second, and is used for
   * create, access, & modification time
   * *sigh*
   */
  inode->i_mtime.tv_sec = inode->i_atime.tv_sec = inode->i_ctime.tv_sec =
    raw_inode->in_time;
  inode->i_mtime.tv_nsec = 0;
  inode->i_atime.tv_nsec = 0;
  inode->i_ctime.tv_nsec = 0;

  /* compute the number of 512-byte blocks used by the file */
  inode->i_blocks = 0;

  /* copy over the data block pointers */
  for (i = 0; i < WUFS_INODE_BPTRS; i++)
    wufs_inode->ini_data[i] = raw_inode->in_block[i];

  /* now, set the inode operations (based on file/device type)
   * n.b. if this inode is a device (signaled in mode), then the
   * first block pointer is used to store the major & minor device numbers
   * (old_decode_dev: see <linux/kdev_t.h>)
   */
  wufs_set_inode(inode, old_decode_dev(raw_inode->in_block[0]));

  /* free raw inode buffer and return */
  brelse(bh);
  unlock_new_inode(inode);
  return inode;
}

/**
 * wufs_write_inode: (vfs superblock operation)
 * Write inode back to disk.
 */
static int wufs_write_inode(struct inode *inode, int wait)
{
  int err = 0;
  struct buffer_head *bh;

  /* update the node */
  bh = wufs_update_inode(inode);
  if (!bh) return -EIO; /* disk version of inode not found */
  
  /* if the wait parameter is set, we synchronize now */
  if (wait && buffer_dirty(bh)) {
    sync_dirty_buffer(bh);
    if (buffer_req(bh) && !buffer_uptodate(bh)) {
      printk("IO error syncing wufs inode [%s:%08lx]\n",
	     inode->i_sb->s_id, inode->i_ino);
      err = -EIO;
    }
  }

  /* release the disk version of the inode */
  brelse (bh);
  return err;
}

/**
 * wufs_update_inode:
 * The wufs function to synchronize an inode: copy in-memory inode
 * data back to the disk version, then flush the disk version back to disk.
 */
static struct buffer_head *wufs_update_inode(struct inode * inode)
{
  struct buffer_head * bh;
  struct wufs_inode * raw_inode;
  struct wufs_inode_info *wufs_inode = wufs_i(inode);
  int i;

  /* fetch the disk version of this inode */
  raw_inode = wufs_raw_inode(inode->i_sb, inode->i_ino, &bh);
  if (!raw_inode) return NULL;

  /* copy over all data */
  raw_inode->in_mode = inode->i_mode;

  /* convert to 16bit uid forms */
  raw_inode->in_uid = fs_high2lowuid(inode->i_uid);
  raw_inode->in_gid = fs_high2lowgid(inode->i_gid);
  raw_inode->in_nlinks = inode->i_nlink;
  raw_inode->in_size = inode->i_size;

  /* for times we depend on the modification time. */
  raw_inode->in_time = inode->i_mtime.tv_sec;

  /* nonregular files have the initial block pointer representing device */
  if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
    raw_inode->in_block[0] = old_encode_dev(inode->i_rdev);
  else {
    /* regular and disk files: copy back the block references */
    for (i = 0; i < WUFS_INODE_BPTRS; i++)
	 raw_inode->in_block[i] = wufs_inode->ini_data[i];
  }

  /* push back the inode data to disk*/
  mark_buffer_dirty(bh);
  return bh;
}

/**
 * wufs_alloc_inode: (vfs superblock operation)
 * Allocate inode information associated with an inode.
 * Returned by wufs_destroy_inode.
 */
static struct inode *wufs_alloc_inode(struct super_block *sb)
{
  struct wufs_inode_info *ei;
  /* allocate kernel memory */
  ei = (struct wufs_inode_info *)kmem_cache_alloc(wufs_inode_cachep, GFP_KERNEL);
  if (!ei) return NULL;

  /* return pointer to associated inode */
  return &ei->ini_vfs_inode;
}

/**
 * wufs_destroy_inode: (vfs superblock operation)
 * Inode info deallocator.
 */
static void wufs_destroy_inode(struct inode *inode)
{
  kmem_cache_free(wufs_inode_cachep, wufs_i(inode));
}

/**
 * wufs_delete_inode: (vfs superblock operation)
 * Called when the VFS wants to delete an inode.
 */
static void wufs_delete_inode(struct inode *inode)
{
  /* truncates an address space mapping, starting at offset 0
   * (required see fs/inode.c)
   */
  truncate_inode_pages(&inode->i_data, 0);
  inode->i_size = 0;
  wufs_truncate(inode);
  wufs_free_inode(inode);
}



/**
 * wufs_remount: (vfs superblock operation)
 * Called when a filesystem is "remounted".  The file system is already
 * mounted, but it's being remounted with different parameters (typically
 * read v. read/write access).
 */
static int wufs_remount (struct super_block * sb, int * flags, char * data)
{
  struct wufs_sb_info * sbi = wufs_sb(sb);
  struct wufs_super_block * ms;

  /* here's the on-disk superblock */
  ms = sbi->sbi_ms;

  /* if the system is read only, and staying that way, cool. */
  if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
    return 0;

  /* something's changing */
  if (*flags & MS_RDONLY) {
    /* we moving to readonly...
     * either
     *  1. on-disk system is valid, or
     *  2. it was invalid, and remains that way
     */
    if (ms->sb_state & WUFS_VALID_FS ||	!(sbi->sbi_state & WUFS_VALID_FS))
      return 0;

    /* otherwise, flush the state out to disk */
    ms->sb_state = sbi->sbi_state;
    mark_buffer_dirty(sbi->sbi_sbh);
  } else {
    /* mount a partition which is read-only, read-write. */
    /* first, capture state and mark disk copy invalid */
    sbi->sbi_state = ms->sb_state;
    ms->sb_state &= ~WUFS_VALID_FS;
    mark_buffer_dirty(sbi->sbi_sbh);

    /* if it was invalid, warn user */
    if (!(sbi->sbi_state & WUFS_VALID_FS))
      printk("WUFS warning: remounting unchecked fs, run fsck!\n");
    else if ((sbi->sbi_state & WUFS_ERROR_FS))
      printk("WUFS warning: remounting fs with errors, run fsck!\n");
  }
  return 0;
}

/**
 * wufs_statfs: (vfs superblock operation)
 * Gather statistics about the filesystem based on any file that sits
 * within the filesystem.
 */
static int wufs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
  /* get the filesystem from the dentry */
  struct super_block *sb = dentry->d_sb;

  /* grab the WUFS associated information */
  struct wufs_sb_info *sbi = wufs_sb(sb);
  
  /* get the device id of the file system */
  u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
  buf->f_fsid.val[0] = (u32)id;
  buf->f_fsid.val[1] = (u32)(id >> 32);

  /* get the file ssytem type */
  buf->f_type = sb->s_magic;

  /* get the optimal size of the blocks on the disk */
  buf->f_bsize = sb->s_blocksize;

  /* get the useful data space */
  buf->f_blocks = sbi->sbi_blocks - sbi->sbi_first_block+1;

  printk("WUFS: statfs reporting %llu blocks available.\n",buf->f_blocks);

  /* get the number of free blocks (see bitmap.c) */
  buf->f_bfree = wufs_count_free_blocks(sbi);
  
  printk("WUFS: reporting %llu free blocks available.\n",buf->f_bfree);

  /* number of these blocks available to normal users (all) */
  buf->f_bavail = buf->f_bfree;

  /* number of file nodes in system */
  buf->f_files = sbi->sbi_inodes;

  /* number of inodes that are free */
  buf->f_ffree = wufs_count_free_inodes(sbi);
  printk("WUFS: reporting %llu free inodes available.\n",buf->f_ffree);

  /* maximum length of file names on this device */
  buf->f_namelen = sbi->sbi_namelen;
  return 0;
}

/**
 * wufs_writepage: (address space operation)
 * This writes a page back to disk.
 */
static int wufs_writepage(struct page *page, struct writeback_control *wbc)
{
  return block_write_full_page(page, wufs_get_blk, wbc);
}

/**
 * wufs_readpage: (address space operation)
 * Read a page from disk.
 */
static int wufs_readpage(struct file *file, struct page *page)
{
  return block_read_full_page(page,wufs_get_blk);
}

/**
 * wufs_write_begin: (address space operation)
 * Prepare for an upcoming write -- perhaps by fetching adjacent blocks
 * to allow a full write to begin.
 */
static int wufs_write_begin(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned flags,
			    struct page **pagep, void **fsdata)
{
  *pagep = NULL;
  return __wufs_write_begin(file, mapping, pos, len, flags, pagep, fsdata);
}

/**
 * __wufs_write_begin: (generic helper function)
 * WUFS-internal function to prepare for write to backing store.
 */
int __wufs_write_begin(struct file *file, struct address_space *mapping,
		       loff_t pos, unsigned len, unsigned flags,
		       struct page **pagep, void **fsdata)
{
  return block_write_begin(file, mapping, pos, len, flags, pagep, fsdata,
			   wufs_get_blk);
}

/**
 * wufs_bmap: (address space operation)
 * Convert between logical block numbers and physical blocks addresses.
 */
static sector_t wufs_bmap(struct address_space *mapping, sector_t block)
{
  return generic_block_bmap(mapping,block,wufs_get_blk);
}

/**
 * wufs_set_inode:
 * Set the operations for the particular inode based on the type of file.
 */
void wufs_set_inode(struct inode *inode, dev_t rdev)
{
  if (S_ISREG(inode->i_mode)) {
    /* these file operations are described in file.c */
    inode->i_op = &wufs_file_inode_operations;
    inode->i_fop = &wufs_file_operations;
    inode->i_mapping->a_ops = &wufs_aops;
  } else if (S_ISDIR(inode->i_mode)) {
    /* most directory-inode operations found in namei.c */
    inode->i_op = &wufs_dir_inode_operations;
    /* most directory operations found in dir.c */
    inode->i_fop = &wufs_dir_operations;
    inode->i_mapping->a_ops = &wufs_aops;
  } else if (S_ISLNK(inode->i_mode)) {
    /* this symlink operations found in fs/file.c */
    inode->i_op = &wufs_symlink_inode_operations;
    inode->i_mapping->a_ops = &wufs_aops;
  } else {
    /* see fs/inode.c */
    init_special_inode(inode, inode->i_mode, rdev);
  }
}

module_init(init_wufs_fs)
module_exit(exit_wufs_fs)
MODULE_LICENSE("GPL");

