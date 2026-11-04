/* ghud_preview: check a .ghud definition against a 2 MB PS1 RAM dump.
 *
 *   cc -O2 -o ghud_preview ghud_preview.c ../grindhud.c
 *   ./ghud_preview game.ghud ram.bin [out.ppm [frame.ppm]]
 *
 * Prints every visible line and, with out.ppm, renders both panels at the
 * default scale so you can see layout without running an emulator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../grindhud.h"

static unsigned char *ram;
static size_t ram_size;

static uint32_t rd(void *user, uint32_t addr, int size)
{
   uint32_t a = addr & 0x1FFFFF, v = 0;
   int i;
   (void)user;
   if ((addr & 0x1FFFFFFF) >= 0x800000 || a + size > ram_size)
      return 0;
   for (i = 0; i < size; i++)
      v |= (uint32_t)ram[a + i] << (8 * i);
   return v;
}

int main(int argc, char **argv)
{
   char err[256];
   ghud_t *g;
   FILE *f;
   int i;
   if (argc < 3) {
      fprintf(stderr, "usage: %s def.ghud ram.bin [out.ppm]\n", argv[0]);
      return 2;
   }
   g = ghud_load_file(argv[1], err, sizeof(err));
   if (!g) {
      fprintf(stderr, "%s: %s\n", argv[1], err);
      return 1;
   }
   f = fopen(argv[2], "rb");
   if (!f) { perror(argv[2]); return 1; }
   ram = malloc(0x200000);
   ram_size = fread(ram, 1, 0x200000, f);
   fclose(f);
   ghud_set_reader(g, rd, NULL);
   ghud_set_show_meta(g, 1);
   for (i = 0; i < 600; i++)          /* 10 s of game time */
      ghud_tick(g, 1.0 / 60.0);
   printf("game: %s\n", ghud_game_name(g));
   if (argc > 3) {
      /* game at 2x in the middle of a 16:9 frame (default 320x240) */
      int gw = 320, gh = 240, sc = 2, W, H, pw, x, y;
      unsigned char *game = NULL;
      ghud_surface_t s;
      unsigned char *px;
      if (argc > 4) {   /* optional game frame (binary PPM) */
         FILE *gf = fopen(argv[4], "rb");
         int maxv;
         if (gf && fscanf(gf, "P6 %d %d %d", &gw, &gh, &maxv) == 3) {
            fgetc(gf);
            game = malloc((size_t)gw * gh * 3);
            if (fread(game, 3, (size_t)gw * gh, gf) != (size_t)gw * gh) { free(game); game = NULL; }
         }
         if (gf) fclose(gf);
      }
      H = gh * sc;
      pw = gw * sc / 6;
      W = gw * sc + 2 * pw;
      px = calloc((size_t)W * H, 4);
      s.pixels = px; s.pitch = W * 4; s.bpp = 4; s.width = W; s.height = H;
      if (game) {
         for (y = 0; y < H; y++)
            for (x = 0; x < gw * sc; x++) {
               unsigned char *src = game + ((size_t)(y / sc) * gw + x / sc) * 3;
               unsigned char *d = px + ((size_t)y * W + pw + x) * 4;
               d[0] = src[2]; d[1] = src[1]; d[2] = src[0];
            }
         free(game);
      }
      else
         ghud_fill_rect(&s, pw, 0, gw * sc, H, 0x202830);
      ghud_render(g, GHUD_LEFT, &s, 0, 0, pw, H, 1, 1);
      ghud_render(g, GHUD_RIGHT, &s, W - pw, 0, pw, H, 1, 1);
      f = fopen(argv[3], "wb");
      fprintf(f, "P6\n%d %d\n255\n", W, H);
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++) {
            unsigned char *p = px + ((size_t)y * W + x) * 4;
            unsigned char rgb[3] = { p[2], p[1], p[0] };
            fwrite(rgb, 1, 3, f);
         }
      fclose(f);
      free(px);
   }
   ghud_free(g);
   free(ram);
   return 0;
}
