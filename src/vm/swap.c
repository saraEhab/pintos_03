#include "vm/swap.h"
#include <bitmap.h>
#include <debug.h>
#include <stdio.h>
#include "vm/frame.h"
#include "vm/page.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/* The swap device. */
static struct block *swap_device;

/* Used swap pages.
 * bitmap means array of bits*/
static struct bitmap *swap_bitmap;

/* Protects swap_bitmap. */
static struct lock swap_lock;

/* Number of sectors per page. */
#define PAGE_SECTORS (PGSIZE / BLOCK_SECTOR_SIZE)

/* Sets up swap. */
void
swap_init(void) {
    /* block_get_role : returns the block device that will do the given ROLE, or a null
   pointer if no block device has been assigned that role. */
    swap_device = block_get_role(BLOCK_SWAP);
    if (swap_device == NULL) {
        printf("no swap device--swap disabled\n");
        swap_bitmap = bitmap_create(0);
    } else
        swap_bitmap = bitmap_create(block_size(swap_device)
                                    / PAGE_SECTORS);
    if (swap_bitmap == NULL)
        PANIC("couldn't create swap bitmap");
    lock_init(&swap_lock);
}

/* Swaps in page P which means put page p in main memory
 * , which must have a locked frame
 *  (and be swapped out). */
void
swap_in(struct page *p) {
    size_t i;

    //make sure that the page has an allocated frame in the main memory
    ASSERT(p->frame != NULL);
    /*make sure that this frame is locked by the current thread
     * which is the thread that is going to move the page sectors in the main memory*/
    ASSERT(lock_held_by_current_thread(&p->frame->lock));
    //check that this sector hasn't been moved before in the main memory
    ASSERT(p->sector != (block_sector_t) - 1);

    for (i = 0; i < PAGE_SECTORS; i++) {
        /*read the block (page) sector by sector and put it in the buffer
           to write it in the allocated frame for that page in the main memory
           */
        block_read(swap_device, p->sector + i,
                   p->frame->base + i * BLOCK_SECTOR_SIZE);
    }
    /*reset(make it equal to false) the sector that has been moved to the main memory*/
    bitmap_reset(swap_bitmap, p->sector / PAGE_SECTORS);
    /*after moving one sector from page p to the main memory
     * decrease the number of sectors for this page by 1*/
    p->sector = (block_sector_t) - 1;
}

/* Swaps out page P, which must have a locked frame. */
bool
swap_out(struct page *p) {
    size_t slot;
    size_t i;

    //make sure that the page has an allocated frame in the main memory
    ASSERT(p->frame != NULL);
    /*make sure that this frame is locked by the current thread
     * which is the thread that is going to move the page sectors in the main memory*/
    ASSERT(lock_held_by_current_thread(&p->frame->lock));

    lock_acquire(&swap_lock);
    /*finds the first group of bits in the swap_bitmap
     * which their values are false and set them to true*/

    /* Finds the first group of CNT consecutive bits in B at or after
   START that are all set to VALUE, flips them all to !VALUE,
   and returns the index of the first bit in the group.
   If there is no such group, returns BITMAP_ERROR.
   If CNT is zero, returns 0.
   Bits are set atomically, but testing bits is not atomic with
   setting them.*/
    /*size_t
    bitmap_scan_and_flip (struct bitmap *b, size_t start, size_t cnt, bool value)*/
    slot = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
    lock_release(&swap_lock);
    if (slot == BITMAP_ERROR)
        //there is no group of bits starts with value = false
        return false;

    p->sector = slot * PAGE_SECTORS;

    /*  Write out page sectors for each modified block. */
    for (i = 0; i < PAGE_SECTORS; i++) {
        //write the sector (p->sector + i) to the swap_device block from the buffer (p->frame->base + i * BLOCK_SECTOR_SIZE)
        block_write(swap_device, p->sector + i,
                    (uint8_t *) p->frame->base + i * BLOCK_SECTOR_SIZE);
    }

    p->write_back = false; /* don't write back to file*/
    p->file = NULL;
    p->file_offset = 0;
    p->file_bytes = 0;/*Bytes to read/write = 0*/

    return true;
}