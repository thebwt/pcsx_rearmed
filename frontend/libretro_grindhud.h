/* Grind HUD glue for the PCSX ReARMed libretro core.
 *
 * When the "Grind HUD" core option is on, the core outputs a 16:9 frame: the
 * game in the middle at 4:3 (integer-scaled), and grindhud panels in the
 * pillarbox columns. With the option off, nothing here runs and the core's
 * output is unchanged.
 *
 * Read-only: emulated RAM is read, never written. */
#ifndef LIBRETRO_GRINDHUD_H
#define LIBRETRO_GRINDHUD_H

#include <stdbool.h>
#include <stddef.h>
#include <libretro.h>

void lrgh_set_log(retro_log_printf_t log);
void lrgh_deinit(void);

/* Re-read core options. Returns 1 if the on/off state or output size class
 * changed (caller must refresh its framebuffer setup and send
 * RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO if a game is running). */
int lrgh_update_options(retro_environment_t env);

int lrgh_active(void);

/* Rewrite geometry for the composed frame when active. */
void lrgh_geometry(unsigned game_w, unsigned game_h, struct retro_game_geometry *geom);

/* Compose and present one frame. `game` may be stale (game_dirty == 0). */
void lrgh_present(retro_video_refresh_t video_cb, const void *game,
      unsigned w, unsigned h, size_t pitch, int bytes_pp, int game_dirty,
      bool can_dupe, double fps);

/* Memory jumped (state load, reset): don't count it as progress. */
void lrgh_discontinuity(void);

#endif
