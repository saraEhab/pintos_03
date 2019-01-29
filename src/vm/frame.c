#include "vm/frame.h"
#include <stdio.h>
#include "vm/page.h"
#include "devices/timer.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

static struct frame *frames;
static size_t frame_cnt;

static struct lock scan_lock;
static size_t hand;

/* Initialize the frame manager.
 * in this function it tries to add (divide) new frames to the main memory*/
void
frame_init(void) {
    //base address (start address) for the new frame
    void *base;
    /* Initializes scan_lock .
     * scan_lock can be held by at most a single thread so one thread is going to allocate the frames .
     * only one thread can be inside this code sector at a time */
    lock_init(&scan_lock);
    /* malloc : obtains and returns a new block(new block means new allocated frame)
     * of at least size (frames * init_ram_pages) bytes.
     * Returns a null pointer if memory is not available. */
    frames = malloc(sizeof *frames * init_ram_pages);
    if (frames == NULL)
        PANIC("out of memory allocating page frames");
/*palloc_get_page(PAL_USER): Obtains a single free page and returns its kernel virtual
   address.
   If PAL_USER is set, the page is obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the page is filled with zeros.  If no pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
/*PAL_USER : is one of the palloc flags ,it equals to 004 and it called a user page*/

//base address for the new frame != null , then it's a new frame added to the memory
    while ((base = palloc_get_page(PAL_USER)) != NULL) {
        //increase the size of the frames that exists in the main memory by one
        struct frame *f = &frames[frame_cnt++];
        /*initialize the frame lock to 1 (1 means that the lock is opened for that frame ,means it's a free  frame )*/
        lock_init(&f->lock);
        //base address for the frame
        f->base = base;
        //doesn't contain a page yet it's a free frame
        f->page = NULL;
    }
}

/* Tries to allocate and lock a frame for PAGE.
   Returns the frame if successful, false on failure. */
static struct frame *
try_frame_alloc_and_lock(struct page *page) {
    size_t i;
/*put a lock so only one thread can search for a free frame at a time*/
    lock_acquire(&scan_lock);

    /* Find a free frame. */
    for (i = 0; i < frame_cnt; i++) {
        struct frame *f = &frames[i];
        if (!lock_try_acquire(&f->lock))
            /*if it isn't a suitable frame for the given page go to the loop to look in the next index (look for another frame) */
            continue;
        /*the below condition means that he find the free frame
         * make sure that it's free by checking that its page equals null*/
        if (f->page == NULL) {
            f->page = page;
            /*release the lock so another thread can enter that part of the code to search for a free frame*/
            lock_release(&scan_lock);
            return f;
        }
        /*release the lock for the current frame which is suitable for this page but it isn't free (like the clock algorithm)*/
        lock_release(&f->lock);
    }

    /*the first loop fails to find a free frame,
     * all frames are busy and their locks are released by the first loop*/
    /* No free frame.  Find a frame to evict. */
    for (i = 0; i < frame_cnt * 2; i++) {
        /* Get a frame at index hand */
        struct frame *f = &frames[hand];
        //if hand + 1 >= the size of the available frames in the main memory (frame_cnt)
        if (++hand >= frame_cnt)
            /*make hand = 0 to search from the beginning because it might not start from the first place
             * as it starts from the last place it puts a new page in it (like the clock algorithm)*/
            hand = 0;

        /*if you cann't find a suitable frame continue which means go ahead for the loop to
         * go to the next position*/
        if (!lock_try_acquire(&f->lock))
            continue;

        /*if you find a suitable frame check that it's free*/
        if (f->page == NULL) {
            f->page = page;
            lock_release(&scan_lock);
            return f;
        }

        /* Returns true if page P's data has been accessed recently,false otherwise*/
        if (page_accessed_recently(f->page)) {
            /*if it's accessed recently release its lock but don't swap it from the main memory*/
            lock_release(&f->lock);
            /*then continue to the loop to go to the next index*/
            continue;
        }
        /*release the scan_lock means that we finally find the required frame and we will free it
         * so allow another thread to enter this part of code to find a frame*/
        lock_release(&scan_lock);

        /* Evict this frame. */
        if (!page_out(f->page)) {
            /*! : means that he fails to evict this frame so release its lock and return null*/
            lock_release(&f->lock);
            return NULL;
        }

        /*if non of this condition happens that means we find the frame .
         * allocate the f-> page to the given page
         * and return it*/
        f->page = page;
        return f;
    }

    /*loop ends and we didn't find any frame to evict so release the scan_lock and return null*/
    lock_release(&scan_lock);
    return NULL;
}


/* Tries really hard to allocate and lock a frame for PAGE.
   Returns the frame if successful, false on failure. */
struct frame *
frame_alloc_and_lock(struct page *page) {
    size_t try;

    for (try = 0; try < 3; try++) {
        struct frame *f = try_frame_alloc_and_lock(page);
        if (f != NULL) {
            ASSERT(lock_held_by_current_thread(&f->lock));
            return f;
        }
        timer_msleep(1000);
    }

    return NULL;
}

/* Locks P's frame into memory, if it has one.
   Upon return, p->frame will not change until P is unlocked. */
void
frame_lock(struct page *p) {
    /* A frame can be asynchronously removed, but never inserted. */
    struct frame *f = p->frame;
    if (f != NULL) {
        /*means that the frame is allocated to a page*/
        /*lock that frame*/
        lock_acquire(&f->lock);
        if (f != p->frame) {
            /*if it isn't p frame release it*/
            lock_release(&f->lock);
            ASSERT(p->frame == NULL);
        }
    }
}

/* Releases frame F for use by another page.
   F must be locked for use by the current process.
   Any data in F is lost. */
void
frame_free(struct frame *f) {
    /*make sure that isn't a free frame*/
    ASSERT(lock_held_by_current_thread(&f->lock));
    /*make its page = null*/
    f->page = NULL;
    /*release the frame by opening its lock*/
    lock_release(&f->lock);
}

/* Unlocks frame F but not freeing it, allowing it to be evicted.
   F must be locked for use by the current process. */
void
frame_unlock(struct frame *f) {
    /*make sure that isn't a free frame*/
    ASSERT(lock_held_by_current_thread(&f->lock));
    /*release the frame by opening its lock*/
    lock_release(&f->lock);
}