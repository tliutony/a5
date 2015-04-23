/*
 * Module-private definitions for the Williams Universal File System.
 * (c) 2011, 2015 duane a. bailey
 *
 * General note:
 *   All WUFS in-memory struct field names have three letter (sbi, ini, dei)
 *   prefixes.
 *   Compare this with VFS field names which have one letter prefixes.
 */
#ifndef FS_WUFS_H
#define FS_WUFS_H
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "wufs_fs.h"

/**
 * wufs_inode_info:
 * wufs fs inode data in memory
 */
struct wufs_inode_info {
  __u16        ini_data[WUFS_INODE_BPTRS];
  struct inode ini_vfs_inode;
};

/*
 * associated wufs super-block data in memory
 */
struct wufs_sb_info {
  /* info directly from fs header: */
  unsigned short       sbi_state; /* uninitialized, clean, or errorful */
  unsigned long        sbi_blocks; /* block count */
  unsigned long        sbi_first_block; /* first data block lba */
  unsigned long        sbi_inodes;	/* count of inodes */
  unsigned long        sbi_imap_bcnt;	/* block count of inode map */
  struct buffer_head **sbi_imap;	/* pointer to blocks of inode map */
  unsigned long        sbi_bmap_bcnt;   /* block count of block map */
  struct buffer_head **sbi_bmap;        /* pointer to blocks of block map */

  /* WUFS inode information */
  unsigned int sbi_version;	/* version number (high nibble of magic) */
  unsigned long sbi_max_fsize;	/* maximum file size, on this file system */
  unsigned long sbi_max_fblks;	/* maximum file size (blocks), on this file system */
  int           sbi_link_max;	/* maximum number of links (silly) */

  /* WUFS dirent information */
  int sbi_dirsize;	/* size of directory entries */
  int sbi_namelen;	/* limit on file name length */

  /* slab pointers to cached superblock */
  struct buffer_head      *sbi_sbh;	/* pointer to buffer head for super */
  struct wufs_super_block *sbi_ms;	/* above, cast as a superblock ptr */
};

/***********************************************************************
 * shared routines
 */

/*
 * From bitmap.c:
 */
extern void               wufs_free_block(struct inode *inode,
					  unsigned long block);
extern int                wufs_new_block(struct inode * inode);
extern unsigned long      wufs_count_free_blocks(struct wufs_sb_info *sbi);
extern void               wufs_free_inode(struct inode * inode);
extern struct wufs_inode *wufs_raw_inode(struct super_block *, ino_t,
					    struct buffer_head **);
extern struct inode      *wufs_new_inode(const struct inode * dir,
					 int * error);
extern unsigned long      wufs_count_free_inodes(struct wufs_sb_info *sbi);

/*
 * From dir.c
 */
extern int                 wufs_add_link(struct dentry*, struct inode*);
extern int                 wufs_delete_entry(struct wufs_dirent*,
					     struct page*);
extern struct wufs_dirent *wufs_dotdot(struct inode*,
				       struct page**);
extern int                 wufs_empty_dir(struct inode*);
extern struct wufs_dirent *wufs_find_entry(struct dentry*, struct page**);
extern ino_t               wufs_inode_by_name(struct dentry*);
extern int                 wufs_make_empty(struct inode*, struct inode*);
extern void                wufs_set_link(struct wufs_dirent*,
					 struct page*, struct inode*);

/*
 * From inode.c:
 */
extern void          wufs_set_inode(struct inode *, dev_t);
extern int         __wufs_write_begin(struct file *file,
			      struct address_space *mapping,
			      loff_t pos, unsigned len,
			      unsigned flags, struct page **pagep,
			      void **fsdata);
extern struct inode *wufs_iget(struct super_block *, unsigned long);

/* From file.c */
extern void wufs_truncate_file(struct inode *);
extern int  wufs_getattr(struct vfsmount *, struct dentry *,
			 struct kstat *);

/* From direct/indirect.c: */
extern void		      wufs_truncate(struct inode *);
extern int                    wufs_get_blk(struct inode *, sector_t,
					struct buffer_head *, int);
extern unsigned               wufs_blocks(loff_t, struct super_block *);

/*
 * Shared structures: class vtables.
 */
extern const struct file_operations wufs_file_operations;        /* file.c */
extern const struct inode_operations wufs_file_inode_operations; /* file.c */
extern const struct inode_operations wufs_symlink_inode_operations;/* file.c */
extern const struct inode_operations wufs_dir_inode_operations;  /* namei.c */
extern const struct file_operations wufs_dir_operations;         /* dir.c */

/*
 * Inlined definitions
 */
static inline struct wufs_sb_info *wufs_sb(struct super_block *sb)
{
  return sb->s_fs_info;
}

static inline struct wufs_inode_info *wufs_i(struct inode *inode)
{
  return list_entry(inode, struct wufs_inode_info, ini_vfs_inode);
}

#endif /* FS_WUFS_H */
