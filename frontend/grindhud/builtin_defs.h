/* Definitions compiled into the core (see tools/gen_builtin.py). */
#ifndef GRINDHUD_BUILTIN_DEFS_H
#define GRINDHUD_BUILTIN_DEFS_H

typedef struct {
   const char *platform;   /* "psx" */
   const char *name;       /* file name it came from */
   const char *text;       /* definition source */
} ghud_builtin_t;

extern const ghud_builtin_t ghud_builtins[];   /* NULL-terminated */

#endif
