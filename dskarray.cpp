/* dskarray.cpp - the page cache behind dskarray.h */
#include <string.h>
#include <fcntl.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#else
#include <io.h>
#endif
#include <sys/stat.h>
#include "dskarray.h"

#ifndef S_IREAD
#define S_IREAD 0400
#define S_IWRITE 0200
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define DA_DEFAULT_PAGES 32
#define DA_MAX_PAGES     256
#define DA_BUCKETS       128    /* a power of two */

static DaSlot s_slots[DA_MAX_PAGES];
static int s_buckets[DA_BUCKETS];
static int s_count = 0;
static unsigned s_next_id = 1;
static unsigned long s_tick = 0;
static unsigned long s_hits = 0, s_misses = 0, s_reads = 0, s_writes = 0;

static unsigned bucket_of(unsigned id, long page)
{
    return ((unsigned)page + id * 37U) & (DA_BUCKETS - 1);
}

static void hash_add(DaSlot *s, int index)
{
    unsigned b = bucket_of(s->owner->id(), s->page);

    s->next = s_buckets[b];
    s_buckets[b] = index;
}

static void hash_remove(DaSlot *s)
{
    int *p = &s_buckets[bucket_of(s->owner->id(), s->page)];

    while (*p >= 0) {
        DaSlot *c = &s_slots[*p];

        if (c == s) {
            *p = c->next;
            return;
        }
        p = &c->next;
    }
}

int da_cache_init(int pages)
{
    da_cache_free();
    if (pages > DA_MAX_PAGES)
        pages = DA_MAX_PAGES;
    for (int b = 0; b < DA_BUCKETS; b++)
        s_buckets[b] = -1;
    while (s_count < pages) {
        char *p = (char *)malloc(DA_PAGE_SIZE);

        if (!p)
            break;
        memset(&s_slots[s_count], 0, sizeof(DaSlot));
        s_slots[s_count].data = p;
        s_slots[s_count].page = -1;
        s_count++;
    }
    return s_count;
}

int da_cache_pages()
{
    return s_count;
}

void da_cache_free()
{
    while (s_count > 0) {
        s_count--;
        free(s_slots[s_count].data);
    }
}

DaStats da_stats()
{
    DaStats st;

    st.hits = s_hits;
    st.misses = s_misses;
    st.reads = s_reads;
    st.writes = s_writes;
    return st;
}

void da_stats_reset()
{
    s_hits = s_misses = s_reads = s_writes = 0;
}

void da_range_error(long i, long n)
{
    fprintf(stderr, "dskarray: index %ld out of range 0..%ld\n", i, n - 1);
    abort();
}

void da_write_slot(DaSlot *s)
{
    DaFile *f = s->owner;

    s->dirty = 0;
    s_writes++;
    if (lseek(f->fd_, s->page << DA_PAGE_SHIFT, SEEK_SET) < 0 ||
        write(f->fd_, s->data, DA_PAGE_SIZE) != DA_PAGE_SIZE)
        f->error_ = 1;
}

void da_read_slot(DaSlot *s, DaFile *f, long n)
{
    s_reads++;
    s->dirty = 0;
    if (lseek(f->fd_, n << DA_PAGE_SHIFT, SEEK_SET) < 0 ||
        read(f->fd_, s->data, DA_PAGE_SIZE) != DA_PAGE_SIZE) {
        f->error_ = 1;
        memset(s->data, 0, DA_PAGE_SIZE);
    }
}

DaFile::DaFile() : fd_(-1), error_(0), keep_(0), pages_(0), last_page_(-1), last_slot_(0),
    last_data_(0), id_(s_next_id++), name_(0)
{
    forget();
}

DaFile::~DaFile()
{
    close();
}

int DaFile::open(const char *name, long pages, const void *fill_page, int keep)
{
    if (s_count == 0 && da_cache_init(DA_DEFAULT_PAGES) == 0)
        return 0;

    pages_ = pages;
    keep_ = keep;
    name_ = (char *)malloc(strlen(name) + 1);
    if (!name_)
        return 0;
    strcpy(name_, name);

    if (keep) {
        fd_ = ::open(name, O_RDWR | O_BINARY);
        if (fd_ >= 0) {
            if (lseek(fd_, 0L, SEEK_END) == pages << DA_PAGE_SHIFT)
                return 1;
            ::close(fd_);
        }
    }

    fd_ = ::open(name, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IREAD | S_IWRITE);
    if (fd_ < 0)
        return 0;
    for (long i = 0; i < pages; i++) {
        if (write(fd_, fill_page, DA_PAGE_SIZE) != DA_PAGE_SIZE) {
            error_ = 1;
            return 0;
        }
    }
    return 1;
}

char *DaFile::fetch(long n, int dirty)
{
    DaSlot *s = 0;
    int i;

    for (i = s_buckets[bucket_of(id_, n)]; i >= 0; i = s_slots[i].next) {
        if (s_slots[i].owner == this && s_slots[i].page == n) {
            s = &s_slots[i];
            break;
        }
    }

    if (s) {
        ++s_hits;
    } else {
        int victim = -1;

        ++s_misses;
        for (i = 0; i < s_count; i++) {
            DaSlot *c = &s_slots[i];

            if (!c->owner) {
                victim = i;
                break;
            }
            /* the page each array is on has no fresh tick: keep it */
            if (c->owner->in_use(c) && s_count > 1 && c->owner != this)
                continue;
            if (victim < 0 || c->tick < s_slots[victim].tick)
                victim = i;
        }
        if (victim < 0)
            victim = 0;
        s = &s_slots[victim];
        if (s->owner) {
            if (s->dirty)
                da_write_slot(s);
            hash_remove(s);
            s->owner->forget();
        }
        da_read_slot(s, this, n);
        s->owner = this;
        s->page = n;
        hash_add(s, victim);
    }

    s->tick = ++s_tick;
    if (dirty)
        s->dirty = 1;
    last_page_ = n;
    last_slot_ = s;
    last_data_ = s->data;
    return last_data_;
}

void DaFile::flush()
{
    for (int i = 0; i < s_count; i++)
        if (s_slots[i].owner == this && s_slots[i].dirty)
            da_write_slot(&s_slots[i]);
}

void DaFile::close()
{
    if (fd_ >= 0) {
        for (int i = 0; i < s_count; i++) {
            DaSlot *s = &s_slots[i];

            if (s->owner == this) {
                if (s->dirty && keep_)
                    da_write_slot(s);
                hash_remove(s);
                s->owner = 0;
                s->page = -1;
                s->dirty = 0;
                s->tick = 0;
            }
        }
        ::close(fd_);
        fd_ = -1;
        if (!keep_ && name_)
            remove(name_);
    }
    forget();
    free(name_);
    name_ = 0;
}
