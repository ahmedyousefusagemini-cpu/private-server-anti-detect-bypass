# Where the position lives: 0x0898 `CMsgWalk`

This is the reference for the jump-to-X,Y feature. It records how the
position field was identified, what the packing is, and which parts are
verified versus assumed.

## Why 0x0898 and not 0x0833

`0x0833` is `CMsgAction` - an entity action / animation message. It carries
a client clock and an action reference, but **no position**. Several
"one tile" captures of `0x0833` were checked and the only field that moved
was a large id (`f3`, delta ~1,021,458 for one tile, and the same value
reappeared ~18 minutes later) - an entity/object handle, not a coordinate.
The fields `f7`/`f14` that looked like an axis pair moved equal-and-opposite
with a **constant sum** (`377+378 = 381+374 = 755`), the signature of one
scalar split across two fields.

`0x0898` is `CMsgWalk`, and `docs/packet-catalog.md` documents it as:

> Movement sync. SEND = client walk request (target pos); RECV =
> authoritative position broadcast.

That is the message that carries the cell.

## The packing

Field 4 of the SEND body holds the target cell as a **fixed-point
position**:

```
f4 = (y << 16) | (x << 8) | frac

x    = (f4 >>  8) & 0xFF     x tile
y    = (f4 >> 16) & 0xFF     y tile
frac =  f4        & 0xFF     fractional x within the tile (0..255)
```

All three components fit a 256-wide Conquer map, and both axes were
confirmed against real movement.

## Evidence

### Set A - three packets on one straight walk

| seq | time | f1 | f4 | x | y | frac |
|---|---|---|---|---|---|---|
| 010880 | 21:11.610 | 94 | 10302897 | 53 | 157 | 177 |
| 010885 | 21:13.141 | 205 | 10304441 | 59 | 157 | 185 |
| 011030 | 21:41.719 | 247 | 10333015 | 171 | 157 | 87 |

`y` is constant at 157 while `x` moves 53 -> 59 -> 171: the walk was along
one axis.

### Set B - eight consecutive packets on one straight walk

| seq | time | f1 | f4 | x | y | frac |
|---|---|---|---|---|---|---|
| 012245 | 24:56.438 | 122 | 10527733 | 163 | 160 | 245 |
| 012250 | 24:56.641 | 10 | 10527931 | 164 | 160 | 187 |
| 012260 | 24:56.829 | 154 | 10528129 | 165 | 160 | 129 |
| 012262 | 24:57.032 | 154 | 10528327 | 166 | 160 | 71 |
| 012267 | 24:57.219 | 154 | 10528516 | 167 | 160 | 4 |
| 012272 | 24:57.422 | 154 | 10528715 | 167 | 160 | 203 |
| 012276 | 24:57.625 | 154 | 10528916 | 168 | 160 | 148 |
| 012282 | 24:57.829 | 67 | 10529114 | 169 | 160 | 90 |

`y` is constant at 160, `x` advances 163 -> 169. The packed value advances
by

```
198, 198, 198, 189, 199, 201, 198
```

- a near-constant sub-tile step. The `frac` byte descends 245 -> 187 ->
  129 -> 71, then wraps, and the `x` byte ticks over each time it crosses.
  **That is a fixed-point coordinate, not two integers packed side by
  side.** The `x` byte is simply `floor(value / 256)`.

Both sets agree: `x` is bits 8..15 and `y` is bits 16..23.

## Fields that are constant

Across every `0x0898` SEND captured, these never changed:

| field | value | note |
|---|---|---|
| 2 | 1353102 | the same scalar that appears as field 1 of `0x0833` - a per-character / session reference, *not* a position |
| 3 | 1 | fixed |
| 5 | 10364 | fixed |

They are copied verbatim from the template capture.

Field 1 varies (94, 205, 247, 122, 10, 154, 67 in the samples above). It is
a direction / animation id, not a coordinate.

## What the jump does

To jump to a whole tile, the fixed-point fraction is zeroed:

```
f4 = (y << 16) | (x << 8) | 0
```

so the character lands exactly on the tile rather than between tiles.
Carrying the capture's `frac` over would aim it off-centre.

The rest of the packet - the id in `body[0..1]`, fields 1, 2, 3 and 5 - is
taken from a real `0x0898` capture. The client's own `DoSendMsg` still
does the id/vtable/size validation (`CALL [vtable+8]` compared against the
`u16` length at `msg+4`), so the message has to remain a well-formed
`CMsgWalk` for this build or it is dropped with `Check Size Failed`.

Because `f4` is a varint whose byte-length depends on x/y, it is rewritten
through `ReplaceVarint()` and never patched in place.

## Still open

- The **`frac` unit per tile** is not exactly known: the observed step was
  ~198 counts per packet, not 256. That is consistent with a fixed-point
  value that the client advances by a per-frame amount rather than exactly
  one tile per packet - the walk animation does not have to land on a
  boundary. A jump sets `frac = 0`, which sidesteps the question.
- Whether the server **clamps or rejects** a jump distance that exceeds the
  normal walk range has not been tested. Start with short jumps (a few
  tiles) before trying long ones.
- The `0x0898` **RECV** shape (`f4 = 140894148`) is the server's
  authoritative broadcast. It uses the same field but was not needed for
  the sender.
