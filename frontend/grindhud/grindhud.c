/* grindhud engine. See grindhud.h. */
#include "grindhud.h"
#include "font8x8.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GH_MAX_LETS     256
#define GH_MAX_TABLES   32
#define GH_MAX_LINES    64
#define GH_MAX_SERIALS  16
#define GH_LINE_CHARS   96
#define GH_MAX_ARGS     4
#define GH_MAX_TABLE_ITEMS 4096
#define GH_MAX_SRC_LINE 4096
static char *gh_strdup(const char *s)
{
   size_t n = strlen(s) + 1;
   char *d = (char *)malloc(n);
   if (d) memcpy(d, s, n);
   return d;
}

#define GH_BAR_MARK     '\x01'   /* followed by one byte: 0..200 = fill/200 */

/* ------------------------------------------------------------------ */
/* values                                                              */

typedef struct {
   int is_str;
   int64_t i;
   const char *s;
} gh_val;

static gh_val vint(int64_t i) { gh_val v; v.is_str = 0; v.i = i; v.s = NULL; return v; }
static gh_val vstr(const char *s) { gh_val v; v.is_str = 1; v.i = 0; v.s = s; return v; }
static int64_t asint(gh_val v) { return v.is_str ? 0 : v.i; }

/* ------------------------------------------------------------------ */
/* AST                                                                 */

enum {
   N_NUM, N_LET, N_UN, N_BIN, N_TERN, N_CALL
};

enum {
   F_U8, F_U16, F_U32, F_S8, F_S16, F_S32,
   F_TBL, F_MIN, F_MAX, F_ABS, F_CLAMP, F_BIT, F_PCT,
   F_SESSION, F_GAIN, F_LOSS, F_COUNT, F_SINCE, F_PERHOUR, F_BAR,
   F_ARR, F_HOLD, F_RATE
};

static const struct { const char *name; int id; int minargs, maxargs; int stateful; } funcs[] = {
   { "u8",      F_U8,      1, 1, 0 },
   { "u16",     F_U16,     1, 1, 0 },
   { "u32",     F_U32,     1, 1, 0 },
   { "s8",      F_S8,      1, 1, 0 },
   { "s16",     F_S16,     1, 1, 0 },
   { "s32",     F_S32,     1, 1, 0 },
   { "tbl",     F_TBL,     2, 2, 0 },
   { "min",     F_MIN,     2, 2, 0 },
   { "max",     F_MAX,     2, 2, 0 },
   { "abs",     F_ABS,     1, 1, 0 },
   { "clamp",   F_CLAMP,   3, 3, 0 },
   { "bit",     F_BIT,     2, 2, 0 },
   { "pct",     F_PCT,     2, 2, 0 },
   { "session", F_SESSION, 0, 0, 0 },
   { "gain",    F_GAIN,    1, 2, 1 },
   { "loss",    F_LOSS,    1, 2, 1 },
   { "count",   F_COUNT,   1, 1, 1 },
   { "since",   F_SINCE,   1, 1, 1 },
   { "perhour", F_PERHOUR, 1, 1, 1 },
   { "bar",     F_BAR,     2, 2, 0 },
   { "arr",     F_ARR,     2, 2, 0 },
   { "hold",    F_HOLD,    2, 2, 1 },
   { "rate",    F_RATE,    2, 2, 1 },
   { NULL, 0, 0, 0, 0 }
};

typedef struct gh_node gh_node;
struct gh_node {
   int kind;
   int op;              /* operator char/code or function id */
   int64_t num;         /* N_NUM, N_LET index, table index for tbl/arr */
   int nargs;
   gh_node *a[GH_MAX_ARGS];
   /* state for stateful functions */
   int64_t prev, acc;
   double last_t;
   int has_prev;
   unsigned epoch;
   /* perhour(): clock starts at the first change */
   double first_t;
   int64_t first_v;
   int has_first;
   /* rate(): history of (time, value) at each change */
   double *ev_t;
   int64_t *ev_v;
   int ev_n, ev_cap;
   gh_node *next_alloc;
};

/* binary operator codes beyond single chars */
enum { OP_OR = 256, OP_AND, OP_EQ, OP_NE, OP_LE, OP_GE, OP_SHL, OP_SHR };

typedef struct {
   char *name;
   int n, cap;
   int numeric;
   char **items;     /* every entry as text (numbers too) */
   int64_t *nums;    /* numeric value, 0 for words */
} gh_table;

typedef struct {
   int is_lit;          /* literal text segment */
   char *text;
   gh_node *expr;
   int width;
   char fmt;            /* 'd','x','t','k','s' */
} gh_seg;

typedef struct {
   int side;
   uint32_t color;
   gh_node *when;
   int nseg;
   gh_seg *segs;
   char out[GH_LINE_CHARS];
   int visible;
   int meta;            /* spoiler-ish info, hidden unless show_meta */
} gh_line;

struct ghud {
   char game[64];
   char serials[GH_MAX_SERIALS][16];
   int nserials;

   char *let_names[GH_MAX_LETS];
   gh_node *let_expr[GH_MAX_LETS];
   gh_val let_val[GH_MAX_LETS];
   int nlets;

   gh_table tables[GH_MAX_TABLES];
   int ntables;

   gh_line lines[GH_MAX_LINES];
   int nlines;

   gh_node *valid;
   int show_meta;

   ghud_read_fn read;
   void *read_user;

   double t_session;     /* seconds of valid game time */
   unsigned epoch;
   int frozen;           /* current tick is not valid */

   gh_node *allocs;
};

/* ------------------------------------------------------------------ */
/* tokenizer                                                           */

enum { T_EOF, T_NUM, T_IDENT, T_STR, T_OP };

typedef struct {
   const char *p;
   int type;
   int64_t num;
   char text[256];
   int op;
   int lineno;
   char *err;
   size_t errlen;
   int failed;
} gh_lex;

static void lex_error(gh_lex *lx, const char *msg)
{
   if (!lx->failed)
      snprintf(lx->err, lx->errlen, "line %d: %s", lx->lineno, msg);
   lx->failed = 1;
}

static void lex_next(gh_lex *lx)
{
   const char *p = lx->p;
   while (*p == ' ' || *p == '\t' || *p == '\r')
      p++;
   if (*p == '\0' || *p == '\n' || *p == '#') {
      lx->type = T_EOF;
      lx->p = p;
      return;
   }
   if (isdigit((unsigned char)*p)) {
      char *end;
      /* decimal, or hex with 0x; never octal (0123 is 123) */
      if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
         lx->num = (int64_t)strtoull(p + 2, &end, 16);
      else
         lx->num = (int64_t)strtoll(p, &end, 10);
      lx->type = T_NUM;
      lx->p = end;
      return;
   }
   if (isalpha((unsigned char)*p) || *p == '_') {
      int n = 0;
      while ((isalnum((unsigned char)*p) || *p == '_') && n < 255)
         lx->text[n++] = *p++;
      lx->text[n] = 0;
      lx->type = T_IDENT;
      lx->p = p;
      return;
   }
   if (*p == '"') {
      int n = 0;
      p++;
      while (*p && *p != '"' && *p != '\n') {
         if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
            p++;
         if (n < 255)
            lx->text[n++] = *p;
         p++;
      }
      if (*p != '"') {
         lex_error(lx, "unterminated string");
         lx->type = T_EOF;
         lx->p = p;
         return;
      }
      lx->text[n] = 0;
      lx->type = T_STR;
      lx->p = p + 1;
      return;
   }
   lx->type = T_OP;
#define TWO(a, b, code) if (p[0] == a && p[1] == b) { lx->op = code; lx->p = p + 2; return; }
   TWO('|', '|', OP_OR);
   TWO('&', '&', OP_AND);
   TWO('=', '=', OP_EQ);
   TWO('!', '=', OP_NE);
   TWO('<', '=', OP_LE);
   TWO('>', '=', OP_GE);
   TWO('<', '<', OP_SHL);
   TWO('>', '>', OP_SHR);
#undef TWO
   if (strchr("+-*/%&|^!~<>?:(),=", *p)) {
      lx->op = *p;
      lx->p = p + 1;
      return;
   }
   lex_error(lx, "unexpected character");
   lx->type = T_EOF;
}

/* ------------------------------------------------------------------ */
/* parser                                                              */

typedef struct {
   ghud_t *g;
   gh_lex *lx;
} gh_parser;

static gh_node *new_node(gh_parser *ps, int kind)
{
   gh_node *n = (gh_node *)calloc(1, sizeof(*n));
   if (!n) {
      lex_error(ps->lx, "out of memory");
      return NULL;
   }
   n->kind = kind;
   n->next_alloc = ps->g->allocs;
   ps->g->allocs = n;
   return n;
}

static int find_let(const ghud_t *g, const char *name)
{
   int i;
   for (i = 0; i < g->nlets; i++)
      if (strcmp(g->let_names[i], name) == 0)
         return i;
   return -1;
}

static int find_table(const ghud_t *g, const char *name)
{
   int i;
   for (i = 0; i < g->ntables; i++)
      if (strcmp(g->tables[i].name, name) == 0)
         return i;
   return -1;
}

static gh_node *parse_expr(gh_parser *ps);

static int is_op(gh_lex *lx, int op) { return lx->type == T_OP && lx->op == op; }

static gh_node *parse_primary(gh_parser *ps)
{
   gh_lex *lx = ps->lx;
   gh_node *n;
   if (lx->failed)
      return NULL;
   if (lx->type == T_NUM) {
      n = new_node(ps, N_NUM);
      if (n) n->num = lx->num;
      lex_next(lx);
      return n;
   }
   if (is_op(lx, '(')) {
      lex_next(lx);
      n = parse_expr(ps);
      if (!is_op(lx, ')'))
         lex_error(lx, "expected ')'");
      lex_next(lx);
      return n;
   }
   if (lx->type == T_IDENT) {
      char name[256];
      strcpy(name, lx->text);
      lex_next(lx);
      if (is_op(lx, '(')) {
         int f, nargs = 0;
         for (f = 0; funcs[f].name; f++)
            if (strcmp(funcs[f].name, name) == 0)
               break;
         if (!funcs[f].name) {
            char m[300];
            snprintf(m, sizeof(m), "unknown function '%s'", name);
            lex_error(lx, m);
            return NULL;
         }
         n = new_node(ps, N_CALL);
         if (!n) return NULL;
         n->op = funcs[f].id;
         lex_next(lx);
         if (n->op == F_TBL || n->op == F_ARR) {
            /* first argument names a table */
            int t;
            if (lx->type != T_IDENT || (t = find_table(ps->g, lx->text)) < 0) {
               lex_error(lx, "tbl()/arr() needs a table name as first argument");
               return NULL;
            }
            n->num = t;
            lex_next(lx);
            if (!is_op(lx, ',')) {
               lex_error(lx, "expected ','");
               return NULL;
            }
            lex_next(lx);
            n->a[0] = parse_expr(ps);
            n->nargs = 1;
         }
         else if (!is_op(lx, ')')) {
            for (;;) {
               if (nargs >= GH_MAX_ARGS) {
                  lex_error(lx, "too many arguments");
                  return NULL;
               }
               n->a[nargs++] = parse_expr(ps);
               if (lx->failed) return NULL;
               if (is_op(lx, ',')) { lex_next(lx); continue; }
               break;
            }
            n->nargs = nargs;
            if ((n->op != F_TBL && n->op != F_ARR) &&
                (nargs < funcs[f].minargs || nargs > funcs[f].maxargs)) {
               char m[300];
               snprintf(m, sizeof(m), "wrong number of arguments to %s()", name);
               lex_error(lx, m);
               return NULL;
            }
         }
         else if (funcs[f].minargs > 0) {
            char m[300];
            snprintf(m, sizeof(m), "%s() needs arguments", name);
            lex_error(lx, m);
            return NULL;
         }
         if (!is_op(lx, ')')) {
            lex_error(lx, "expected ')'");
            return NULL;
         }
         lex_next(lx);
         return n;
      }
      {
         int l = find_let(ps->g, name);
         if (l < 0) {
            char m[300];
            snprintf(m, sizeof(m), "unknown name '%s' (define it with let first)", name);
            lex_error(lx, m);
            return NULL;
         }
         n = new_node(ps, N_LET);
         if (n) n->num = l;
         return n;
      }
   }
   lex_error(lx, "expected a value");
   return NULL;
}

static gh_node *parse_unary(gh_parser *ps)
{
   gh_lex *lx = ps->lx;
   if (lx->type == T_OP && (lx->op == '-' || lx->op == '!' || lx->op == '~')) {
      int op = lx->op;
      gh_node *n;
      lex_next(lx);
      n = new_node(ps, N_UN);
      if (!n) return NULL;
      n->op = op;
      n->a[0] = parse_unary(ps);
      return n;
   }
   return parse_primary(ps);
}

static int prec(int op)
{
   switch (op) {
   case OP_OR: return 1;
   case OP_AND: return 2;
   case '|': return 3;
   case '^': return 4;
   case '&': return 5;
   case OP_EQ: case OP_NE: return 6;
   case '<': case '>': case OP_LE: case OP_GE: return 7;
   case OP_SHL: case OP_SHR: return 8;
   case '+': case '-': return 9;
   case '*': case '/': case '%': return 10;
   }
   return 0;
}

static gh_node *parse_binary(gh_parser *ps, int minprec)
{
   gh_lex *lx = ps->lx;
   gh_node *lhs = parse_unary(ps);
   while (!lx->failed && lx->type == T_OP && prec(lx->op) >= minprec && prec(lx->op) > 0) {
      int op = lx->op, p = prec(op);
      gh_node *n;
      lex_next(lx);
      n = new_node(ps, N_BIN);
      if (!n) return NULL;
      n->op = op;
      n->a[0] = lhs;
      n->a[1] = parse_binary(ps, p + 1);
      lhs = n;
   }
   return lhs;
}

static gh_node *parse_expr(gh_parser *ps)
{
   gh_lex *lx = ps->lx;
   gh_node *c = parse_binary(ps, 1);
   if (!lx->failed && is_op(lx, '?')) {
      gh_node *n = new_node(ps, N_TERN);
      if (!n) return NULL;
      lex_next(lx);
      n->a[0] = c;
      n->a[1] = parse_expr(ps);
      if (!is_op(lx, ':')) {
         lex_error(lx, "expected ':' in ?: expression");
         return NULL;
      }
      lex_next(lx);
      n->a[2] = parse_expr(ps);
      return n;
   }
   return c;
}

/* Parse a standalone expression from a string (used for template parts). */
static gh_node *parse_expr_str(gh_parser *ps, const char *s)
{
   gh_lex *outer = ps->lx;
   gh_lex sub;
   gh_node *n;
   memset(&sub, 0, sizeof(sub));
   sub.p = s;
   sub.err = outer->err;
   sub.errlen = outer->errlen;
   sub.lineno = outer->lineno;
   ps->lx = &sub;
   lex_next(&sub);
   n = parse_expr(ps);
   if (!sub.failed && sub.type != T_EOF)
      lex_error(&sub, "junk after expression in template");
   ps->lx = outer;
   if (sub.failed) {
      outer->failed = 1;
      return NULL;
   }
   return n;
}

static int is_fmt_spec(const char *s)
{
   /* [width][d|x|t|k|s] */
   while (isdigit((unsigned char)*s))
      s++;
   if (*s == 0)
      return 1;
   if (strchr("dxtks", *s) && s[1] == 0)
      return 1;
   return 0;
}

static int parse_template(gh_parser *ps, gh_line *ln, const char *t)
{
   gh_seg segs[32];
   int n = 0;
   char lit[GH_LINE_CHARS];
   int ll = 0;
   const char *p = t;

#define FLUSH_LIT() do { if (ll) { lit[ll] = 0; segs[n].is_lit = 1; segs[n].text = gh_strdup(lit); \
      segs[n].expr = NULL; n++; ll = 0; } } while (0)

   while (*p) {
      if (n >= 30) {
         lex_error(ps->lx, "template too long");
         return 0;
      }
      if (p[0] == '{' && p[1] == '{') { if (ll < GH_LINE_CHARS - 1) lit[ll++] = '{'; p += 2; continue; }
      if (p[0] == '}' && p[1] == '}') { if (ll < GH_LINE_CHARS - 1) lit[ll++] = '}'; p += 2; continue; }
      if (*p == '{') {
         const char *e = strchr(p, '}');
         char inner[256], *colon;
         size_t len;
         if (!e) {
            lex_error(ps->lx, "missing '}' in template");
            return 0;
         }
         len = (size_t)(e - p - 1);
         if (len >= sizeof(inner)) len = sizeof(inner) - 1;
         memcpy(inner, p + 1, len);
         inner[len] = 0;
         FLUSH_LIT();
         segs[n].is_lit = 0;
         segs[n].text = NULL;
         segs[n].width = 0;
         segs[n].fmt = 'd';
         colon = strrchr(inner, ':');
         if (colon && is_fmt_spec(colon + 1)) {
            char *f = colon + 1;
            *colon = 0;
            segs[n].width = atoi(f);
            while (isdigit((unsigned char)*f)) f++;
            if (*f) segs[n].fmt = *f;
         }
         segs[n].expr = parse_expr_str(ps, inner);
         if (!segs[n].expr)
            return 0;
         n++;
         p = e + 1;
         continue;
      }
      if (ll < GH_LINE_CHARS - 1)
         lit[ll++] = *p;
      p++;
   }
   FLUSH_LIT();
#undef FLUSH_LIT
   ln->segs = (gh_seg *)calloc(n ? n : 1, sizeof(gh_seg));
   if (!ln->segs)
      return 0;
   memcpy(ln->segs, segs, n * sizeof(gh_seg));
   ln->nseg = n;
   return 1;
}

static const struct { const char *name; uint32_t rgb; } colors[] = {
   { "white",   0xF0F0F0 }, { "gray",    0xA0A0A0 }, { "dim",     0x707070 },
   { "yellow",  0xF0D040 }, { "orange",  0xF09030 }, { "red",     0xF05050 },
   { "green",   0x60E060 }, { "cyan",    0x60D0F0 }, { "blue",    0x6090F0 },
   { "magenta", 0xE070E0 }, { "gold",    0xE0B848 }, { NULL, 0 }
};

static int parse_color(gh_lex *lx, uint32_t *out)
{
   int i;
   if (lx->type == T_IDENT) {
      for (i = 0; colors[i].name; i++)
         if (strcmp(colors[i].name, lx->text) == 0) {
            *out = colors[i].rgb;
            lex_next(lx);
            return 1;
         }
   }
   if (lx->type == T_NUM) {   /* 0xRRGGBB */
      *out = (uint32_t)lx->num & 0xFFFFFF;
      lex_next(lx);
      return 1;
   }
   lex_error(lx, "unknown color (white gray dim yellow orange red green cyan blue magenta gold or 0xRRGGBB)");
   return 0;
}

static int parse_statement(gh_parser *ps, const char *line)
{
   gh_lex *lx = ps->lx;
   ghud_t *g = ps->g;
   char kw[64];

   lx->p = line;
   lex_next(lx);
   if (lx->type == T_EOF)
      return 1;
   if (lx->type != T_IDENT) {
      lex_error(lx, "expected a keyword");
      return 0;
   }
   strncpy(kw, lx->text, sizeof(kw) - 1);
   kw[sizeof(kw) - 1] = 0;

   if (strcmp(kw, "grindhud") == 0) {
      return 1;   /* version marker, ignored for now */
   }
   if (strcmp(kw, "game") == 0) {
      const char *p = lx->p;
      size_t n;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '"') {
         lex_next(lx);
         if (lx->type != T_STR) return 0;
         snprintf(g->game, sizeof(g->game), "%s", lx->text);
      }
      else {
         snprintf(g->game, sizeof(g->game), "%s", p);
         n = strlen(g->game);
         while (n && (g->game[n - 1] == '\n' || g->game[n - 1] == '\r' || g->game[n - 1] == ' '))
            g->game[--n] = 0;
      }
      return 1;
   }
   if (strcmp(kw, "serial") == 0) {
      /* serials are bare words like SCUS-94491; split on whitespace */
      const char *p = lx->p;
      while (*p && *p != '\n' && *p != '#') {
         char raw[64];
         int n = 0;
         while (*p == ' ' || *p == '\t' || *p == ',') p++;
         while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '#' && n < 63)
            raw[n++] = *p++;
         raw[n] = 0;
         if (n && g->nserials < GH_MAX_SERIALS)
            ghud_normalize_serial(raw, g->serials[g->nserials++], 16);
      }
      return 1;
   }
   if (strcmp(kw, "table") == 0) {
      /* table NAME item item ...   (repeat the statement to append rows) */
      gh_table *t;
      int ti, numeric_line = 1;
      lex_next(lx);
      if (lx->type != T_IDENT) { lex_error(lx, "table needs a name"); return 0; }
      ti = find_table(g, lx->text);
      if (ti < 0) {
         if (g->ntables >= GH_MAX_TABLES) { lex_error(lx, "too many tables"); return 0; }
         ti = g->ntables++;
         t = &g->tables[ti];
         memset(t, 0, sizeof(*t));
         t->name = gh_strdup(lx->text);
         t->numeric = 1;
      }
      t = &g->tables[ti];
      lex_next(lx);
      while (lx->type != T_EOF) {
         char b[300];
         int neg = 0;
         if (is_op(lx, ',')) { lex_next(lx); continue; }
         if (is_op(lx, '-')) { neg = 1; lex_next(lx); }
         if (lx->type == T_NUM)
            snprintf(b, sizeof(b), "%s%lld", neg ? "-" : "", (long long)lx->num);
         else if (!neg && (lx->type == T_STR || lx->type == T_IDENT)) {
            snprintf(b, sizeof(b), "%s", lx->text);
            numeric_line = 0;
         }
         else {
            lex_error(lx, "bad table entry");
            return 0;
         }
         if (t->n >= GH_MAX_TABLE_ITEMS) { lex_error(lx, "table too long"); return 0; }
         if (t->n >= t->cap) {
            int nc = t->cap ? t->cap * 2 : 64;
            char **ni = (char **)realloc(t->items, nc * sizeof(char *));
            int64_t *nn;
            if (!ni) { lex_error(lx, "out of memory"); return 0; }
            t->items = ni;
            nn = (int64_t *)realloc(t->nums, nc * sizeof(int64_t));
            if (!nn) { lex_error(lx, "out of memory"); return 0; }
            t->nums = nn;
            t->cap = nc;
         }
         t->items[t->n] = gh_strdup(b);
         t->nums[t->n] = (lx->type == T_NUM) ? (neg ? -lx->num : lx->num) : 0;
         t->n++;
         lex_next(lx);
      }
      if (!numeric_line) t->numeric = 0;
      return !lx->failed;
   }
   if (strcmp(kw, "let") == 0) {
      char name[256];
      gh_node *e;
      lex_next(lx);
      if (lx->type != T_IDENT) { lex_error(lx, "let needs a name"); return 0; }
      strcpy(name, lx->text);
      lex_next(lx);
      if (!is_op(lx, '=')) { lex_error(lx, "expected '='"); return 0; }
      lex_next(lx);
      if (g->nlets >= GH_MAX_LETS) { lex_error(lx, "too many lets"); return 0; }
      if (find_let(g, name) >= 0) { lex_error(lx, "name defined twice"); return 0; }
      e = parse_expr(ps);
      if (lx->failed) return 0;
      if (lx->type != T_EOF) { lex_error(lx, "junk after expression"); return 0; }
      g->let_names[g->nlets] = gh_strdup(name);
      g->let_expr[g->nlets] = e;
      g->nlets++;
      return 1;
   }
   if (strcmp(kw, "valid") == 0) {
      lex_next(lx);
      g->valid = parse_expr(ps);
      if (!lx->failed && lx->type != T_EOF) lex_error(lx, "junk after expression");
      return !lx->failed;
   }
   if (strcmp(kw, "left") == 0 || strcmp(kw, "right") == 0) {
      gh_line *ln;
      if (g->nlines >= GH_MAX_LINES) { lex_error(lx, "too many lines"); return 0; }
      ln = &g->lines[g->nlines];
      memset(ln, 0, sizeof(*ln));
      ln->side = (kw[0] == 'l') ? GHUD_LEFT : GHUD_RIGHT;
      ln->color = 0xF0F0F0;
      lex_next(lx);
      for (;;) {
         if (lx->type == T_IDENT && strcmp(lx->text, "when") == 0) {
            lex_next(lx);
            ln->when = parse_expr(ps);
            if (lx->failed) return 0;
            continue;
         }
         if (lx->type == T_IDENT && strcmp(lx->text, "meta") == 0) {
            ln->meta = 1;
            lex_next(lx);
            continue;
         }
         if (lx->type == T_IDENT && strcmp(lx->text, "color") == 0) {
            lex_next(lx);
            if (!parse_color(lx, &ln->color)) return 0;
            continue;
         }
         break;
      }
      if (lx->type != T_STR) { lex_error(lx, "expected \"template\""); return 0; }
      {
         char tmpl[256];
         strcpy(tmpl, lx->text);
         lex_next(lx);
         if (lx->type != T_EOF) { lex_error(lx, "junk after template"); return 0; }
         if (!parse_template(ps, ln, tmpl)) return 0;
      }
      g->nlines++;
      return 1;
   }
   {
      char m[128];
      snprintf(m, sizeof(m), "unknown keyword '%s'", kw);
      lex_error(lx, m);
   }
   return 0;
}

ghud_t *ghud_load_string(const char *text, char *err, size_t errlen)
{
   ghud_t *g = (ghud_t *)calloc(1, sizeof(*g));
   gh_lex lx;
   gh_parser ps;
   const char *p = text;
   char linebuf[GH_MAX_SRC_LINE];
   int lineno = 0;

   if (!g) {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }
   memset(&lx, 0, sizeof(lx));
   lx.err = err;
   lx.errlen = errlen;
   ps.g = g;
   ps.lx = &lx;
   if (errlen) err[0] = 0;

   while (*p) {
      const char *e = strchr(p, '\n');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      lineno++;
      if (len >= sizeof(linebuf)) len = sizeof(linebuf) - 1;
      memcpy(linebuf, p, len);
      linebuf[len] = 0;
      lx.lineno = lineno;
      if (!parse_statement(&ps, linebuf) || lx.failed) {
         if (!lx.failed)
            snprintf(err, errlen, "line %d: parse error", lineno);
         ghud_free(g);
         return NULL;
      }
      p += len;
      if (*p == '\n') p++;
   }
   if (g->nlines == 0) {
      snprintf(err, errlen, "definition has no left/right lines");
      ghud_free(g);
      return NULL;
   }
   ghud_reset_session(g);
   return g;
}

ghud_t *ghud_load_file(const char *path, char *err, size_t errlen)
{
   FILE *f = fopen(path, "rb");
   char *buf;
   long sz;
   ghud_t *g;
   if (!f) {
      snprintf(err, errlen, "cannot open %s", path);
      return NULL;
   }
   fseek(f, 0, SEEK_END);
   sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz < 0 || sz > 1024 * 1024) {
      fclose(f);
      snprintf(err, errlen, "bad file size");
      return NULL;
   }
   buf = (char *)malloc((size_t)sz + 1);
   if (!buf) {
      fclose(f);
      snprintf(err, errlen, "out of memory");
      return NULL;
   }
   if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
      fclose(f);
      free(buf);
      snprintf(err, errlen, "read error");
      return NULL;
   }
   fclose(f);
   buf[sz] = 0;
   g = ghud_load_string(buf, err, errlen);
   free(buf);
   return g;
}

void ghud_free(ghud_t *g)
{
   int i, j;
   gh_node *n;
   if (!g) return;
   n = g->allocs;
   while (n) {
      gh_node *nx = n->next_alloc;
      free(n->ev_t);
      free(n->ev_v);
      free(n);
      n = nx;
   }
   for (i = 0; i < g->nlets; i++)
      free(g->let_names[i]);
   for (i = 0; i < g->ntables; i++) {
      free(g->tables[i].name);
      for (j = 0; j < g->tables[i].n; j++)
         free(g->tables[i].items[j]);
      free(g->tables[i].items);
      free(g->tables[i].nums);
   }
   for (i = 0; i < g->nlines; i++) {
      for (j = 0; j < g->lines[i].nseg; j++)
         free(g->lines[i].segs[j].text);
      free(g->lines[i].segs);
   }
   free(g);
}

void ghud_normalize_serial(const char *in, char *out, size_t outlen)
{
   size_t n = 0;
   if (!outlen) return;
   for (; *in && n + 1 < outlen; in++)
      if (isalnum((unsigned char)*in))
         out[n++] = (char)toupper((unsigned char)*in);
   out[n] = 0;
}

int ghud_text_has_serial(const char *text, const char *norm_serial)
{
   const char *p = text;
   while (*p) {
      const char *e = strchr(p, '\n');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      const char *q = p;
      while (q < p + len && (*q == ' ' || *q == '\t')) q++;
      if ((size_t)(p + len - q) > 7 && strncmp(q, "serial", 6) == 0 && isspace((unsigned char)q[6])) {
         char line[512];
         const char *s;
         size_t l = (size_t)(p + len - q);
         if (l >= sizeof(line)) l = sizeof(line) - 1;
         memcpy(line, q, l);
         line[l] = 0;
         s = line + 6;
         while (*s && *s != '#') {
            char raw[64], norm[16];
            int n = 0;
            while (*s == ' ' || *s == '\t' || *s == ',') s++;
            while (*s && !isspace((unsigned char)*s) && *s != ',' && *s != '#' && n < 63)
               raw[n++] = *s++;
            raw[n] = 0;
            if (!n) break;
            ghud_normalize_serial(raw, norm, sizeof(norm));
            if (strcmp(norm, norm_serial) == 0)
               return 1;
         }
      }
      p += len;
      if (*p == '\n') p++;
   }
   return 0;
}

const char *ghud_game_name(const ghud_t *g) { return g->game; }

void ghud_set_show_meta(ghud_t *g, int show) { g->show_meta = show; }

int ghud_line_count(const ghud_t *g) { return g->nlines; }

const char *ghud_line_text(const ghud_t *g, int i, int *side, int *visible)
{
   if (i < 0 || i >= g->nlines) return NULL;
   if (side) *side = g->lines[i].side;
   if (visible) *visible = g->lines[i].visible && (!g->lines[i].meta || g->show_meta);
   return g->lines[i].out;
}

void ghud_set_reader(ghud_t *g, ghud_read_fn fn, void *user)
{
   g->read = fn;
   g->read_user = user;
}

/* ------------------------------------------------------------------ */
/* evaluation                                                          */

static uint32_t rd(ghud_t *g, int64_t addr, int size)
{
   if (!g->read) return 0;
   return g->read(g->read_user, (uint32_t)addr, size);
}

static gh_val eval(ghud_t *g, gh_node *n);

static int64_t stateful_update(ghud_t *g, gh_node *n, int64_t cur)
{
   /* returns accumulated value for gain/loss/count; handles baselining */
   if (g->frozen)
      return n->acc;
   if (!n->has_prev || n->epoch != g->epoch) {
      n->prev = cur;
      n->has_prev = 1;
      n->epoch = g->epoch;
      return n->acc;
   }
   switch (n->op) {
   case F_GAIN:
   case F_LOSS: {
      int64_t d = cur - n->prev;
      int64_t maxstep = (n->nargs > 1) ? asint(eval(g, n->a[1])) : 0;
      if (n->op == F_LOSS) d = -d;
      if (d > 0 && (maxstep <= 0 || d <= maxstep))
         n->acc += d;
      break;
   }
   case F_COUNT:
      if (cur && !n->prev)
         n->acc++;
      break;
   case F_SINCE:
      if (cur && !n->prev)
         n->last_t = g->t_session;
      break;
   }
   n->prev = cur;
   return n->acc;
}

static gh_val eval(ghud_t *g, gh_node *n)
{
   if (!n) return vint(0);
   switch (n->kind) {
   case N_NUM:
      return vint(n->num);
   case N_LET:
      return g->let_val[n->num];
   case N_UN: {
      int64_t a = asint(eval(g, n->a[0]));
      switch (n->op) {
      case '-': return vint(-a);
      case '!': return vint(!a);
      case '~': return vint(~a);
      }
      return vint(0);
   }
   case N_TERN:
      return asint(eval(g, n->a[0])) ? eval(g, n->a[1]) : eval(g, n->a[2]);
   case N_BIN: {
      gh_val va, vb;
      int64_t a, b;
      if (n->op == OP_AND) {
         if (!asint(eval(g, n->a[0]))) return vint(0);
         return vint(asint(eval(g, n->a[1])) != 0);
      }
      if (n->op == OP_OR) {
         if (asint(eval(g, n->a[0]))) return vint(1);
         return vint(asint(eval(g, n->a[1])) != 0);
      }
      va = eval(g, n->a[0]);
      vb = eval(g, n->a[1]);
      if ((n->op == OP_EQ || n->op == OP_NE) && va.is_str && vb.is_str) {
         int eq = strcmp(va.s, vb.s) == 0;
         return vint(n->op == OP_EQ ? eq : !eq);
      }
      a = asint(va);
      b = asint(vb);
      switch (n->op) {
      case '+': return vint(a + b);
      case '-': return vint(a - b);
      case '*': return vint(a * b);
      case '/': return vint(b ? a / b : 0);
      case '%': return vint(b ? a % b : 0);
      case '&': return vint(a & b);
      case '|': return vint(a | b);
      case '^': return vint(a ^ b);
      case '<': return vint(a < b);
      case '>': return vint(a > b);
      case OP_LE: return vint(a <= b);
      case OP_GE: return vint(a >= b);
      case OP_EQ: return vint(a == b);
      case OP_NE: return vint(a != b);
      case OP_SHL: return vint((b >= 0 && b < 63) ? (a << b) : 0);
      case OP_SHR: return vint((b >= 0 && b < 63) ? (a >> b) : 0);
      }
      return vint(0);
   }
   case N_CALL:
      switch (n->op) {
      case F_U8:  return vint(rd(g, asint(eval(g, n->a[0])), 1) & 0xFF);
      case F_U16: return vint(rd(g, asint(eval(g, n->a[0])), 2) & 0xFFFF);
      case F_U32: return vint(rd(g, asint(eval(g, n->a[0])), 4));
      case F_S8:  return vint((int8_t)rd(g, asint(eval(g, n->a[0])), 1));
      case F_S16: return vint((int16_t)rd(g, asint(eval(g, n->a[0])), 2));
      case F_S32: return vint((int32_t)rd(g, asint(eval(g, n->a[0])), 4));
      case F_TBL:
      case F_ARR: {
         gh_table *t = &g->tables[n->num];
         int64_t i = asint(eval(g, n->a[0]));
         if (i < 0 || i >= t->n)
            return n->op == F_TBL ? vstr("?") : vint(0);
         return n->op == F_TBL ? vstr(t->items[i]) : vint(t->nums[i]);
      }
      case F_MIN: { int64_t a = asint(eval(g, n->a[0])), b = asint(eval(g, n->a[1])); return vint(a < b ? a : b); }
      case F_MAX: { int64_t a = asint(eval(g, n->a[0])), b = asint(eval(g, n->a[1])); return vint(a > b ? a : b); }
      case F_ABS: { int64_t a = asint(eval(g, n->a[0])); return vint(a < 0 ? -a : a); }
      case F_CLAMP: {
         int64_t a = asint(eval(g, n->a[0])), lo = asint(eval(g, n->a[1])), hi = asint(eval(g, n->a[2]));
         return vint(a < lo ? lo : (a > hi ? hi : a));
      }
      case F_BIT: {
         int64_t a = asint(eval(g, n->a[0])), b = asint(eval(g, n->a[1]));
         return vint((b >= 0 && b < 63) ? ((a >> b) & 1) : 0);
      }
      case F_PCT: {
         int64_t a = asint(eval(g, n->a[0])), b = asint(eval(g, n->a[1]));
         return vint(b ? a * 100 / b : 0);
      }
      case F_SESSION:
         return vint((int64_t)g->t_session);
      case F_HOLD: {
         /* hold(x, cond): x while cond is true, else the last x seen while true */
         int64_t cond = asint(eval(g, n->a[1]));
         gh_val x = eval(g, n->a[0]);
         if (cond || !n->has_prev) {
            n->prev = asint(x);
            n->has_prev = cond ? 1 : 0;
            if (!cond) return x;
         }
         return vint(n->prev);
      }
      case F_GAIN:
      case F_LOSS:
      case F_COUNT:
         return vint(stateful_update(g, n, asint(eval(g, n->a[0]))));
      case F_SINCE: {
         int64_t cur = asint(eval(g, n->a[0])) != 0;
         stateful_update(g, n, cur);
         return vint((int64_t)(g->t_session - n->last_t));
      }
      case F_PERHOUR: {
         /* Average rate of a growing counter since it first moved. The first
          * step is left out of the total: its time (load, title screen, the
          * walk to the first fight) was before the clock started, so counting
          * it would inflate the rate. "--" until there is a second step. */
         int64_t v = asint(eval(g, n->a[0]));
         double dt;
         if (!n->has_first) {
            if (!n->has_prev) { n->prev = v; n->has_prev = 1; return vstr("--"); }
            if (v != n->prev) {
               n->has_first = 1;
               n->first_t = g->t_session;
               n->first_v = v;
            }
            n->prev = v;
            return vstr("--");
         }
         dt = g->t_session - n->first_t;
         if (v == n->first_v || dt < 1.0)
            return vstr("--");
         return vint((int64_t)((double)(v - n->first_v) * 3600.0 / dt));
      }
      case F_RATE: {
         /* Rolling per-hour rate over the last `window` seconds of game time:
          * (now - value at window start) / window. Before the counter first
          * moves this is "--"; until a full window has passed since then, the
          * window starts at the first move (same rule as perhour). */
         int64_t v = asint(eval(g, n->a[0]));
         double win = (double)asint(eval(g, n->a[1]));
         double now = g->t_session, start, dt;
         int64_t base;
         int i, keep;
         if (win < 10.0) win = 10.0;
         if (n->ev_n == 0 || v != n->ev_v[n->ev_n - 1]) {
            if (n->ev_n == n->ev_cap) {
               int nc = n->ev_cap ? n->ev_cap * 2 : 32;
               double *nt = (double *)realloc(n->ev_t, nc * sizeof(double));
               int64_t *nv;
               if (!nt) return vint(0);
               n->ev_t = nt;
               nv = (int64_t *)realloc(n->ev_v, nc * sizeof(int64_t));
               if (!nv) return vint(0);
               n->ev_v = nv;
               n->ev_cap = nc;
            }
            n->ev_t[n->ev_n] = now;
            n->ev_v[n->ev_n] = v;
            n->ev_n++;
         }
         /* drop history older than needed: keep the last event at or before
          * the window start, it holds the value at the start */
         keep = 0;
         for (i = 0; i < n->ev_n; i++)
            if (n->ev_t[i] <= now - win)
               keep = i;
         if (keep > 0) {
            memmove(n->ev_t, n->ev_t + keep, (n->ev_n - keep) * sizeof(double));
            memmove(n->ev_v, n->ev_v + keep, (n->ev_n - keep) * sizeof(int64_t));
            n->ev_n -= keep;
         }
         if (n->ev_n < 2 && !(n->ev_n == 1 && n->ev_t[0] <= now - win))
            return vstr("--");   /* hasn't moved yet */
         if (n->ev_t[0] <= now - win) {
            base = n->ev_v[0];
            start = now - win;
         }
         else {
            /* less than a window since the first move: start at the first
             * move, excluding it (index 1 is the first change) */
            base = n->ev_v[1];
            start = n->ev_t[1];
            if (v == base) return vstr("--");
         }
         dt = now - start;
         if (dt < 1.0) return vstr("--");
         return vint((int64_t)((double)(v - base) * 3600.0 / dt));
      }
      case F_BAR: {
         /* handled by the formatter; encode fill in a static buffer per node */
         static char bars[GH_MAX_LINES * 4][3];
         static int bar_i;
         int64_t v = asint(eval(g, n->a[0])), m = asint(eval(g, n->a[1]));
         int fill = (m > 0) ? (int)((v < 0 ? 0 : (v > m ? m : v)) * 200 / m) : 0;
         char *b = bars[bar_i++ % (GH_MAX_LINES * 4)];
         b[0] = GH_BAR_MARK;
         b[1] = (char)(fill + 1);  /* avoid NUL */
         b[2] = 0;
         return vstr(b);
      }
      }
      return vint(0);
   }
   return vint(0);
}

static void fmt_compact(char *out, size_t len, int64_t v)
{
   int64_t a = v < 0 ? -v : v;
   const char *sign = v < 0 ? "-" : "";
   if (a < 10000)
      snprintf(out, len, "%lld", (long long)v);
   else if (a < 1000000)
      snprintf(out, len, "%s%lld.%lldk", sign, (long long)(a / 1000), (long long)((a % 1000) / 100));
   else
      snprintf(out, len, "%s%lld.%lldM", sign, (long long)(a / 1000000), (long long)((a % 1000000) / 100000));
   if (a >= 100000 && a < 1000000)   /* 123.4k -> 123k */
      snprintf(out, len, "%s%lldk", sign, (long long)(a / 1000));
}

static void format_line(ghud_t *g, gh_line *ln, char *out, size_t outlen)
{
   size_t o = 0;
   int i;
   out[0] = 0;
   for (i = 0; i < ln->nseg; i++) {
      gh_seg *s = &ln->segs[i];
      char tmp[64];
      const char *piece = tmp;
      if (s->is_lit) {
         piece = s->text;
      }
      else {
         gh_val v = eval(g, s->expr);
         if (v.is_str)
            piece = v.s;
         else
            switch (s->fmt) {
            case 'x': snprintf(tmp, sizeof(tmp), "%llX", (unsigned long long)v.i); break;
            case 'k': fmt_compact(tmp, sizeof(tmp), v.i); break;
            case 't': {
               int64_t t = v.i < 0 ? 0 : v.i;
               if (t >= 3600)
                  snprintf(tmp, sizeof(tmp), "%lld:%02lld:%02lld", (long long)(t / 3600),
                        (long long)((t / 60) % 60), (long long)(t % 60));
               else
                  snprintf(tmp, sizeof(tmp), "%lld:%02lld", (long long)(t / 60), (long long)(t % 60));
               break;
            }
            default: snprintf(tmp, sizeof(tmp), "%lld", (long long)v.i); break;
            }
         if (s->width > 0 && piece[0] != GH_BAR_MARK) {
            int pad = s->width - (int)strlen(piece);
            while (pad-- > 0 && o + 1 < outlen)
               out[o++] = ' ';
         }
      }
      while (*piece && o + 1 < outlen)
         out[o++] = *piece++;
   }
   out[o] = 0;
}

int ghud_tick(ghud_t *g, double dt)
{
   int i, changed = 0;

   /* lets first (in order), so later lets/lines can use them */
   g->frozen = 0;
   if (g->valid) {
      /* valid may reference lets: evaluate lets without counters first */
      g->frozen = 1;
      for (i = 0; i < g->nlets; i++)
         g->let_val[i] = eval(g, g->let_expr[i]);
      if (!asint(eval(g, g->valid)))
         g->epoch++;          /* stay frozen; re-baseline once valid again */
      else
         g->frozen = 0;
   }
   if (!g->frozen)
      g->t_session += dt;
   for (i = 0; i < g->nlets; i++)
      g->let_val[i] = eval(g, g->let_expr[i]);

   for (i = 0; i < g->nlines; i++) {
      gh_line *ln = &g->lines[i];
      char buf[GH_LINE_CHARS];
      int vis = ln->when ? (asint(eval(g, ln->when)) != 0) : 1;
      format_line(g, ln, buf, sizeof(buf));   /* always: keeps counters fed */
      if (vis != ln->visible || strcmp(buf, ln->out) != 0) {
         changed = 1;
         ln->visible = vis;
         memcpy(ln->out, buf, sizeof(buf));
      }
   }
   return changed;
}

void ghud_discontinuity(ghud_t *g)
{
   g->epoch++;
}

void ghud_reset_session(ghud_t *g)
{
   gh_node *n;
   int i;
   for (n = g->allocs; n; n = n->next_alloc) {
      n->acc = 0;
      n->has_prev = 0;
      n->last_t = 0;
      n->has_first = 0;
      n->ev_n = 0;
   }
   g->t_session = 0;
   g->epoch++;
   for (i = 0; i < g->nlines; i++) {
      g->lines[i].out[0] = 0;
      g->lines[i].visible = 0;
   }
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

static uint32_t to_native(const ghud_surface_t *s, uint32_t rgb)
{
   if (s->bpp == 2) {
      uint32_t r = (rgb >> 16) & 0xFF, gg = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
      return ((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3);
   }
   return rgb & 0xFFFFFF;
}

static void put_span(const ghud_surface_t *s, int x, int y, int w, uint32_t c)
{
   if (y < 0 || y >= s->height) return;
   if (x < 0) { w += x; x = 0; }
   if (x + w > s->width) w = s->width - x;
   if (w <= 0) return;
   if (s->bpp == 2) {
      uint16_t *p = (uint16_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch) + x;
      while (w--) *p++ = (uint16_t)c;
   }
   else {
      uint32_t *p = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch) + x;
      while (w--) *p++ = c;
   }
}

void ghud_fill_rect(const ghud_surface_t *surf, int x, int y, int w, int h, uint32_t rgb)
{
   uint32_t c = to_native(surf, rgb);
   int j;
   for (j = 0; j < h; j++)
      put_span(surf, x, y + j, w, c);
}

static void draw_glyph(const ghud_surface_t *s, int x, int y, unsigned char ch, uint32_t c, int fx, int fy)
{
   const unsigned char *gl = ghud_font8x8[ch & 0x7F];
   int row, col, k;
   for (row = 0; row < 8; row++) {
      unsigned bits = gl[row];
      if (!bits) continue;
      for (col = 0; col < 8; col++)
         if (bits & (1u << col))
            for (k = 0; k < fy; k++)
               put_span(s, x + col * fx, y + row * fy + k, fx, c);
   }
}

static void draw_bar(const ghud_surface_t *s, int x, int y, int w, int fill200, uint32_t rgb, int fy)
{
   int h = 6 * fy, top = y + fy;
   int inner = w - 2;
   int f = inner * fill200 / 200;
   uint32_t frame = to_native(s, 0x505050);
   uint32_t c = to_native(s, rgb);
   int j;
   if (w < 4) return;
   put_span(s, x, top, w, frame);
   put_span(s, x, top + h - 1, w, frame);
   for (j = 1; j < h - 1; j++) {
      put_span(s, x, top + j, 1, frame);
      put_span(s, x + w - 1, top + j, 1, frame);
      put_span(s, x + 1, top + j, f, c);
   }
}

void ghud_draw_text(const ghud_surface_t *surf, int x, int y, int maxw,
      const char *text, uint32_t rgb, int fx, int fy)
{
   uint32_t c = to_native(surf, rgb);
   int cx = x, cw = 8 * fx;
   const unsigned char *p = (const unsigned char *)text;
   while (*p) {
      if (*p == (unsigned char)GH_BAR_MARK && p[1]) {
         int fill = (int)p[1] - 1;
         draw_bar(surf, cx, y, x + maxw - cx, fill, rgb, fy);
         return;   /* a bar fills the rest of the line */
      }
      if (cx + cw > x + maxw)
         break;
      if (*p != ' ')
         draw_glyph(surf, cx, y, *p, c, fx, fy);
      cx += cw;
      p++;
   }
}

void ghud_render(const ghud_t *g, int side, const ghud_surface_t *surf,
      int x, int y, int w, int h, int fx, int fy)
{
   int i, line_h = 8 * fy + 2 * fy;
   int margin_x = 4 * fx, cy = y + 4 * fy;
   for (i = 0; i < g->nlines; i++) {
      const gh_line *ln = &g->lines[i];
      if (ln->side != side || !ln->visible || (ln->meta && !g->show_meta))
         continue;
      if (cy + 8 * fy > y + h)
         break;
      ghud_draw_text(surf, x + margin_x, cy, w - 2 * margin_x, ln->out, ln->color, fx, fy);
      cy += line_h;
   }
}
