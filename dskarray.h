/* dskarray.h - arrays stored on disk, used with normal array syntax.
 *
 *   DiskArray<double>  v("v.tmp", 100000L);       v[i] = 1.5;
 *   DiskArray2<float>  a("a.tmp", 1000, 1000);    a[x][y] = a[x-1][y] + 1;
 *
 * Elements live in a file. A small pool of 4 KB pages, shared by all arrays,
 * holds the ones in use (LRU, written back when evicted), and 2D arrays are
 * laid out in square tiles, so neighbors in either direction share a page.
 *
 * Plain C++98 for Open Watcom 16-bit (large model), and other compilers.
 * Link dskarray.cpp. Call da_cache_init() first to choose the pool size.
 *
 * An element is a proxy, not a T: varargs need a cast
 * (printf("%f", (double)(float)a[1][2])), and a[i].member doesn't work: copy
 * a struct out, change it, and assign it back. A T* can't be taken.
 * Define DA_CHECK to abort on an out-of-range index.
 */
#ifndef DSKARRAY_H
#define DSKARRAY_H

#include <stdio.h>
#include <stdlib.h>

#define DA_PAGE_SHIFT 12
#define DA_PAGE_SIZE  4096

#define DA_NO_WINDOW 0x7FFFFFFFL

/* log2 of how many elements of size s fit in a page, as a constant */
#define DA_LOG2(s) ((s) <= 1 ? 12 : (s) <= 2 ? 11 : (s) <= 4 ? 10 : (s) <= 8 ? 9 : \
                    (s) <= 16 ? 8 : (s) <= 32 ? 7 : (s) <= 64 ? 6 : (s) <= 128 ? 5 : \
                    (s) <= 256 ? 4 : (s) <= 512 ? 3 : (s) <= 1024 ? 2 : (s) <= 2048 ? 1 : 0)

class DaFile;

struct DaSlot {
    char *data;
    DaFile *owner;
    long page;
    unsigned long tick;
    int next;                   /* next slot in the hash chain, or -1 */
    char dirty;
};

struct DaStats {
    unsigned long hits, misses, reads, writes;
};

/* Sets the pool to at most pages pages (fewer if memory runs out) and returns
 * how many it got. Can't be changed while arrays are open. */
int da_cache_init(int pages);
int da_cache_pages();
void da_cache_free();
DaStats da_stats();
void da_stats_reset();

class DaFile {
public:
    DaFile();
    ~DaFile();
    /* A kept file with the right size is reopened with its contents; otherwise
     * the file is created with every page a copy of fill_page. A file that
     * isn't kept is deleted on close. */
    int open(const char *name, long pages, const void *fill_page, int keep);
    void close();
    void flush();
    int ok() const { return fd_ >= 0 && !error_; }

    /* The page's memory. Valid until the next page() call. */
    char *page(long n, int dirty)
    {
        if (n == last_page_) {
            if (dirty)
                last_slot_->dirty = 1;
            return last_data_;
        }
        return fetch(n, dirty);
    }

    void forget()
    {
        last_page_ = -1;
        win_lo = DA_NO_WINDOW;
        win_tx = win_ty = (unsigned)-1;
    }

    /* The elements of the current page, so that the arrays can reach it with
     * one comparison. forget() clears them when the page goes. */
    char *data() const { return last_data_; }
    void mark_dirty() { last_slot_->dirty = 1; }
    long win_lo;                /* DiskArray: first element */
    unsigned win_tx, win_ty;    /* DiskArray2: tile */
    int in_use(const DaSlot *s) const { return s == last_slot_ && last_page_ >= 0; }
    unsigned id() const { return id_; }

private:
    DaFile(const DaFile &);
    DaFile &operator=(const DaFile &);
    char *fetch(long n, int dirty);
    friend void da_write_slot(DaSlot *s);
    friend void da_read_slot(DaSlot *s, DaFile *f, long n);

    int fd_;
    int error_;
    int keep_;
    long pages_;
    long last_page_;
    DaSlot *last_slot_;
    char *last_data_;
    unsigned id_;
    char *name_;
};

void da_range_error(long i, long n);

#ifdef DA_CHECK
#define DA_RANGE(i, n) ((unsigned long)(i) < (unsigned long)(n) ? (void)0 : da_range_error((long)(i), (long)(n)))
#else
#define DA_RANGE(i, n) ((void)0)
#endif

/* Reads and writes of one element, resolved when used: looking at another
 * element in between may evict a page. A holds get() and put(). */
template <class T, class A, class I>
class DaRef {
public:
    DaRef(A *a, I x, I y) : a_(a), x_(x), y_(y) {}
    operator T() const { return a_->get(x_, y_); }
    DaRef &operator=(const T &v) { a_->put(x_, y_, v); return *this; }
    DaRef &operator=(const DaRef &r) { a_->put(x_, y_, (T)r); return *this; }
    DaRef &operator+=(const T &v) { T t = *this; t += v; a_->put(x_, y_, t); return *this; }
    DaRef &operator-=(const T &v) { T t = *this; t -= v; a_->put(x_, y_, t); return *this; }
    DaRef &operator*=(const T &v) { T t = *this; t *= v; a_->put(x_, y_, t); return *this; }
    DaRef &operator/=(const T &v) { T t = *this; t /= v; a_->put(x_, y_, t); return *this; }
    DaRef &operator++() { T t = *this; ++t; a_->put(x_, y_, t); return *this; }
    DaRef &operator--() { T t = *this; --t; a_->put(x_, y_, t); return *this; }

private:
    A *a_;
    I x_, y_;
};

template <class T>
class DiskArray {
public:
    typedef DaRef<T, DiskArray<T>, long> Ref;
    enum { SHIFT = DA_LOG2(sizeof(T)) };

    DiskArray(const char *name, long n, const T &fill = T(), int keep = 0) : n_(n)
    {
        open_file(name, fill, keep);
    }

    int operator!() const { return !f_.ok(); }
    long size() const { return n_; }
    void flush() { f_.flush(); }
    Ref operator[](long i) { return Ref(this, i, 0); }

    T get(long i, long)
    {
        unsigned long d = (unsigned long)(i - f_.win_lo);

        DA_RANGE(i, n_);
        if (d < (1UL << SHIFT))
            return ((T *)f_.data())[(unsigned)d];
        return *locate(i, 0);
    }

    void put(long i, long, const T &v)
    {
        unsigned long d = (unsigned long)(i - f_.win_lo);

        DA_RANGE(i, n_);
        if (d < (1UL << SHIFT)) {
            f_.mark_dirty();
            ((T *)f_.data())[(unsigned)d] = v;
        } else {
            *locate(i, 1) = v;
        }
    }

private:
    DiskArray(const DiskArray &);
    DiskArray &operator=(const DiskArray &);

    T *locate(long i, int dirty)
    {
        long n = i >> SHIFT;
        T *t = (T *)f_.page(n, dirty);

        f_.win_lo = n << SHIFT;
        return t + (unsigned)(i - f_.win_lo);
    }

    void open_file(const char *name, const T &fill, int keep)
    {
        char *page = (char *)malloc(DA_PAGE_SIZE);
        unsigned count = 1U << SHIFT;

        if (!page)
            return;
        for (unsigned i = 0; i < count; i++)
            ((T *)page)[i] = fill;
        f_.open(name, (n_ + (1L << SHIFT) - 1) >> SHIFT, page, keep);
        free(page);
    }

    DaFile f_;
    long n_;
};

template <class T>
class DiskArray2 {
public:
    typedef DaRef<T, DiskArray2<T>, unsigned> Ref;

    /* tiles as square as a power of two allows */
    enum {
        YBITS = DA_LOG2(sizeof(T)) / 2,
        XBITS = DA_LOG2(sizeof(T)) - YBITS,
        XMASK = (1 << XBITS) - 1,
        YMASK = (1 << YBITS) - 1
    };

    class Row {
    public:
        Row(DiskArray2<T> *a, unsigned x) : a_(a), x_(x) {}
        Ref operator[](unsigned y) { return Ref(a_, x_, y); }
    private:
        DiskArray2<T> *a_;
        unsigned x_;
    };

    DiskArray2(const char *name, unsigned xsize, unsigned ysize, const T &fill = T(), int keep = 0)
        : xsize_(xsize), ysize_(ysize)
    {
        xtiles_ = (long)(((unsigned long)xsize + XMASK) >> XBITS);
        ytiles_ = (long)(((unsigned long)ysize + YMASK) >> YBITS);
        open_file(name, fill, keep);
    }

    int operator!() const { return !f_.ok(); }
    unsigned xsize() const { return xsize_; }
    unsigned ysize() const { return ysize_; }
    void flush() { f_.flush(); }
    Row operator[](unsigned x) { return Row(this, x); }

    T get(unsigned x, unsigned y)
    {
        DA_RANGE(x, xsize_);
        DA_RANGE(y, ysize_);
        if ((x >> XBITS) != f_.win_tx || (y >> YBITS) != f_.win_ty)
            locate(x, y, 0);
        return ((T *)f_.data())[index_of(x, y)];
    }

    void put(unsigned x, unsigned y, const T &v)
    {
        DA_RANGE(x, xsize_);
        DA_RANGE(y, ysize_);
        if ((x >> XBITS) != f_.win_tx || (y >> YBITS) != f_.win_ty)
            locate(x, y, 1);
        else
            f_.mark_dirty();
        ((T *)f_.data())[index_of(x, y)] = v;
    }

private:
    DiskArray2(const DiskArray2 &);
    DiskArray2 &operator=(const DiskArray2 &);

    unsigned index_of(unsigned x, unsigned y) const
    {
        return ((y & YMASK) << XBITS) | (x & XMASK);
    }

    void locate(unsigned x, unsigned y, int dirty)
    {
        unsigned tx = x >> XBITS, ty = y >> YBITS;

        f_.page((long)ty * xtiles_ + tx, dirty);
        f_.win_tx = tx;
        f_.win_ty = ty;
    }

    void open_file(const char *name, const T &fill, int keep)
    {
        char *page = (char *)malloc(DA_PAGE_SIZE);
        unsigned count = 1U << (XBITS + YBITS);

        if (!page)
            return;
        for (unsigned i = 0; i < count; i++)
            ((T *)page)[i] = fill;
        f_.open(name, xtiles_ * ytiles_, page, keep);
        free(page);
    }

    DaFile f_;
    unsigned xsize_, ysize_;
    long xtiles_, ytiles_;
};

#endif
