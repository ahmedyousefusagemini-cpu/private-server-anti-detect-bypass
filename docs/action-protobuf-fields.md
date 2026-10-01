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
| **9** | **client clock, milliseconds** | Two jumps 27.03 min apart: field delta `1,622,069` ms vs wall-clock `1,622,072` ms. A 3 ms match. This is the field the "Lead (ms/jump)" control advances. |
| **1** | **action reference** (stable id for the action) | Byte-identical (`1353102`) across two *different* action shapes, including a non-jump one. Constant across a whole session. |
| **20** | **"no target" sentinel** | Always `0xFFFFFFFFFFFFFFFF` (an all-ones `uint64`), encoded as the maximum-length 10-byte varint. Present in every capture so far. |
| 8, 12, 17 | fixed in every capture seen | `226`, `137`, `10364`. Note field 17's tag is **two bytes** (`88 01`). |

## Suspected, not confirmed

| field | guess | why it is only a guess |
|---|---|---|
| 7, 14 | a **position pair** | They move by equal and opposite amounts with a constant sum: `377+378 = 381+374 = 755`. |

The fixed-sum behaviour is the important detail: if `(7, 14)` were independent
`(x, y)` coordinates, a jump would not move both by 4 in *opposite* directions
while the total stays constant. That pattern is a single scalar split across
two fields — e.g. `(position, total - position)` — not two axes. Editing them
as if they were x/y would produce an internally inconsistent position.

## Also unresolved

* `13` changed `0 → 7` and `15` changed `216 → 217` between the two captures.
  Small counters, or a direction index. Two samples are not enough to say.
* `CMsgWalk` (`0x0898`) is the message that actually *walks* the character
  somewhere. If a coordinate pair exists on the wire, it is at least as likely
  to live there as in `CMsgAction`, which is an animation/action message.
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
