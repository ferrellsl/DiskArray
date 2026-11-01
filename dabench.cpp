/* dabench.cpp - element accesses per second, RAM vs the old one-seek-per-element
 * method (as disk_arr does it) vs dskarray, for several access patterns. */
#include <string.h>
#include <time.h>
#include "dskarray.h"

#define XS 200
#define YS 200

static float **ram;
static FILE *oldf;

static long old_off(unsigned x, unsigned y) { return ((long)x * YS + y) * sizeof(float); }
static float old_get(unsigned x, unsigned y)
{
    float v;
    fseek(oldf, old_off(x, y), SEEK_SET);
    fread(&v, sizeof(v), 1, oldf);
    return v;
}
static void old_put(unsigned x, unsigned y, float v)
{
    fseek(oldf, old_off(x, y), SEEK_SET);
    fwrite(&v, sizeof(v), 1, oldf);
}

static unsigned long seed;
static unsigned rnd(unsigned n)
{
    seed = seed * 1103515245UL + 12345UL;
    return (unsigned)(((seed >> 8) & 0xFFFFUL) % n);
}

static void report(const char *name, clock_t t0, long ops)
{
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;

    printf("  %-18s %8ld ops %7.2f s  %9.0f ops/s\n", name, ops, s, s > 0 ? ops / s : 0.0);
}

enum { ROWS, COLS, NEAR_, RANDOM_ };

template <class A>
static float run(A &a, const char *name, int pattern, long limit)
{
    clock_t t0 = clock();
    float sum = 0;
    long ops = 0;
    unsigned x, y;

    seed = 99;
    switch (pattern) {
    case ROWS:
        for (y = 0; y < YS && ops < limit; y++)
            for (x = 0; x < XS; x++, ops++)
                sum += a.get(x, y);
        break;
    case COLS:
        for (x = 0; x < XS && ops < limit; x++)
            for (y = 0; y < YS; y++, ops++)
                sum += a.get(x, y);
        break;
    case NEAR_:
        x = y = 100;
        for (ops = 0; ops < limit; ops++) {
            x = (x + rnd(5) + XS - 2) % XS;
            y = (y + rnd(5) + YS - 2) % YS;
            sum += a.get(x, y);
        }
        break;
    case RANDOM_:
        for (ops = 0; ops < limit; ops++) {
            x = rnd(XS);
            y = rnd(YS);
            sum += a.get(x, y);
        }
        break;
    }
    report(name, t0, ops);
    return sum;
}

template <class A>
static void fill(A &a, long limit)
{
    clock_t t0 = clock();
    long ops = 0;

    for (unsigned y = 0; y < YS && ops < limit; y++)
        for (unsigned x = 0; x < XS; x++, ops++)
            a.put(x, y, (float)(x + y));
    report("write rows", t0, ops);
}

struct Ram {
    float get(unsigned x, unsigned y) { return ram[x][y]; }
    void put(unsigned x, unsigned y, float v) { ram[x][y] = v; }
};
struct Old {
    float get(unsigned x, unsigned y) { return old_get(x, y); }
    void put(unsigned x, unsigned y, float v) { old_put(x, y, v); }
};

template <class A>
static void all(A &a, const char *title, long limit)
{
    float s = 0;

    printf("%s\n", title);
    fill(a, limit);
    s += run(a, "read rows", ROWS, limit);
    s += run(a, "read columns", COLS, limit);
    s += run(a, "read nearby", NEAR_, limit);
    s += run(a, "read random", RANDOM_, limit);
    printf("  (check %.0f)\n", (double)s);
}

int main()
{
    long full = (long)XS * YS;
    int i;

    ram = (float **)malloc(sizeof(float *) * XS);
    for (i = 0; ram && i < XS; i++) {
        ram[i] = (float *)malloc(sizeof(float) * YS);
        if (!ram[i]) {
            printf("no memory for the RAM baseline at row %d\n", i);
            return 1;
        }
    }
    {
        Ram r;
        all(r, "RAM (plain float array)", full);
    }
    for (i = 0; i < XS; i++)
        free(ram[i]);
    free(ram);

    {
        static char zero[4096];
        Old o;

        oldf = fopen("old.tmp", "w+b");
        for (i = 0; i < 40; i++)
            fwrite(zero, 1, sizeof(zero), oldf);
        all(o, "Old method (fseek + fread for each element)", 4000L);
        fclose(oldf);
        remove("old.tmp");
    }

    {
        int pools[] = { 8, 32, 64 };

        for (i = 0; i < 3; i++) {
            char title[60];
            int got = da_cache_init(pools[i]);
            sprintf(title, "dskarray, %d pages (%d KB)", got, got * 4);
            {
                DiskArray2<float> a("bench.tmp", XS, YS);
                if (!a) {
                    printf("open failed\n");
                    return 1;
                }
                da_stats_reset();
                all(a, title, full);
                {
                    DaStats st = da_stats();
                    printf("  hits %lu misses %lu reads %lu writes %lu\n", st.hits, st.misses, st.reads, st.writes);
                }
            }
        }
    }
    printf("done\n");
    return 0;
}
