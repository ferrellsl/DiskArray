/*--------------------------------------------------------------------------*/
/* XYZ2MES : ASCII XYZ points to a qwtplot3d mesh (.mes) by inverse         */
/* distance weighting, for DOS.                                             */
/*                                                                          */
/* The screen is the Plain Design GUI library (DOSGUI), at 800 x 600 when   */
/* the card has it and 640 x 480 when not.  The points are kept on disk in  */
/* DiskArrays (engine.cpp), so the number of points isn't limited by the    */
/* 640 KB of a 16-bit DOS program.                                          */
/*--------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include "gui.h"
#include "engine.h"

#define VERSION   "1.0"
#define T(s)      ((unsigned char *)(s))

#define PW        76            /* the main panel, in text cells            */
#define PH        27

#define C_PANEL   0xF0          /* black on white                           */
#define C_LABEL   0xF4
#define C_HEAD    0xF1
#define C_NOTE    0xF8
#define C_GOOD    0xF2
#define C_BAD     0xF4

#define ID_OPEN   ALTO
#define ID_SET    ALTS
#define ID_GRID   ALTG
#define ID_ABOUT  ALTA
#define ID_EXIT   ALTX

static int px, py;              /* the panel's corner                       */
static char in_path[130];
static char out_path[130];
static int loaded;
static XyzInfo info;
static XyzParams params = { 0.0, 8, 2.0 };
static int spacing_set;         /* the user chose the spacing               */
static int batch;               /* /B: no dialogs and no waiting for keys   */
static int batch_rc;
static char batch_text[200];
static unsigned free_at_start;  /* KB of DOS memory free when we started    */

/*--------------------------------------------------------------------------*/
/* memory, as DOS sees it                                                   */
/*--------------------------------------------------------------------------*/
/* the largest block DOS could still give, in KB */
static unsigned dos_free_kb(void)
{
  union REGS r;

  r.h.ah = 0x48;
  r.x.bx = 0xFFFF;
  intdos(&r, &r);
  return r.x.bx / 64;
}

/* the program's own block: code, data and stack */
static unsigned program_kb(void)
{
  unsigned far *size = (unsigned far *) MK_FP(_psp - 1, 3);

  return *size / 64;
}

#ifdef SHOTS
/*--------------------------------------------------------------------------*/
/* screenshots for the README (a build with -dSHOTS): the screen as a       */
/* 16-color BMP, read back pixel by pixel                                   */
/*--------------------------------------------------------------------------*/
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8, f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned) (v & 0xFFFF)); put16(f, (unsigned) (v >> 16)); }

static void save_bmp(const char *name)
{
  static const unsigned char pal[16][3] = {
    {0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
    {85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}};
  FILE *f = fopen(name, "wb");
  int  w = gr_scr_xsize, h = gr_scr_ysize, x, y, i;
  unsigned long row = ((unsigned long) w / 2 + 3) & ~3UL;

  if (!f) return;
  hide_mouse_cursor();
  fputc('B', f); fputc('M', f);
  put32(f, 118 + row * h); put32(f, 0); put32(f, 118);
  put32(f, 40); put32(f, w); put32(f, h); put16(f, 1); put16(f, 4);
  put32(f, 0); put32(f, row * h); put32(f, 2835); put32(f, 2835); put32(f, 16); put32(f, 0);
  for (i = 0; i < 16; i++) { fputc(pal[i][2], f); fputc(pal[i][1], f); fputc(pal[i][0], f); fputc(0, f); }
  for (y = h - 1; y >= 0; y--)
  {
    for (x = 0; x < w; x += 2) fputc(((GETPIX(x, y) & 15) << 4) | (GETPIX(x + 1, y) & 15), f);
    for (x = w / 2; x < (int) row; x++) fputc(0, f);
  }
  fclose(f);
  show_mouse_cursor();
}
#endif

/*--------------------------------------------------------------------------*/
/* text in the panel, padded to width                                       */
/*--------------------------------------------------------------------------*/
static void put(int x, int y, int width, const char *s, int color)
{
  char buf[120];
  int  n = (int) strlen(s);

  if (width > (int) sizeof(buf) - 1) width = sizeof(buf) - 1;
  if (n > width)
  {
    /* long paths: keep the end */
    strcpy(buf, "...");
    strcat(buf, s + n - (width - 3));
  }
  else
  {
    strcpy(buf, s);
    while (n < width) buf[n++] = ' ';
    buf[n] = 0;
  }
  hide_mouse_cursor();
  pls(px + x, py + y, T(buf), color);
  show_mouse_cursor();
}

static void label(int x, int y, const char *s)
{
  put(x, y, (int) strlen(s), s, C_LABEL);
}

/*--------------------------------------------------------------------------*/
/* a window like the demo's: white with a blue frame and a title            */
/*--------------------------------------------------------------------------*/
static int open_window(int x, int y, int xsize, int ysize, const char *title)
{
  int wh, i;

  wh = messagebox(x, y, xsize, ysize, 0xF0, 0x08, YES);
  hide_mouse_cursor();
  grect(x * 8 + 1, y * 16 + 1, (x + xsize) * 8 - 2, (y + ysize) * 16 - 1, 0);
  for (i = 1; i <= 5; i++)
    grect(x * 8 + 1 + i, y * 16 + 1 + i, (x + xsize) * 8 - 2 - i, (y + ysize) * 16 - 1 - i, 1);
  gfillrect(x * 8 + 8, y * 16 + 8, (x + xsize) * 8 - 9, y * 16 + 27, 1);
  gpls((x + xsize / 2 - (int) strlen(title) / 2) * 8, (y + 1) * 16 - 5, T(title), 0x1F);
  show_mouse_cursor();
  return wh;
}

static void close_window(int wh)
{
  destroy_messagebox(wh);
}

/* waits for the one button */
static void one_button(char *text, int x, int y, int key, int acc)
{
  int h, k;

  deactivate_activebuttons();
  h = draw_button(text, x, y, (int) strlen(text) * 8 + 14, 23, key, acc);
  for (;;)
  {
    k = check_button(getkey());
    gcurs(MOVE);

    /* the buttons' letters work without Alt too */
    switch (k)
    {
      case 'o': case 'O': k = ID_OPEN;  break;
      case 's': case 'S': k = ID_SET;   break;
      case 'g': case 'G': k = ID_GRID;  break;
      case 'a': case 'A': k = ID_ABOUT; break;
      case 'x': case 'X': k = ID_EXIT;  break;
    }
    if (k == key || k == ESC) break;
  }
  disable_button(h);
  reactivate_activebuttons();
}

/*--------------------------------------------------------------------------*/
/* a message with an OK button; up to three lines                           */
/*--------------------------------------------------------------------------*/
static void message(const char *title, const char *l1, const char *l2, const char *l3)
{
  int w = 58, h = 10;
  int x = (txt_scr_xsize - w) / 2, y = (txt_scr_ysize - h) / 2;
  int wh;

  if (batch)
  {
    sprintf(batch_text, "%s: %s %s %s", title, l1 ? l1 : "", l2 ? l2 : "", l3 ? l3 : "");
    return;
  }
  wh = open_window(x, y, w, h, title);

  hide_mouse_cursor();
  if (l1) pls(x + 3, y + 3, T(l1), C_PANEL);
  if (l2) pls(x + 3, y + 4, T(l2), C_PANEL);
  if (l3) pls(x + 3, y + 5, T(l3), C_PANEL);
  show_mouse_cursor();
  one_button("  OK  ", x + w / 2 - 3, y + 7, ENTER, ESC);
  close_window(wh);
}

/*--------------------------------------------------------------------------*/
/* the main panel                                                           */
/*--------------------------------------------------------------------------*/
static void show_grid_size(void)
{
  char s[100];
  long nx, ny;

  if (!loaded || params.spacing <= 0)
  {
    put(19, 13, 54, "(open a file first)", C_NOTE);
    return;
  }
  xyz_grid_size(&params, &nx, &ny);
  put(19, 13, 54, "", C_PANEL);
  sprintf(s, "%g", params.spacing);
  put(19, 13, 12, s, C_PANEL);
  sprintf(s, "-> %ld x %ld nodes (%ld)", nx, ny, nx * ny);
  put(33, 13, 40, s, C_NOTE);
}

static void show_params(void)
{
  char s[40];

  show_grid_size();
  sprintf(s, "%d", params.nearest);
  put(19, 14, 12, s, C_PANEL);
  sprintf(s, "%g", params.power);
  put(19, 15, 12, s, C_PANEL);
}

static void show_info(void)
{
  char s[100];

  put(16, 3, 58, in_path[0] ? in_path : "(none)", C_PANEL);
  if (!loaded)
  {
    put(16, 4, 58, "", C_PANEL);
    put(16, 5, 58, "", C_PANEL);
    put(16, 6, 58, "", C_PANEL);
    put(16, 7, 58, "", C_PANEL);
    put(16, 8, 58, "", C_PANEL);
    put(16, 9, 58, "", C_PANEL);
    return;
  }
  sprintf(s, "%ld", info.count);
  if (info.skipped) sprintf(s + strlen(s), "   (%ld other lines skipped)", info.skipped);
  put(16, 4, 58, s, C_PANEL);
  sprintf(s, "%.2f  to  %.2f   (%.2f)", info.xmin, info.xmax, info.xmax - info.xmin);
  put(16, 5, 58, s, C_PANEL);
  sprintf(s, "%.2f  to  %.2f   (%.2f)", info.ymin, info.ymax, info.ymax - info.ymin);
  put(16, 6, 58, s, C_PANEL);
  sprintf(s, "%.2f  to  %.2f", info.zmin, info.zmax);
  put(16, 7, 58, s, C_PANEL);
  sprintf(s, "%.3f average", info.spacing);
  put(16, 8, 58, s, C_PANEL);
  sprintf(s, "%u x %u buckets, read in %.1f s", info.ncx, info.ncy, info.seconds);
  put(16, 9, 58, s, C_NOTE);
}

static void status(const char *s, int color)
{
  put(3, 21, 70, s, color);
}

static void draw_panel(void)
{
  char s[80];

  hide_mouse_cursor();
  gfillrect(px * 8, py * 16, (px + PW) * 8 - 1, (py + PH) * 16 - 1, 15);
  grect(px * 8, py * 16, (px + PW) * 8 - 1, (py + PH) * 16 - 1, 0);
  grect(px * 8 + 2, py * 16 + 2, (px + PW) * 8 - 3, (py + PH) * 16 - 3, 1);
  gfillrect(px * 8 + 4, py * 16 + 4, (px + PW) * 8 - 5, py * 16 + 23, 1);
  strcpy(s, "XYZ to MES  -  Inverse Distance Gridding  " VERSION);
  gpls((px + PW / 2 - (int) strlen(s) / 2) * 8, py * 16 + 6, T(s), 0x1F);
  show_mouse_cursor();

  put(3, 2, 20, "Points", C_HEAD);
  label(3, 3, "File");
  label(3, 4, "Points");
  label(3, 5, "X");
  label(3, 6, "Y");
  label(3, 7, "Z");
  label(3, 8, "Spacing");

  put(3, 12, 20, "Grid", C_HEAD);
  label(3, 13, "Node spacing");
  label(3, 14, "Nearest points");
  label(3, 15, "Power");

  put(3, 17, 20, "Output", C_HEAD);
  label(3, 18, "File");
  put(16, 18, 58, out_path[0] ? out_path : "(none yet)", C_PANEL);

  {
    int pages = loaded ? info.cache_pages : 0;

    if (pages) sprintf(s, "DiskArray cache: %d pages (%d KB). Temporary files go in the current directory.", pages, pages * 4);
    else strcpy(s, "Open an ASCII XYZ file (x y z per line; spaces, tabs or commas).");
    status(s, C_NOTE);
  }

  show_info();
  show_params();
}

static void draw_buttons(void)
{
  int y = py + PH - 3;
  int x = px + 4;

  draw_button("^Open XYZ...", x,      y, 12 * 8 + 14, 23, ID_OPEN, ALTO);
  draw_button("^Settings...", x + 15, y, 12 * 8 + 14, 23, ID_SET, ALTS);
  draw_button("^Grid to MES...", x + 30, y, 15 * 8 + 14, 23, ID_GRID, ALTG);
  draw_button("^About", x + 48, y, 6 * 8 + 14, 23, ID_ABOUT, ALTA);
  draw_button("E^xit", x + 58, y, 5 * 8 + 14, 23, ID_EXIT, ALTX);
}

/*--------------------------------------------------------------------------*/
/* the progress window                                                      */
/*--------------------------------------------------------------------------*/
static int  pg_wh = -1, pg_stage, pg_x, pg_y, pg_bar;
static char pg_name[20];
#define PG_W 52
#define PG_H 10

static void progress_open(const char *title, const char *name)
{
  const char *p = strrchr(name, '\\');

  strncpy(pg_name, p ? p + 1 : name, sizeof(pg_name) - 1);
  pg_name[sizeof(pg_name) - 1] = 0;
  pg_x = (txt_scr_xsize - PG_W) / 2;
  pg_y = (txt_scr_ysize - PG_H) / 2;
  pg_wh = open_window(pg_x, pg_y, PG_W, PG_H, title);
  pg_stage = 0;
  pg_bar = 0;
}

static void progress_close(void)
{
  if (pg_bar) show_percent_bar(-1, 0, (pg_x + 4) * 8, (pg_y + 4) * 16, (PG_W - 8) * 8, 24, 0, 1, 15);
  pg_bar = 0;
  if (pg_wh >= 0) close_window(pg_wh);
  pg_wh = -1;
}

static int progress(int stage, long done, long total)
{
  int bx = (pg_x + 4) * 8, by = (pg_y + 4) * 16, bw = (PG_W - 8) * 8;
  int a;

  if (stage != pg_stage)
  {
    char s[80];

    if (pg_bar) show_percent_bar(-1, 0, bx, by, bw, 24, 0, 1, 15);
    switch (stage)
    {
      case XYZ_READING:  sprintf(s, "Reading %s", pg_name); break;
      case XYZ_COUNTING: strcpy(s, "Counting the points in each bucket"); break;
      case XYZ_SORTING:  strcpy(s, "Sorting the points into buckets"); break;
      default:           sprintf(s, "Gridding and writing %s", pg_name); break;
    }
    hide_mouse_cursor();
    gfillrect((pg_x + 2) * 8, (pg_y + 2) * 16, (pg_x + PG_W - 2) * 8, (pg_y + 3) * 16 - 1, 15);
    pls(pg_x + 4, pg_y + 2, T(s), C_PANEL);
    show_mouse_cursor();
    show_percent_bar(0, 999, bx, by, bw, 24, 0, 1, 15);
    pg_bar = 1;
    pg_stage = stage;
  }
  if (stage == XYZ_GRIDDING)
  {
    char s[40];

    sprintf(s, "row %ld of %ld   ", done, total);
    hide_mouse_cursor();
    pls(pg_x + 4, pg_y + 3, T(s), C_NOTE);
    show_mouse_cursor();
  }
#ifdef SHOTS
  if (batch && stage == XYZ_GRIDDING && done == total / 2) save_bmp("SHOT2.BMP");
#endif
  a = 1 + (int) ((double) done * 998.0 / (double) (total > 0 ? total : 1));
  if (a > 999) a = 999;
  return show_percent_bar(a, 999, bx, by, bw, 24, 0, 1, 15) == -1;
}

/*--------------------------------------------------------------------------*/
/* the commands                                                             */
/*--------------------------------------------------------------------------*/
static void do_open(const char *given)
{
  char name[130];
  int  r;

  if (given)
  {
    strncpy(name, given, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
  }
  else
  {
    deactivate_activebuttons();
    r = file_open_dialog("Open XYZ Points", "*.*", name);
    reactivate_activebuttons();
    if (r != YES) return;
  }

  strcpy(in_path, name);
  loaded = 0;
  show_info();
  status("Reading...", C_NOTE);

  deactivate_activebuttons();
  progress_open("Reading Points", name);
  r = xyz_load(name, &info, progress);
  progress_close();
  reactivate_activebuttons();

  if (r != XYZ_OK)
  {
    in_path[0] = 0;
    show_info();
    show_grid_size();
    status(xyz_error(r), C_BAD);
    if (r != XYZ_CANCEL) message("Open XYZ", name, xyz_error(r), NULL);
    return;
  }
  loaded = 1;
  if (!spacing_set) params.spacing = info.suggested;
  show_info();
  show_params();
  {
    char s[100];

    long kb = (long) info.cache_pages * 4L;

    sprintf(s, "Read in %.1f s.", info.seconds);
    sprintf(s + strlen(s), " DiskArray cache %ld KB, DOS memory free %u KB.", kb, dos_free_kb());
    status(s, C_GOOD);
  }
}

static void do_about(void);

static int read_number(const char *s, double *v)
{
  char *end;

  *v = strtod(s, &end);
  while (*end == ' ') end++;
  return end != s && *end == 0;
}

static void do_settings(void)
{
  struct xfield pf[3];
  int  w = 50, h = 15;
  int  x = (txt_scr_xsize - w) / 2, y = (txt_scr_ysize - h) / 2;
  int  wh, h1, h2, i, fret, fptr = 0, done = 0;
  char s[60];

  wh = open_window(x, y, w, h, "Grid Settings");

  for (i = 0; i < 3; i++)
  {
    pf[i].type = F_TEXT;
    pf[i].xstart = 0;
    pf[i].fcolor = 0x70;
    pf[i].tcolor = C_LABEL;
    pf[i].x = x + 22;
    pf[i].y = y + 3 + i * 2;
    pf[i].length = 12;
    pf[i].title[0] = 0;
  }
  sprintf(pf[0].f, "%g", params.spacing);
  sprintf(pf[1].f, "%d", params.nearest);
  sprintf(pf[2].f, "%g", params.power);

  hide_mouse_cursor();
  pls(x + 3, y + 3, T("Node spacing"), C_LABEL);
  pls(x + 3, y + 5, T("Nearest points"), C_LABEL);
  pls(x + 3, y + 7, T("Power"), C_LABEL);
  pls(x + 36, y + 5, T("1 to 32"), C_NOTE);
  pls(x + 36, y + 7, T("2 is usual"), C_NOTE);
  if (loaded)
  {
    sprintf(s, "suggested %g", info.suggested);
    pls(x + 36, y + 3, T(s), C_NOTE);
  }
  pls(x + 3, y + 9, T("TAB moves to the next field and the buttons."), C_NOTE);
  show_mouse_cursor();

  alt_control(INIT, 0);
  for (i = 0; i < 3; i++) text_field(&pf[i], INIT);

  deactivate_activebuttons();
  h1 = draw_button("^Continue", x + 12, y + 11, 78, 23, ALTC, ALTC);
  h2 = draw_button("Cancel", x + 28, y + 11, 78, 23, ESC, ESC);

  for (;;)
  {
    gcurs(MOVE);
    if (fptr != -1) i = text_field(&pf[fptr], -2);
    else            i = getkey();

    fret = check_button(i);
    if (fptr == -1 && fret != EOF) fptr = 0;

    if (mouse_left_pressed() == YES)
    {
      get_mouse_location();
      for (i = 0; i < 3; i++)
        if (mouse_x >= pf[i].x && mouse_x < pf[i].x + pf[i].length && mouse_y == pf[i].y)
        {
          fptr = i;
          break;
        }
    }

    switch (fret)
    {
      case TAB:
        fptr++;
        if (fptr >= 3)
        {
          fptr = -1;
          check_button(CONTROL);
          check_button(TAB);
        }
        break;
      case ENTER:
      case DOWNKEY:
        if (fptr == 2 && fret == ENTER) done = ALTC;
        else if (++fptr >= 3) fptr = 0;
        break;
      case SHIFTTAB:
      case UPKEY:
        if (--fptr < 0) fptr = 2;
        break;
      case ESC:
        done = ESC;
        break;
      case ALTC:
        done = ALTC;
        break;
    }

    if (done == ALTC)
    {
      double sp, ne, pw;
      const char *bad = NULL;

      if (!read_number(pf[0].f, &sp) || sp <= 0) bad = "The node spacing must be a number above 0.";
      else if (!read_number(pf[1].f, &ne) || ne < 1 || ne > 32 || ne != (int) ne) bad = "Nearest points must be a whole number, 1 to 32.";
      else if (!read_number(pf[2].f, &pw) || pw <= 0 || pw > 10) bad = "The power must be a number above 0, up to 10.";
      if (bad)
      {
        message("Grid Settings", bad, NULL, NULL);
        done = 0;
        fptr = 0;
        continue;
      }
      if (sp != params.spacing) spacing_set = 1;
      params.spacing = sp;
      params.nearest = (int) ne;
      params.power = pw;
    }
    if (done) break;
  }

  disable_button(h1);
  disable_button(h2);
  reactivate_activebuttons();
  close_window(wh);
  alt_control(INIT, 0);
  show_params();
}

static void do_grid(const char *given)
{
  char name[130];
  int  r;
  long nx, ny;
  XyzResult res;

  if (!loaded)
  {
    message("Grid to MES", "Open an XYZ file first.", NULL, NULL);
    return;
  }
  xyz_grid_size(&params, &nx, &ny);
  if (nx > 32000L || ny > 32000L)
  {
    message("Grid to MES", xyz_error(XYZ_ETOOBIG), NULL, NULL);
    return;
  }

  if (given)
  {
    strncpy(name, given, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
  }
  else
  {
    deactivate_activebuttons();
    r = file_save_dialog("Save the Grid", "*.MES", name);
    reactivate_activebuttons();
    if (r != YES) return;
  }

  strcpy(out_path, name);
  put(16, 18, 58, out_path, C_PANEL);
  status("Gridding...", C_NOTE);

  deactivate_activebuttons();
  progress_open("Gridding", name);
  r = xyz_grid(name, &params, &res, progress);
  progress_close();
  reactivate_activebuttons();

  if (r != XYZ_OK)
  {
    status(xyz_error(r), C_BAD);
    if (r != XYZ_CANCEL) message("Grid to MES", name, xyz_error(r), NULL);
    return;
  }
  {
    char s1[100], s2[100], s3[100];

    sprintf(s1, "%ld x %ld nodes", res.nx, res.ny);
    sprintf(s1 + strlen(s1), " in %.1f s,", res.seconds);
    sprintf(s1 + strlen(s1), " %ld KB", (res.bytes + 512) / 1024);
    status(s1, C_GOOD);
    batch_rc = 0;
    sprintf(s2, "X %.2f to %.2f, Y %.2f to %.2f", res.x0, res.x1, res.y0, res.y1);
    sprintf(s3, "Z %.2f to %.2f", res.zmin, res.zmax);
    message("Grid Written", s1, s2, s3);
  }
}

static void do_about(void)
{
  int w = 60, h = 19;
  int x = (txt_scr_xsize - w) / 2, y = (txt_scr_ysize - h) / 2;
  int wh = open_window(x, y, w, h, "About XYZ to MES");
  char s1[80], s2[80];

  sprintf(s1, "Memory: program %u KB, DiskArray cache %d KB,", program_kb(), loaded ? info.cache_pages * 4 : 0);
  sprintf(s2, "DOS memory free now %u KB (%u KB at start).", dos_free_kb(), free_at_start);

  hide_mouse_cursor();
  pls(x + 3, y + 3, T("XYZ to MES " VERSION " for DOS"), C_HEAD);
  pls(x + 3, y + 5, T("Grids ASCII X Y Z points by inverse distance weighting"), C_PANEL);
  pls(x + 3, y + 6, T("and writes a qwtplot3d mesh file (.mes)."), C_PANEL);
  pls(x + 3, y + 8, T("Points are kept on disk with DiskArray, so the number"), C_PANEL);
  pls(x + 3, y + 9, T("of points isn't limited by conventional memory."), C_PANEL);
  pls(x + 3, y + 10, T("Screen: Plain Design GUI library (DOSGUI)."), C_PANEL);
  pls(x + 3, y + 12, T(s1), C_NOTE);
  pls(x + 3, y + 13, T(s2), C_NOTE);
  show_mouse_cursor();
#ifdef SHOTS
  if (batch)
  {
    int h = draw_button("  OK  ", x + w / 2 - 3, y + 16, 6 * 8 + 14, 23, ENTER, ESC);

    save_bmp("SHOT3.BMP");
    disable_button(h);
    close_window(wh);
    return;
  }
#endif
  one_button("  OK  ", x + w / 2 - 3, y + 16, ENTER, ESC);
  close_window(wh);
}

/*--------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
  int k, a = 1;
  const char *bin = NULL, *bout = NULL;

  free_at_start = dos_free_kb();

  /* XYZ2MES [mode] [/B points.xyz grid.mes]
   * mode: a video mode in hex (800 x 600 if the card has it, else 640 x 480)
   * /B:   open and grid without dialogs, then exit (for batch files) */
  if (argc > a && argv[a][0] != '/')
  {
    if (set_video_mode((int) strtol(argv[a++], NULL, 16)) < 0)
    {
      printf("The video card will not set mode %s.\n", argv[1]);
      return 1;
    }
  }
  else if (set_video_mode(MODE_800x600x16) < 0 && set_video_mode(MODE_640x480x16) < 0)
  {
    printf("XYZ2MES needs a VGA card.\n");
    return 1;
  }

  if (argc > a + 2 && (argv[a][1] == 'B' || argv[a][1] == 'b'))
  {
    bin  = argv[a + 1];
    bout = argv[a + 2];
  }

  initdisplay();
  show_mouse_cursor();

  hide_mouse_cursor();
  gfillrect(0, 0, gr_scr_xsize - 1, gr_scr_ysize - 1, 8);
  show_mouse_cursor();

  px = (txt_scr_xsize - PW) / 2;
  py = (txt_scr_ysize - PH) / 2;

  draw_panel();
  alt_control(INIT, 0);
  draw_buttons();

  if (bin)
  {
    batch = 1;
    batch_rc = 1;
    strcpy(batch_text, "not gridded");
    do_open(bin);
#ifdef SHOTS
    if (loaded) save_bmp("SHOT1.BMP");
#endif
    if (loaded) do_grid(bout);
#ifdef SHOTS
    do_about();
#endif
    xyz_unload();
    set_video_mode(MODE_TEXT);
    printf("%s\n", batch_text);
    printf("Memory: program %u KB, DiskArray cache %d KB, DOS free at start %u KB\n",
           program_kb(), loaded ? info.cache_pages * 4 : 0, free_at_start);
    return batch_rc;
  }

  for (;;)
  {
    k = check_button(getkey());
    gcurs(MOVE);

    /* the buttons' letters work without Alt too */
    switch (k)
    {
      case 'o': case 'O': k = ID_OPEN;  break;
      case 's': case 'S': k = ID_SET;   break;
      case 'g': case 'G': k = ID_GRID;  break;
      case 'a': case 'A': k = ID_ABOUT; break;
      case 'x': case 'X': k = ID_EXIT;  break;
    }

    switch (k)
    {
      case ID_OPEN:  do_open(NULL); break;
      case ID_SET:   do_settings(); break;
      case ID_GRID:  do_grid(NULL); break;
      case ID_ABOUT: do_about();    break;
      case ID_EXIT:
      case ESC:
        xyz_unload();
        set_video_mode(MODE_TEXT);
        return 0;
    }
  }
}
