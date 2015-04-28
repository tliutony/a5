/*
 * Support for indirect block references in the Williams Ultimate File System.
 * (c) 2011, 2015 duane a. bailey, Reid Pryzant, Tony Liu
 */
#include <linux/buffer_head.h>
#include "wufs.h"

/*
 * Types.
 */
typedef __u16 block_t;	/* 16 bit, host order */

/*
 * Global routines.
 */
int      wufs_get_blk(struct inode * inode, sector_t block,
			   struct buffer_head *bh_result, int create);
void     wufs_truncate(struct inode * inode);
unsigned wufs_blocks(loff_t size, struct super_block *sb);



/*
 * Local routines.
 */
static inline               block_t *bptrs(struct inode *inode);
static int retrieve_indirect(block_t *ptr, struct inode *inode, int create, struct buffer_head *bh, sector_t block);
static int retrieve_direct(block_t *ptr, struct inode *inode, int create, struct buffer_head *bh);
/*
 * Global variables
 */

/**
 * pointers_lock: (read-write lock)
 * Reader/writer lock protecting the inode block pointer access.
 */
static DEFINE_RWLOCK(pointers_lock);

/*
 * Code.
 */

/**
 * bptrs: (utility function)
 * Given an inode, get the array of block pointers
 */
static inline block_t *bptrs(struct inode *inode)
{
  return (block_t *)wufs_i(inode)->ini_data;
}


/**
 * wufs_get_block: (module-wide utility function)
 * Get the buffer associated with a particular block.
 * If create=1, create the block if missing; otherwise return with error
 */
int wufs_get_blk(struct inode * inode, sector_t block, struct buffer_head *bh, int create)
{
  /* get the meta-data associated with the file system superblock */
  struct wufs_sb_info *sbi = wufs_sb(inode->i_sb);
  block_t *bptr, *ptr;

  if (block < 0 || block >= sbi->sbi_max_fblks) {
    return -EIO;
  }

  bptr = bptrs(inode);

  //WUFS_INODE_BPTRS-1 is 7, index of the indirect ptr
  if(block >= WUFS_INODE_BPTRS-1) {
    ptr = bptr+WUFS_INODE_BPTRS-1;
    block -= WUFS_INODE_BPTRS-1;
    return retrieve_indirect(ptr, inode, create, bh, block);
  }
  else {
    ptr = bptr+block;
    return retrieve_direct(ptr, inode, create, bh);
  }


  return 0;
}

/**
 * direct block retrieval (same as Duane's original code)
 */
int retrieve_direct(block_t *ptr, struct inode *inode, int create, struct buffer_head *bh) {
  /* now, ensure there's a block reference at the end of the pointer */
 start:
  if (!*ptr) {
    int n; /* number of any new block */
    
    /* if we're not allowed to create it, claim an I/O error */
    if (!create) return -EIO;
    
    /* grab a new block */
    n = wufs_new_block(inode);
    /* not possible? must have run out of space! */
    if (!n) return -ENOSPC;

    /* critical block update section */
    write_lock(&pointers_lock);
    if (*ptr) {
      /* some other thread has set this! yikes: back out */
      write_unlock(&pointers_lock);
      /* return block to the pool */
      wufs_free_block(inode,n);
      goto start; /* above */
    } else {
      /* we're good to modify the block pointer */
      *ptr = n;
      /* done with critical path */
      write_unlock(&pointers_lock);
      
      /* update time and flush changes to disk */
      inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
      mark_inode_dirty(inode);
      
      /*
       * tell the buffer system this a new, valid block
       * (see <linux/include/linux/buffer_head.h>)
       */
      set_buffer_new(bh);
    }
  }

  /* 
   * at this point, *ptr is non-zero
   * assign a disk mapping associated with the file system and block number
   */
  map_bh(bh, inode->i_sb, *ptr);
  return 0;
}

/**
 * indirect block retrieval oh boy
 *
 */
int retrieve_indirect(block_t *ptr, struct inode *inode, int create, struct buffer_head *bh, sector_t block) {
  // initialize block to be mapped to outgoing bh
  int data_LBA;

 start:
  //case when indirect block is not allocated: allocate indirect block and data block
  if (!*ptr) {
    int indirect_LBA; /* number of our new indirect block */
    
    /* if we're not allowed to create it, claim an I/O error */
    if (!create) return -EIO;
    
    /* grab a new block */
    indirect_LBA = wufs_new_block(inode);
    /* not possible? must have run out of space! */
    if (!indirect_LBA) return -ENOSPC;

    //TODO: do we need to mark this as dirty? do something?
    data_LBA = wufs_new_block(inode); /* the direct block to be hung off the indirect block */

    /* not possible? must have run out of space! */
    if (!data_LBA) return -ENOSPC;

    /* get a buffer head associated with the indirect block. Worry: int?  */
    struct buffer_head *indir_ptr = sb_getblk(inode->i_sb, indirect_LBA); 
    set_buffer_new(indir_ptr);
    map_bh(indir_ptr, inode->i_sb, indirect_LBA); //from Duane's notes
 
    block_t *blk_data = (block_t *)indir_ptr->b_data; //????
    //Worry: what if block is too large? is l61 test enough?
    blk_data += block;
 
    //Worry: buffer rollback needed? 
    lock_buffer(indir_ptr); //ENTERING CRITIZAL ZECTION
    *blk_data = data_LBA;
    unlock_buffer(indir_ptr); //EXITING...whew

    //Time to write to ptr
    write_lock(&pointers_lock);
    if (*ptr) {
      /* some other thread has set this! yikes: back out */
      write_unlock(&pointers_lock);
      /* need to forget that bh we allocated */
      bforget(indir_ptr);
      /* return blocks to the pool */
      wufs_free_block(inode,indirect_LBA);
      wufs_free_block(inode,data_LBA);
      goto start; /* above */
    } else {
      /* we're good to modify  the block pointer */
      *ptr = indirect_LBA;
      /* done with critical path */
      write_unlock(&pointers_lock);
      

      /* update time and flush changes to disk */
      inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
      mark_inode_dirty(inode);
      
      //we mark the indir_ptr bh as dirty
      mark_buffer_dirty_inode(indir_ptr, inode);
      brelse(indir_ptr);

      /*
       * tell the buffer system this a new, valid block
       * (see <linux/include/linux/buffer_head.h>)
       */
      set_buffer_new(bh);
    }
  // *ptr exists, as does the indirection block
  } else {
  
    struct buffer_head *indir_ptr = sb_bread(inode->i_sb, *ptr);     
    block_t *blk_data = (block_t *)indir_ptr->b_data;

  start_indirection:  
    blk_data += block;

    // create new datablock, mark indirection block as dirty          
    if (!*blk_data) {
      data_LBA = wufs_new_block(inode);
      if (!data_LBA) return -ENOSPC;
    
      lock_buffer(indir_ptr);
      // time to write to the indirection block
      if (*blk_data) {
	// some other thread has set this! Yikes! back out
	unlock_buffer(indir_ptr);
	bforget(indir_ptr);
	wufs_free_block(inode, data_LBA);
	goto start_indirection;
      
      } else {
	// we're good to insert the new data block pointer into the indirection block
	*blk_data = data_LBA;
	unlock_buffer(indir_ptr);
	// mark the indirection bh as dirty
	mark_buffer_dirty_inode(indir_ptr, inode);
	// release indirection bufferhead
	brelse(indir_ptr);
      } 
    // retrieve existing datablock (the nicest case = just retrieve indirect lba)
    } else {
      data_LBA = *blk_data;
    }
  }
  // map data lba to outgoing bh
  map_bh(bh, inode->i_sb, data_LBA); 
  return 0;
}
/**
 * wufs_truncate: (module-wide utility function)
 * Set the file allocation to exactly match the size of the file.
 * (wufs_get_block expands file, so only contraction is considered here.)
 */
void wufs_truncate(struct inode *inode)
{
  block_t *blk = bptrs(inode);
  int i;
  long bcnt;

  block_truncate_page(inode->i_mapping, inode->i_size, wufs_get_blk);

  write_lock(&pointers_lock);
  /* compute the number of blocks needed by this file */
  bcnt = (inode->i_size + WUFS_BLOCKSIZE -1) / WUFS_BLOCKSIZE;

  /* set all blocks referenced beyond file size to 0 (null) */
  for (i = bcnt; i < WUFS_INODE_BPTRS; i++) {
    if (blk[i]) {
      wufs_free_block(inode,blk[i]);
    }
    blk[i] = 0;
  }
  write_unlock(&pointers_lock);

  /* My what a big change we made!  Timestamp and flush it to disk. */
  inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
  mark_inode_dirty(inode);
}

/**
 * wufs_blocks: (utility function)
 * Compute the number of blocks needed to cover a size "size" file.
 */
unsigned int wufs_blocks(loff_t size, struct super_block *sb)
{
  return (size + sb->s_blocksize - 1) / sb->s_blocksize;
}
