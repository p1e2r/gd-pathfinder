# Pathfinder

A Geode mod for **Geometry Dash 2.2091 / Geode 6.0.0-alpha.2** (Windows, macOS, Android64)
that searches a classic-mode level for a sequence of clicks that beats it and saves the
result as a **`.gdr`** macro. Play the macro back with xdBot or any other bot that reads GDR.

MIT licensed. Not for the Geode index: Geode doesn't accept AI-written mods there.

## Use

1. Open a level (normal or practice; practice is turned off automatically).
2. Press **Alt+J**, or pause and press **Pathfind** in the bottom-left corner.
3. Watch the top-left label: best %, tries, search depth. Press Alt+J again to stop.
4. When it beats the level it replays the path from a clean start to check it, then writes
   `<level>_pathfinder.gdr` to the save folder (default `{gd_dir}/macros`, the same one
   xdBot loads from).

The search doesn't save deaths, percentages or a completion. It does count attempts.

## How it works

Physics runs at a fixed 240 ticks per second, so a path is just a list of ticks where
the jump button flips. The bot runs many ticks per rendered frame (see *Time per frame*).

- **Run** with the current path until the player dies at tick `d`. Deaths are caught in
  `PlayLayer::destroyPlayer` and never happen in the game.
- **Branch**: try one new flip at `d-1, d-2, …` down to `d - window` (closest first). Each
  try rewinds to the nearest snapshot and simulates forward. If that try dies later, it
  gets its own window. That makes a depth-first search over click timings.
- **Backtrack**: when a window runs out it widens once, then the search goes up a level.
- **Prune**: a try that dies at exactly the same tick and place as one already seen is
  not expanded (*Skip identical deaths*).
- **Snapshots** combine GD's `CheckpointObject` (level state) with a full member-by-member
  copy of both `PlayerObject`s (`src/player_state.hpp`, generated from the bindings).
  GD's own `PlayerCheckpoint` is lossy.
- **Verify**: the found path is replayed from `resetLevelFromStart` with no rewinds. If it
  drifts and dies, the bot searches again from there, using snapshots from that clean
  run (*Max repairs*). *Exact mode* skips snapshots entirely: every try restarts the level.

Input timing matches GDR playback: an input at `frame` F is sent with `handleButton` right
after the physics step that brings `m_currentProgress` to F. This is the same rule xdBot
uses for non-xdBot macros.

## Settings

| Setting | Default | Notes |
|---|---|---|
| Start / stop | Alt+J | |
| Search window | 60 ticks | How far before a death to try new inputs |
| Tick step | 1 | Try every Nth tick in the window |
| Skip identical deaths | on | Big speedup; turn off if it gives up too early |
| Exact mode | off | Bit-exact but slow on long levels |
| Time per frame | 14 ms | Search speed vs. smoothness |
| Snapshot every N ticks / Max snapshots | 4 / 400 | Memory vs. rewind speed |
| Max repairs | 20 | Verify, then re-search rounds |
| Save location / .gdr.json | macros / off | |
| RNG seed | 1 | Used for random triggers; stored in the .gdr |

## Limitations

- Classic mode only, jump button only. In 2-player levels only player 1 is searched.
  Platformer levels are refused.
- Levels with **random triggers** only replay correctly if your playback bot uses the same
  RNG seed (xdBot's *Seed* option).
- Visuals such as the camera, particles and music are wrong while it searches. Music is muted.
- Start positions work, but the macro then starts from that start position.
- Built and compile-checked, not yet play-tested.

## Building

GitHub Actions builds all three platforms on every push to `main` and publishes a `latest`
release. Locally: `geode build`.

`src/player_state.hpp` is generated. When bumping `bindings-ref` in the workflow, run:

```
python3 tools/gen_player_state.py <bindings>/bindings/2.2091/GeometryDash.bro > src/player_state.hpp
```
