<!-- Portions Copyright 2026 The Hollycast Authors -->
# Rewind

Rewind keeps recent gameplay in memory so you can undo a mistake: hold a
button to move backwards through recent history, then release to keep playing
from that point. It is an original Hollycast feature, written against
Hollycast's own save-state, rendering, audio, and input systems. The
hold-to-rewind interaction is modeled on the behavior popularized by
DuckStation; no DuckStation source was used.

## Enabling Rewind

1. Open **Settings > Advanced > Rewind** and turn on **Enable Rewind**.
2. Start the game. The setting takes effect the next time a game starts, and
   it can be changed per title with per-game settings.

Rewind is off by default. While a game is running, the enable toggle and the
settings below are locked, because changing them clears recorded history.

| Setting | Default | Options | Meaning |
| --- | --- | --- | --- |
| Snapshot Interval | 0.25 s | 0.1 / 0.25 / 0.5 / 1 s | Guest time between snapshots |
| History Duration | 30 s | 5–120 s | How far back rewind can go |
| Compression | Fast | Fast / Balanced / Maximum | Trade capture CPU for memory use |

History Duration sets the target window. Recorded history can be shorter
when compression cannot keep up or the internal memory ceiling is reached.
Memory use grows with longer duration, shorter intervals, and larger game
states. Compression controls
how much memory that same duration needs: Fast favors smooth gameplay,
Balanced and Maximum spend more CPU per snapshot to store history smaller.
Capturing also needs temporary raw-state and compression workspace, each
bounded at roughly 128 MiB. A fixed internal safety ceiling still protects
the device from runaway allocation. Allocation, serialization, or compression
failures stop rewind and report an error.

## Controls

- **Hold to rewind.** Hold the **Rewind (hold)** button under
  **Settings > Controls** to move backwards; release it to continue from
  there. A new keyboard mapping binds it to Backspace. A saved mapping from
  an older install has no binding until you assign one.
- **Step Back.** The in-game Commands menu has a **Step Back** button. It
  restores the previous snapshot and resumes gameplay.

Moving backwards discards the abandoned future for good. At the oldest
snapshot, holding simply waits; it never wraps or advances the game.

History lives in memory only. Stopping the game, loading a state, resetting,
changing discs, reconnecting controller devices, or changing Rewind settings
clears it. RetroAchievements hardcore behavior is unaffected because Rewind
turns itself off there.

## Saves stay on the current timeline

Rewinding must never resurrect an old save over a newer one:

- Guest writes to durable storage — VMU card blocks, system flash/NVRAM,
  Naomi/Atomiswave SRAM and EEPROM — end recorded history. Snapshots taken
  before such a write are dropped instead of being restorable across it.
- As a second line of defense, rewinding does not restore the contents of
  persistent flash/SRAM chips at all. They keep their live contents, so a
  later pause or stop always saves the newest data.

Normal save states are unchanged:

- **Making a manual save state keeps rewind history.** You can save, keep
  playing, and still rewind to before the moment you saved. The save file
  itself is kept exactly as you made it.
- **Loading a save state clears rewind history.** History is not stored
  inside save-state files: with dozens of full snapshots it would add
  hundreds of megabytes to every save and break compatibility with other
  builds. When you load a state, recording restarts from that point.
- **Autosave captures where you are.** If you rewind and then exit, the
  exit autosave (and Continue Playing resume) uses the rewound position,
  not the furthest point you had reached before rewinding.

## When Rewind is unavailable

The Commands menu and the hold button explain the reason. The first version
does not run:

- on Hikaru or System SP;
- in RetroAchievements hardcore mode;
- during online, rollback (GGPO), or linked-cabinet play;
- with passthrough DreamLink hardware connected;
- while phone VMU sharing is running;
- in the libretro core. Use your frontend's own rewind controls there.

When Rewind does run, it uses shared emulation/rendering instead of threaded
rendering, which can reduce performance. Your stored multi-threading
preference is not changed; it applies again to sessions without Rewind. The
on-screen touch controls have no Rewind button in this version; use Step
Back or a mapped physical/keyboard button.

## For testers

The native test harness can hold and release Rewind in disposable sessions.
Enable it through the profile keys (`Dreamcast.RewindEnable` and friends,
with `rend.ThreadedRendering = false`) and drive it with the `rewind`
command. See [the testing guide](../tools/testing/README.md).
