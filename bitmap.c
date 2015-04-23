/*
 * Bitmap manipulation routines for the Williams Utilitarian File System (WUFS)
 * (c) 2011, 2015 duane a. bailey
 * (c) 1991 and 1992 linus torvalds
 *
 * These routines maintain the bitmaps that identify free inodes and blocks.
 * If a bit is zero, the entity is free.  If a bit is one, it's not available
 * either because it is used or is not a legal index.
 *
 * A subtle issue must be dealt with in this module.  By tradition the first
 * inode is numbered 1.  So, if there are 16 inodes on the file system for use
 * they have logical values 1 through 16.  They are represented, however, by
 * bits 0 through 15 respectively.  In addition, the inode structures
 * themselves are stored in an array-like form that is 0-origin indexed.  We
 * will refer to the inode *number* as the 1-origin value, while the inode
 * *index* is the 0-origin value.  Generally, we try to avoid exposing
 * anything other than the numbers to higher level (e.g. vfs) parts of the
 * file system hierarchy.
 *
 * Blocks are kept track of using a 0-origin logical block address (an lba
 * or block_t).
 */
#include <linux/buffer_head.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include "wufs.h"

/**
 * Exported routines.
 */
unsigned long      wufs_count_free_blocks(struct wufs_sb_info *sbi);
unsigned long      wufs_count_free_inodes(struct wufs_sb_info *sbi);
void               wufs_free_block(struct inode *inode, unsigned long block);
void               wufs_free_inode(struct inode * inode);
int                wufs_new_block(struct inode * inode);
struct inode      *wufs_new_inode(const struct inode * dir, int * error);
struct wufs_inode *wufs_raw_inode(struct super_block *sb, ino_t ino,
				     struct buffer_head **bh);

/*
 * Local routines
 */
static unsigned long count_free(struct buffer_head **map,unsigned numblocks);
static void          wufs_clear_inode(struct inode *inode);

/*
 * Note:
 * External bit operations used in this module are described in
 *  <linux-kernel-distro/Documentation/atomic_ops.txt>
 * In particular, all __x routines are non-atomic variants of x, typically
 * depending on more general locking strategies.
 */

/*
 * Global structures.
 */

/**
 * ztab: (local table)
 * This structure returns the number of bits that are zero for each byte value.
 */
static const int ztab[] = {
  /* 0x00-0x0f: */ 8,7,7,6,7,6,6,5,7,6,6,5,6,5,5,4,
  /* 0x10-0x1f: */ 7,6,6,5,6,5,5,4,6,5,5,4,5,4,4,3,
  /* 0x20-0x2f: */ 7,6,6,5,6,5,5,4,6,5,5,4,5,4,4,3,
  /* 0x30-0x3f: */ 6,5,5,4,5,4,4,3,5,4,4,3,4,3,3,2,
  /* 0x40-0x4f: */ 7,6,6,5,6,5,5,4,6,5,5,4,5,4,4,3,
  /* 0x50-0x5f: */ 6,5,5,4,5,4,4,3,5,4,4,3,4,3,3,2,
  /* 0x60-0x6f: */ 6,5,5,4,5,4,4,3,5,4,4,3,4,3,3,2,
  /* 0x70-0x7f: */ 5,4,4,3,4,3,3,2,4,3,3,2,3,2,2,1,
  /* 0x80-0x8f: */ 7,6,6,5,6,5,5,4,6,5,5,4,5,4,4,3,
  /* 0x90-0x9f: */ 6,5,5,4,5,4,4,3,5,4,4,3,4,3,3,2,
  /* 0xa0-0xaf: */ 6,5,5,4,5,4,4,3,5,4,4,3,4,3,3,2,
  /* 0xb0-0xbf: */ 5,4,4,3,4,3,3,2,4,3,3,2,3,2,2,1,
  /* 0xc0-0xcf: */ 6,5,5,4,5,4,4,3,5,4,4,3,4,3,3,2,
  /* 0xd0-0xdf: */ 5,4,4,3,4,3,3,2,4,3,3,2,3,2,2,1,
  /* 0xe0-0xef: */ 5,4,4,3,4,3,3,2,4,3,3,2,3,2,2,1,
  /* 0xf0-0xff: */ 4,3,3,2,3,2,2,1,3,2,2,1,2,1,1,0,
};

/* ZEROS:
 * return the number of zeros in a byte
 */
inline int ZEROS(char x) { return ztab[(__u8)x]; }

/**
 * bitmap_lock:
 * This lock is used to control short accesses to the bitmaps associated
 * with the WUFS filesystem.
 * This lock can cause a busy wait, with no preemption.
 */
static DEFINE_SPINLOCK(bitmap_lock);

/**
 * wufs_count_free_blocks: (utility function)
 * Count the number of zeros in the block bitmap.
 * We start at bit zero.
 */
unsigned long wufs_count_free_blocks(struct wufs_sb_info *sbi)
{
  /* count the number of bits that are zero in the bmap */
  return count_free(sbi->sbi_bmap, sbi->sbi_bmap_bcnt);
}

/**
 * wufs_count_free_inodes: (utility function)
 * Count the number of zeros in the inode bitmap.
 * We start at bit index 0 (corresponding to inode 1).
 */
unsigned long wufs_count_free_inodes(struct wufs_sb_info *sbi)
{
  return count_free(sbi->sbi_imap, sbi->sbi_imap_bcnt);
}

/**
 * count_free:
 * Counts the number of zero bits pointed to by a bitmap.
 * The structures that contain the bitmap are pointed to by map
 * but are (likely) not contiguous, so we have to chunk through
 * by blocks.
 */
static unsigned long count_free(struct buffer_head **map,
				unsigned numblocks)
{
  unsigned sum = 0, remaining;
  struct buffer_head *bh;
  __u8 *p;

  /* 
   * This code assumes that the bitmap takes up an even number of blocks.
   */
  while (numblocks--) {
    /* sanity check: all map entries should be defined */
    if (!(bh=*map++)) return 0;
    p = bh->b_data;
    remaining = bh->b_size;
    /* for each buffer_head, scan through the bytes (chars) and count zeros */
    while (remaining--) sum += ZEROS(*p++);
  }
  return(sum);
}

/** 
 * wufs_new_block: (utility function)
 * Allocate a new block on the disk.  Disk block numbering starts at 0,
 * but the first few blocks are always used to hold boot code, superblock,
 * etc.  We could, instead, start at first block.
 */
int wufs_new_block(struct inode * inode)
{
  /* grab the superblock info.. */
  struct wufs_sb_info *sbi = wufs_sb(inode->i_sb);

  /* determine how many bits of the bitmap are stored in each block */
  int bits_per_block = 8 * inode->i_sb->s_blocksize;
  int i;

  /* zip through the block map blocks */
  for (i = 0; i < sbi->sbi_bmap_bcnt; i++) {
    struct buffer_head *bh = sbi->sbi_bmap[i];
    int j;

    /* get exclusive access to bitmap */
    spin_lock(&bitmap_lock);

    /* returns the bit offset of the first zero bit, or just beyond if none */
    j = find_first_zero_bit((unsigned long *)bh->b_data, bits_per_block);
    if (j < bits_per_block) { /* found a free block */
      /* mark it allocated */
      __set_bit(j, (unsigned long*)bh->b_data); /* see <linux/Documentation/atomic_ops.txt> */
      spin_unlock(&bitmap_lock);

      /* push the bitmap back to the disk */
      mark_buffer_dirty(bh);

      /*
       * we now compute the actual bit offset from the beginning of the
       * entire bitmap; ie. compute the LBA of the disk block.
       * this should range from 0 <= j < sbi->sbi_blocks, *but* 
       * we only use this routine to allocate blocks at or after 
       * sbi->sbi_first_block, so 0 can be used to signal "not found".
       */
      j += i*bits_per_block;
      if (sbi->sbi_first_block <= j && j < sbi->sbi_blocks) {
	return j;
      } else {
	return 0;
      }
    }
    spin_unlock(&bitmap_lock);
  }
  return 0;
}

/**
 * wufs_free_block: (utility function)
 * Undoes the accounting of allocating a block.
 * Simply: clear the bit at the appropriate offset in the bitmap.
 */
void wufs_free_block(struct inode *inode, unsigned long block)
{
  /* grab our local info structures */
  struct super_block *sb = inode->i_sb;
  struct wufs_sb_info *sbi = wufs_sb(sb);
  struct buffer_head *bh;
  int bits_per_block = 8 * inode->i_sb->s_blocksize;
  unsigned long bit, mapBlock;
  int previous;

  /* sanity check: we're only working with data blocks */
  if (block < sbi->sbi_first_block || block >= sbi->sbi_blocks) {
    printk("wufs_free_block: Trying to free non-data block %lu\n",block);
    return;
  }

  /* break bit offset into block offset in map and bit offset in block */
  bit = block % bits_per_block;
  mapBlock = block/bits_per_block;
  if (mapBlock >= sbi->sbi_bmap_bcnt) {
    printk("wufs_free_block: nonexistent bitmap buffer, %lu\n",mapBlock);
    return;
  }
  /* grab the buffer head */
  bh = sbi->sbi_bmap[mapBlock];

  /* get exclusive access */
  spin_lock(&bitmap_lock);
  previous = __test_and_clear_bit(bit, (unsigned long*)bh->b_data); /* see <linux/Documentation/atomic_ops.txt> */
  spin_unlock(&bitmap_lock);
  
  /* check status (outside the critical section!) */
  if (!previous) printk("wufs_free_block (%s:%lu): bit already cleared\n",
			sb->s_id, block);

  /* flush bitmap buffer */
  mark_buffer_dirty(bh);
  return;
}

/**
 * wufs_new_inode: (utility function)
 * Allocate a new inode within a particular directory.
 * Returns error code by reference.
 */
struct inode *wufs_new_inode(const struct inode *dir, int *error)
{
  /* given parent directory, determine the device */
  struct super_block *sb = dir->i_sb;
  struct wufs_sb_info *sbi = wufs_sb(sb);

  /* 
   * allocate a new vfs inode (see linux/fs/inode.c)
   * this calls (indirectly) wufs_alloc_inode (see inode.c)
   */
  struct inode *inode = new_inode(sb);
  struct buffer_head * bh;
  int i;

  /* compute the number of map bits that occur in each block of imap */
  int bits_per_block = 8 * sb->s_blocksize;
  unsigned long ino;

  /* verify that vfs could create an inode */
  if (!inode) { *error = -ENOMEM; return NULL; }

  /* set sentinel values for failed lookup */
  ino = bits_per_block;
  bh = NULL;
  *error = -ENOSPC;
  
  /* lock down bitmap */
  spin_lock(&bitmap_lock);
  for (i = 0; i < sbi->sbi_imap_bcnt; i++) {
    bh = sbi->sbi_imap[i];
    ino = find_first_zero_bit((unsigned long*)bh->b_data, bits_per_block);
    if (ino < bits_per_block) {
      /* found an available inode index */
      break;
    }
  }

  /* 
   * At this point, i is the block number, bh is its buffer_head, and ino,
   * if a reasonable value, is the distance within that block
   * First, some sanity checking:
   */
  if (!bh || ino >= bits_per_block) {
    spin_unlock(&bitmap_lock);

    /* iput is the mechanism for getting vfs to destroy an inode */
    iput(inode);
    return NULL;
  }
  /* we're still locked...set the bit */
  if (__test_and_set_bit(ino, (unsigned long*)bh->b_data)) {
    /* for some reason, the bit was set - shouldn't happen, of course */
    spin_unlock(&bitmap_lock);
    printk("wufs_new_inode: bit already set\n");

    /* iput is the mechanism for getting vfs to destroy an inode */
    iput(inode);
    return NULL;
  }
  spin_unlock(&bitmap_lock);

  /* great - bitmap is set; write it out */
  mark_buffer_dirty(bh);

  /* now compute the actual inode *number* */
  ino += i * bits_per_block + 1;

  /* sanity check */
  if (!ino || ino > sbi->sbi_inodes) {
    printk("wufs_new_inode: attempted to allocate illegal inode number %lu\n",
	   ino);
    iput(inode);
    return NULL;
  }

  /* fill out vfs inode fields */
  inode->i_uid = current_fsuid(); /* see <linux/cred.h> */
  inode->i_gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current_fsgid();
  inode->i_ino = ino;

  /*
   * remember: we can't call time(2), so we grab kernel time
   * (see ~/linux/kernel/timekeeping.c, get_seconds)
   */
  inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;

  /* initialize all data & size fields */
  inode->i_blocks = 0;
  for (i = 0; i < WUFS_INODE_BPTRS; i++) {
    wufs_i(inode)->ini_data[i] = 0;
  }

  /* insert this into the inode hash table (for fast lookup)
   * (see <linux/fs.h> and linux/fs/inode.c)
   */
  insert_inode_hash(inode);

  /* flush the inode changes to disk */
  mark_inode_dirty(inode);

  /* made it: clear pessimistic error reference */
  *error = 0;
  return inode;
}

/**
 * wufs_free_inode: (utility routine)
 * Reverse the effects of wufs_new_inode.
 */
void wufs_free_inode(struct inode * inode)
{
  struct wufs_sb_info *sbi = wufs_sb(inode->i_sb);
  struct buffer_head *bh;
  int bits_per_block = 8 * WUFS_BLOCKSIZE;
  unsigned long ino, bit, mapBlock;

  /* grab the inode *number* */
  ino = inode->i_ino;
  if (ino < 1 || ino > sbi->sbi_inodes) {
    printk("wufs_free_inode: nonexistent inode (%lu)\n",ino);
    goto out;
  }

  /* we now compute the inode index as a block and bit offset */
  ino--;
  bit = ino % bits_per_block;
  mapBlock = ino/bits_per_block;
  if (mapBlock >= sbi->sbi_imap_bcnt) {
    printk("wufs_free_inode: nonexistent imap block (%lu) in superblock\n",
	   mapBlock);
    goto out;
  }

  /* mark the on-disk inode as free */
  wufs_clear_inode(inode);

  /* now, clear the associated bit */
  bh = sbi->sbi_imap[mapBlock];

  spin_lock(&bitmap_lock);
  /* clear the bit: */
  if (!__test_and_clear_bit(bit, (unsigned long*)bh->b_data))
    printk("wufs_free_inode: bit %lu already cleared\n", bit);
  spin_unlock(&bitmap_lock);
  /* write back bitmap */
  mark_buffer_dirty(bh);
 out:
  /* clear the vfs inode, marking it for deletion (see linux/fs/inode.c) */
  clear_inode(inode);
}

/**
 * wufs_clear_inode: (utility function)
 * Clear the fields of an on-disk inode.
 */
static void wufs_clear_inode(struct inode *inode)
{
  struct buffer_head *bh = NULL;

  /* find the WUFS on-disk ("raw") inode structure: */
  struct wufs_inode *raw_inode;
  raw_inode = wufs_raw_inode(inode->i_sb, inode->i_ino, &bh);
  if (raw_inode) {
    /* indicative of a free inode: */
    raw_inode->in_mode = 0;
    raw_inode->in_nlinks = 0;
  }

  if (bh) {
    /* flush the inode to disk */
    mark_buffer_dirty(bh);
    /* and release the buffer_head */
    brelse (bh);
  }
}


/**
 * wufs_raw_inode: (utility function)
 * Get the WUFS disk-resident inode from inode number.
 * Returns pointer to associated buffer head for use by caller.
 */
struct wufs_inode *
wufs_raw_inode(struct super_block *sb, ino_t ino, struct buffer_head **bh)
{
  int block;
  /* get the superblock info structure */
  struct wufs_sb_info *sbi = wufs_sb(sb);
  struct wufs_inode *inodep;

  /*
   * These are inode *numbers*, which start at 1 and range to sbi_inodes
   */
  if (!ino || ino > sbi->sbi_inodes) {
    printk("wufs_raw_inode: Bad inode number on dev %s: %ld is out of range\n",
	   sb->s_id, (long)ino);
    return NULL;
  }
  /*
   * We now work with an inode *index*, which is 0 origin.
   * Somehow, I think we have Ken Thompson to thank for all of this.
   */
  ino--;
  /*
   * Compute the LBA of the inode, skipping boot, super, and map blocks, and
   * reaching into the inode array block set
   */
  block = 2 + sbi->sbi_imap_bcnt + sbi->sbi_bmap_bcnt; /* LBAs before array */
  block += ino / WUFS_INODES_PER_BLOCK;

  /* read the block, based on superblock info (see <linux/buffer_head.h>) */
  *bh = sb_bread(sb, block);
  if (!*bh) {
    printk("wufs_raw_inode: Unable to read inode %d, block %d\n",
	   (int)(ino+1),block);
    return NULL;
  }

  /* compute (raw) inode pointer */
  inodep = (void *)(*bh)->b_data;
  return inodep + (ino % WUFS_INODES_PER_BLOCK);
}
