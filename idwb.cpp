/* idwb.cpp - inverse-distance gridding from the nearest points, found through
 * buckets: the points are stored on disk in cell order, so each node reads only
 * nearby cells, and those sit on nearby pages.
 *
 *   idwb <points> <grid> [check]
 *
 * check also searches every point for every node and compares the two grids.
 * A ring of cells around a node is only skipped once the nearest points found
 * are closer than anything outside the rings read so far, so the result is the
 * same as the full search. */
#include <math.h>
#include <time.h>
#include "dskarray.h"

#define NEAREST 6
#define PER_CELL 8

struct Point {
    float x, y, z;
};

static unsigned long seed;

static float rnd01()
{
    seed = seed * 1103515245UL + 12345UL;
    return (float)((seed >> 8) & 0xFFFFUL) / 65536.0f;
}

static double seconds(clock_t t0)
{
    return (double)(clock() - t0) / CLOCKS_PER_SEC;
}

struct Near {
    float d[NEAREST];
    float z[NEAREST];
    int n;

    void clear() { n = 0; }

    void add(float dist, float val)
    {
        int i;

        if (n == NEAREST) {
            if (dist >= d[NEAREST - 1])
                return;
            i = NEAREST - 1;
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

    float value() const
    {
        float s1 = 0, s2 = 0;

        for (int i = 0; i < n; i++) {
            float w = 1 / (d[i] + 1e-6f);

            s1 += z[i] * w;
            s2 += w;
        }
        return s1 / s2;
    }
};

static unsigned cells;

static unsigned cell_of(float v)
{
    int c = (int)(v * cells);

    return c >= (int)cells ? cells - 1 : (c < 0 ? 0 : c);
}

int main(int argc, char **argv)
{
    long points = argc > 1 ? atol(argv[1]) : 1200L;
    int size = argc > 2 ? atoi(argv[2]) : 20;
    int check = argc > 3;
    long i;
    int gx, gy;
    clock_t t0;
    long examined = 0;

    da_cache_init(64);
    printf("cache %d pages (%d KB)\n", da_cache_pages(), da_cache_pages() * 4);
    cells = (unsigned)sqrt((double)points / PER_CELL);
    if (cells < 1)
        cells = 1;
    printf("%ld points (%ld KB), %d x %d grid, %u x %u cells\n", points, points * 12 / 1024, size, size, cells, cells);

    DiskArray<Point> raw("raw.tmp", points);
    DiskArray<Point> sorted("sorted.tmp", points);
    DiskArray<long> start("start.tmp", (long)cells * cells + 1, 0L);
    DiskArray2<float> grid("grid.tmp", size, size);

    if (!raw || !sorted || !start || !grid) {
        printf("could not open the array files\n");
        return 1;
    }

    /* the points, and how many fall in each cell */
    t0 = clock();
    seed = 4242;
    for (i = 0; i < points; i++) {
        Point p;

        p.x = rnd01();
        p.y = rnd01();
        p.z = 100 * p.x + 50 * p.y * p.y;
        raw[i] = p;
        start[(long)cell_of(p.y) * cells + cell_of(p.x) + 1] += 1;
    }
    printf("points stored in %.2f s\n", seconds(t0));

    /* copy them in cell order (start[c] counts up while filling, and then
     * holds the end of cell c: the start of cell c is start[c - 1]) */
    t0 = clock();
    {
        long total = 0, cellcount = (long)cells * cells;
        long c;

        for (c = 1; c <= cellcount; c++) {
            long n = start[c];

            start[c] = total;
            total += n;
        }
        for (i = 0; i < points; i++) {
            Point p = raw[i];
            long at = (long)cell_of(p.y) * cells + cell_of(p.x) + 1;
            long to = start[at];

            sorted[to] = p;
            start[at] = to + 1;
        }
    }
    printf("sorted into cells in %.2f s\n", seconds(t0));

    t0 = clock();
    {
        Near best;
        float cellsize = 1.0f / cells;

        for (gy = 0; gy < size; gy++) {
            float ny = (gy + 0.5f) / size;
            int cy = (int)cell_of(ny);

            for (gx = 0; gx < size; gx++) {
                float nx = (gx + 0.5f) / size;
                int cx = (int)cell_of(nx);
                int r;

                best.clear();
                for (r = 0; r < (int)cells; r++) {
                    int yy, xx;

                    /* the outline of the square r cells around the node's cell */
                    for (yy = cy - r; yy <= cy + r; yy++) {
                        int edge = (yy == cy - r || yy == cy + r);
                        int step = (edge || r == 0) ? 1 : 2 * r;

                        if (yy < 0 || yy >= (int)cells)
                            continue;
                        for (xx = cx - r; xx <= cx + r; xx += step) {
                            long c, a, z;

                            if (xx < 0 || xx >= (int)cells)
                                continue;
                            c = (long)yy * cells + xx;
                            a = c ? (long)start[c] : 0L;
                            z = start[c + 1];
                            for (; a < z; a++) {
                                Point p = sorted[a];
                                float dx = p.x - nx, dy = p.y - ny;

                                best.add(dx * dx + dy * dy, p.z);
                                examined++;
                            }
                        }
                    }
                    /* everything outside is at least r cell widths away */
                    if (best.n == NEAREST) {
                        float reach = r * cellsize;

                        if (best.d[NEAREST - 1] <= reach * reach)
                            break;
                    }
                }
                grid[gx][gy] = best.value();
            }
        }
    }
    printf("gridded in %.2f s, %ld point looks, %ld per node\n", seconds(t0), examined, examined / ((long)size * size));

    {
        DaStats st = da_stats();
        printf("hits %lu misses %lu reads %lu writes %lu\n", st.hits, st.misses, st.reads, st.writes);
    }

    if (check) {
        long bad = 0;
        float worst = 0;

        t0 = clock();
        for (gy = 0; gy < size; gy++) {
            float ny = (gy + 0.5f) / size;

            for (gx = 0; gx < size; gx++) {
                float nx = (gx + 0.5f) / size, v, e;
                Near best;

                best.clear();
                for (i = 0; i < points; i++) {
                    Point p = sorted[i];
                    float dx = p.x - nx, dy = p.y - ny;

                    best.add(dx * dx + dy * dy, p.z);
                }
                v = best.value();
                e = (float)fabs(v - (float)grid[gx][gy]);
                if (e > worst)
                    worst = e;
                if (e != 0)
                    bad++;
            }
        }
        printf("all-points search took %.2f s: %ld of %ld nodes differ, worst %g\n", seconds(t0), bad, (long)size * size, (double)worst);
    }
    return 0;
}
