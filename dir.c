/*
 * VFS/Williams Unartful File System directory entry interface.
 * (c) 2011, 2015 duane a. bailey
 * (c) 1991, 1992 linus torvalds
 */

#include "wufs.h"
#include <linux/buffer_head.h>
#include <linux/highmem.h>
#include <linux/swap.h>

typedef struct wufs_dirent wufs_dentry;

/*
 * Exported entrypoints.
 */
int          wufs_add_link(struct dentry *dentry,
			   struct inode *inode);
int          wufs_delete_entry(wufs_dentry *de,
			       struct page *page);
wufs_dentry *wufs_dotdot (struct inode *dir, struct page **p);
int          wufs_empty_dir(struct inode * inode);  
wufs_dentry *wufs_find_entry(struct dentry *dentry,
			     struct page **res_page);
ino_t        wufs_inode_by_name(struct dentry *dentry);
int          wufs_make_empty(struct inode *inode, struct inode *dir);
void         wufs_set_link(wufs_dentry *de,
			   struct page *page, struct inode *inode);

/*
 * Local entrypoints.
 */
static int                  dir_commit_chunk(struct page *page,
					     loff_t pos, unsigned len);
static struct page         *dir_get_page(struct inode *dir, unsigned long n);
static inline unsigned long dir_pages(struct inode *inode);
static inline void          dir_put_page(struct page *page);
static inline int           namecompare(int len, int maxlen,
			                const char * name, const char * buffer);
static unsigned             wufs_last_byte(struct inode *inode,
					   unsigned long page_nr);
static inline void         *wufs_next_entry(void *de, struct wufs_sb_info *sbi);
static int                  wufs_readdir(struct file * filp,
					 void * dirent, filldir_t filldir);

/*
 * Global variables.
 */
const struct file_operations wufs_dir_operations = {
  .llseek	= generic_file_llseek,
  .read		= generic_read_dir,
  .readdir	= wufs_readdir,
  .fsync	= simple_fsync,
};

/*
 * Code.
 */
/**
 * wufs_readdir: (vfs dir file operation)
 * Reads the next vfs directory entry (dirent) of a directory (dir).
 * Callback function filldir is used to fill in the appropriate dir field.
 * Called for side effect.
 */
static int wufs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
  /* get the current position through the directory file */
  unsigned long pos = filp->f_pos;

  /* get the inode associated with the directory */
  struct inode *inode = filp->f_dentry->d_inode;
  /* get the associated vfs super block */
  struct super_block *sb = inode->i_sb;

  /* convert position to offset and page position (n) */
  unsigned offset = pos % PAGE_CACHE_SIZE;
  unsigned long n = pos / PAGE_CACHE_SIZE;
  /* number of pages used by the directory */
  unsigned long npages = dir_pages(inode);

  /* determine the size of WUFS dirents (should be WUFS_DIRENT_SIZE) */
  struct wufs_sb_info *sbi = wufs_sb(sb);
  unsigned chunk_size = sbi->sbi_dirsize;
  char *name;
  __u32 inumber;

  /* find the offset to the base of the next dirent */
  pos = (pos + chunk_size-1) & ~(chunk_size-1);
  
  /* check for end-of-file */
  if (pos >= inode->i_size) { goto done; }

  /* continuable 'while': */
  for ( ; n < npages; n++, offset = 0) {
    char *p, *kaddr, *limit;
    /* get the directory page containing position pos */
    struct page *page = dir_get_page(inode, n);

    if (IS_ERR(page)) continue;

    /* get the kernel address associated with mapped page */
    kaddr = (char *)page_address(page);
    /* p is the kernel address of the next dirent record */
    p = kaddr+offset;
    /* p cannot be beyond limit, lest the dirent extend past end of page */
    limit = kaddr + wufs_last_byte(inode, n) - chunk_size;
    for ( ; p <= limit; p = wufs_next_entry(p, sbi)) {
      /* here's the next raw dentry */
      wufs_dentry *de = (wufs_dentry *)p;
      /* get to the name */
      name = de->de_name;
      /* inode number of the file */
      inumber = de->de_ino;
      if (inumber) { /* entry is valid if inumber non-zero */
	int over;

	/* (carefully) compute the length of the entry name */
	unsigned l = strnlen(name, sbi->sbi_namelen);
	/* recompute the offset into the current page */
	offset = p - kaddr;

	/*
	 * call the callback function to fill in the vfs directory entry
	 * fields from the WUFS dentry.
	 */
	over = filldir(dirent, name, l, (n << PAGE_CACHE_SHIFT) | offset, inumber, DT_UNKNOWN);
	if (over) {
	  /* free the directory page */
	  dir_put_page(page);
	  goto done;
	}
      }
    }
    dir_put_page(page);
  }

 done:
  /* set the file position to the dirent */
  filp->f_pos = (n << PAGE_CACHE_SHIFT) | offset;
  return 0;
}

/**
 * namecompare: (utility function)
 * Compares length len name against buffer; returns 1 on match 0 otherwise
 */
static inline int namecompare(int len, int maxlen,
			      const char * name, const char * buffer)
{
  /* quick test: name shorter than dirent name */
  if (len < maxlen && buffer[len]) return 0;
  return !memcmp(name, buffer, len);
}

/**
 * wufs_find_entry: (utility function)
 * Searches a directory for an entry with specific name.
 * Page containing dirent is returned through res_page reference.
 * Result is a pointer to the WUFS dentry.
 */
wufs_dentry *wufs_find_entry(struct dentry *dentry, struct page **res_page)
{
  /* get name of the entry to be found (dentry is a template) */
  const char *name = dentry->d_name.name;
  int namelen = dentry->d_name.len;
  /* get a pointer to the directory inode and superblock information */
  struct inode *dir = dentry->d_parent->d_inode;
  struct super_block *sb = dir->i_sb;
  struct wufs_sb_info *sbi = wufs_sb(sb);
  /* setup for directory search */
  unsigned long n;
  unsigned long npages = dir_pages(dir);
  struct page *page = NULL;
  char *p;

  char *namx;
  __u32 inumber;
  *res_page = NULL;

  /* start search from the beginning of directory */
  for (n = 0; n < npages; n++) {
    char *kaddr, *limit;

    /* get the page into memorry */
    page = dir_get_page(dir, n);
    if (IS_ERR(page))
      continue;

    /* determine the kernel address of the mapped directory file */
    kaddr = (char*)page_address(page);
    limit = kaddr + wufs_last_byte(dir, n) - sbi->sbi_dirsize;
    for (p = kaddr; p <= limit; p = wufs_next_entry(p, sbi)) {
      /* consider the next entry */
      wufs_dentry *de = (wufs_dentry *)p;
      /* get raw WUFS dentry name */
      namx = de->de_name;
      /*  ... and inode */
      inumber = de->de_ino;
      if (!inumber) continue; /* unused dentry */
      /* now, check the name */
      if (namecompare(namelen, sbi->sbi_namelen, name, namx))
	goto found; /* found it */
    }
    /* free page and move along to next directory page */
    dir_put_page(page);
  }
  return NULL;

 found:
  *res_page = page;
  return (wufs_dentry *)p;
}

/**
 * wufs_next_entry: (utility function)
 * Get pointer to the next (possible) directory entry, based on valid ptr
 */
static inline void *wufs_next_entry(void *de, struct wufs_sb_info *sbi)
{
  return (void*)((char*)de + sbi->sbi_dirsize);
}

/**
 * wufs_delete_entry: (utility function)
 */
int wufs_delete_entry(wufs_dentry *de, struct page *page)
{
  /* get mapping into kernel address space */
  struct address_space *mapping = page->mapping;
  char *kaddr = page_address(page);
  /* directory being mapped */
  struct inode *inode = (struct inode*)mapping->host;
  /* determine the position of de in the directory file */
  loff_t pos = page_offset(page) + (char*)de - kaddr;
  /* determine size of the directory entry */
  struct wufs_sb_info *sbi = wufs_sb(inode->i_sb);
  unsigned len = sbi->sbi_dirsize;
  int err;

  /* avoid race conditions; lock page, update; unlock */
  lock_page(page);
  /* prepare for write */
  err = __wufs_write_begin(NULL, mapping, pos, len,
			   AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
  if (err == 0) {
    /* we're ready; zero the inode, indicating empty dentry */
    de->de_ino = 0;
    /* force write */
    err = dir_commit_chunk(page, pos, len);
  } else {
    /* failed on write, unlock page and return */
    unlock_page(page);
  }
  dir_put_page(page);

  /* now, update the directory modification time */
  inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
  /* and flush to disk */
  mark_inode_dirty(inode);
  return err;
}

/**
 * wufs_make_empty: (utility function)
 * Removes the entries found in (directory) pointed to by inode.
 * Establishes "." and ".." entries (second to dir, the "parent" of
 * inode)
 */
int wufs_make_empty(struct inode *inode, struct inode *dir)
{
  /* Grab the page associated with the (mapped) directory inode */
  struct address_space *mapping = inode->i_mapping;
  struct page *page = grab_cache_page(mapping, 0);
  /* Get info associated with the containing file system */
  struct wufs_sb_info *sbi = wufs_sb(inode->i_sb);
  wufs_dentry *de;
  char *kaddr;
  int err;

  if (!page)
    return -ENOMEM;
  /* Get ready to write 2*sizeof(wufs_dentry) bytes from dir's 0 page.
   * Capture page number into page.
   */
  err = __wufs_write_begin(NULL, mapping, 0, 2 * sbi->sbi_dirsize,
			   AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
  if (err) {
    /* no ready: backout */
    unlock_page(page);
    goto fail;
  }

  /* map page to memory, zero it out, fill in with an empty directory */
  kaddr = kmap_atomic(page, KM_USER0);
  /* ...zap any dirents */
  memset(kaddr, 0, PAGE_CACHE_SIZE);
  /* write two dentries: "." and ".." */
  de = (wufs_dentry *)kaddr;
  de->de_ino = inode->i_ino;
  strcpy(de->de_name, ".");
  /* move on to second entry */
  de = wufs_next_entry(de, sbi);
  de->de_ino = dir->i_ino;
  strcpy(de->de_name, "..");
  kunmap_atomic(kaddr, KM_USER0);

  /* Now, do the write */
  err = dir_commit_chunk(page, 0, 2 * sbi->sbi_dirsize);
 fail:
  page_cache_release(page);
  return err;
}

/**
 * wufs_empty_dir: (utility function)
 * Return true iff the directory pointed to by inode is empty.
 */
int wufs_empty_dir(struct inode *inode)
{
  struct page *page = NULL;
  /* Determine the length (in pages) of the directory */
  unsigned long i, npages = dir_pages(inode);
  /* Get file system parameters */
  struct wufs_sb_info *sbi = wufs_sb(inode->i_sb);
  char *name;
  __u32 inumber;

  /* consider each page, in turn */
  for (i = 0; i < npages; i++) {
    char *p, *kaddr, *limit;

    /* get i-th directory page */
    page = dir_get_page(inode, i);
    if (IS_ERR(page))
      continue;

    /* get bounds on the kernel address space that contain the page */
    kaddr = (char *)page_address(page);
    limit = kaddr + wufs_last_byte(inode, i) - sbi->sbi_dirsize;
    for (p = kaddr; p <= limit; p = wufs_next_entry(p, sbi)) {
      /* consider the next entry: */
      wufs_dentry *de = (wufs_dentry *)p;

      /* get the name and inode */
      name = de->de_name;
      inumber = de->de_ino;

      if (inumber != 0) { /* valid directory entry - better be . or .. */
	/* check for . and .. */
	if (name[0] != '.')
	  goto not_empty;
	if (!name[1]) { /* badness: . doesn't point to this directory */
	  if (inumber != inode->i_ino)
	    goto not_empty;
	} else if (name[1] != '.') /* other dotted file */
	  goto not_empty;
	else if (name[2]) /* anything longer: not empty */
	  goto not_empty;
      }
    }
    /* finished with page */
    dir_put_page(page);
  }
  return 1;

 not_empty:
  /* not found; release page */
  dir_put_page(page);
  return 0;
}

/**
 * wufs_add_link: (utility function)
 * Create a new hard link from dentry to inode.
 */
int wufs_add_link(struct dentry *dentry, struct inode *inode)
{
  /* dir is the inode of the directory containing new hard link */
  struct inode *dir = dentry->d_parent->d_inode;

  /* name is the name of the new link */
  const char *name = dentry->d_name.name;
  int namelen = dentry->d_name.len;

  /* get super block and super block info for file system */
  struct super_block * sb = dir->i_sb;
  struct wufs_sb_info * sbi = wufs_sb(sb);

  /* set up for a directory search */
  struct page *page = NULL;
  unsigned long npages = dir_pages(dir);
  unsigned long n;
  char *kaddr, *p;
  wufs_dentry *de;
  loff_t pos;
  int err;
  char *namx = NULL;
  __u32 inumber;

  /*
   * Because the directory may expand we have to reach beyond
   * the directory's end.  We lock the page to protect the critical 
   * code.
   */
  for (n = 0; n <= npages; n++) {
    char *limit, *dir_end;

    /* get n'th page of the directory */
    page = dir_get_page(dir, n);
    /* negative values are really errors...*/
    err = PTR_ERR(page);
    if (IS_ERR(page)) goto out;

    /* lock the valid page */
    lock_page(page);
    /* find page and bounding pointer in the kernel address space */
    kaddr = (char*)page_address(page);
    /* end of directory pointer */
    dir_end = kaddr + wufs_last_byte(dir, n);
    /* end of page in memory */
    limit = kaddr + PAGE_CACHE_SIZE - sbi->sbi_dirsize;
    for (p = kaddr; p <= limit; p = wufs_next_entry(p, sbi)) {
      /* consider the next entry */
      de = (wufs_dentry *)p;
      /* get the name and inode */
      namx = de->de_name;
      inumber = de->de_ino;
      if (p == dir_end) {
	/* bummer, we have to expand directory */
	de->de_ino = 0;
	goto got_it;
      }
      if (!inumber) /* an empty dirent; use this one */
	goto got_it;
      err = -EEXIST;
      /* check for similarly named entry */
      if (namecompare(namelen, sbi->sbi_namelen, name, namx))
	goto out_unlock;
    }
    /* back out of this page, move onto next */
    unlock_page(page);
    dir_put_page(page);
  }
  /* it would be bad to get here. Perhaps a short directory */
  BUG();
  return -EINVAL;

 got_it:
  /* here's the position of the dentry in the directory file */
  pos = page_offset(page) + p - (char *)page_address(page);
  /* setup for a WUFS_DIRENT_SIZE write from p on */
  err = __wufs_write_begin(NULL, page->mapping, pos, sbi->sbi_dirsize,
			   AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
  if (err) /* can't do it; bail */
    goto out_unlock;

  /* copy over the name into the dirent */
  memcpy (namx, name, namelen);

  /* pad with zeros to end of name field */
  memset (namx + namelen, 0, sbi->sbi_dirsize - namelen - 2);

  /* establish the link between the dentries */
  de->de_ino = inode->i_ino;

  /* now, write the chunk of memory to disk */
  err = dir_commit_chunk(page, pos, sbi->sbi_dirsize);

  /* update the containing directory's modification time */
  dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
  /* ...and flush it out */
  mark_inode_dirty(dir);
 out_put:
  /* release the directory page */
  dir_put_page(page);
 out:
  return err;

  /* who said goto was harmful? */

 out_unlock:
  /* unlock access */
  unlock_page(page);
  goto out_put;
}

/**
 * wufs_set_link: (utility function)
 * Take an existing "raw" directory entry and force it to link
 * to inode.  page is the page that (fully contains) the directory entry.
 */
void wufs_set_link(wufs_dentry *de, struct page *page,
		   struct inode *inode)
{
  /* find the directory that contains the directory entry */
  struct address_space *mapping = page->mapping;
  struct inode *dir = mapping->host;

  /* get dirent size */
  struct wufs_sb_info *sbi = wufs_sb(dir->i_sb);

  /* determine the position, within page, of the "raw" dentry */
  loff_t pos = page_offset(page) +
    (char *)de-(char*)page_address(page);
  int err;

  /* Lock down page for modification and writing */
  lock_page(page);
  /* prepare to write WUFS_DIRENT_SIZE bytes on page */
  err = __wufs_write_begin(NULL, mapping, pos, sbi->sbi_dirsize,
			   AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
  if (err == 0) { /* ready: mod and write */
    /* add link */
    de->de_ino = inode->i_ino;
    /* write */
    err = dir_commit_chunk(page, pos, sbi->sbi_dirsize);
  } else {
    /* didn't work; bail */
    unlock_page(page);
  }
  /* release the page */
  dir_put_page(page);

  /* update directory modification time and flush */
  dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
  mark_inode_dirty(dir);
}

/**
 * wufs_dotdot: (utility function)
 * Return the ".." entry for directory dir.
 * Side effect: p points to mapped page.
 * FIXME: probably should be some sanity checks.
 */
wufs_dentry *wufs_dotdot (struct inode *dir, struct page **p)
{
  /* get the directory page */
  struct page *page = dir_get_page(dir, 0);
  struct wufs_sb_info *sbi = wufs_sb(dir->i_sb);
  wufs_dentry *de = NULL;

  if (!IS_ERR(page)) {
    /* if the page is valid, skip one entry (".") and return address of next */
    *p = page;
    de = wufs_next_entry(page_address(page), sbi);
  }
  return de;
}

/**
 * wufs_inode_by_name: (utility function)
 * From a template directory entry, get the associated inode.
 */
ino_t wufs_inode_by_name(struct dentry *dentry)
{
  struct page *page;
  /* find the "raw" WUFS dentry */
  wufs_dentry *de = wufs_find_entry(dentry, &page);
  ino_t res = 0;

  if (de) {
    /* get inode and free the page */
    res = de->de_ino;
    dir_put_page(page);
  }
  return res;
}

/**
 * dir_put_page:
 * Release a directory page.
 */
static inline void dir_put_page(struct page *page)
{
  kunmap(page);
  page_cache_release(page);
}

/**
 * wufs_last_byte: (utility function)
 * Return the offset (origin 1) of the last used byte on page pno.
 * Useful for bounding accesses to valid parts of address space.
 */
static unsigned wufs_last_byte(struct inode *inode, unsigned long pno)
{
  /* on most pages, it's just past the end of the page */
  unsigned long last_byte = PAGE_CACHE_SIZE;
  unsigned long last_page = inode->i_size / PAGE_CACHE_SIZE;
  /* on the last page: it's whatever's left over */
  if (pno == last_page) last_byte = inode->i_size % PAGE_CACHE_SIZE;
  return last_byte;
}

/**
 * dir_pages: (utility function)
 * Return the number of data pages used by a directory.
 */
static inline unsigned long dir_pages(struct inode *inode)
{
  return (inode->i_size+PAGE_CACHE_SIZE-1)/PAGE_CACHE_SIZE;
}

/**
 * dir_commit_chunk: (utility function)
 * Actually finish the write announced by __wufs_write_begin.
 * Writing back the directory has the residual effect of updating
 * the modificatin time on the *directory that contains the directory*.
 */
static int dir_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
  struct address_space *mapping = page->mapping;
  /* get the directory inode that refers to the page being written */
  struct inode *dir = mapping->host;
  int err = 0;
  /* perform the write */
  block_write_end(NULL, mapping, pos, len, len, page, NULL);

  /* if directory write extends beyond end of the directory, extend it */
  if (pos+len > dir->i_size) {
    /* this is how the directory size is atomically updated */
    i_size_write(dir, pos+len);
    /* now, flush the larger directory */
    mark_inode_dirty(dir);
  }

  /* perform page access rundown; whatever type is necessary */
  if (IS_DIRSYNC(dir))
    err = write_one_page(page, 1);
  else
    unlock_page(page);
  return err;
}

/**
 * dir_get_page: (utility function)
 * Map page n of the directory and return kernel memory pointer.
 */

static struct page *dir_get_page(struct inode *dir, unsigned long n)
{
  /* grab the mapping associated with this directory inode */
  struct address_space *mapping = dir->i_mapping;
  /* read in the n'th page, if not already read in */
  struct page *page = read_mapping_page(mapping, n, NULL);
  /* map it in */
  if (!IS_ERR(page)) {
    kmap(page);
    if (!PageUptodate(page))
      goto fail;
  }
  /* return the page pointer */
  return page;

 fail:
  /* otherwise, dereference page an release */
  dir_put_page(page);
  return ERR_PTR(-EIO);
}


