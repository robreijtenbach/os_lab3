/* EdFS -- An educational file system
 *
 * Copyright (C) 2019  Leiden University, The Netherlands.
 */

#include "edfs-common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * EdFS image management
 */

void
edfs_image_close(edfs_image_t *img)
{
  if (!img)
    return;

  if (img->fd >= 0)
    close(img->fd);

  free(img);
}

/* Read and verify super block. */
static bool
edfs_read_super(edfs_image_t *img)
{
  if (pread(img->fd, &img->sb, sizeof(edfs_super_block_t), EDFS_SUPER_BLOCK_OFFSET) < 0)
    {
      fprintf(stderr, "error: file '%s': %s\n",
              img->filename, strerror(errno));
      return false;
    }

  if (img->sb.magic != EDFS_MAGIC)
    {
      fprintf(stderr, "error: file '%s': EdFS magic number mismatch.\n",
              img->filename);
      return false;
    }

  /* Simple sanity check of size of file system image. */
  struct stat buf;

  if (fstat(img->fd, &buf) < 0)
    {
      fprintf(stderr, "error: file '%s': stat failed? (%s)\n",
              img->filename, strerror(errno));
      return false;
    }

  if (buf.st_size < edfs_get_size(&img->sb))
    {
      fprintf(stderr, "error: file '%s': file system size larger than image size.\n",
              img->filename);
      return false;
    }

  /* FIXME: implement more sanity checks? */

  return true;
}

edfs_image_t *
edfs_image_open(const char *filename, bool read_super)
{
  edfs_image_t *img = malloc(sizeof(edfs_image_t));

  img->filename = filename;
  img->fd = open(img->filename, O_RDWR);
  if (img->fd < 0)
    {
      fprintf(stderr, "error: could not open file '%s': %s\n",
              img->filename, strerror(errno));
      edfs_image_close(img);
      return NULL;
    }

  /* Load super block into memory. */
  if (read_super && !edfs_read_super(img))
    {
      edfs_image_close(img);
      return NULL;
    }

  return img;
}

/*
 * Inode-related routine helper functions
 */

/*
 * read entire blk into blk_buf, assume blk_buf is large enough
 * trailing bytes are 0 padded
 */
static int edfs_read_inode_data_blk(edfs_image_t *img, edfs_inode_t *inode,
                                    uint32_t id, void *buf)
{
    const uint16_t BLK_SIZE = img->sb.block_size;

    // TODO hele berg checks size etc. kijken of id niet te groot is

    edfs_block_t block;
    if (id < EDFS_INODE_N_DIRECT_BLOCKS) {
        block = inode->inode.direct[id];
    }
    else {
        edfs_block_t indirect_block = inode->inode.indirect;
        if (indirect_block == EDFS_BLOCK_INVALID)
            return -EIO;
        off_t indirect_block_off = edfs_get_block_offset(&img->sb, indirect_block);

        edfs_block_t indirect_blocks[EDFS_MAX_BLOCK_SIZE / sizeof(edfs_block_t)];
        int indirect_blocks_size = img->sb.block_size;

        ssize_t ret = pread(img->fd, indirect_blocks, indirect_blocks_size, indirect_block_off);
        if (ret != indirect_blocks_size)
            return -EIO; 

        block = indirect_blocks[id - EDFS_INODE_N_DIRECT_BLOCKS];
    }

    fprintf(stderr, "id == %d blocknumber = %d%c", (int)id, (int)block, '\n');

    if (block == EDFS_BLOCK_INVALID)
        return 0;

    // TODO kijen of het block wel past, dan 0 padden ofzo als je daar nog zin
    // in hebt

    off_t off = edfs_get_block_offset(&img->sb, block);
    fprintf(stderr, "off = %d\n", (int)off);
    if (pread(img->fd, buf, BLK_SIZE, off) != BLK_SIZE)
        return -errno; 

    return BLK_SIZE;
}

static int edfs_write_inode_data_blk(edfs_image_t *img, edfs_inode_t *inode,
                                    uint32_t id, const void *buf)
{
    const uint16_t BLK_SIZE = img->sb.block_size;

    // TODO hele berg checks size etc. kijken of id niet te groot is

    edfs_block_t block;
    if (id < EDFS_INODE_N_DIRECT_BLOCKS) {
        block = inode->inode.direct[id];
    }
    else {
        edfs_block_t indirect_block = inode->inode.indirect;
        if (indirect_block == EDFS_BLOCK_INVALID)
            return -EIO;
        off_t indirect_block_off = edfs_get_block_offset(&img->sb, indirect_block);

        edfs_block_t indirect_blocks[EDFS_MAX_BLOCK_SIZE / sizeof(edfs_block_t)];
        int indirect_blocks_size = img->sb.block_size;

        ssize_t ret = pread(img->fd, indirect_blocks, indirect_blocks_size, indirect_block_off);
        if (ret != indirect_blocks_size)
            return -EIO; 

        block = indirect_blocks[id - EDFS_INODE_N_DIRECT_BLOCKS];
    }

    fprintf(stderr, "id == %d blocknumber = %d%c", (int)id, (int)block, '\n');

    if (block == EDFS_BLOCK_INVALID)
        return 0;

    off_t off = edfs_get_block_offset(&img->sb, block);
    fprintf(stderr, "off = %d\n", (int)off);
    if (pwrite(img->fd, buf, BLK_SIZE, off) != BLK_SIZE)
        return -errno; 

    return BLK_SIZE;
}

/*
 * Inode-related routines
 */


/* Read inode from disk, inode->inumber must be set to the inode number
 * to be read from disk.
 */
int
edfs_read_inode(edfs_image_t *img,
                edfs_inode_t *inode)
{
  if (inode->inumber >= img->sb.inode_table_n_inodes)
    return -ENOENT;

  off_t offset = edfs_get_inode_offset(&img->sb, inode->inumber);
  return pread(img->fd, &inode->inode, sizeof(edfs_disk_inode_t), offset);
}

/* Reads the root inode from disk. @inode must point to a valid
 * inode structure.
 */
int
edfs_read_root_inode(edfs_image_t *img,
                     edfs_inode_t *inode)
{
  inode->inumber = img->sb.root_inumber;
  fprintf(stderr, "root inode = %d", (int)img->sb.root_inumber);
  return edfs_read_inode(img, inode);
}

/* Writes @inode to disk, inode->inumber must be set to a valid
 * inode number to which the inode will be written.
 */
int
edfs_write_inode(edfs_image_t *img, edfs_inode_t *inode)
{
  if (inode->inumber >= img->sb.inode_table_n_inodes)
    return -ENOENT;

  off_t offset = edfs_get_inode_offset(&img->sb, inode->inumber);
  return pwrite(img->fd, &inode->inode, sizeof(edfs_disk_inode_t), offset);
}

/* Clears the specified inode on disk, based on inode->inumber.
 */
int
edfs_clear_inode(edfs_image_t *img, edfs_inode_t *inode)
{
  if (inode->inumber >= img->sb.inode_table_n_inodes)
    return -ENOENT;

  off_t offset = edfs_get_inode_offset(&img->sb, inode->inumber);

  edfs_disk_inode_t disk_inode;
  memset(&disk_inode, 0, sizeof(edfs_disk_inode_t));
  return pwrite(img->fd, &disk_inode, sizeof(edfs_disk_inode_t), offset);
}

/* Finds a free inode and returns the inumber. NOTE: this does NOT
 * allocate the inode. Only after a valid inode has been written
 * to this inumber, this inode is allocated in the table.
 */
edfs_inumber_t
edfs_find_free_inode(edfs_image_t *img)
{
  edfs_inode_t inode = { .inumber = 1 };

  while (inode.inumber < img->sb.inode_table_n_inodes)
    {
      if (edfs_read_inode(img, &inode) > 0 &&
          inode.inode.type == EDFS_INODE_TYPE_FREE)
        return inode.inumber;

      inode.inumber++;
    }

  return 0;
}

/* Create a new inode. Searches for a free inode in the inode table (returns
 * -ENOSPC if the inode table is full). @inode is initialized accordingly.
 */
int
edfs_new_inode(edfs_image_t *img,
               edfs_inode_t *inode,
               edfs_inode_type_t type)
{
  edfs_inumber_t inumber;

  inumber = edfs_find_free_inode(img);
  if (inumber == 0)
    return -ENOSPC;

  memset(inode, 0, sizeof(edfs_inode_t));
  inode->inumber = inumber;
  inode->inode.type = type;

  return 0;
}

int
edfs_read_inode_data(edfs_image_t *img,
                     edfs_inode_t *inode,
                     void *buf,
                     uint32_t size,
                     uint32_t off)
{
    // TODO dingen verifiereren 

    const uint16_t BLK_SIZE = img->sb.block_size;
    printf("blk _size %d\n", (int)BLK_SIZE);

    for (uint32_t pos = off; pos < off + size; )
    {
        uint32_t blk_id = pos / BLK_SIZE;
        uint32_t blk_off = pos % BLK_SIZE;
        uint32_t blk_size = (off + size) - pos;
        if (blk_size > BLK_SIZE - blk_off)
            blk_size = BLK_SIZE - blk_off;

        fprintf(stderr, "reading blk = %d from %d to %d\n",
            (int)blk_id, (int)blk_off, (int)(blk_off + blk_size));

        char blk_buf[EDFS_MAX_BLOCK_SIZE];
        int ret = edfs_read_inode_data_blk(img, inode, blk_id, blk_buf);
        if (ret <= 0)
            return ret;

        memcpy((char *)buf + (pos - off), blk_buf + blk_off, blk_size);

        pos += blk_size;
    }

    return size;
}

int
edfs_write_inode_data(edfs_image_t *img,
                     edfs_inode_t *inode,
                     const void *buf,
                     uint32_t size,
                     uint32_t off)
{
    // TODO dingen verifiereren 

    const uint16_t BLK_SIZE = img->sb.block_size;
    printf("blk _size %d\n", (int)BLK_SIZE);

    for (uint32_t pos = off; pos < off + size; )
    {
        uint32_t blk_id = pos / BLK_SIZE;
        uint32_t blk_off = pos % BLK_SIZE;
        uint32_t blk_size = (off + size) - pos;
        if (blk_size > BLK_SIZE - blk_off)
            blk_size = BLK_SIZE - blk_off;

        fprintf(stderr, "writing blk = %d from %d to %d\n",
            (int)blk_id, (int)blk_off, (int)(blk_off + blk_size));

        char blk_buf[EDFS_MAX_BLOCK_SIZE];

        int ret = edfs_read_inode_data_blk(img, inode, blk_id, blk_buf);
        if (ret <= 0)
            return ret;

        memcpy(blk_buf + blk_off, (char *)buf + (pos - off), blk_size);

        ret = edfs_write_inode_data_blk(img, inode, blk_id, blk_buf);
        if (ret <= 0)
            return ret;


        pos += blk_size;
    }

    return size;
}


int
edfs_bitmap_clear(edfs_image_t *img, edfs_block_t block)
{
    //check if block number is valid
    if (block >= img->sb.n_blocks)
        return -EINVAL;

    size_t byte_off = img->sb.bitmap_start + (block / 8);
    int bit_off = block % 8;

    uint8_t bitmap_data;
    
    ssize_t ret = pread(img->fd, &bitmap_data, 1, byte_off);
    if (ret < 0)
        return -errno;
    else if (ret != 1)
        return -EIO;
    
    bitmap_data &= ~(((uint8_t)1) << bit_off);
    
    ret = pwrite(img->fd, &bitmap_data, 1, byte_off);
    if (ret < 0)
        return -errno;
    else if (ret != 1)
        return -EIO;

    return 0;
}

int
edfs_bitmap_set(edfs_image_t *img, edfs_block_t block)
{
    //check if block number is valid
    if (block >= img->sb.n_blocks)
        return -EINVAL;

    size_t byte_off = img->sb.bitmap_start + block / 8;
    int bit_off = block % 8;

    uint8_t bitmap_data;
    
    ssize_t ret = pread(img->fd, &bitmap_data, 1, byte_off);
    if (ret < 0)
        return -errno;
    else if (ret != 1)
        return -EIO;
    
    bitmap_data |= ((uint8_t)1) << bit_off;
    
    ret = pwrite(img->fd, &bitmap_data, 1, byte_off);
    if (ret < 0)
        return -errno;
    else if (ret != 1)
        return -EIO;

    return 0;
}

