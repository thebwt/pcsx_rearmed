/* Quick checks for the stateful rate functions. Build:
 *   cc -O2 -o test_rates test_rates.c ../grindhud.c && ./test_rates */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../grindhud.h"

static unsigned char ram[0x200000];

static uint32_t rd(void *u, uint32_t a, int size)
{
   uint32_t v = 0;
   int i;
   (void)u;
   a &= 0x1FFFFF;
   for (i = 0; i < size; i++)
      v |= (uint32_t)ram[a + i] << (8 * i);
   return v;
}

static void set32(uint32_t a, uint32_t v)
{
   memcpy(ram + a, &v, 4);
}

static const char *def =
   "let xp = u32(0x100)\n"
   "let g = gain(xp)\n"
   "right \"{g}|{perhour(g)}|{rate(g, 600)}\"\n";

int main(void)
{
   char err[200];
   ghud_t *h = ghud_load_string(def, err, sizeof(err));
   unsigned frame, xp = 5000;
   int fails = 0;
   if (!h) { printf("parse: %s\n", err); return 1; }
   ghud_set_reader(h, rd, NULL);
   set32(0x100, xp);
   /* 120 s idle (title, walk-in), then +100 every 60 s for 30 minutes,
    * then 10 minutes idle */
   for (frame = 0; frame < 60 * 60 * 40; frame++) {
      unsigned t = frame / 60;
      if (frame % 60 == 0 && t >= 120 && t < 120 + 1800 && (t - 120) % 60 == 0) {
         xp += 100;
         set32(0x100, xp);
      }
      ghud_tick(h, 1.0 / 60.0);
      if (frame % 60 == 0 && (t == 100 || t == 125 || t == 185 || t == 600 ||
                              t == 1900 || t == 2400)) {
         /* read the rendered line back through a tiny surface-free path */
         printf("t=%4us  gain|avg/h|10m/h = %s\n", t, ghud_line_text(h, 0, NULL, NULL));
      }
   }
   ghud_free(h);
   return fails;
}
