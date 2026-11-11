/* engine.cpp - XYZ points to a qwtplot3d .mes grid by inverse distance.
 *
 * The points never sit in memory: they go into DiskArrays (dskarray.h),
 * first in file order, then sorted into square buckets so that a grid node
 * only reads the buckets around it, and those are on nearby pages of the
 * array file.  Grid rows are written as they are computed, so the grid isn't
 * kept either.  Memory use is the DiskArray page cache plus a few buffers,
 * whatever the number of points.
 *
 * Coordinates are kept as floats relative to the first point, so state plane
 * or UTM values (7 digits before the point) don't lose their decimals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef __WATCOMC__
#include <malloc.h>
#endif
#include "dskarray.h"
#include "engine.h"

#define PER_CELL   8            /* points per bucket, on average            */
#define MAX_NEAR   32
#define MAX_SIDE   32000L       /* grid nodes along a side                  */
#define RAW_FILE   "XYZRAW.TMP"
#define BND_FILE   "XYZBND.TMP"
#define MAX_BANDS  200
#define CEL_FILE   "XYZCEL.TMP"

struct Pt {
    float x, y, z;
};

static DiskArray<Pt>   *g_pts;      /* sorted by bucket (file XYZRAW.TMP)   */
static DiskArray<long> *g_start;    /* bucket c is g_pts[start[c]..start[c+1]) */
static XyzInfo g_info;
static double  g_x0, g_y0;          /* the first point: the origin          */
static float   g_fx, g_fy;          /* the data's lower left, relative      */
static float   g_cell;              /* bucket size                          */
static float   g_inv;
static int     g_cache_ready;

static double seconds_since(clock_t t0)
{
    return (double)(clock() - t0) / CLOCKS_PER_SEC;
}

/* The page cache takes what memory there is, less a reserve that the GUI
 * needs for the windows it saves the screen under. */
static void cache_setup()
{
    if (g_cache_ready)
        return;
#ifdef __WATCOMC__
    void __huge *reserve = halloc(160L * 1024L, 1);
#else
    void *reserve = malloc(160L * 1024L);
#endif
#if !defined(__WATCOMC__)
    /* for testing: XYZ_PAGES=8 runs with a cache as small as DOS might get */
    if (getenv("XYZ_PAGES")) {
        da_cache_init(atoi(getenv("XYZ_PAGES")));
        g_cache_ready = 1;
        return;
    }
#endif
    da_cache_init(96);
#ifdef __WATCOMC__
    if (reserve)
        hfree(reserve);
#else
    free(reserve);
#endif
    g_cache_ready = 1;
}

/* ---------------------------------------------------------------- reading */

/* one number from s; returns the character after it, or NULL if none */
static const char *number(const char *s, double *v)
{
    static const double tens[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
                                   1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16 };
    /* The first 9 digits are collected as an integer, so a typical number
     * costs one floating-point conversion and one divide instead of a
     * multiply and add per digit (slow on a machine without an 8087).
     * Only digits beyond 9 go through floating point.  Either way the digits
     * make the same exact integer, so the result doesn't change. */
    unsigned long im = 0;
    double m = 0;
    int neg = 0, frac = 0, digits = 0, wide = 0;

    while (*s == ' ' || *s == '\t' || *s == ',' || *s == ';')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
#define DIGIT(d)                                    \
    do {                                            \
        if (wide)                                   \
            m = m * 10 + (d);                       \
        else if (digits < 9)                        \
            im = im * 10 + (unsigned)(d);           \
        else {                                      \
            m = (double)im * 10 + (d);              \
            wide = 1;                               \
        }                                           \
        digits++;                                   \
    } while (0)

    while (*s >= '0' && *s <= '9')
        DIGIT(*s++ - '0');
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            DIGIT(*s++ - '0');
            frac++;
        }
    }
#undef DIGIT
    if (digits == 0)
        return NULL;
    if (!wide)
        m = (double)im;
    if (*s == 'e' || *s == 'E') {
        int e = 0, eneg = 0;

        s++;
        if (*s == '-') {
            eneg = 1;
            s++;
        } else if (*s == '+') {
            s++;
        }
        while (*s >= '0' && *s <= '9')
            e = e * 10 + (*s++ - '0');
        frac += eneg ? e : -e;
    }
    if (frac > 0)
        m = frac <= 16 ? m / tens[frac] : m / pow(10.0, frac);
    else if (frac < 0)
        m = -frac <= 16 ? m * tens[-frac] : m * pow(10.0, -frac);
    *v = neg ? -m : m;
    return s;
}

/* a line's x, y and z (anything after them is ignored) */
static int parse_line(const char *s, double *x, double *y, double *z)
{
    if ((s = number(s, x)) == NULL)
        return 0;
    if ((s = number(s, y)) == NULL)
        return 0;
    if ((s = number(s, z)) == NULL)
        return 0;
    return *s == 0 || *s == ' ' || *s == '\t' || *s == ',' || *s == ';' || *s == '\r' || *s == '\n';
}

static long count_lines(FILE *f, long size, XyzProgress progress, int *cancel)
{
    static char buf[4096];
    long lines = 0, done = 0;
    size_t n;
    int last = '\n';

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++)
            if (buf[i] == '\n')
                lines++;
        last = buf[n - 1];
        done += (long)n;
        if (progress && (done & 0xFFFFL) < 4096 && progress(XYZ_READING, done / 8, size)) {
            *cancel = 1;
            return 0;
        }
    }
    if (last != '\n')
        lines++;
    return lines;
}

static unsigned bucket_x(float x)
{
    long c = (long)((x - g_fx) * g_inv);

    return c < 0 ? 0 : (c >= (long)g_info.ncx ? g_info.ncx - 1 : (unsigned)c);
}

static unsigned bucket_y(float y)
{
    long c = (long)((y - g_fy) * g_inv);

    return c < 0 ? 0 : (c >= (long)g_info.ncy ? g_info.ncy - 1 : (unsigned)c);
}

static double round_spacing(double s)
{
    static const double steps[] = { 1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10 };
    double p = pow(10.0, floor(log10(s)));
    int i;

    for (i = 0; i < 10; i++)
        if (steps[i] * p >= s * 0.95)
            return steps[i] * p;
    return 10 * p;
}

int xyz_load(const char *path, XyzInfo *info, XyzProgress progress)
{
    FILE *f;
    char *vbuf;
    char line[256];
    long size, capacity, i, done = 0;
    int cancel = 0, first = 1;
    clock_t t0 = clock();
    DiskArray<Pt> *raw;

    xyz_unload();
    memset(&g_info, 0, sizeof(g_info));
    cache_setup();
    g_info.cache_pages = da_cache_pages();

    if ((f = fopen(path, "rb")) == NULL)
        return XYZ_EOPEN;
    vbuf = (char *)malloc(8192);
    if (vbuf)
        setvbuf(f, vbuf, _IOFBF, 8192);
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    fseek(f, 0L, SEEK_SET);

    /* how many points there can be, so that the array can be made */
    capacity = count_lines(f, size, progress, &cancel);
    if (cancel || capacity == 0) {
        fclose(f);
        free(vbuf);
        return cancel ? XYZ_CANCEL : XYZ_ENOPTS;
    }
    fseek(f, 0L, SEEK_SET);

    raw = new DiskArray<Pt>(RAW_FILE, capacity);
    if (!raw || !*raw) {
        delete raw;
        fclose(f);
        free(vbuf);
        return XYZ_EDISK;
    }

    while (fgets(line, sizeof(line), f)) {
        double x, y, z;

        done += (long)strlen(line);
        if (!parse_line(line, &x, &y, &z)) {
            g_info.skipped++;
            continue;
        }
        if (g_info.count >= capacity)
            break;
        if (first) {
            g_x0 = x;
            g_y0 = y;
            g_info.xmin = g_info.xmax = x;
            g_info.ymin = g_info.ymax = y;
            g_info.zmin = g_info.zmax = z;
            first = 0;
        }
        if (x < g_info.xmin) g_info.xmin = x;
        if (x > g_info.xmax) g_info.xmax = x;
        if (y < g_info.ymin) g_info.ymin = y;
        if (y > g_info.ymax) g_info.ymax = y;
        if (z < g_info.zmin) g_info.zmin = z;
        if (z > g_info.zmax) g_info.zmax = z;
        {
            Pt p;

            p.x = (float)(x - g_x0);
            p.y = (float)(y - g_y0);
            p.z = (float)z;
            (*raw)[g_info.count] = p;
        }
        g_info.count++;
        if ((g_info.count & 511) == 0 && progress &&
            progress(XYZ_READING, size / 8 + done / 8 * 7, size)) {
            cancel = 1;
            break;
        }
    }
    fclose(f);
    free(vbuf);
    if (cancel || g_info.count == 0) {
        delete raw;
        return cancel ? XYZ_CANCEL : XYZ_ENOPTS;
    }

    /* the buckets: square, about PER_CELL points each */
    {
        double w = g_info.xmax - g_info.xmin, h = g_info.ymax - g_info.ymin;
        double area = w * h, cell;

        if (area <= 0) {
            double side = w > h ? w : h;

            area = side > 0 ? side * side : 1.0;
        }
        g_info.spacing = sqrt(area / g_info.count);
        g_info.suggested = round_spacing(g_info.spacing);
        cell = sqrt(area * PER_CELL / g_info.count);
        if (w / cell > 16000.0 || h / cell > 16000.0)
            cell = (w > h ? w : h) / 16000.0;
        g_info.ncx = (unsigned)(w / cell) + 1;
        g_info.ncy = (unsigned)(h / cell) + 1;
        g_cell = (float)cell;
        g_inv = (float)(1.0 / cell);
        g_fx = (float)(g_info.xmin - g_x0);
        g_fy = (float)(g_info.ymin - g_y0);
    }

    /* Sorting into buckets, in two passes so that the writes stay in the
     * cache: first into bands of bucket rows (one write position per band),
     * then within each band, which is small enough to be cached whole.
     * Writing straight to bucket order instead sends nearly every point to
     * a page that isn't in the cache. */
    {
        long cells = (long)g_info.ncx * g_info.ncy, c, total = 0, band_points;
        long band_pos[MAX_BANDS + 1];
        unsigned char *band_of_row;
        unsigned row;
        int bands = 0, k;
        DiskArray<Pt> *banded;

        k = da_cache_pages() / 2;
        if (k < 1) k = 1;
        if (k > MAX_BANDS) k = MAX_BANDS;
        band_points = g_info.count / k + 1;

        band_of_row = (unsigned char *)malloc(g_info.ncy);
        g_start = new DiskArray<long>(CEL_FILE, cells + 1, 0L);
        banded = new DiskArray<Pt>(BND_FILE, g_info.count);
        if (!band_of_row || !g_start || !*g_start || !banded || !*banded) {
            free(band_of_row);
            delete banded;
            delete raw;
            xyz_unload();
            return XYZ_EDISK;
        }

        /* how many in each bucket (in start[c + 1]) */
        for (i = 0; i < g_info.count; i++) {
            Pt p = (*raw)[i];

            (*g_start)[(long)bucket_y(p.y) * g_info.ncx + bucket_x(p.x) + 1] += 1L;
            if ((i & 511) == 0 && progress && progress(XYZ_COUNTING, i, g_info.count)) {
                free(band_of_row);
                delete banded;
                delete raw;
                xyz_unload();
                return XYZ_CANCEL;
            }
        }
        /* where each bucket starts: start[c + 1] (it then counts up while
         * filling, and ends as the end of bucket c), and the bands, which
         * start on a row of buckets */
        for (row = 0; row < g_info.ncy; row++) {
            long first = (long)row * g_info.ncx + 1;

            if (bands == 0 || (total - band_pos[bands - 1] >= band_points && bands < MAX_BANDS))
                band_pos[bands++] = total;
            band_of_row[row] = (unsigned char)(bands - 1);
            for (c = first; c < first + (long)g_info.ncx; c++) {
                long n = (*g_start)[c];

                (*g_start)[c] = total;
                total += n;
            }
        }

        /* pass 1: file order -> band order */
        for (i = 0; i < g_info.count; i++) {
            Pt p = (*raw)[i];

            (*banded)[band_pos[band_of_row[bucket_y(p.y)]]++] = p;
            if ((i & 511) == 0 && progress && progress(XYZ_SORTING, i, 2 * g_info.count)) {
                free(band_of_row);
                delete banded;
                delete raw;
                xyz_unload();
                return XYZ_CANCEL;
            }
        }
        free(band_of_row);

        /* pass 2: band order -> bucket order, back into the first array */
        for (i = 0; i < g_info.count; i++) {
            Pt p = (*banded)[i];
            long at = (long)bucket_y(p.y) * g_info.ncx + bucket_x(p.x) + 1;
            long to = (*g_start)[at];

            (*raw)[to] = p;
            (*g_start)[at] = to + 1;
            if ((i & 511) == 0 && progress && progress(XYZ_SORTING, g_info.count + i, 2 * g_info.count)) {
                delete banded;
                delete raw;
                xyz_unload();
                return XYZ_CANCEL;
            }
        }
        delete banded;
    }
    g_pts = raw;
    g_pts->flush();
    g_start->flush();

    g_info.seconds = seconds_since(t0);
    *info = g_info;
    return XYZ_OK;
}

/* --------------------------------------------------------------- gridding */

struct Near {
    float d[MAX_NEAR];
    float z[MAX_NEAR];
    int n, max;

    void add(float dist, float val)
    {
        int i;

        if (n == max) {
            if (dist >= d[max - 1])
                return;
            i = max - 1;
        } else {
            i = n++;
        }
        while (i > 0 && d[i - 1] > dist) {
            d[i] = d[i - 1];
            z[i] = z[i - 1];
            i--;
        }
        d[i] = dist;
        z[i] = val;
    }

    double value(double power) const
    {
        double s1 = 0, s2 = 0, w;
        int i;

        if (n == 0)
            return 0;
        if (d[0] < 1e-12f)
            return z[0];
        for (i = 0; i < n; i++) {
            if (power == 2.0)
                w = 1.0 / d[i];
            else if (power == 1.0)
                w = 1.0 / sqrt((double)d[i]);
            else
                w = pow((double)d[i], -0.5 * power);
            s1 += z[i] * w;
            s2 += w;
        }
        return s1 / s2;
    }
};

static double grid_origin(double lo, double s)
{
    return floor(lo / s) * s;
}

void xyz_grid_size(const XyzParams *p, long *nx, long *ny)
{
    double gx = grid_origin(g_info.xmin, p->spacing);
    double gy = grid_origin(g_info.ymin, p->spacing);
    double fx = ceil((g_info.xmax - gx) / p->spacing - 1e-9);
    double fy = ceil((g_info.ymax - gy) / p->spacing - 1e-9);

    *nx = fx + 1 > 2e9 ? 2000000000L : (long)fx + 1;
    *ny = fy + 1 > 2e9 ? 2000000000L : (long)fy + 1;
}

/* z with 3 decimals, quicker than printf */
static char *put_z(char *o, double z)
{
    char tmp[16];
    unsigned long v;
    int n = 0;

    if (z > 2.0e6 || z < -2.0e6)
        return o + sprintf(o, "%.3f", z);
    if (z < 0) {
        *o++ = '-';
        z = -z;
    }
    v = (unsigned long)(z * 1000.0 + 0.5);
    tmp[n++] = (char)('0' + v % 10); v /= 10;
    tmp[n++] = (char)('0' + v % 10); v /= 10;
    tmp[n++] = (char)('0' + v % 10); v /= 10;
    tmp[n++] = '.';
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *o++ = tmp[--n];
    return o;
}

int xyz_grid(const char *mes_path, const XyzParams *p, XyzResult *r, XyzProgress progress)
{
    FILE *f;
    char *vbuf;
    char text[64];
    long nx, ny, i, j;
    double gx, gy;
    int nearest = p->nearest < 1 ? 1 : (p->nearest > MAX_NEAR ? MAX_NEAR : p->nearest);
    int maxring = (int)(g_info.ncx > g_info.ncy ? g_info.ncx : g_info.ncy);
    clock_t t0 = clock();
    Near best;

    memset(r, 0, sizeof(*r));
    if (!g_pts)
        return XYZ_ENOPTS;
    DiskArray<Pt> &pts = *g_pts;
    DiskArray<long> &start = *g_start;
    xyz_grid_size(p, &nx, &ny);
    if (nx > MAX_SIDE || ny > MAX_SIDE)
        return XYZ_ETOOBIG;
    gx = grid_origin(g_info.xmin, p->spacing);
    gy = grid_origin(g_info.ymin, p->spacing);

    if ((f = fopen(mes_path, "wb")) == NULL)
        return XYZ_ECREATE;
    vbuf = (char *)malloc(16384);
    if (vbuf)
        setvbuf(f, vbuf, _IOFBF, 16384);

    fprintf(f, "jk:11051895-17021986\nMESH\n%ld %ld\n", nx, ny);
    fprintf(f, "%.6f %.6f\n%.6f %.6f\n", gx, gx + (nx - 1) * p->spacing, gy, gy + (ny - 1) * p->spacing);

    r->zmin = 1e30;
    r->zmax = -1e30;
    best.max = nearest;
    for (j = 0; j < ny; j++) {
        float ry = (float)(gy + j * p->spacing - g_y0);
        int cy = (int)bucket_y(ry);

        for (i = 0; i < nx; i++) {
            float rx = (float)(gx + i * p->spacing - g_x0);
            int cx = (int)bucket_x(rx);
            int ring;
            double z;

            best.n = 0;
            for (ring = 0; ring <= maxring; ring++) {
                int yy, xx;

                /* the outline of the square ring buckets around the node's */
                for (yy = cy - ring; yy <= cy + ring; yy++) {
                    int edge = (yy == cy - ring || yy == cy + ring);
                    int step = (edge || ring == 0) ? 1 : 2 * ring;

                    if (yy < 0 || yy >= (int)g_info.ncy)
                        continue;
                    for (xx = cx - ring; xx <= cx + ring; xx += step) {
                        long c, a, e;

                        if (xx < 0 || xx >= (int)g_info.ncx)
                            continue;
                        c = (long)yy * g_info.ncx + xx;
                        a = start[c];
                        e = start[c + 1];
                        for (; a < e; a++) {
                            Pt q = pts[a];
                            float dx = q.x - rx, dy = q.y - ry;

                            best.add(dx * dx + dy * dy, q.z);
                        }
                    }
                }
                /* everything not read yet is at least ring buckets away */
                if (best.n == nearest) {
                    float reach = ring * g_cell;

                    if (best.d[nearest - 1] <= reach * reach)
                        break;
                }
            }
            z = best.value(p->power);
            if (z < r->zmin) r->zmin = z;
            if (z > r->zmax) r->zmax = z;
            {
                char *o = text;

                if (i > 0)
                    *o++ = ' ';
                o = put_z(o, z);
                fwrite(text, 1, (size_t)(o - text), f);
            }
        }
        fputc('\n', f);
        if (progress && progress(XYZ_GRIDDING, j + 1, ny)) {
            fclose(f);
            free(vbuf);
            remove(mes_path);
            return XYZ_CANCEL;
        }
    }
    fflush(f);
    r->bytes = ftell(f);
    if (ferror(f)) {
        fclose(f);
        free(vbuf);
        remove(mes_path);
        return XYZ_EWRITE;
    }
    if (fclose(f) != 0) {
        free(vbuf);
        remove(mes_path);
        return XYZ_EWRITE;
    }
    free(vbuf);
    r->nx = nx;
    r->ny = ny;
    r->x0 = gx;
    r->y0 = gy;
    r->x1 = gx + (nx - 1) * p->spacing;
    r->y1 = gy + (ny - 1) * p->spacing;
    r->seconds = seconds_since(t0);
    return XYZ_OK;
}

/* here, after the DiskArray types are complete (Open Watcom would otherwise
 * skip their destructors) */
void xyz_unload(void)
{
    delete g_pts;
    delete g_start;
    g_pts = NULL;
    g_start = NULL;
}

const char *xyz_error(int code)
{
    switch (code) {
    case XYZ_OK:      return "Done.";
    case XYZ_CANCEL:  return "Canceled.";
    case XYZ_EOPEN:   return "The file can't be opened.";
    case XYZ_ENOPTS:  return "No lines with X Y Z numbers were found.";
    case XYZ_EDISK:   return "Temporary files couldn't be made (disk full?).";
    case XYZ_ECREATE: return "The output file can't be created.";
    case XYZ_ETOOBIG: return "Too many grid nodes: use a larger spacing.";
    case XYZ_EWRITE:  return "Writing the output failed (disk full?).";
    }
    return "Unknown error.";
}
