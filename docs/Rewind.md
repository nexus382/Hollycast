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
| Enable Rewind | Off | Off / On | Records gameplay history when you start a supported game. |
| Snapshot Interval | 0.25 s | 0.1 / 0.25 / 0.5 / 1 s | Time inside the game between captures. Shorter intervals give more precise steps and increase CPU and memory use. |
| History Duration | 30 s | 5–120 s | Target amount of gameplay to keep. Shorten it to use less memory. |
| Compression | Fast | Fast / Balanced / Maximum | Fast spends the least CPU time. Balanced and Maximum spend more CPU time to try to keep snapshots smaller. |

Start with the defaults. If gameplay slows down, keep **Fast** compression
and try **0.5 s** or **1 s** between snapshots. If memory use is the issue,
shorten **History Duration** first. Use **0.1 s** when you want finer steps
and your device has room for the extra work.

## How history works

Hollycast periodically captures the emulated machine's state and compresses
it on a background worker. If the worker is busy, Hollycast skips that
capture. When history reaches the configured duration or memory ceiling,
the oldest snapshots are discarded. The available history can also be
shorter after a save or other event that clears it.

Memory use grows with longer duration, shorter intervals, and larger game
states. Hollycast retains up to 256 MiB of
compressed history on 32-bit, Android, iOS, Switch and UWP builds, and up to
2 GiB on other 64-bit builds. Capture and playback also use temporary memory.
Each raw snapshot is limited to 128 MiB. Allocation, serialization, or
compression failures stop rewind and report an error.

## Controls

- **Hold to rewind.** Hold the **Rewind (hold)** button under
  **Settings > Controls** to move backwards; release it to continue from
  there. A new keyboard mapping binds it to Backspace. A saved mapping from
  an older install has no binding until you assign one.
- **Step Back.** The in-game Commands menu has a **Step Back** button. It
  restores the most recent snapshot earlier than the current position and
  resumes gameplay. It becomes available once there is an earlier snapshot.

Holding Rewind plays back older snapshots one by one and discards the newer
snapshots along the way. Audio is muted while snapshots play back. At the
oldest snapshot, emulation waits. Release the button to continue playing from
the restored point.

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
- Exporting an arcade printout starts a new history so rewinding cannot
  replay the command that wrote the image.

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

Open the in-game Commands menu to see why Rewind is unavailable. The first version
does not run:

- on Hikaru or System SP;
- in RetroAchievements hardcore mode;
- during online play, Capcom Direct matchmaking, rollback (GGPO), Battle
  Cable, or linked-cabinet play;
- with a NetDIMM cartridge, arcade card reader, or medal hopper;
- with host serial output enabled;
- in Fish Life or Alien Front, which use serial devices whose state cannot be rewound;
- with passthrough DreamLink hardware connected;
- while phone VMU sharing is running;
- in the libretro core. Use your frontend's own rewind controls there.

When Rewind does run, it uses shared emulation/rendering instead of threaded
rendering, which can reduce performance. Your stored multi-threading
preference is not changed; it applies again to sessions without Rewind. The
on-screen touch controls have no Rewind button in this version; use Step
Back or a mapped physical/keyboard button.

## If Rewind is missing or history is short

- **Step Back is gray.** Check the reason shown in Commands. If Rewind is
  enabled and available, play a little longer to record an earlier snapshot.
- **The keyboard or controller doesn't rewind.** Open **Settings > Controls**,
  edit the device's mapping, and assign **Rewind (hold)**.
- **Rewind stops at a recent point.** An in-game save, reset, or other history
  boundary may have cleared older snapshots. The configured duration is the
  target for uninterrupted gameplay.

## For testers

The native test harness can hold and release Rewind in disposable sessions.
Set `Dreamcast.RewindEnable = true` before launching a supported game.
Hollycast selects shared threading for the session. The harness `rewind`
command is documented in [the testing guide](../tools/testing/README.md).
