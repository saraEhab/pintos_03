#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"

/* Maximum size of process stack, in bytes. */
/* Right now it is 1 megabyte. */
#define STACK_MAX (1024 * 1024)

/* Destroys a page, which must be in the current process's
   page table.*/
static void
destroy_page(struct hash_elem *p_, void *aux UNUSED) {
    /* Converts pointer to hash element HASH_ELEM (p_) into a pointer to
     * the structure that HASH_ELEM (p_) is embedded inside.
     *Supply the name of the outer structure STRUCT (page)
     * and the member name MEMBER (hash_elem) of the hash element.*/
    struct page *p = hash_entry(p_,
    struct page, hash_elem);
    /*if page p has a frame lock it into the main memory*/
    frame_lock(p);
    if (p->frame)
        /*if p has a frame free it*/
        frame_free(p->frame);
    /*free the page which means make it equals to null*/
    free(p);
}

/* Destroys the current process's page table. */
void
page_exit(void) {
    /*take all the pages for the current thread , put it in a hash*/
    struct hash *h = thread_current()->pages;
    if (h != NULL)
        /*means the pages are loaded successfully in the hash table*/
        /*call destroy_page function each time you destroy a page in the hash table*/
        hash_destroy(h, destroy_page);
}

/* Returns the page containing the given virtual ADDRESS,
   or a null pointer if no such page exists.
   Allocates stack pages as necessary. */
static struct page *
page_for_addr(const void *address) {
    /*the address is smaller than the physical address base */
    if (address < PHYS_BASE) {
        struct page p;
        struct hash_elem *e;

        /* Find existing page. */
        p.addr = (void *) pg_round_down(address);   /* Round down to nearest page boundary. */
        e = hash_find(thread_current()->pages, &p.hash_elem);
        if (e != NULL)
            return hash_entry(e,
        struct page, hash_elem);

        /* -We need to determine if the program is attempting to access the stack.
           -First condition,makes sure that the address is not beyond the bounds of the stack space (1 MB in this
            case).
           -Second condition :As long as the user is attempting to access an address within 32 bytes (determined by the space
            needed for a PUSHA command) of the stack pointers, we assume that the address is valid.
            In that case, we should allocate one more stack page accordingly.*/
        if ((p.addr > PHYS_BASE - STACK_MAX) && ((void *) thread_current()->user_esp - 32 < address)) {
            return page_allocate(p.addr, false);/*add a map for the page in the page table,false means it isn't a read only page*/
        }
    }

    return NULL;
}

/* Locks a frame for page P.
   Returns true if successful, false on failure. */
static bool
do_page_in(struct page *p) {
    /* Get a frame for the page p */
    p->frame = frame_alloc_and_lock(p);
    if (p->frame == NULL)
        return false;

    /* Copy data into the frame. */
    if (p->sector != (block_sector_t) - 1) {
        /*this condition makes sure that the page has its all sectors*/
        /* swap in -> put the page in main memory */
        swap_in(p);
    } else if (p->file != NULL) {
        /* Get data from file to be written to the memory*/
        /* file_read_at () :
         * Reads SIZE bytes(p->file_bytes) from FILE (p->file)into BUFFER (p->frame->base),
         *  starting at offset FILE_OFS (p->file_offset) in the file.
         *  Returns the number of bytes actually read,
         *  which may be less than SIZE if end of file is reached.
         *  The file's current position is unaffected. */
        off_t read_bytes = file_read_at(p->file, p->frame->base,p->file_bytes, p->file_offset);
        /*the size of the zero bytes in the swapped page is :*/
        off_t zero_bytes = PGSIZE - read_bytes;
        memset(p->frame->base + read_bytes, 0, zero_bytes);/*fill the rest of the page with zeros*/
        if (read_bytes != p->file_bytes) /*error:the bytes that are read != the actual bytes that we have to transfer*/
            printf("bytes read (%"PROTd") != bytes requested (%"PROTd")\n",read_bytes, p->file_bytes);
    } else {
        /* Provide all-zero page. */
        memset(p->frame->base, 0, PGSIZE);
    }
    return true;
}

/*trying to add a page to the main memory
 * after making a page fault.
   Returns true if successful, false on failure. */
bool
page_in(void *fault_addr) {
    struct page *p;
    bool success;

    /* Can't handle page faults*/
    if (thread_current()->pages == NULL)
        return false;

    p = page_for_addr(fault_addr);/*the address for the page that mad the page fault*/
    if (p == NULL)
        return false;

    frame_lock(p);/*lock a frame for that page in the main memory to load it in*/
    if (p->frame == NULL) {
        /*we couldn't find a frame for the page*/
        if (!do_page_in(p))/*we couldn't lock a frame for the page*/
            return false;
    }
    /*check if the current thread has a lock on a frame for that page .
     * means that we find a free frame for the page*/
    ASSERT(lock_held_by_current_thread(&p->frame->lock));

    /* adding a map for a page directory into page table. */
    /* Adds a mapping in page directory (thread_current()->pagedir)
     * from user virtual page (p->addr)
     * to the physical frame identified by kernel virtual address (p->frame->base).
     *If WRITABLE is true, the new page is read/write;
     *otherwise it is read-only.
     *Returns true if successful, false if memory allocation failed. */
    success = pagedir_set_page(thread_current()->pagedir, p->addr,
                               p->frame->base, !p->read_only);

    /* Release frame. */
    frame_unlock(p->frame);

    return success;
}

/* remove page P from main memory.
   P must have a locked frame.
   Return true if successful, false on failure. */
bool
page_out(struct page *p) {
    bool dirty;
    bool ok = false;

    /*make sure that page p has a frame in main memory*/
    ASSERT(p->frame != NULL);
    /*make sure that the frame is locked*/
    ASSERT(lock_held_by_current_thread(&p->frame->lock));

    /* Mark page not present in page table,
     *forcing accesses by the process to fault.
     * This must happen before checking the dirty bit,
     * to prevent a race with the process dirtying the page. */
    pagedir_clear_page(p->thread->pagedir, (void *) p->addr);

    /* Has the frame been modified? */
    /* If the frame has been modified, set 'dirty' to true. */
    dirty = pagedir_is_dirty(p->thread->pagedir, (const void *) p->addr);

    /* If the frame is not dirty (and file != NULL), we have sucsessfully evicted the page. */
    if (!dirty) {
        ok = true;
    }

    /* If the file is null, we definitely don't want to write the frame to disk. We must swap out the
       frame and save whether or not the swap was successful. This could overwrite the previous value of
       'ok'. */
    if (p->file == NULL) {
        ok = swap_out(p);
    }
        /* Otherwise, a file exists for this page. If file contents have been modified, then they must be
           be written back to the file system on disk, or swapped out. This is determined by the write_back bool
           variable associated with the page. */
    else {
        if (dirty) {
            if (p->write_back) {
                ok = swap_out(p);
            } else {
                ok = file_write_at(p->file, (const void *) p->frame->base, p->file_bytes, p->file_offset);
            }
        }
    }

    /* Nullify the frame held by the page. */
    if (ok) {
        p->frame = NULL;
    }
    return ok;
}

/* Returns true if page P's data has been accessed recently,
   false otherwise.
   P must have a frame locked into memory. */
bool
page_accessed_recently(struct page *p) {
    bool was_accessed;

    /*make sure that the page has a frame in the main memory*/
    ASSERT(p->frame != NULL);
    /*and the frame is lock by the current thread*/
    ASSERT(lock_held_by_current_thread(&p->frame->lock));

    was_accessed = pagedir_is_accessed(p->thread->pagedir, p->addr);
    if (was_accessed)
        /*Sets the accessed bit to ACCESSED in the Page Table Entry for virtual page*/
        pagedir_set_accessed(p->thread->pagedir, p->addr, false);
    return was_accessed;
}

/* Adds a mapping for user virtual address VADDR to the page hash table.
 * Fails if VADDR is already mapped or if memory allocation fails. */
struct page *
page_allocate(void *vaddr, bool read_only) {
    struct thread *t = thread_current();
    /* Obtains and returns a new block of at least SIZE of (p) bytes.
   Returns a null pointer if memory is not available. */
    struct page *p = malloc(sizeof *p);
    if (p != NULL) {
        p->addr = pg_round_down(vaddr);/* Round down to nearest page boundary. */
        p->read_only = read_only;
        p->write_back = !read_only;
        p->frame = NULL;
        p->sector = (block_sector_t) - 1;
        p->file = NULL;
        p->file_offset = 0;
        p->file_bytes = 0;
        p->thread = thread_current();

        if (hash_insert(t->pages, &p->hash_elem) != NULL) {
            /* Already mapped. */
            free(p);/* Frees block P, which must have been previously allocated with malloc()*/
            p = NULL;
        }
    }
    return p;
}

/* remove the page containing address VADDR from the main memory
   and removes it from the page table. */
void
page_deallocate(void *vaddr) {
    struct page *p = page_for_addr(vaddr);
    ASSERT(p != NULL);/*make sure that it is a valid page*/
    frame_lock(p);/* Locks P's frame into memory, if it has one.*/
    if (p->frame) {
        /*if it has a  frame in the main memory*/
        struct frame *f = p->frame;
        if (p->file && !p->write_back)
            page_out(p);
        frame_free(f);
    }
    hash_delete(thread_current()->pages, &p->hash_elem);
    free(p);
}

/* Returns a hash value for the page that E refers to. */
unsigned
page_hash(const struct hash_elem *e, void *aux UNUSED) {
    const struct page *p = hash_entry(e,
    struct page, hash_elem);
    return ((uintptr_t) p->addr) >> PGBITS;
}

/* Returns true if page A precedes page B. */
bool
page_less(const struct hash_elem *a_, const struct hash_elem *b_,
          void *aux UNUSED) {
    const struct page *a = hash_entry(a_,
    struct page, hash_elem);
    const struct page *b = hash_entry(b_,
    struct page, hash_elem);

    return a->addr < b->addr;
}

/* Tries to lock the page containing ADDR into physical memory.
   If WILL_WRITE is true, the page must be writeable;
   otherwise it may be read-only.
   Returns true if successful, false on failure. */
bool
page_lock(const void *addr, bool will_write) {
    struct page *p = page_for_addr(addr);
    if (p == NULL || (p->read_only && will_write))
        return false;

    frame_lock(p);
    if (p->frame == NULL)
        /*if the page frame is null
         * means that the page doesn't have a lock frame in the memory
         * so try to add one for it and if succeed return true*/
        return (do_page_in(p)&& pagedir_set_page(thread_current()->pagedir, p->addr,p->frame->base, !p->read_only));
    else
        return true;
}

/* Unlocks a page locked with page_lock(). */
void
page_unlock(const void *addr) {
    struct page *p = page_for_addr(addr);
    ASSERT(p != NULL);
    frame_unlock(p->frame);
}