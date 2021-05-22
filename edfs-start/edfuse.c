/* EdFS -- An educational file system
 *
 * Copyright (C) 2017,2019  Leiden University, The Netherlands.
 */

#define FUSE_USE_VERSION 26


#include "edfs-common.h"


#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>

#include <stdbool.h>

/* 
 * Checks if file or folder names are valid
 */
static int edfs_check_filename(const char *name)
{
    if (name == NULL)
        return -EINVAL;

    size_t len = strlen(name);

    if (len == 0) {
        return -EINVAL;
    } else if (len > 59) {
        return -ENAMETOOLONG;
    }

    for (size_t i = 0; i < len; i++) {
        char curr_char = name[i];
        if (!(isalpha(curr_char) ||
              isdigit(curr_char) ||
              curr_char == ' ' || 
              curr_char == '.'))
            return -EINVAL;
    }

    return 0;
}

static int
edfs_add_dir_entry(edfs_image_t *img, edfs_inode_t *inode,
                   const char *name, edfs_inumber_t inumber)
{
    const uint32_t n_entries =
        EDFS_INODE_N_DIRECT_BLOCKS * img->sb.block_size / sizeof(edfs_dir_entry_t);

    int ret = edfs_check_filename(name);
    if (ret != 0)
        return ret;

    uint32_t entry_off = UINT32_MAX;
    for (uint32_t i = 0; i < n_entries; i++)
    {
        edfs_dir_entry_t tmp;
        uint32_t off = i * sizeof(edfs_dir_entry_t);
        ret = edfs_read_inode_data(img, inode, &tmp, sizeof(tmp), off);
        if (ret < 0)
            return ret;
        else if (ret == 0) {
            /*
             * if ret == 0, we attempt to read from an unallocated block,
             * thus we can store an new entry at this location
             */
            if (entry_off == UINT32_MAX)
                entry_off = off;

            continue;
        }

        if (tmp.inumber == EDFS_BLOCK_INVALID) {
            if (entry_off == UINT32_MAX)
                entry_off = off;
            
            continue;
        }

        if (strcmp(name, tmp.filename) == 0)
            return -EEXIST;
    }

    if (entry_off == UINT32_MAX)
        return -ENOMSG;

    edfs_dir_entry_t tmp;
    tmp.inumber = inumber;
    strcpy(tmp.filename, name);
    ret = 0;
    ret = edfs_write_inode_data(img, inode, &tmp, sizeof(tmp), entry_off);
    if (ret < 0)
        return ret;
    else if (ret != sizeof(tmp))
        return -EIO;

    return 0;
}

static int 
edfs_remove_dir_entry(edfs_image_t *img, edfs_inode_t *inode, const char *name) 
{
    const uint32_t n_entries =
        EDFS_INODE_N_DIRECT_BLOCKS * img->sb.block_size / sizeof(edfs_dir_entry_t);

    for (uint32_t i = 0; i < n_entries; i++)
    {

        edfs_dir_entry_t tmp;
        uint32_t off = i * sizeof(edfs_dir_entry_t);
        int ret = edfs_read_inode_data(img, inode, &tmp, sizeof(tmp), off);
        if (ret < 0)
            return ret;
        else if (ret == 0)
            continue;

        if (tmp.inumber == EDFS_BLOCK_INVALID)
            continue;

        if (strcmp(name, tmp.filename) == 0) {
            // clear entry and write
            memset(&tmp, 0, sizeof(tmp));
            ret = edfs_write_inode_data(img, inode, &tmp, sizeof(tmp), off);
            if (ret < 0)
                return ret;
            else if (ret == 0)
                return -EIO;

            return 0;
        }
    }

    return -ENOENT;
}


static inline edfs_image_t *
get_edfs_image(void)
{
  return (edfs_image_t *)fuse_get_context()->private_data;
}


/*
 * returns negative on error or the number of entries on success
 * entries must be at least EDFS_MAX_DIR_ENTRIES
*/
static int edfs_get_dir_entries(edfs_image_t *img, edfs_inode_t *inode, edfs_dir_entry_t *entries)
{
    uint32_t n_entries = EDFS_INODE_N_DIRECT_BLOCKS * img->sb.block_size / sizeof(edfs_dir_entry_t);
    int count = 0;
    for (uint32_t i = 0; i < n_entries; i++)
    {
        edfs_dir_entry_t tmp;
        uint32_t off = i * sizeof(edfs_dir_entry_t);
        int ret = edfs_read_inode_data(img, inode, &tmp, sizeof(tmp), off);
        if (ret < 0)
            return ret;
        else if (ret == 0)
            continue;

        if (tmp.inumber == EDFS_BLOCK_INVALID)
            continue;

        entries[count++] = tmp;
    }

    return count;
}

/* Searches the file system hierarchy to find the inode for
 * the given path. Returns true if the operation succeeded.
 */
static bool
edfs_find_inode(edfs_image_t *img,
                const char *path,
                edfs_inode_t *inode)
{
  if (strlen(path) == 0 || path[0] != '/')
    return false;

  edfs_inode_t current_inode;
  edfs_read_root_inode(img, &current_inode);

  while (path && (path = strchr(path, '/')))
    {
      /* Ignore path separator */
      while (*path == '/')
        path++;

      /* Find end of new component */
      char *end = strchr(path, '/');
      if (!end)
        {
          int len = strnlen(path, PATH_MAX);
          if (len > 0)
            end = (char *)&path[len];
          else
            {
              /* We are done: return current entry. */
              *inode = current_inode;
              return true;
            }
        }

      /* Verify length of component is not larger than maximum allowed
       * filename size.
       */
      int len = end - path;
      if (len >= EDFS_FILENAME_SIZE)
        return false;

      /* Within the directory pointed to by parent_inode, find the
       * inode number for path, len.
       */
      edfs_dir_entry_t direntry = { 0, };
      strncpy(direntry.filename, path, len);
      direntry.filename[len] = 0;

      if (direntry.filename[0] != 0)
        {
          bool found = false;

          const uint32_t n_entries =
            EDFS_INODE_N_DIRECT_BLOCKS * img->sb.block_size / sizeof(edfs_dir_entry_t);
          for (uint32_t i = 0; i < n_entries; i++)
          {
              edfs_dir_entry_t tmp;
              uint32_t off = i * sizeof(edfs_dir_entry_t);
              int ret = edfs_read_inode_data(img, &current_inode, &tmp, sizeof(tmp), off);
              if (ret < 0) {
                  return false;
              }
              else if (ret == 0)
                  continue; // this can happen when we try to read from an invalid block

              if (tmp.inumber == 0)
                  continue; // empty file

              if (strcmp(direntry.filename, tmp.filename) == 0) {
                  direntry.inumber = tmp.inumber;
                  found = true;
                  break; // no need to keep searching
              }
          }

          if (found)
            {
              /* Found what we were looking for, now get our new inode. */
              current_inode.inumber = direntry.inumber;
              edfs_read_inode(img, &current_inode);
            }
          else
            return false;
        }

      path = end;
    }

  *inode = current_inode;

  return true;
}

static inline void
drop_trailing_slashes(char *path_copy)
{
  int len = strlen(path_copy);
  while (len > 0 && path_copy[len-1] == '/')
    {
      path_copy[len-1] = 0;
      len--;
    }
}

/* Return the parent inode, for the containing directory of the inode (file or
 * directory) specified in @path. Returns 0 on success, error code otherwise.
 *
 * (This function is not yet used, but will be useful for your
 * implementation.)
 */
static int
edfs_get_parent_inode(edfs_image_t *img,
                      const char *path,
                      edfs_inode_t *parent_inode)
{
  int res;
  char *path_copy = strdup(path);

  drop_trailing_slashes(path_copy);

  if (strlen(path_copy) == 0)
    {
      res = -EINVAL;
      goto out;
    }

  /* Extract parent component */
  char *sep = strrchr(path_copy, '/');
  if (!sep)
    {
      res = -EINVAL;
      goto out;
    }

  if (path_copy == sep)
    {
      /* The parent is the root directory. */
      edfs_read_root_inode(img, parent_inode);
      res = 0;
      goto out;
    }

  /* If not the root directory for certain, start a usual search. */
  *sep = 0;
  char *dirname = path_copy;

  if (!edfs_find_inode(img, dirname, parent_inode))
    {
      res = -ENOENT;
      goto out;
    }

  res = 0;

out:
  free(path_copy);

  return res;
}

/* Separates the basename (the actual name of the file) from the path.
 * The return value must be freed.
 *
 * (This function is not yet used, but will be useful for your
 * implementation.)
 */
static char *
edfs_get_basename(const char *path)
{
  char *res = NULL;
  char *path_copy = strdup(path);

  drop_trailing_slashes(path_copy);

  if (strlen(path_copy) == 0)
    {
      res = NULL;
      goto out;
    }

  /* Find beginning of basename. */
  char *sep = strrchr(path_copy, '/');
  if (!sep)
    {
      res = NULL;
      goto out;
    }

  res = strdup(sep + 1);

out:
  free(path_copy);

  return res;
}


/*
 * Implementation of necessary FUSE operations.
 */

static int
edfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info *fi)
{
    edfs_image_t *img = get_edfs_image();
    edfs_inode_t inode = { 0, };

    if (!edfs_find_inode(img, path, &inode))
    return -ENOENT;

    if (!edfs_disk_inode_is_directory(&inode.inode))
    return -ENOTDIR;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    /* TODO: traverse all valid directory entries of @inode and call
    * the filler function (as done above) for each entry. The second
    * argument of the filler function is the filename you want to add.
    */

    // TODO eigenlijk dir entries functie gebruiken en dan geen dubbele code etc.
    uint32_t n_entries = EDFS_INODE_N_DIRECT_BLOCKS * img->sb.block_size / sizeof(edfs_dir_entry_t);
    for (uint32_t i = 0; i < n_entries; i++)
    {
        edfs_dir_entry_t tmp;
        uint32_t off = i * sizeof(edfs_dir_entry_t);
        int ret = edfs_read_inode_data(img, &inode, &tmp, sizeof(tmp), off);
        if (ret < 0)
            return -1; 
        else if (ret == 0)
            continue;

        if (tmp.inumber == EDFS_BLOCK_INVALID)
            continue;

        filler(buf, tmp.filename, NULL, 0);
    }

    return 0;
}

static int
edfuse_mkdir(const char *path, mode_t mode)
{
    /* TODO: Fix
     *
     * Create a new inode, register in parent directory, write inode to
     * disk.
     */

    edfs_image_t *img = get_edfs_image();

    char *name = edfs_get_basename(path);
    if (name == NULL)
        return -ENOMSG;

    int ret = edfs_check_filename(name);
    if (ret != 0) {
        free(name);
        return ret;
    }

    edfs_inode_t parent_inode;
    ret = edfs_get_parent_inode(img, path, &parent_inode);
    if (ret != 0) {
        free(name);
        return ret;
    }

    edfs_inode_t inode;
    ret = edfs_new_inode(img, &inode, EDFS_INODE_TYPE_DIRECTORY);
    if (ret != 0) {
        free(name);
        return ret;
    }

    ret = edfs_write_inode(img, &inode);
    if (ret < 0) {
        free(name);
        return ret;
    }

    ret = edfs_add_dir_entry(img, &parent_inode, name, inode.inumber);
    if (ret != 0) {
        free(name);
        return ret;
    }

    free(name);

    return 0;
}

static int
edfuse_rmdir(const char *path)
{
    /* 
     * Validate @path exists and is a directory; remove directory entry
     * from parent directory; release allocated blocks; release inode.
     */

    edfs_image_t *img = get_edfs_image();

    char *name = edfs_get_basename(path);
    if (name == NULL)
        return -ENOMSG;

    int ret = edfs_check_filename(name); 
    if (ret != 0) {
        free(name);
        return ret;
    }

    edfs_inode_t inode;
    if (!edfs_find_inode(img, path, &inode)) {
        free(name);
        return -ENOENT;
    }

    /* check if dirisempty */
    edfs_dir_entry_t entries[EDFS_MAX_DIR_ENTRIES];
    ret = edfs_get_dir_entries(img, &inode, entries);
    if (ret < 0) {
        free(name);
        return ret;
    }
    else if (ret != 0) {
        free(name);
        return -ENOTEMPTY;
    }

    edfs_inode_t parent_inode;
    if (edfs_get_parent_inode(img, path, &parent_inode) != 0) {
        free(name);
        return -EIO; /* or ENOENT but this should not happen anyway */
    }

    /* remove in parent */
    if (edfs_remove_dir_entry(img, &parent_inode, name) != 0) {
        free(name);
        return -EIO;
    }

    free(name);

    /* After this point the the FS is corrupted if any of the following operations fail */
      
    for (int i = 0; i < EDFS_INODE_N_DIRECT_BLOCKS; i++)
    {
        edfs_block_t block = inode.inode.direct[i];
        if (block == EDFS_BLOCK_INVALID)
            continue;

        edfs_bitmap_clear(img, block);
    }

    /* set inode to free state */
    if (edfs_clear_inode(img, &inode) <= 0)
        return -EIO;

    return 0;
}


/* Get attributes of @path, fill @stbuf. At least mode, nlink and
 * size must be filled here, otherwise the "ls" listings appear busted.
 * We assume all files and directories have rw permissions for owner and
 * group.
 */
static int
edfuse_getattr(const char *path, struct stat *stbuf)
{
  int res = 0;
  edfs_image_t *img = get_edfs_image();


  memset(stbuf, 0, sizeof(struct stat));
  if (strcmp(path, "/") == 0)
    {
      stbuf->st_mode = S_IFDIR | 0755;
      stbuf->st_nlink = 2;
      return res;
    }

  edfs_inode_t inode;
  if (!edfs_find_inode(img, path, &inode)) {
    res = -ENOENT;
  }
  else
    {
      if (edfs_disk_inode_is_directory(&inode.inode))
        {
          stbuf->st_mode = S_IFDIR | 0770;
          stbuf->st_nlink = 2;
        }
      else
        {
          stbuf->st_mode = S_IFREG | 0660;
          stbuf->st_nlink = 1;
        }
      stbuf->st_size = inode.inode.size;

      /* Note that this setting is ignored, unless the FUSE file system
       * is mounted with the 'use_ino' option.
       */
      stbuf->st_ino = inode.inumber;
    }

  return res;
}

/* Open file at @path. Verify it exists by finding the inode and
 * verify the found inode is not a directory. We do not maintain
 * state of opened files.
 */
static int
edfuse_open(const char *path, struct fuse_file_info *fi)
{
  edfs_image_t *img = get_edfs_image();

  edfs_inode_t inode;
  if (!edfs_find_inode(img, path, &inode))
    return -ENOENT;

  /* Open may only be called on files. */
  if (edfs_disk_inode_is_directory(&inode.inode))
    return -EISDIR;

  return 0;
}

static int
edfuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
  /* TODO: implement
   *
   * Create a new inode, attempt to register in parent directory,
   * write inode to disk.
   */
  return -ENOSYS;
}

/* Since we don't maintain link count, we'll treat unlink as a file
 * remove operation.
 */
static int
edfuse_unlink(const char *path)
{
  /* TODO: implement
   *
   * Validate @path exists and is not a directory; remove directory entry
   * from parent directory; release allocated blocks; release inode.
   */
  return -ENOSYS;
}

static int
edfuse_read(const char *path, char *buf, size_t size, off_t offset,
            struct fuse_file_info *fi)
{
    edfs_image_t *img = get_edfs_image();

    edfs_inode_t inode;
    if (!edfs_find_inode(img, path, &inode))
        return -ENOENT;

    if (inode.inode.size <= offset)
        return -EINVAL;

    if (inode.inode.type == EDFS_INODE_TYPE_DIRECTORY)
        return -EISDIR;
    else if (inode.inode.type != EDFS_INODE_TYPE_FILE)
        return -EIO; // can happen when fs is corrupted

    size_t bytes_to_read = size;
    if (inode.inode.size - offset < size)
        bytes_to_read = inode.inode.size - offset;

    // note: because we check the size of bytes_to_read the value always
    // fits into a uint32_t hence the cast is valid
    return edfs_read_inode_data(img, &inode, buf, (uint32_t)bytes_to_read, offset);
}

static int
edfuse_write(const char *path, const char *buf, size_t size, off_t offset,
             struct fuse_file_info *fi)
{
  /* TODO: implement
   *
   * Write @size bytes of data from @buf to @path starting at @offset.
   * Allocate new blocks as necessary. You may have to fill holes! Update
   * the file size if necessary.
   */
  return -ENOSYS;
}


static int
edfuse_truncate(const char *path, off_t offset)
{
  /* TODO: implement
   *
   * The size of @path must be set to be @offset. Release now superfluous
   * blocks or allocate new blocks that are necessary to cover offset.
   */
  return -ENOSYS;
}


/*
 * FUSE setup
 */

static struct fuse_operations edfs_oper =
{
  .readdir   = edfuse_readdir,
  .mkdir     = edfuse_mkdir,
  .rmdir     = edfuse_rmdir,
  .getattr   = edfuse_getattr,
  .open      = edfuse_open,
  .create    = edfuse_create,
  .unlink    = edfuse_unlink,
  .read      = edfuse_read,
  .write     = edfuse_write,
  .truncate  = edfuse_truncate,
};

int
main(int argc, char *argv[])
{
  /* Count number of arguments without hyphens; excluding execname */
  int count = 0;
  for (int i = 1; i < argc; ++i)
    if (argv[i][0] != '-')
      count++;

  if (count != 2)
    {
      fprintf(stderr, "error: file and mountpoint arguments required.\n");
      return -1;
    }

  /* Extract filename argument; we expect this to be the
   * penultimate argument.
   */
  /* FIXME: can't this be better handled using some FUSE API? */
  const char *filename = argv[argc-2];
  argv[argc-2] = argv[argc-1];
  argv[argc-1] = NULL;
  argc--;

  /* Try to open the file system */
  edfs_image_t *img = edfs_image_open(filename, true);
  if (!img)
    return -1;

  /* Start fuse main loop */
  int ret = fuse_main(argc, argv, &edfs_oper, img);
  edfs_image_close(img);

  return ret;
}
