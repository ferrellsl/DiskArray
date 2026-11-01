/* dafuzz.cpp - checks dskarray against plain arrays with random reads and
 * writes, with a pool small enough to force evictions, for several element
 * types and with arrays sharing the pool. Exit code 0 = all agree. */
#include <string.h>
#include "dskarray.h"

#define W 90
#define H 70
#define N 1500

struct Rec {
    double a;
    long b;
    char c[5];
    int operator==(const Rec &r) const { return a == r.a && b == r.b && !memcmp(c, r.c, 5); }
    int operator!=(const Rec &r) const { return !(*this == r); }
};

static unsigned long seed = 12345;

static unsigned long rnd()
{
    seed = seed * 1103515245UL + 12345UL;
    return (seed >> 8) & 0xFFFFFFUL;
}

static long failures = 0;

static void fail(const char *what, long i)
{
    if (failures++ < 10)
        printf("MISMATCH %s at %ld\n", what, i);
}

int main()
{
    static float sf[W][H];
    static double sd[N];
    static Rec *sr;
    long ops = 0;
    int pool[] = { 2, 3, 8, 32 };

    sr = (Rec *)malloc(sizeof(Rec) * 300);
    if (!sr)
        return 2;

    for (int p = 0; p < 4; p++) {
        printf("pool %d pages: got %d\n", pool[p], da_cache_init(pool[p]));
        {
            DiskArray2<float> a("f.tmp", W, H, (float)-1);
            DiskArray<double> d("d.tmp", N, 2.5);
            DiskArray2<Rec> r("r.tmp", 20, 15);

            if (!a || !d || !r) {
                printf("open failed\n");
                return 2;
            }
            memset(sr, 0, sizeof(Rec) * 300);
            for (int x = 0; x < W; x++)
                for (int y = 0; y < H; y++)
                    sf[x][y] = -1;
            for (int i = 0; i < N; i++)
                sd[i] = 2.5;

            for (long k = 0; k < 60000L; k++) {
                unsigned long v = rnd();
                unsigned x = (unsigned)(rnd() % W), y = (unsigned)(rnd() % H);
                unsigned i = (unsigned)(rnd() % N);
                unsigned rx = (unsigned)(rnd() % 20), ry = (unsigned)(rnd() % 15);

                switch (v % 7) {
                case 0:
                    a[x][y] = (float)v;
                    sf[x][y] = (float)v;
                    break;
                case 1:
                    if ((float)a[x][y] != sf[x][y])
                        fail("float", k);
                    break;
                case 2:
                    d[i] = v * 0.5;
                    sd[i] = v * 0.5;
                    break;
                case 3:
                    if ((double)d[i] != sd[i])
                        fail("double", k);
                    break;
                case 4: {
                    Rec t;
                    memset(&t, 0, sizeof(t));
                    t.a = v;
                    t.b = (long)k;
                    t.c[(unsigned)(v % 5)] = 'x';
                    r[rx][ry] = t;
                    sr[ry * 20 + rx] = t;
                    break;
                }
                case 5:
                    if ((Rec)r[rx][ry] != sr[ry * 20 + rx])
                        fail("record", k);
                    break;
                case 6:
                    /* element-to-element and compound, across pages */
                    a[x][y] = a[(x + 37) % W][(y + 23) % H];
                    sf[x][y] = sf[(x + 37) % W][(y + 23) % H];
                    a[x][y] += 2;
                    sf[x][y] += 2;
                    d[i] *= 2;
                    sd[i] *= 2;
                    break;
                }
                ops++;
            }

            for (int x2 = 0; x2 < W; x2++)
                for (int y2 = 0; y2 < H; y2++)
                    if ((float)a[x2][y2] != sf[x2][y2])
                        fail("final float", x2);
            for (int i2 = 0; i2 < N; i2++)
                if ((double)d[i2] != sd[i2])
                    fail("final double", i2);
            for (int r2 = 0; r2 < 300; r2++)
                if ((Rec)r[r2 % 20][r2 / 20] != sr[r2])
                    fail("final record", r2);
        }
    }

    /* contents survive in a kept file */
    {
        DiskArray2<float> *k = new DiskArray2<float>("keep.tmp", 300, 200, (float)0, 1);
        for (int x = 0; x < 300; x += 7)
            for (int y = 0; y < 200; y += 3)
                (*k)[x][y] = (float)(x * 1000 + y);
        delete k;
        k = new DiskArray2<float>("keep.tmp", 300, 200, (float)0, 1);
        for (int x3 = 0; x3 < 300; x3++)
            for (int y3 = 0; y3 < 200; y3++) {
                float want = (x3 % 7 == 0 && y3 % 3 == 0) ? (float)(x3 * 1000 + y3) : (float)0;
                if ((float)(*k)[x3][y3] != want)
                    fail("kept", x3);
            }
        delete k;
        remove("keep.tmp");
    }

    {
        DaStats st = da_stats();
        printf("%ld ops, hits %lu, misses %lu, reads %lu, writes %lu\n", ops, st.hits, st.misses, st.reads, st.writes);
    }
    printf(failures ? "FAILED: %ld mismatches\n" : "OK: %ld mismatches\n", failures);
    return failures ? 1 : 0;
}
