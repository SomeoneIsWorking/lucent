# Rollback netplay (`lucent::rollback`)

`include/lucent/rollback/session.h`, `src/rollback/session.cpp`, `tests/test_rollback.cpp`. Core
Lucent: no transport, no crypto, no threads.

## Owns

- Input exchange for up to 8 slots of up to 8 bytes each, every peer driving one or more slots.
- Prediction (a slot's newest confirmed input repeats), rewind to the earliest frame a late input
  changed, and resimulation up to the present.
- Input delay: a local input plays `input_delay` frames after it is given; the first
  `input_delay` frames play zero input for local slots.
- The stall rule: no frame runs once the earliest unconfirmed remote frame is more than
  `max_rollback` frames back, so the ring always holds the frame a rewind needs.
- Desync detection: each peer's hash after its newest final frame rides every message; a frame
  whose hashes differ is `desync_frame()` and every later `tick` returns `Desynced`.

It does not own the link (the application sends `message_for` over a sealed `ReliableLink`, a
`Path` datagram or anything else that may drop or reorder), the save-state storage, or the game.

## Inputs and outputs

The game implements `Runtime`: `save(slot)`/`load(slot)` for ring slots `0..max_rollback`,
`setInputs` (every slot's bytes, slot-major), `advance(resimulating)` (no audio or presentation
when resimulating) and `hash()`. Each tick the application calls `receive(link, message)` for what
arrived, `tick(local)` once, then sends `message_for(link)` on every link.

## Data flow

1. `tick` refuses with `Stalled` if `frame - max_rollback` passed the earliest unconfirmed remote
   frame; the local input is not taken, so the application repeats it next tick.
2. The local input is confirmed at `frame + input_delay`.
3. If a received input differs from the one a frame ran with, the state at the start of the
   earliest such frame is loaded and every frame up to the present re-runs with `resimulating`.
4. The present frame is saved to `ring(frame) = frame % (max_rollback + 1)` and runs.
5. Every frame that ran and has every input confirmed becomes final: the hash it ran to is kept
   and compared with the peers'. `finalized_frame()` is the bound for reading `final_hash`;
   `confirmed_frame()` can be past `frame()` with input delay, for frames that have not run.

## Wire (version 1, big-endian)

`u8 version, u8 slots, u32 confirmed_until[slots], u8 runs, runs × (u8 slot, u32 start, u8 count,
count × input_bytes), u8 has_hash, [u32 frame, u64 hash]`. A run is a slot's confirmed inputs from
the link's acknowledgement on, at most 64 frames; a link further behind catches up over several
messages. Every message repeats what is not acknowledged, so loss only delays. Inputs learned on
one link go out on the others, so a host relays a star; a peer never accepts inputs for its own
slots. A message that does not read is ignored.

## Invariants

- Same inputs per frame on every peer ⇒ same hash per frame; a peer's final hashes equal a
  lockstep run of the true inputs.
- A confirmed input is never replaced; only the next frame of a slot is accepted.
- `load` only names a ring slot written within the last `max_rollback + 1` frames.

## Checking it

`ctest -R rollback`: two peers on a mesh and four on a host-relayed star, over links with 3 ticks
of delay, ±1 jitter and 20% loss, match lockstep for the last 100 of 600 frames and resimulate; a
game that drifts is reported within 40 frames of the drift; a peer that hears nothing runs
`max_rollback + 1` frames and stalls. In a game, two processes over `tools.nat_lab` show the same
`final_hash` on both sides and no `Desynced`.

Design reference: GGPO (MIT, github.com/pond3r/ggpo), read for its sync and input-queue design;
no code is taken.
