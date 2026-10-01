# Where the move target lives: `0x0833` fields 7 and 8

This is the reference for the move-to-X,Y feature. It records how the
position fields were identified, what is verified, and what is still a
reasonable inference.

## Summary

A move is a `0x0833` `CMsgAction` carrying **both ends** of the step:

```
f7  = target X        f8  = target Y
f14 = origin X        f15 = origin Y
f12 = action mode     (137 = 0x89 is the move mode)
f1  = character id    f9  = client clock (ms)
```

To move, take a real `0x0833` capture, write the destination into f7/f8 and
the current position into f14/f15, and send.

## How the field numbers were found

The client's own sender is **`FUN_00d94ff5`**. It is short and explicit: it
stamps `*(u16*)(this + 6) = 0x833` (the wire id) and stores each of its
arguments at a fixed offset in the message object:

| object offset | argument | protobuf field |
|---|---|---|
| `+0x42C` | arg2 | f1 |
| `+0x448` | arg7 | f7 |
| `+0x44C` | arg8 | f8 |
| `+0x450` | arg9 | **f9** |
| `+0x45C` | arg6 | **f12** |
| `+0x460` | arg5 | f13 |
| `+0x464` | arg3 | f14 |
| `+0x46C` | arg4 | f15 |
| `+0x474` | arg10 | f17 |
| `+0x480` | arg11 | f20 |

Sorting the offsets ascending lines them up with the fields ascending. Two
of those alignments were **already established independently** before this
decompilation:

* `+0x450` is the timestamp — the value that tracks the wall clock to
  within 3 ms across a 27-minute gap.
* `+0x45C` is the mode — `CMsgAction::Process` switches on exactly this
  offset, and the captured mode values (102 / 137 / 410 / 420) are what vary
  between message shapes.

Because two anchors agree, the rest of the alignment is trustworthy.

The sender also special-cases `arg6 == 0x89` (137), which matches the mode
seen in every movement capture. That ties the sender to the move path.

## How the pairs were confirmed

Across 13 consecutive movement packets, **f14/f15 of one packet equal
f7/f8 of the previous packet in 12 cases**:

| seq | f7 | f8 | f14 | f15 | f14 == prev f7 | f15 == prev f8 |
|---|---|---|---|---|---|---|
| 013588 | 377 | 218 | 381 | 210 | – | – |
| 013620 | 377 | 220 | 377 | 218 | yes | yes |
| 013622 | 377 | 222 | 377 | 220 | yes | yes |
| 013631 | 377 | 224 | 377 | 222 | yes | yes |
| 013635 | 377 | 226 | 377 | 224 | yes | yes |
| 013641 | 377 | 228 | 377 | 226 | yes | yes |
| 013645 | 377 | 229 | 377 | 228 | yes | yes |
| 013647 | 377 | 231 | 377 | 229 | yes | yes |
| 014458 | 377 | 224 | 377 | 231 | yes | yes |
| 014466 | 381 | 226 | 377 | 224 | yes | yes |
| 014480 | 379 | 220 | 381 | 226 | yes | yes |
| 014486 | 376 | 220 | 379 | 220 | yes | yes |
| 014515 | 381 | 220 | 377 | 222 | no | no |
| 014528 | 377 | 214 | 381 | 220 | yes | yes |

A message whose "from" half equals the previous message's "to" half is a
movement action. That is what these two pairs are.

## A correction

An earlier pass concluded that `0x0833` carried **no** position, and that
the target lived only in `0x0898` `CMsgWalk` field 4 (a packed
`(y<<16)|(x<<8)|frac` cell). That was wrong.

The mistake was reasoning from value ranges alone: during the capture used
for that pass the character barely moved, so f7 sat at 376–381 and f8 at
210–231. Those look like small counters, and they were labelled as
"phase counters". They are in fact coordinates — the character simply was
not going anywhere.

The `0x0898` packed-cell finding is still correct for that message; it is
just not the message to drive a move with.

## What is still inferred, not proven

* **Which of f7/f8 is X and which is Y.** They are assigned X-then-Y
  because that is the order they appear in and the convention everywhere
  else in this codebase. If a test move lands mirrored, swap the two field
  numbers in the panel (`Xf` / `Yf`) — no rebuild needed.
* **Whether the server accepts a step longer than the normal walk range.**
  Untested. The origin (f14/f15) is what the server can check against its
  own record, so a long step may be rejected or rubber-banded. **Start with
  a few tiles**, not across the map.
* **The exact meaning of f13.** It varies and is labelled "direction", but
  it was not isolated.

## Why the old bot project helped

`H:\tools\CoClassicBot-master` targets a *different* client — `ImConquer.exe`,
64-bit, "Classic Conquer 2.0" — so none of its RVAs apply here. What it did
provide is the **approach**:

* It moves by sending `CMsgAction` with a jump mode and x/y, not by
  hand-crafting a walk packet. That is what pointed at `0x0833` rather than
  `0x0898`, and the Ghidra work then confirmed it.
* It calls the game's own `CHero::Jump(this, x, y)` at a fixed RVA
  (`0x22A0B0` in that build) when it wants the native behaviour.
* After sending, it **also updates the local position**
  (`hero->m_posMap = destination`, plus the world/screen coordinates), so
  the client does not snap back.

That last point is worth keeping in mind: a packet the server accepts but
the client does not reflect will look exactly like "the character did not
move".
