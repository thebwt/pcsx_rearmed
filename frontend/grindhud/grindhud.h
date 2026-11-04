/* grindhud: read-only RAM-driven side-panel HUD for emulator cores.
 *
 * Portable C99, no libretro dependency. A core supplies a memory reader and a
 * pixel surface; the engine loads a per-game definition (.ghud), evaluates it
 * once per frame, and draws text panels into the pillarbox columns.
 *
 * The engine NEVER writes emulated memory.
 *
 * Definition format: see frontend/grindhud/README.md
 */
#ifndef GRINDHUD_H
#define GRINDHUD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ghud ghud_t;

/* Read `size` (1, 2 or 4) bytes little-endian at emulated address `addr`.
 * Must return 0 for unmapped addresses; must not have side effects. */
typedef uint32_t (*ghud_read_fn)(void *user, uint32_t addr, int size);

typedef struct ghud_surface {
   void *pixels;
   int pitch;     /* bytes per row */
   int bpp;       /* 2 = RGB565, 4 = XRGB8888 */
   int width;
   int height;
} ghud_surface_t;

enum { GHUD_LEFT = 0, GHUD_RIGHT = 1 };

/* Parse a definition. Returns NULL on error and writes a message to err. */
ghud_t *ghud_load_string(const char *text, char *err, size_t errlen);
ghud_t *ghud_load_file(const char *path, char *err, size_t errlen);
void ghud_free(ghud_t *g);

/* Normalise a disc serial for comparison: keep [A-Z0-9], uppercase. */
void ghud_normalize_serial(const char *in, char *out, size_t outlen);
/* Does definition text declare `serial` (already normalised)? */
int ghud_text_has_serial(const char *text, const char *norm_serial);

const char *ghud_game_name(const ghud_t *g);

void ghud_set_reader(ghud_t *g, ghud_read_fn fn, void *user);

/* Lines marked `meta` (encounter counters, enemy HP, RNG...) are only drawn
 * when this is on. They are still evaluated so counters stay correct. */
void ghud_set_show_meta(ghud_t *g, int show);

/* Advance one frame of emulated time (dt seconds) and re-evaluate.
 * Returns 1 if any panel text changed (caller should redraw panels). */
int ghud_tick(ghud_t *g, double dt);

/* Memory jumped (save state loaded, disc swap, reset): stateful counters
 * re-baseline on the next tick instead of counting the jump. */
void ghud_discontinuity(ghud_t *g);

/* Start a fresh session: zero all counters and timers. */
void ghud_reset_session(ghud_t *g);

/* Draw one side's panel into the rect (x, y, w, h) of surf.
 * fx/fy: integer glyph scale (cell is 8*fx by 8*fy, plus line gap). */
void ghud_render(const ghud_t *g, int side, const ghud_surface_t *surf,
      int x, int y, int w, int h, int fx, int fy);

/* Utility used by the core for status text ("no definition for ..."). */
void ghud_fill_rect(const ghud_surface_t *surf, int x, int y, int w, int h,
      uint32_t rgb);
void ghud_draw_text(const ghud_surface_t *surf, int x, int y, int maxw,
      const char *text, uint32_t rgb, int fx, int fy);

#ifdef __cplusplus
}
#endif

#endif
