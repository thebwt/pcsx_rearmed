/* Grind HUD glue for the PCSX ReARMed libretro core. See libretro_grindhud.h. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif

#include "libretro_grindhud.h"
#include "grindhud/grindhud.h"
#include "grindhud/builtin_defs.h"
#include "../libpcsxcore/misc.h"
#include "../libpcsxcore/r3000a.h"

#ifdef _WIN32
#define GH_SLASH '\\'
#else
#define GH_SLASH '/'
#endif

/* limits of the composed frame (reported as max geometry when active) */
#define GH_MAX_W 1712
#define GH_MAX_H 960

static retro_log_printf_t gh_log;

static int opt_enabled;
static int opt_size;      /* 0 small, 1 medium, 2 large */
static int opt_meta;

static ghud_t *gh;
static char gh_serial[16];          /* serial the current lookup was for */
static char gh_path[1024];          /* definition in use ("" = none) */
static time_t gh_mtime;
static char gh_status[4][64];       /* shown when there is no definition */
static unsigned gh_frame;
static char gh_sysdir[1024];

static uint8_t *buf;
static size_t buf_size;
static struct lay {
   int w, h, bpp;                   /* game frame */
   int s, fx, fy;                   /* game scale, glyph scale */
   int out_w, out_h, pad;
} L;
static int panels_dirty = 1;

/* ------------------------------------------------------------------ */

void lrgh_set_log(retro_log_printf_t log) { gh_log = log; }

#define LOG(lvl, ...) do { if (gh_log) gh_log(lvl, "[grindhud] " __VA_ARGS__); } while (0)

static uint32_t gh_read(void *user, uint32_t addr, int size)
{
   uint32_t a = addr & 0x1FFFFFFF, off, lim, v = 0;
   const uint8_t *base;
   int i;
   (void)user;
   if (a < 0x800000) {                        /* main RAM and its mirrors */
      base = psxRegs.ptrs.psxM;
      off = a & 0x1FFFFF;
      lim = 0x200000;
   }
   else if (a >= 0x1F800000 && a < 0x1F800400) { /* scratchpad */
      base = psxRegs.ptrs.psxH;
      off = a - 0x1F800000;
      lim = 0x400;
   }
   else
      return 0;
   if (!base || off + (uint32_t)size > lim)
      return 0;
   for (i = 0; i < size; i++)
      v |= (uint32_t)base[off + i] << (8 * i);
   return v;
}

static void set_status(const char *a, const char *b, const char *c, const char *d)
{
   snprintf(gh_status[0], sizeof(gh_status[0]), "%s", a ? a : "");
   snprintf(gh_status[1], sizeof(gh_status[1]), "%s", b ? b : "");
   snprintf(gh_status[2], sizeof(gh_status[2]), "%s", c ? c : "");
   snprintf(gh_status[3], sizeof(gh_status[3]), "%s", d ? d : "");
   panels_dirty = 1;
}

static char *read_text(const char *path)
{
   FILE *f = fopen(path, "rb");
   long sz;
   char *t;
   if (!f) return NULL;
   fseek(f, 0, SEEK_END);
   sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz < 0 || sz > 1024 * 1024) { fclose(f); return NULL; }
   t = (char *)malloc((size_t)sz + 1);
   if (t && fread(t, 1, (size_t)sz, f) != (size_t)sz) { free(t); t = NULL; }
   if (t) t[sz] = 0;
   fclose(f);
   return t;
}

/* <system>/grindhud/psx/<SERIAL>.ghud, else any *.ghud declaring the serial */
static int find_in_dir(const char *dir, const char *serial, char *out, size_t outlen)
{
   struct stat st;
   snprintf(out, outlen, "%s%c%s.ghud", dir, GH_SLASH, serial);
   if (stat(out, &st) == 0)
      return 1;
#if !defined(_WIN32)
   {
      DIR *d = opendir(dir);
      struct dirent *e;
      int found = 0;
      if (!d) return 0;
      while (!found && (e = readdir(d))) {
         size_t n = strlen(e->d_name);
         char *t;
         if (n < 6 || strcmp(e->d_name + n - 5, ".ghud") != 0)
            continue;
         snprintf(out, outlen, "%s%c%s", dir, GH_SLASH, e->d_name);
         t = read_text(out);
         if (t && ghud_text_has_serial(t, serial))
            found = 1;
         free(t);
      }
      closedir(d);
      return found;
   }
#else
   return 0;
#endif
}

/* Search order: a shared folder anyone can write to (on Android the
 * frontend's private system dir is unreachable without root), then the
 * frontend's system dir. Built-in definitions are the last resort. */
static int find_definition(const char *serial, char *out, size_t outlen)
{
   char dir[1100];
#if defined(ANDROID) || defined(__ANDROID__)
   static const char *shared[] = {
      "/storage/emulated/0/GrindHUD/psx",
      "/sdcard/GrindHUD/psx",
      NULL
   };
   int i;
   for (i = 0; shared[i]; i++)
      if (find_in_dir(shared[i], serial, out, outlen))
         return 1;
#endif
   snprintf(dir, sizeof(dir), "%s%cgrindhud%cpsx", gh_sysdir, GH_SLASH, GH_SLASH);
   return find_in_dir(dir, serial, out, outlen);
}

static const ghud_builtin_t *find_builtin(const char *serial)
{
   const ghud_builtin_t *b;
   for (b = ghud_builtins; b->text; b++)
      if (strcmp(b->platform, "psx") == 0 && ghud_text_has_serial(b->text, serial))
         return b;
   return NULL;
}

static void load_builtin(const ghud_builtin_t *b)
{
   char err[200];
   ghud_t *ng = ghud_load_string(b->text, err, sizeof(err));
   if (!ng) {
      LOG(RETRO_LOG_ERROR, "built-in %s: %s\n", b->name, err);
      set_status("GRIND HUD", "Built-in def", "error:", err);
      return;
   }
   if (gh) ghud_free(gh);
   gh = ng;
   ghud_set_reader(gh, gh_read, NULL);
   ghud_set_show_meta(gh, opt_meta);
   snprintf(gh_path, sizeof(gh_path), "builtin:%s", b->name);
   gh_mtime = 0;
   panels_dirty = 1;
   LOG(RETRO_LOG_INFO, "using built-in %s (%s)\n", b->name, ghud_game_name(gh));
}

static int is_builtin_path(void) { return strncmp(gh_path, "builtin:", 8) == 0; }

static void load_definition(const char *path)
{
   char err[200];
   struct stat st;
   ghud_t *ng = ghud_load_file(path, err, sizeof(err));
   if (!ng) {
      LOG(RETRO_LOG_WARN, "%s: %s\n", path, err);
      if (gh) { ghud_free(gh); gh = NULL; }
      set_status("GRIND HUD", "Definition", "error:", err);
      gh_path[0] = 0;
      return;
   }
   if (gh) ghud_free(gh);
   gh = ng;
   ghud_set_reader(gh, gh_read, NULL);
   ghud_set_show_meta(gh, opt_meta);
   snprintf(gh_path, sizeof(gh_path), "%s", path);
   gh_mtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
   panels_dirty = 1;
   LOG(RETRO_LOG_INFO, "loaded %s (%s)\n", path, ghud_game_name(gh));
}

/* Called every frame while active; cheap except every ~2 s. */
static void refresh_definition(void)
{
   char serial[16], path[1024];
   struct stat st;
   ghud_normalize_serial(CdromId, serial, sizeof(serial));
   if (strcmp(serial, gh_serial) != 0) {
      /* disc swap or first frame: keep the session if the same file covers it */
      snprintf(gh_serial, sizeof(gh_serial), "%s", serial);
      if (!serial[0]) {
         set_status("GRIND HUD", "No disc", "serial yet", NULL);
         return;
      }
      if (find_definition(serial, path, sizeof(path))) {
         if (gh && strcmp(path, gh_path) == 0)
            ghud_discontinuity(gh);
         else
            load_definition(path);
      }
      else if (find_builtin(serial)) {
         const ghud_builtin_t *b = find_builtin(serial);
         char bp[1024];
         snprintf(bp, sizeof(bp), "builtin:%s", b->name);
         if (gh && strcmp(bp, gh_path) == 0)
            ghud_discontinuity(gh);   /* disc swap within the same game */
         else
            load_builtin(b);
      }
      else {
         if (gh) { ghud_free(gh); gh = NULL; }
         gh_path[0] = 0;
         set_status("GRIND HUD", "No file for", serial, "grindhud/psx/");
         LOG(RETRO_LOG_INFO, "no definition for %s under %s/grindhud/psx\n", serial, gh_sysdir);
      }
      return;
   }
   if (gh_frame % 120 != 0)
      return;
   if (gh_path[0] && is_builtin_path()) {
      /* a file dropped into system/grindhud/psx overrides the built-in */
      if (gh_frame % 300 == 0 && find_definition(serial, path, sizeof(path)))
         load_definition(path);
   }
   else if (gh_path[0]) {
      /* hot reload while editing */
      if (stat(gh_path, &st) == 0 && st.st_mtime != gh_mtime)
         load_definition(gh_path);
   }
   else if (serial[0] && gh_frame % 300 == 0 && find_definition(serial, path, sizeof(path)))
      load_definition(path);
}

/* ------------------------------------------------------------------ */

static void compute_layout(int w, int h, struct lay *l)
{
   static const double target[3] = { 1.0 / 60, 1.0 / 45, 1.0 / 30 };
   double best = 1e9, t = target[opt_size < 0 || opt_size > 2 ? 0 : opt_size];
   int s, f;
   memset(l, 0, sizeof(*l));
   l->w = w;
   l->h = h;
   l->s = 1;
   l->fy = 1;
   for (s = 1; s <= 4; s++) {
      if (h * s > GH_MAX_H || (w * s * 4) / 3 > GH_MAX_W)
         break;
      for (f = 1; f <= 4; f++) {
         double err = fabs(8.0 * f / (h * s) - t) / t + 0.05 * (s - 1);
         if (err < best) {
            best = err;
            l->s = s;
            l->fy = f;
         }
      }
   }
   {
      /* source pixels aren't square unless w:h is 4:3; keep glyphs upright */
      double par = (4.0 / 3.0) * h / (double)w;
      l->fx = (int)lround(l->fy / par);
      if (l->fx < 1) l->fx = 1;
   }
   l->pad = (w * l->s + 3) / 6;
   l->out_w = w * l->s + 2 * l->pad;
   l->out_h = h * l->s;
}

int lrgh_active(void) { return opt_enabled; }

void lrgh_geometry(unsigned game_w, unsigned game_h, struct retro_game_geometry *geom)
{
   struct lay l;
   if (!opt_enabled || !game_w || !game_h)
      return;
   compute_layout((int)game_w, (int)game_h, &l);
   geom->base_width = l.out_w;
   geom->base_height = l.out_h;
   if (geom->max_width < GH_MAX_W) geom->max_width = GH_MAX_W;
   if (geom->max_height < GH_MAX_H) geom->max_height = GH_MAX_H;
   geom->aspect_ratio = 16.0f / 9.0f;
}

int lrgh_update_options(retro_environment_t env)
{
   struct retro_variable var;
   const char *sys = NULL;
   int was_enabled = opt_enabled, was_size = opt_size, was_meta = opt_meta;

   var.key = "pcsx_rearmed_grindhud";
   var.value = NULL;
   opt_enabled = env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
         strcmp(var.value, "enabled") == 0;

   var.key = "pcsx_rearmed_grindhud_size";
   var.value = NULL;
   opt_size = 0;
   if (env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "medium") == 0) opt_size = 1;
      else if (strcmp(var.value, "large") == 0) opt_size = 2;
   }

   var.key = "pcsx_rearmed_grindhud_meta";
   var.value = NULL;
   opt_meta = env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
         strcmp(var.value, "enabled") == 0;

   if (!gh_sysdir[0] && env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys) && sys)
      snprintf(gh_sysdir, sizeof(gh_sysdir), "%s", sys);

   if (gh && opt_meta != was_meta) {
      ghud_set_show_meta(gh, opt_meta);
      panels_dirty = 1;
   }
   if (opt_enabled != was_enabled || opt_size != was_size) {
      L.out_w = 0;          /* force re-layout */
      return 1;
   }
   return 0;
}

static void blit_scaled(const uint8_t *src, size_t pitch, const struct lay *l)
{
   int y, x, k, bpp = l->bpp, s = l->s;
   size_t out_pitch = (size_t)l->out_w * bpp;
   for (y = 0; y < l->h; y++) {
      const uint8_t *row = src + (size_t)y * pitch;
      uint8_t *dst = buf + (size_t)(y * s) * out_pitch + (size_t)l->pad * bpp;
      if (s == 1)
         memcpy(dst, row, (size_t)l->w * bpp);
      else if (bpp == 2) {
         const uint16_t *r = (const uint16_t *)row;
         uint16_t *d = (uint16_t *)dst;
         for (x = 0; x < l->w; x++)
            for (k = 0; k < s; k++)
               *d++ = r[x];
      }
      else {
         const uint32_t *r = (const uint32_t *)row;
         uint32_t *d = (uint32_t *)dst;
         for (x = 0; x < l->w; x++)
            for (k = 0; k < s; k++)
               *d++ = r[x];
      }
      for (k = 1; k < s; k++)
         memcpy(dst + (size_t)k * out_pitch, dst, (size_t)l->w * s * bpp);
   }
}

static void draw_panels(void)
{
   ghud_surface_t surf;
   surf.pixels = buf;
   surf.pitch = L.out_w * L.bpp;
   surf.bpp = L.bpp;
   surf.width = L.out_w;
   surf.height = L.out_h;
   ghud_fill_rect(&surf, 0, 0, L.pad, L.out_h, 0x000000);
   ghud_fill_rect(&surf, L.out_w - L.pad, 0, L.pad, L.out_h, 0x000000);
   if (gh) {
      ghud_render(gh, GHUD_LEFT, &surf, 0, 0, L.pad, L.out_h, L.fx, L.fy);
      ghud_render(gh, GHUD_RIGHT, &surf, L.out_w - L.pad, 0, L.pad, L.out_h, L.fx, L.fy);
   }
   else {
      int i, lh = 10 * L.fy;
      for (i = 0; i < 4; i++)
         ghud_draw_text(&surf, L.out_w - L.pad + 4 * L.fx, 4 * L.fy + i * lh,
               L.pad - 8 * L.fx, gh_status[i], i ? 0xA0A0A0 : 0x60D0F0, L.fx, L.fy);
   }
}

void lrgh_present(retro_video_refresh_t video_cb, const void *game,
      unsigned w, unsigned h, size_t pitch, int bytes_pp, int game_dirty,
      bool can_dupe, double fps)
{
   int relayout;
   if (!video_cb)
      return;
   gh_frame++;
   refresh_definition();
   if (gh && ghud_tick(gh, fps > 1.0 ? 1.0 / fps : 1.0 / 60.0))
      panels_dirty = 1;

   relayout = (L.out_w == 0 || (int)w != L.w || (int)h != L.h || bytes_pp != L.bpp);
   if (relayout) {
      size_t need;
      compute_layout((int)w, (int)h, &L);
      L.bpp = bytes_pp;
      need = (size_t)L.out_w * L.out_h * L.bpp;
      if (need > buf_size) {
         uint8_t *nb = (uint8_t *)realloc(buf, need);
         if (!nb) {
            video_cb(game, w, h, pitch);   /* give up gracefully: plain frame */
            return;
         }
         buf = nb;
         buf_size = need;
      }
      memset(buf, 0, need);
      game_dirty = 1;
      panels_dirty = 1;
   }

   if (game_dirty && game)
      blit_scaled((const uint8_t *)game, pitch, &L);
   if (panels_dirty)
      draw_panels();

   if (!game_dirty && !panels_dirty && can_dupe)
      video_cb(NULL, L.out_w, L.out_h, (size_t)L.out_w * L.bpp);
   else
      video_cb(buf, L.out_w, L.out_h, (size_t)L.out_w * L.bpp);
   panels_dirty = 0;
}

void lrgh_discontinuity(void)
{
   if (gh)
      ghud_discontinuity(gh);
}

void lrgh_deinit(void)
{
   if (gh) ghud_free(gh);
   gh = NULL;
   free(buf);
   buf = NULL;
   buf_size = 0;
   memset(&L, 0, sizeof(L));
   gh_serial[0] = 0;
   gh_path[0] = 0;
   gh_sysdir[0] = 0;
   opt_enabled = 0;
}
