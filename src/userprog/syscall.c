#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"

static void syscall_handler (struct intr_frame *);
void* check_addr(const void*);
struct proc_file* list_search(struct list* files, int fd);
static struct lock fs_lock;
extern bool running;

struct proc_file {
    struct file* ptr;
    int fd;
    struct list_elem elem;
};

void
syscall_init (void)
{
    intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED)
{
    int * p = f->esp;

    check_addr(p);



    int system_call = * p;
    switch (system_call)
    {
        case SYS_HALT:
            shutdown_power_off();
            break;

        case SYS_EXIT:
            check_addr(p+1);
            exit_proc(*(p+1));
            break;

        case SYS_EXEC:
            check_addr(p+1);
            check_addr(*(p+1));
            f->eax = exec_proc(*(p+1));
            break;

        case SYS_WAIT:
            check_addr(p+1);
            f->eax = process_wait(*(p+1));
            break;

        case SYS_CREATE:
            check_addr(p+5);
            check_addr(*(p+4));
            acquire_filesys_lock();
            f->eax = filesys_create(*(p+4),*(p+5));
            release_filesys_lock();
            break;

        case SYS_REMOVE:
            check_addr(p+1);
            check_addr(*(p+1));
            acquire_filesys_lock();
            if(filesys_remove(*(p+1))==NULL)
                f->eax = false;
            else
                f->eax = true;
            release_filesys_lock();
            break;

        case SYS_OPEN:
            check_addr(p+1);
            check_addr(*(p+1));

            acquire_filesys_lock();
            struct file* fptr = filesys_open (*(p+1));
            release_filesys_lock();
            if(fptr==NULL)
                f->eax = -1;
            else
            {
                struct proc_file *pfile = malloc(sizeof(*pfile));
                pfile->ptr = fptr;
                pfile->fd = thread_current()->fd_count;
                thread_current()->fd_count++;
                list_push_back (&thread_current()->files, &pfile->elem);
                f->eax = pfile->fd;

            }
            break;

        case SYS_FILESIZE:
            check_addr(p+1);
            acquire_filesys_lock();
            f->eax = file_length (list_search(&thread_current()->files, *(p+1))->ptr);
            release_filesys_lock();
            break;

        case SYS_READ:
            check_addr(p+7);
            check_addr(*(p+6));
            if(*(p+5)==0)
            {
                int i;
                uint8_t* buffer = *(p+6);
                for(i=0;i<*(p+7);i++)
                    buffer[i] = input_getc();
                f->eax = *(p+7);
            }
            else
            {
                struct proc_file* fptr = list_search(&thread_current()->files, *(p+5));
                if(fptr==NULL)
                    f->eax=-1;
                else
                {
                    acquire_filesys_lock();
                    f->eax = file_read (fptr->ptr, *(p+6), *(p+7));
                    release_filesys_lock();
                }
            }
            break;

        case SYS_WRITE:
            check_addr(p+7);
            check_addr(*(p+6));
            if(*(p+5)==1)
            {
                putbuf(*(p+6),*(p+7));
                f->eax = *(p+7);
            }
            else
            {
                struct proc_file* fptr = list_search(&thread_current()->files, *(p+5));
                if(fptr==NULL)
                    f->eax=-1;
                else
                {
                    acquire_filesys_lock();
                    f->eax = file_write (fptr->ptr, *(p+6), *(p+7));
                    release_filesys_lock();
                }
            }
            break;

        case SYS_SEEK:
            check_addr(p+5);
            acquire_filesys_lock();
            file_seek(list_search(&thread_current()->files, *(p+4))->ptr,*(p+5));
            release_filesys_lock();
            break;

        case SYS_TELL:
            check_addr(p+1);
            acquire_filesys_lock();
            f->eax = file_tell(list_search(&thread_current()->files, *(p+1))->ptr);
            release_filesys_lock();
            break;

        case SYS_CLOSE:
            check_addr(p+1);
            acquire_filesys_lock();
            close_file(&thread_current()->files,*(p+1));
            release_filesys_lock();
            break;

        case SYS_MUNMAP:
            check_addr(p+1);
            acquire_filesys_lock();
            f->eax = sys_munmap(*(p+1));
            release_filesys_lock();
            break;

        case SYS_MMAP:
            check_addr(p+5);
            check_addr(*(p+4));
            acquire_filesys_lock();
            f->eax = sys_mmap(*(p+4),*(p+5));
            release_filesys_lock();
            break;

        default:
            printf("Default %d\n",*p);
    }
}

int exec_proc(char *file_name)
{
    acquire_filesys_lock();
    char * fn_cp = malloc (strlen(file_name)+1);
    strlcpy(fn_cp, file_name, strlen(file_name)+1);

    char * save_ptr;
    fn_cp = strtok_r(fn_cp," ",&save_ptr);

    struct file* f = filesys_open (fn_cp);

    if(f==NULL)
    {
        release_filesys_lock();
        return -1;
    }
    else
    {
        file_close(f);
        release_filesys_lock();
        return process_execute(file_name);
    }
}

void exit_proc(int status)
{
    //printf("Exit : %s %d %d\n",thread_current()->name, thread_current()->tid, status);
    struct list_elem *e;

    for (e = list_begin (&thread_current()->parent->child_proc); e != list_end (&thread_current()->parent->child_proc);
         e = list_next (e))
    {
        struct child *f = list_entry (e, struct child, elem);
        if(f->tid == thread_current()->tid)
        {
            f->used = true;
            f->exit_error = status;
        }
    }


    thread_current()->exit_error = status;

    if(thread_current()->parent->waitingon == thread_current()->tid)
        sema_up(&thread_current()->parent->child_lock);

    thread_exit();
}

void* check_addr(const void *vaddr)
{
    if (!is_user_vaddr(vaddr))
    {
        exit_proc(-1);
        return 0;
    }
    void *ptr = pagedir_get_page(thread_current()->pagedir, vaddr);
    if (!ptr)
    {
        exit_proc(-1);
        return 0;
    }
    return ptr;
}

struct proc_file* list_search(struct list* files, int fd)
{

    struct list_elem *e;

    for (e = list_begin (files); e != list_end (files);
         e = list_next (e))
    {
        struct proc_file *f = list_entry (e, struct proc_file, elem);
        if(f->fd == fd)
            return f;
    }
    return NULL;
}

void close_file(struct list* files, int fd)
{

    struct list_elem *e;

    struct proc_file *f;

    for (e = list_begin (files); e != list_end (files);
         e = list_next (e))
    {
        f = list_entry (e, struct proc_file, elem);
        if(f->fd == fd)
        {
            file_close(f->ptr);
            list_remove(e);
        }
    }

    free(f);
}

void close_all_files(struct list* files)
{

    struct list_elem *e;

    while(!list_empty(files))
    {
        e = list_pop_front(files);

        struct proc_file *f = list_entry (e, struct proc_file, elem);

        file_close(f->ptr);
        list_remove(e);
        free(f);


    }


}


/* Binds a mapping id to a region of memory and a file. */
struct mapping
{
    struct list_elem elem;      /* List element. */
    int handle;                 /* Mapping id. */
    struct file *file;          /* File. */
    uint8_t *base;              /* Start of memory mapping. */
    size_t page_cnt;            /* Number of pages mapped. */
};

/* Returns the file descriptor associated with the given handle.
   Terminates the process if HANDLE is not associated with a
   memory mapping. */
static struct mapping *
lookup_mapping (int handle)
{
    struct thread *cur = thread_current ();
    struct list_elem *e;

    for (e = list_begin (&cur->mappings); e != list_end (&cur->mappings);
         e = list_next (e))
    {
        struct mapping *m = list_entry (e, struct mapping, elem);
        if (m->handle == handle)
            return m;
    }

    thread_exit ();
}

/* Remove mapping M from the virtual address space,
   writing back any pages that have changed. */
static void
unmap (struct mapping *m)
{
    /* Remove this mapping from the list of mappings for this process. */
    list_remove(&m->elem);

    /* For each page in the memory mapped file... */
    for(int i = 0; i < m->page_cnt; i++)
    {
        /* ...determine whether or not the page is dirty (modified). If so, write that page back out to disk. */
        if (pagedir_is_dirty(thread_current()->pagedir, ((const void *) ((m->base) + (PGSIZE * i)))))
        {
            lock_acquire (&fs_lock);
            file_write_at(m->file, (const void *) (m->base + (PGSIZE * i)), (PGSIZE*(m->page_cnt)), (PGSIZE * i));
            lock_release (&fs_lock);
        }
    }

    /* Finally, deallocate all memory mapped pages (free up the process memory). */
    for(int i = 0; i < m->page_cnt; i++)
    {
        page_deallocate((void *) ((m->base) + (PGSIZE * i)));
    }
}

/* Mmap system call. */
static int
sys_mmap (int handle, void *addr)
{
    struct proc_file *fd = lookup_fd (handle);
    struct mapping *m = malloc (sizeof *m);
    size_t offset;
    off_t length;

    if (m == NULL || addr == NULL || pg_ofs (addr) != 0)
        return -1;

    m->handle = thread_current ()->next_handle++;
    lock_acquire (&fs_lock);
    m->file = file_reopen (fd->ptr);
    lock_release (&fs_lock);
    if (m->file == NULL)
    {
        free (m);
        return -1;
    }
    m->base = addr;
    m->page_cnt = 0;
    list_push_front (&thread_current ()->mappings, &m->elem);

    offset = 0;
    lock_acquire (&fs_lock);
    length = file_length (m->file);
    lock_release (&fs_lock);
    while (length > 0)
    {
        struct page *p = page_allocate ((uint8_t *) addr + offset, false);
        if (p == NULL)
        {
            unmap (m);
            return -1;
        }
        p->write_back = false;
        p->file = m->file;
        p->file_offset = offset;
        p->file_bytes = length >= PGSIZE ? PGSIZE : length;
        offset += p->file_bytes;
        length -= p->file_bytes;
        m->page_cnt++;
    }

    return m->handle;
}

/* Munmap system call. */
static int
sys_munmap (int mapping)
{
    /* Get the map corresponding to the given map id, and attempt to unmap. */
    struct mapping *map = lookup_mapping(mapping);
    unmap(map);
    return 0;
}

/* On thread exit, close all open files and unmap all mappings. */
void
syscall_exit (void)
{
    struct thread *cur = thread_current ();
    struct list_elem *e, *next;

    for (e = list_begin (&cur->fds); e != list_end (&cur->fds); e = next)
    {
        struct proc_file *fd = list_entry (e, struct proc_file, elem);
        next = list_next (e);
        lock_acquire (&fs_lock);
        file_close (fd->ptr);
        lock_release (&fs_lock);
        free (fd);
    }

    for (e = list_begin (&cur->mappings); e != list_end (&cur->mappings);
         e = next)
    {
        struct mapping *m = list_entry (e, struct mapping, elem);
        next = list_next (e);
        unmap (m);
    }
}
