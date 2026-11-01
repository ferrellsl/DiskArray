/* idwdemo.cpp - inverse-distance gridding of scattered points, the job the old
 * example.cpp did, written with ordinary array syntax. Each grid node looks
 * at every point, so this is the simple all-points search, not bucketing.
 *
 * USE_RAM uses plain arrays instead, to compare results and time.
 * BIG holds more points than conventional memory can (720 KB of floats). */
#include <math.h>
#include <time.h>
#include "dskarray.h"

#ifdef BIG
#define POINTS 60000L
#define GRID   4
#else
#define POINTS 1200L
#define GRID   20
#endif

#ifdef USE_RAM
#define POINT_ARRAY(name) float *name = (float *)malloc(POINTS * sizeof(float))
#define GRID_ARRAY(name)  static float name[GRID][GRID]
#else
#define POINT_ARRAY(name) DiskArray<float> name(#name ".tmp", POINTS)
#define GRID_ARRAY(name)  DiskArray2<float> name("grid.tmp", GRID, GRID)
#endif

static unsigned long seed = 4242;

static float rnd01()
{
    seed = seed * 1103515245UL + 12345UL;
    return (float)((seed >> 8) & 0xFFFFUL) / 65536.0f;
}

int main()
{
    POINT_ARRAY(px);
    POINT_ARRAY(py);
    POINT_ARRAY(pz);
    GRID_ARRAY(grid);
    long i;
    int gx, gy;
    float sum = 0;
    clock_t t0;

#ifndef USE_RAM
    printf("cache: %d pages (%d KB)\n", da_cache_init(32), da_cache_pages() * 4);
#endif
    printf("%ld points (%ld KB), %d x %d grid\n", POINTS, POINTS * 12 / 1024, GRID, GRID);

    t0 = clock();
    for (i = 0; i < POINTS; i++) {
        float x = rnd01(), y = rnd01();

        px[i] = x;
        py[i] = y;
        pz[i] = 100 * x + 50 * y * y;
    }
    printf("points stored in %.2f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);

    t0 = clock();
    for (gy = 0; gy < GRID; gy++) {
        for (gx = 0; gx < GRID; gx++) {
            float nx = (gx + 0.5f) / GRID, ny = (gy + 0.5f) / GRID;
            float s1 = 0, s2 = 0;

            for (i = 0; i < POINTS; i++) {
                float dx = px[i] - nx, dy = py[i] - ny;
                float d = dx * dx + dy * dy + 1e-6f;
                float w = 1 / (d * d);

                s1 += pz[i] * w;
                s2 += w;
            }
            grid[gx][gy] = s1 / s2;
        }
    }
    printf("gridded in %.2f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);

    for (gy = 0; gy < GRID; gy++)
        for (gx = 0; gx < GRID; gx++)
            sum += grid[gx][gy];
    printf("grid sum %.3f, corners %.3f %.3f\n", (double)sum, (double)(float)grid[0][0], (double)(float)grid[GRID - 1][GRID - 1]);
#ifndef USE_RAM
    {
        DaStats st = da_stats();
        printf("hits %lu misses %lu reads %lu writes %lu\n", st.hits, st.misses, st.reads, st.writes);
    }
#endif
    return 0;
}
