/* cli.cpp - the engine without the screen, for testing: cli <xyz> <mes> [spacing] [nearest] [power] */
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"
static int quiet(int, long, long) { return 0; }
int main(int argc, char **argv)
{
    XyzInfo info; XyzParams p; XyzResult r; int e;
    if (argc < 3) { printf("cli <xyz> <mes> [spacing] [nearest] [power]\n"); return 1; }
    if ((e = xyz_load(argv[1], &info, quiet)) != XYZ_OK) { printf("%s\n", xyz_error(e)); return 1; }
    printf("%ld points (%ld skipped), spacing %.3f, suggested %g, %u x %u buckets, %.1f s\n",
           info.count, info.skipped, info.spacing, info.suggested, info.ncx, info.ncy, info.seconds);
    p.spacing = argc > 3 ? atof(argv[3]) : info.suggested;
    p.nearest = argc > 4 ? atoi(argv[4]) : 8;
    p.power = argc > 5 ? atof(argv[5]) : 2.0;
    if ((e = xyz_grid(argv[2], &p, &r, quiet)) != XYZ_OK) { printf("%s\n", xyz_error(e)); return 1; }
    printf("%ld x %ld, z %.3f..%.3f, %ld bytes, %.1f s\n", r.nx, r.ny, r.zmin, r.zmax, r.bytes, r.seconds);
    xyz_unload();
    return 0;
}
