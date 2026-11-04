# Grind HUD

Read-only side panels for long RPG grinds. With the core option
**Grind HUD (Side Panels)** on, the core outputs a 16:9 frame: the game in the
middle at 4:3 (integer-scaled), and text panels in the side bars. With it off,
the core's output is unchanged.

The panels come from a per-game **definition file** (`.ghud`) in the libretro
system directory: `system/grindhud/psx/`. The core looks for
`<SERIAL>.ghud` first (e.g. `SCUS94491.ghud`), then any `.ghud` whose `serial`
line lists the disc. Files reload automatically when they change.

The engine never writes emulated memory.

## Core options
- `pcsx_rearmed_grindhud`: disabled / enabled
- `pcsx_rearmed_grindhud_size`: small / medium / large
- `pcsx_rearmed_grindhud_meta`: show `meta` lines (encounter counters and similar)

## Definition format (one statement per line, `#` comments)
```
grindhud 1
game The Legend of Dragoon
serial SCUS-94491 SCUS-94584          # disc serials this file covers
table name DART LAVITZ "TWO WORDS"    # repeat `table name ...` to append
let gold = u32(0xBAC5C)               # named values, evaluated each frame in order
valid gold < 99999999                 # counters only run while this is true
left  [when EXPR] [meta] [color NAME] "Gold {gold}"
right color cyan "{perhour(gain(gold)):k}/h"
```
Expressions: integers, `+ - * / % & | ^ << >> == != < <= > >= && || ! ~ ?:`.

Functions:
- memory: `u8 u16 u32 s8 s16 s32(addr)` (PS1 addresses; mirrors handled)
- tables: `tbl(table, i)` (text), `arr(table, i)` (number)
- math: `min max abs clamp bit pct`
- session (game time while `valid`): `session()` seconds, `gain(x[, maxstep])`
  sum of increases, `loss(x[, maxstep])`, `count(cond)` rising edges,
  `since(cond)` seconds since last rising edge, `perhour(x)`,
  `hold(x, cond)` last x seen while cond was true
- display: `bar(value, max)` fills the rest of the line

Template formats: `{expr}`, `{expr:8}` (right-align width 8), `{expr:x}` hex,
`{expr:t}` h:mm:ss from seconds, `{expr:k}` compact (12.3k). Use parentheses
for `?:` inside a template.

Colors: white gray dim yellow orange red green cyan blue magenta gold, or
`0xRRGGBB`.

## Tools
`tools/ghud_preview.c` renders a definition against a 2 MB RAM dump (and
optionally a game frame) without an emulator.
