# `0x0833` `CMsgAction` — what each protobuf field is

This records what is **verified** about the `CMsgAction` body, what is only
**suspected**, and how to settle the unknowns. It exists because the natural
next request — "jump to a chosen x,y" — needs the coordinate fields, and a
guess there would silently send wrong coordinates.

The class is a protobuf message (`CMsgActionPB`); every field is a varint and
unset fields are simply absent from the wire. The captured jump carries ten of
them. Because protobuf omits what is not set, the field *numbers* are stable
but the *set* of fields present varies by action sub-type.

## Verified

| field | meaning | evidence |
|---|---|---|
| **9** | **client clock, milliseconds** | Two jumps 27.03 min apart: field delta `1,622,069` ms vs wall-clock `1,622,072` ms. A 3 ms match. Across a 33-packet capture it took 32 distinct values, strictly increasing. This is the field the "Lead (ms/jump)" control advances. |
| **1** | **action reference** (stable id for the action) | Byte-identical (`1353102`) in **every** one of the 33 packets captured, across all four body shapes. Constant for the whole session, so it is a character/session reference. |
| **12** | **the action sub-type** | Takes exactly four values in the capture: `102`, `137`, `410`, `420`. The sub-type drives which fields are present, so this is the field that selects the message shape. |
| **20** | **"no target" sentinel** | Always `0xFFFFFFFFFFFFFFFF` (an all-ones `uint64`), encoded as the maximum-length 10-byte varint. Appears only in the 42-byte shape. |
| **17** | fixed at `10364` | Present in the 42-byte shape only. Its tag is **two bytes** (`88 01`). |

## Ruled out as position

**Field 3 is an entity/object id, not a coordinate.** In the 33-packet capture
it took four values:

```
421282, 57456, 1442740, 1442136
```

`1442740` and `1442136` alternate (differing by 604) and recur across more
than 20 minutes of capture. A coordinate would not sit on one large value and
toggle by 604 — that is a *thing*, not a *place*.

**Fields 7, 14, 8, 15 are not a coordinate pair.** They float in two narrow
bands:

```
f7  / f14 :  376 .. 381     (5 wide)
f8  / f15 :  210 .. 231     (21 wide)
```

A coordinate that addresses a map spans hundreds of values. These move within
a couple of dozen, and there is no consistent relation *within* a message
(`f7 == f14` in some packets, false in others; same for `f8 == f15`). They are
sub-tile phases or animation frame indices — small, bounded, per-frame state.

## The four body shapes

`0x0833` appears in four distinct lengths in this capture, and the field set
depends on the sub-type:

| body bytes | fields | note |
|---|---|---|
| 16 | `1, 5, 9, 12, 13` | shortest — no entity id |
| 20 | `1, 3, 5, 9, 12, 13` | adds the entity id |
| 25 | `1, 3, 9, 10, 12, 13, 14, 15` | `f10 = 0` |
| 42 | `1, 7, 8, 9, 12, 13, 14, 15, 17, 20` | the "full" shape |

Because protobuf omits unset fields, the field *numbers* are stable but the
*set* present varies. The builder's decoded view handles this — it walks
whatever is there and shows an unparsed tail if the walk stops early.

## Conclusion

**`CMsgAction` carries no position.** It is an animation/action message: a
clock, a session reference, a sub-type selector, an entity id and some small
per-frame counters. No field in it addresses a map cell.

The position lives in **`0x0898 CMsgWalk`**, field 4 — see
[walk-position-fields.md](walk-position-fields.md). The jump-to-X,Y feature
uses that message, not this one.
  `0x0988 CMsgMapItem` was observed with `f4=448, f5=270` — a plausible `(x,y)`
  shape, in a different message.

## How to settle it — the built-in diff

The builder has a **reference diff** for exactly this:

1. Jump once, then **Load from Capture** on the Packets tab.
2. Press **Set Reference**. That snapshots the current field values.
3. Move the character somewhere clearly different (or teleport).
4. Jump again — this captures a fresh packet with a new position.
5. Press **Reload Template**, then **Set Reference** is still armed: the
   decoded list now flags every field that changed with `changed +N` / `-N`.

The coordinate fields are whichever two move together on a position change.
Use a **large** movement (a teleport, not a 4-unit hop) so the signal is
unmistakable and cannot be confused with a counter or the clock.

Once identified, the field numbers should be recorded in the table above and
(when a jump-to-x,y control is added) wired to that control.
