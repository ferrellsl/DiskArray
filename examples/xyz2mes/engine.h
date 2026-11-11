/* engine.h - reading XYZ points and gridding them by inverse distance, with
 * the points kept on disk in DiskArrays.  Callable from C. */
#ifndef ENGINE_H
#define ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define XYZ_OK        0
#define XYZ_CANCEL    1         /* the progress function asked to stop     */
#define XYZ_EOPEN     2         /* the input file can't be read            */
#define XYZ_ENOPTS    3         /* no lines with three numbers             */
#define XYZ_EDISK     4         /* a temporary file couldn't be made       */
#define XYZ_ECREATE   5         /* the output file can't be written        */
#define XYZ_ETOOBIG   6         /* too many grid nodes along a side        */
#define XYZ_EWRITE    7         /* writing the output failed (disk full?)  */

/* stages, for the progress function */
#define XYZ_READING   1
#define XYZ_COUNTING  2
#define XYZ_SORTING   3
#define XYZ_GRIDDING  4

typedef struct {
    long   count;               /* points read                             */
    long   skipped;             /* lines without three numbers             */
    double xmin, xmax, ymin, ymax, zmin, zmax;
    double spacing;             /* average distance between points         */
    double suggested;           /* a round node spacing near that          */
    unsigned ncx, ncy;          /* buckets                                 */
    int    cache_pages;         /* 4 KB pages in the DiskArray cache       */
    double seconds;
} XyzInfo;

typedef struct {
    double spacing;             /* between grid nodes, in data units       */
    int    nearest;             /* points used per node                    */
    double power;               /* weight = 1 / distance^power             */
} XyzParams;

typedef struct {
    long   nx, ny;
    double x0, y0, x1, y1;      /* the grid's corners                      */
    double zmin, zmax;          /* of the grid                             */
    double seconds;
    long   bytes;
} XyzResult;

/* Returns nonzero to cancel.  done goes from 0 to total within a stage. */
typedef int (*XyzProgress)(int stage, long done, long total);

int  xyz_load(const char *path, XyzInfo *info, XyzProgress progress);
int  xyz_grid(const char *mes_path, const XyzParams *p, XyzResult *r, XyzProgress progress);
void xyz_grid_size(const XyzParams *p, long *nx, long *ny);
void xyz_unload(void);
const char *xyz_error(int code);

#ifdef __cplusplus
}
#endif

#endif
