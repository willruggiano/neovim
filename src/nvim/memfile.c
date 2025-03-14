/// An abstraction to handle blocks of memory which can be stored in a file.
/// This is the implementation of a sort of virtual memory.
///
/// A memfile consists of a sequence of blocks:
/// - Blocks numbered from 0 upwards have been assigned a place in the actual
///   file. The block number is equal to the page number in the file.
/// - Blocks with negative numbers are currently in memory only. They can be
///   assigned a place in the file when too much memory is being used. At that
///   moment, they get a new, positive, number. A list is used for translation
///   of negative to positive numbers.
///
/// The size of a block is a multiple of a page size, normally the page size of
/// the device the file is on. Most blocks are 1 page long. A block of multiple
/// pages is used for a line that does not fit in a single page.
///
/// Each block can be in memory and/or in a file. The block stays in memory
/// as long as it is locked. If it is no longer locked it can be swapped out to
/// the file. It is only written to the file if it has been changed.
///
/// Under normal operation the file is created when opening the memory file and
/// deleted when closing the memory file. Only with recovery an existing memory
/// file is opened.
///
/// The functions for using a memfile:
///
/// mf_open()         open a new or existing memfile
/// mf_open_file()    open a swap file for an existing memfile
/// mf_close()        close (and delete) a memfile
/// mf_new()          create a new block in a memfile and lock it
/// mf_get()          get an existing block and lock it
/// mf_put()          unlock a block, may be marked for writing
/// mf_free()         remove a block
/// mf_sync()         sync changed parts of memfile to disk
/// mf_release_all()  release as much memory as possible
/// mf_trans_del()    may translate negative to positive block number
/// mf_fullname()     make file name full path (use before first :cd)

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "nvim/assert.h"
#include "nvim/buffer_defs.h"
#include "nvim/fileio.h"
#include "nvim/gettext.h"
#include "nvim/globals.h"
#include "nvim/map.h"
#include "nvim/memfile.h"
#include "nvim/memfile_defs.h"
#include "nvim/memline.h"
#include "nvim/memory.h"
#include "nvim/message.h"
#include "nvim/os/fs_defs.h"
#include "nvim/os/input.h"
#include "nvim/os/os.h"
#include "nvim/path.h"
#include "nvim/pos.h"
#include "nvim/vim.h"

#define MEMFILE_PAGE_SIZE 4096       /// default page size

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "memfile.c.generated.h"
#endif

static const char e_block_was_not_locked[] = N_("E293: Block was not locked");

/// Open a new or existing memory block file.
///
/// @param fname  Name of file to use.
///               - If NULL, it means no file (use memory only).
///               - If not NULL:
///                 * Should correspond to an existing file.
///                 * String must have been allocated (it is not copied).
///                 * If opening the file fails, it is freed and function fails.

/// @param flags  Flags for open() call.
///
/// @return - The open memory file, on success.
///         - NULL, on failure (e.g. file does not exist).
memfile_T *mf_open(char *fname, int flags)
{
  memfile_T *mfp = xmalloc(sizeof(memfile_T));

  if (fname == NULL) {               // no file, use memory only
    mfp->mf_fname = NULL;
    mfp->mf_ffname = NULL;
    mfp->mf_fd = -1;
  } else {                           // try to open the file
    if (!mf_do_open(mfp, fname, flags)) {
      xfree(mfp);
      return NULL;                   // fail if file could not be opened
    }
  }

  mfp->mf_free_first = NULL;         // free list is empty
  mfp->mf_dirty = MF_DIRTY_NO;
  mfp->mf_hash = (PMap(int64_t)) MAP_INIT;
  mfp->mf_trans = (Map(int64_t, int64_t)) MAP_INIT;
  mfp->mf_page_size = MEMFILE_PAGE_SIZE;

  // Try to set the page size equal to device's block size. Speeds up I/O a lot.
  FileInfo file_info;
  if (mfp->mf_fd >= 0 && os_fileinfo_fd(mfp->mf_fd, &file_info)) {
    uint64_t blocksize = os_fileinfo_blocksize(&file_info);
    if (blocksize >= MIN_SWAP_PAGE_SIZE && blocksize <= MAX_SWAP_PAGE_SIZE) {
      STATIC_ASSERT(MAX_SWAP_PAGE_SIZE <= UINT_MAX,
                    "MAX_SWAP_PAGE_SIZE must fit into an unsigned");
      mfp->mf_page_size = (unsigned)blocksize;
    }
  }

  off_T size;

  // When recovering, the actual block size will be retrieved from block 0
  // in ml_recover(). The size used here may be wrong, therefore mf_blocknr_max
  // must be rounded up.
  if (mfp->mf_fd < 0
      || (flags & (O_TRUNC|O_EXCL))
      || (size = vim_lseek(mfp->mf_fd, 0, SEEK_END)) <= 0) {
    // no file or empty file
    mfp->mf_blocknr_max = 0;
  } else {
    assert(sizeof(off_T) <= sizeof(blocknr_T)
           && mfp->mf_page_size > 0
           && mfp->mf_page_size - 1 <= INT64_MAX - size);
    mfp->mf_blocknr_max = (((blocknr_T)size + mfp->mf_page_size - 1)
                           / mfp->mf_page_size);
  }
  mfp->mf_blocknr_min = -1;
  mfp->mf_neg_count = 0;
  mfp->mf_infile_count = mfp->mf_blocknr_max;

  return mfp;
}

/// Open a file for an existing memfile.
///
/// Used when updatecount set from 0 to some value.
///
/// @param fname  Name of file to use.
///               - If NULL, it means no file (use memory only).
///               - If not NULL:
///                 * Should correspond to an existing file.
///                 * String must have been allocated (it is not copied).
///                 * If opening the file fails, it is freed and function fails.
///
/// @return OK    On success.
///         FAIL  If file could not be opened.
int mf_open_file(memfile_T *mfp, char *fname)
{
  if (mf_do_open(mfp, fname, O_RDWR | O_CREAT | O_EXCL)) {
    mfp->mf_dirty = MF_DIRTY_YES;
    return OK;
  }

  return FAIL;
}

/// Close a memory file and optionally delete the associated file.
///
/// @param del_file  Whether to delete associated file.
void mf_close(memfile_T *mfp, bool del_file)
{
  if (mfp == NULL) {                    // safety check
    return;
  }
  if (mfp->mf_fd >= 0 && close(mfp->mf_fd) < 0) {
    emsg(_(e_swapclose));
  }
  if (del_file && mfp->mf_fname != NULL) {
    os_remove(mfp->mf_fname);
  }

  // free entries in used list
  bhdr_T *hp;
  map_foreach_value(&mfp->mf_hash, hp, {
    mf_free_bhdr(hp);
  })
  while (mfp->mf_free_first != NULL) {  // free entries in free list
    xfree(mf_rem_free(mfp));
  }
  map_destroy(int64_t, &mfp->mf_hash);
  map_destroy(int64_t, &mfp->mf_trans);  // free hashtable and its items
  mf_free_fnames(mfp);
  xfree(mfp);
}

/// Close the swap file for a memfile. Used when 'swapfile' is reset.
///
/// @param getlines  Whether to get all lines into memory.
void mf_close_file(buf_T *buf, bool getlines)
{
  memfile_T *mfp = buf->b_ml.ml_mfp;
  if (mfp == NULL || mfp->mf_fd < 0) {   // nothing to close
    return;
  }

  if (getlines) {
    // get all blocks in memory by accessing all lines (clumsy!)
    for (linenr_T lnum = 1; lnum <= buf->b_ml.ml_line_count; lnum++) {
      (void)ml_get_buf(buf, lnum);
    }
  }

  if (close(mfp->mf_fd) < 0) {           // close the file
    emsg(_(e_swapclose));
  }
  mfp->mf_fd = -1;

  if (mfp->mf_fname != NULL) {
    os_remove(mfp->mf_fname);    // delete the swap file
    mf_free_fnames(mfp);
  }
}

/// Set new size for a memfile. Used when block 0 of a swapfile has been read
/// and the size it indicates differs from what was guessed.
void mf_new_page_size(memfile_T *mfp, unsigned new_size)
{
  mfp->mf_page_size = new_size;
}

/// Get a new block
///
/// @param negative    Whether a negative block number is desired (data block).
/// @param page_count  Desired number of pages.
bhdr_T *mf_new(memfile_T *mfp, bool negative, unsigned page_count)
{
  bhdr_T *hp = NULL;

  // Decide on the number to use:
  // If there is a free block, use its number.
  // Otherwise use mf_block_min for a negative number, mf_block_max for
  // a positive number.
  bhdr_T *freep = mfp->mf_free_first;        // first free block
  if (!negative && freep != NULL && freep->bh_page_count >= page_count) {
    if (freep->bh_page_count > page_count) {
      // If the block in the free list has more pages, take only the number
      // of pages needed and allocate a new bhdr_T with data.
      hp = mf_alloc_bhdr(mfp, page_count);
      hp->bh_bnum = freep->bh_bnum;
      freep->bh_bnum += page_count;
      freep->bh_page_count -= page_count;
    } else {    // need to allocate memory for this block
      // If the number of pages matches use the bhdr_T from the free list and
      // allocate the data.
      void *p = xmalloc((size_t)mfp->mf_page_size * page_count);
      hp = mf_rem_free(mfp);
      hp->bh_data = p;
    }
  } else {                      // get a new number
    hp = mf_alloc_bhdr(mfp, page_count);
    if (negative) {
      hp->bh_bnum = mfp->mf_blocknr_min--;
      mfp->mf_neg_count++;
    } else {
      hp->bh_bnum = mfp->mf_blocknr_max;
      mfp->mf_blocknr_max += page_count;
    }
  }
  hp->bh_flags = BH_LOCKED | BH_DIRTY;    // new block is always dirty
  mfp->mf_dirty = MF_DIRTY_YES;
  hp->bh_page_count = page_count;
  pmap_put(int64_t)(&mfp->mf_hash, hp->bh_bnum, hp);

  // Init the data to all zero, to avoid reading uninitialized data.
  // This also avoids that the passwd file ends up in the swap file!
  (void)memset(hp->bh_data, 0, (size_t)mfp->mf_page_size * page_count);

  return hp;
}

// Get existing block "nr" with "page_count" pages.
//
// Caller should first check a negative nr with mf_trans_del().
//
// @return  NULL if not found
bhdr_T *mf_get(memfile_T *mfp, blocknr_T nr, unsigned page_count)
{
  // check block number exists
  if (nr >= mfp->mf_blocknr_max || nr <= mfp->mf_blocknr_min) {
    return NULL;
  }

  // see if it is in the cache
  bhdr_T *hp = pmap_get(int64_t)(&mfp->mf_hash, nr);
  if (hp == NULL) {                             // not in the hash list
    if (nr < 0 || nr >= mfp->mf_infile_count) {  // can't be in the file
      return NULL;
    }

    // could check here if the block is in the free list

    if (page_count > 0) {
      hp = mf_alloc_bhdr(mfp, page_count);
    }
    if (hp == NULL) {
      return NULL;
    }

    hp->bh_bnum = nr;
    hp->bh_flags = 0;
    hp->bh_page_count = page_count;
    if (mf_read(mfp, hp) == FAIL) {             // cannot read the block
      mf_free_bhdr(hp);
      return NULL;
    }
  } else {
    pmap_del(int64_t)(&mfp->mf_hash, hp->bh_bnum, NULL);
  }

  hp->bh_flags |= BH_LOCKED;
  pmap_put(int64_t)(&mfp->mf_hash, hp->bh_bnum, hp);  // put in front of hash table

  return hp;
}

/// Release the block *hp.
///
/// @param dirty   Whether block must be written to file later.
/// @param infile  Whether block should be in file (needed for recovery).
void mf_put(memfile_T *mfp, bhdr_T *hp, bool dirty, bool infile)
{
  unsigned flags = hp->bh_flags;

  if ((flags & BH_LOCKED) == 0) {
    iemsg(_(e_block_was_not_locked));
  }
  flags &= ~BH_LOCKED;
  if (dirty) {
    flags |= BH_DIRTY;
    if (mfp->mf_dirty != MF_DIRTY_YES_NOSYNC) {
      mfp->mf_dirty = MF_DIRTY_YES;
    }
  }
  hp->bh_flags = flags;
  if (infile) {
    mf_trans_add(mfp, hp);      // may translate negative in positive nr
  }
}

/// Signal block as no longer used (may put it in the free list).
void mf_free(memfile_T *mfp, bhdr_T *hp)
{
  xfree(hp->bh_data);           // free data
  pmap_del(int64_t)(&mfp->mf_hash, hp->bh_bnum, NULL);  // get *hp out of the hash table
  if (hp->bh_bnum < 0) {
    xfree(hp);                  // don't want negative numbers in free list
    mfp->mf_neg_count--;
  } else {
    mf_ins_free(mfp, hp);       // put *hp in the free list
  }
}

/// Sync memory file to disk.
///
/// @param flags  MFS_ALL    If not given, blocks with negative numbers are not
///                          synced, even when they are dirty.
///               MFS_STOP   Stop syncing when a character becomes available,
///                          but sync at least one block.
///               MFS_FLUSH  Make sure buffers are flushed to disk, so they will
///                          survive a system crash.
///               MFS_ZERO   Only write block 0.
///
/// @return FAIL  If failure. Possible causes:
///               - No file (nothing to do).
///               - Write error (probably full disk).
///         OK    Otherwise.
int mf_sync(memfile_T *mfp, int flags)
{
  int got_int_save = got_int;

  if (mfp->mf_fd < 0) {
    // there is no file, nothing to do
    mfp->mf_dirty = MF_DIRTY_NO;
    return FAIL;
  }

  // Only a CTRL-C while writing will break us here, not one typed previously.
  got_int = false;

  // Sync from last to first (may reduce the probability of an inconsistent
  // file). If a write fails, it is very likely caused by a full filesystem.
  // Then we only try to write blocks within the existing file. If that also
  // fails then we give up.
  int status = OK;
  bhdr_T *hp;
  // note, "last" block is typically earlier in the hash list
  map_foreach_value(&mfp->mf_hash, hp, {
    if (((flags & MFS_ALL) || hp->bh_bnum >= 0)
        && (hp->bh_flags & BH_DIRTY)
        && (status == OK || (hp->bh_bnum >= 0
                             && hp->bh_bnum < mfp->mf_infile_count))) {
      if ((flags & MFS_ZERO) && hp->bh_bnum != 0) {
        continue;
      }
      if (mf_write(mfp, hp) == FAIL) {
        if (status == FAIL) {   // double error: quit syncing
          break;
        }
        status = FAIL;
      }
      if (flags & MFS_STOP) {   // Stop when char available now.
        if (os_char_avail()) {
          break;
        }
      } else {
        os_breakcheck();
      }
      if (got_int) {
        break;
      }
    }
  })

  // If the whole list is flushed, the memfile is not dirty anymore.
  // In case of an error, dirty flag is also set, to avoid trying all the time.
  if (hp == NULL || status == FAIL) {
    mfp->mf_dirty = MF_DIRTY_NO;
  }

  if (flags & MFS_FLUSH) {
    if (os_fsync(mfp->mf_fd)) {
      status = FAIL;
    }
  }

  got_int |= got_int_save;

  return status;
}

/// Set dirty flag for all blocks in memory file with a positive block number.
/// These are blocks that need to be written to a newly created swapfile.
void mf_set_dirty(memfile_T *mfp)
{
  bhdr_T *hp;
  map_foreach_value(&mfp->mf_hash, hp, {
    if (hp->bh_bnum > 0) {
      hp->bh_flags |= BH_DIRTY;
    }
  })
  mfp->mf_dirty = MF_DIRTY_YES;
}

/// Release as many blocks as possible.
///
/// Used in case of out of memory
///
/// @return  Whether any memory was released.
bool mf_release_all(void)
{
  bool retval = false;
  FOR_ALL_BUFFERS(buf) {
    memfile_T *mfp = buf->b_ml.ml_mfp;
    if (mfp != NULL) {
      // If no swap file yet, try to open one.
      if (mfp->mf_fd < 0 && buf->b_may_swap) {
        ml_open_file(buf);
      }

      // Flush as many blocks as possible, only if there is a swapfile.
      if (mfp->mf_fd >= 0) {
        for (int i = 0; i < (int)map_size(&mfp->mf_hash);) {
          bhdr_T *hp = mfp->mf_hash.values[i];
          if (!(hp->bh_flags & BH_LOCKED)
              && (!(hp->bh_flags & BH_DIRTY)
                  || mf_write(mfp, hp) != FAIL)) {
            pmap_del(int64_t)(&mfp->mf_hash, hp->bh_bnum, NULL);
            mf_free_bhdr(hp);
            retval = true;
            // Rerun with the same value of i. another item will have taken
            // its place (or it was the last)
          } else {
            i++;
          }
        }
      }
    }
  }
  return retval;
}

/// Allocate a block header and a block of memory for it.
static bhdr_T *mf_alloc_bhdr(memfile_T *mfp, unsigned page_count)
{
  bhdr_T *hp = xmalloc(sizeof(bhdr_T));
  hp->bh_data = xmalloc((size_t)mfp->mf_page_size * page_count);
  hp->bh_page_count = page_count;
  return hp;
}

/// Free a block header and its block memory.
static void mf_free_bhdr(bhdr_T *hp)
{
  xfree(hp->bh_data);
  xfree(hp);
}

/// Insert a block in the free list.
static void mf_ins_free(memfile_T *mfp, bhdr_T *hp)
{
  hp->bh_data = mfp->mf_free_first;
  mfp->mf_free_first = hp;
}

/// Remove the first block in the free list and return it.
///
/// Caller must check that mfp->mf_free_first is not NULL.
static bhdr_T *mf_rem_free(memfile_T *mfp)
{
  bhdr_T *hp = mfp->mf_free_first;
  mfp->mf_free_first = hp->bh_data;
  return hp;
}

/// Read a block from disk.
///
/// @return  OK    On success.
///          FAIL  On failure. Could be:
///                - No file.
///                - Error reading file.
static int mf_read(memfile_T *mfp, bhdr_T *hp)
{
  if (mfp->mf_fd < 0) {     // there is no file, can't read
    return FAIL;
  }

  unsigned page_size = mfp->mf_page_size;
  // TODO(elmart): Check (page_size * hp->bh_bnum) within off_T bounds.
  off_T offset = (off_T)(page_size * hp->bh_bnum);
  if (vim_lseek(mfp->mf_fd, offset, SEEK_SET) != offset) {
    PERROR(_("E294: Seek error in swap file read"));
    return FAIL;
  }
  // check for overflow; we know that page_size must be > 0
  assert(hp->bh_page_count <= UINT_MAX / page_size);
  unsigned size = page_size * hp->bh_page_count;
  if ((unsigned)read_eintr(mfp->mf_fd, hp->bh_data, size) != size) {
    PERROR(_("E295: Read error in swap file"));
    return FAIL;
  }

  return OK;
}

/// Write a block to disk.
///
/// @return  OK    On success.
///          FAIL  On failure. Could be:
///                - No file.
///                - Could not translate negative block number to positive.
///                - Seek error in swap file.
///                - Write error in swap file.
static int mf_write(memfile_T *mfp, bhdr_T *hp)
{
  bhdr_T *hp2;
  unsigned page_count;      // number of pages written

  if (mfp->mf_fd < 0) {     // there is no file, can't write
    return FAIL;
  }

  if (hp->bh_bnum < 0) {    // must assign file block number
    if (mf_trans_add(mfp, hp) == FAIL) {
      return FAIL;
    }
  }

  unsigned page_size = mfp->mf_page_size;  // number of bytes in a page

  /// We don't want gaps in the file. Write the blocks in front of *hp
  /// to extend the file.
  /// If block 'mf_infile_count' is not in the hash list, it has been
  /// freed. Fill the space in the file with data from the current block.
  while (true) {
    blocknr_T nr = hp->bh_bnum;  // block nr which is being written
    if (nr > mfp->mf_infile_count) {            // beyond end of file
      nr = mfp->mf_infile_count;
      hp2 = pmap_get(int64_t)(&mfp->mf_hash, nr);  // NULL caught below
    } else {
      hp2 = hp;
    }

    // TODO(elmart): Check (page_size * nr) within off_T bounds.
    off_T offset = (off_T)(page_size * nr);  // offset in the file
    if (vim_lseek(mfp->mf_fd, offset, SEEK_SET) != offset) {
      PERROR(_("E296: Seek error in swap file write"));
      return FAIL;
    }
    if (hp2 == NULL) {              // freed block, fill with dummy data
      page_count = 1;
    } else {
      page_count = hp2->bh_page_count;
    }
    unsigned size = page_size * page_count;  // number of bytes written
    void *data = (hp2 == NULL) ? hp->bh_data : hp2->bh_data;
    if ((unsigned)write_eintr(mfp->mf_fd, data, size) != size) {
      /// Avoid repeating the error message, this mostly happens when the
      /// disk is full. We give the message again only after a successful
      /// write or when hitting a key. We keep on trying, in case some
      /// space becomes available.
      if (!did_swapwrite_msg) {
        emsg(_("E297: Write error in swap file"));
      }
      did_swapwrite_msg = true;
      return FAIL;
    }
    did_swapwrite_msg = false;
    if (hp2 != NULL) {                             // written a non-dummy block
      hp2->bh_flags &= ~BH_DIRTY;
    }
    if (nr + (blocknr_T)page_count > mfp->mf_infile_count) {  // appended to file
      mfp->mf_infile_count = nr + page_count;
    }
    if (nr == hp->bh_bnum) {                       // written the desired block
      break;
    }
  }
  return OK;
}

/// Make block number positive and add it to the translation list.
///
/// @return  OK    On success.
///          FAIL  On failure.
static int mf_trans_add(memfile_T *mfp, bhdr_T *hp)
{
  if (hp->bh_bnum >= 0) {                   // it's already positive
    return OK;
  }

  // Get a new number for the block.
  // If the first item in the free list has sufficient pages, use its number.
  // Otherwise use mf_blocknr_max.
  blocknr_T new_bnum;
  bhdr_T *freep = mfp->mf_free_first;
  unsigned page_count = hp->bh_page_count;
  if (freep != NULL && freep->bh_page_count >= page_count) {
    new_bnum = freep->bh_bnum;
    // If the page count of the free block was larger, reduce it.
    // If the page count matches, remove the block from the free list.
    if (freep->bh_page_count > page_count) {
      freep->bh_bnum += page_count;
      freep->bh_page_count -= page_count;
    } else {
      freep = mf_rem_free(mfp);
      xfree(freep);
    }
  } else {
    new_bnum = mfp->mf_blocknr_max;
    mfp->mf_blocknr_max += page_count;
  }

  blocknr_T old_bnum = hp->bh_bnum;            // adjust number
  pmap_del(int64_t)(&mfp->mf_hash, hp->bh_bnum, NULL);
  hp->bh_bnum = new_bnum;
  pmap_put(int64_t)(&mfp->mf_hash, new_bnum, hp);

  // Insert "np" into "mf_trans" hashtable with key "np->nt_old_bnum".
  map_put(int64_t, int64_t)(&mfp->mf_trans, old_bnum, new_bnum);

  return OK;
}

/// Lookup translation from trans list and delete the entry.
///
/// @return  The positive new number  When found.
///          The old number           When not found.
blocknr_T mf_trans_del(memfile_T *mfp, blocknr_T old_nr)
{
  blocknr_T *num = map_ref(int64_t, int64_t)(&mfp->mf_trans, old_nr, false);
  if (num == NULL) {  // not found
    return old_nr;
  }

  mfp->mf_neg_count--;
  blocknr_T new_bnum = *num;

  // remove entry from the trans list
  map_del(int64_t, int64_t)(&mfp->mf_trans, old_nr, NULL);

  return new_bnum;
}

/// Frees mf_fname and mf_ffname.
void mf_free_fnames(memfile_T *mfp)
{
  XFREE_CLEAR(mfp->mf_fname);
  XFREE_CLEAR(mfp->mf_ffname);
}

/// Set the simple file name and the full file name of memfile's swapfile, out
/// of simple file name and some other considerations.
///
/// Only called when creating or renaming the swapfile. Either way it's a new
/// name so we must work out the full path name.
void mf_set_fnames(memfile_T *mfp, char *fname)
{
  mfp->mf_fname = fname;
  mfp->mf_ffname = FullName_save(mfp->mf_fname, false);
}

/// Make name of memfile's swapfile a full path.
///
/// Used before doing a :cd
void mf_fullname(memfile_T *mfp)
{
  if (mfp == NULL || mfp->mf_fname == NULL || mfp->mf_ffname == NULL) {
    return;
  }

  xfree(mfp->mf_fname);
  mfp->mf_fname = mfp->mf_ffname;
  mfp->mf_ffname = NULL;
}

/// Return true if there are any translations pending for memfile.
bool mf_need_trans(memfile_T *mfp)
{
  return mfp->mf_fname != NULL && mfp->mf_neg_count > 0;
}

/// Open memfile's swapfile.
///
/// "fname" must be in allocated memory, and is consumed (also when error).
///
/// @param  flags  Flags for open().
/// @return A bool indicating success of the `open` call.
static bool mf_do_open(memfile_T *mfp, char *fname, int flags)
{
  // fname cannot be NameBuff, because it must have been allocated.
  mf_set_fnames(mfp, fname);
  assert(mfp->mf_fname != NULL);

  /// Extra security check: When creating a swap file it really shouldn't
  /// exist yet. If there is a symbolic link, this is most likely an attack.
  FileInfo file_info;
  if ((flags & O_CREAT)
      && os_fileinfo_link(mfp->mf_fname, &file_info)) {
    mfp->mf_fd = -1;
    emsg(_("E300: Swap file already exists (symlink attack?)"));
  } else {
    // try to open the file
    mfp->mf_fd = os_open(mfp->mf_fname, flags | O_NOFOLLOW, S_IREAD | S_IWRITE);
  }

  // If the file cannot be opened, use memory only
  if (mfp->mf_fd < 0) {
    mf_free_fnames(mfp);
    return false;
  }

  (void)os_set_cloexec(mfp->mf_fd);

  return true;
}
