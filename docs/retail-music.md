# Retail TA music: selection, battle status, GOG MP3 shim

Observed behaviour of retail `TotalA.exe` v3.1c (GOG Commander Pack) and the GOG
`win32.dll` CD-audio shim. Clean-room notes: these describe behaviour and cite
addresses only. No code or data from the binaries goes into the engine.

Scripts used (addresses only): `tools/re/music_dis.py` (annotated disassembly),
`music_calls.py` (call-site finder), `music_win32dis.py` (shim disassembly),
`music_hpi.py` (HPI v1 reader, used for `MUSICRT.GUI`/`MUSIC.GUI`/`ALLSOUND.TDF`),
`music_emu.py` (unicorn runs of the retail routines; sub-commands `monitor`,
`select`, `fade`, `tables`).

Confidence tags: **[S]** static only, **[S+D]** static plus unicorn run of the
retail routine, **[U]** unconfirmed or inferred.

## 1. CD player object and track categories

There is one global CD player object (pointer `0x51ff14`, also `game+0x10`). Its fields:

| field | meaning |
|---|---|
| `+0x1fc` | play mode 0..4 (from registry `cdmode`; see §3) |
| `+0x200` | logical track count (audio tracks, data track excluded) |
| `+0x204` | the "Repeat" track (setter `0x4ce580`, clamped to ≤ count) |
| `+0x208` | current logical track |
| `+0x20c` | state: 0 stopped, 1 playing, 2 paused |
| `+0x214+i` | category byte of logical track *i* (set `0x4ce7c0`, get `0x4ce7e0`) |
| `+0x278` | requested category (set `0x4ce690`, get `0x4ce680`) |
| `+0x27c` | music enabled (setter `0x4cedc0`; 0 also stops playback) |
| `+0x280` | data-track offset (1 if CD track 1 is not "audio") |
| `+0x284` | fade step (non-zero while a fade-out is running) |
| `+0x20` | base CD volume 0..0xFFFF |

Category values come from the `TRACKTYPE` button in `MUSICRT.GUI`/`MUSIC.GUI`:
**0 Building, 1 Battle, 2 Victory, 3 Defeat, 4 Unused**. A requested
category of 4 means "silence". [S]

**Track numbering.** CD track = logical track + `+0x280`. `0x4cda00` asks for
"number of tracks". The GOG build patches the reply: a code cave at
`0x4fb92c` writes the string "17" when the MCI call succeeds. The game then
asks for "type track 1". If the reply is not "audio" it sets `+0x280=1` and
reduces the count by one, giving 16 logical tracks for CD tracks 2..17. [S]
The play string is built by `0x4ceb60`: `play cdaudio from <L+off> to <L+off+1> notify`.
The ` to` part is left off when `L+off >= count`, i.e. for logical 15 and 16. On a
real CD, logical 15 therefore plays on into 16 before the notify arrives. [S+D]

**Default category table.** `0x4ce260` (open) first fills `cat[i] = i%4+1` as a
placeholder [S+D]. The real table is chosen by `0x490fe0`, which runs at
startup from `0x491200` and again whenever a CD is inserted:
- It looks up the CD's volume serial (first `DRIVE_CDROM`, `0x4bb190`/`0x4bb260`;
  0 if there is no drive or no disc) in the `CDLISTS` most-recently-used list.
  The list holds 20 entries of 0x88 bytes (serial + per-track bytes) and is
  stored in registry value `CDLISTS` (0xAA0 bytes).
  - If the serial is found, that entry's table is copied into the categories (`0x4ce3e0`).
  - If it is not found, the count is 16 and track 1 is non-audio (`0x4ce460`), the
    built-in table is used: **logical 1-7 = Battle, logical 8-16 = Building**. That is,
    CD tracks 2-8 are battle and CD tracks 9-17 are building. A new MRU entry is
    created. [S]
- On exit, `0x4916a0`/`0x490f80` write the current table into entry 0 and save `CDLISTS`. [S]
- **GOG quirk [S, not run]:** on a fresh install `CDLISTS` is missing, so
  `0x490f40`/`0x491412` zero-fill the list. With no CD drive the serial is 0,
  which matches the zeroed entry 0, so every track becomes **Building (0)**.
  Battle selection then finds no track. The game sends "stop cdaudio", which the
  shim ignores (§4), so the current building track keeps playing. Setting
  per-track types in Options→Music (Custom mode) fixes this, and the fix persists
  through `CDLISTS`.
- No code path ever assigns Victory (2), Defeat (3) or Unused (4) by default.

## 2. Battle vs. building state (battle monitor)

**Event sources.** Points go into a 30-slot ring buffer (`0x51e710`, index
`0x51f2dc`). The adder is `0x494ff0`, which adds to the current slot. [S]
- `+1` for each damage packet (handler `0x489ce0`, call at `0x489de6`) where an
  attacker unit exists and either the attacker's owner or the damaged unit's
  owner is the **local player** (`game+0x2a42`). Repair/heal packets (type 0xA)
  return earlier and add nothing. The amount of damage does not matter.
- `+5` when a unit dies and its last attacker's player (`unit+0xf4`) is the
  local player (unit-death path, call at `0x4869eb`). This is the same block
  that increments the killer's kill count.
- Combat between other players adds nothing, even if it is on screen. There is
  no line-of-sight or view test.

**Sampler `0x494e70`.** It is called every frame from the game loop (`0x4999a1`).
It runs only while the in-game flag is set (`game+0x2a44 & 4`), and it skips the
load/sync phase in which `game+0x38d75` bit 0 is set and bit 1 is clear.
Time is wall-clock: `0x4b6340` = GetTickCount·30/1000, with the rate of 30 set
at `0x49e991`. A sample runs when `now > last+30`, so roughly once per 1.03 s,
frame-quantised. [S+D] Each sample does the following:
1. `counter++` (`0x51f2fc`). If `counter <= 10`, it only advances the ring.
2. `total` = sum of all 30 slots, including the slot still being filled.
   `recent` = sum of the 5 most recently *completed* slots.
3. If the current category is **0**: when (`total > 50` **or** `recent > 30`)
   **and** the local player owns **more than 30 units** ("Total Units" word
   `player+0x1ca7`, player stride 0x14B), the new category is 1.
4. If the current category is **1**: when `total < 10` **and** `recent == 0`
   **and** `counter > 60`, the new category is 0.
5. If the new value differs from the last value the monitor issued (`0x5091d0`,
   initially -1), it calls `setCategory(new)`, sets `counter=0`, and remembers the value.
6. It advances the ring index (mod 30) and zeroes the new slot.

The result is hysteresis. Battle is entered no sooner than about 11 s after
the previous switch. It is left only after about 61 s since the switch, plus 5
quiet seconds, plus fewer than 10 points in the last 30 s. When the current
category is anything other than 0 or 1, the monitor does nothing. [S+D:
`music_emu.py monitor`. With 7 pts/s and 31 units it switched at sample 11; with
30 units it never switched; a single 51-point burst switched and a 50-point burst
did not; a 31-point burst counts only once its slot is complete; battle→build
happened at sample 61; 1 point every 4 s kept battle forever.]

**Per-game reset:** `0x4917d0` (called from the load path at `0x497581`)
clears the ring and its index. It does **not** reset `counter` or the
last-issued value `0x5091d0`. The game load code `0x497f40` calls `setCategory(0)`
at `0x49848b`. **Retail bug [S+D]:** if the previous game in the same process
ended with the monitor's last value at 1, the next game cannot enter battle.
The monitor computes 1, sees it equals the stale last value, and never calls
`setCategory`. Verified with the stale-last run.

## 3. Modes, options, registry

Registry key `HKCU\Software\Cavedog Entertainment\Total Annihilation`. Settings
are loaded in `0x42f9a0` and saved by `0x430f00`, which has 27 callers. [S]

| value | field | default | meaning |
|---|---|---|---|
| `musicmode` | `game+0x37f14` bit 0 | 1 | CD music On/Off (`NOTRAK` button); sets `+0x27c` |
| `cdmode` | `game+0x37f16` → `+0x1fc` | **4** | play mode; GUI `TRACKMODE` stage = cdmode-1 (`0x45d161`, `0x45d67b`) |
| `musicvol` | `game+0x37f10` | **32** | 0..64 (slider max 0x40 at `0x45d88c`) |
| `CDLISTS` | `0x51e828` | zeroed | per-CD category tables (§1) |
| `CDAudioVolume`/`WaveOutVolume`/`RestoreVolume` | | | saved system mixer levels to restore on exit [S, not traced further] |

"Restore Defaults" (`0x45d55b`) sets musicvol 32, mode 4, music On. It does
not reset the track types. [S]

Modes, from the `0x4cdb40` jump table at `0x4ce00c` [S+D `music_emu.py select`]:
- **0** (not reachable from the GUI): stop.
- **1 Play All**: next = current+1. Retail plays logical 17, which does not exist,
  after 16 and then wraps current to 1. On a real CD the failed play means no
  notify, so music stops after the last track.
- **2 Random**: `rand()%count+1` (MSVC rand, `0x4e4870`). The same track can repeat.
- **3 Repeat**: play `+0x204` (chosen with the TRACKNUM/CD buttons). If it is
  already playing, do nothing, so the track loops.
- **4 Custom** ("play by battle status"): play from the requested category.
  `TRACKTYPE` is editable only in this mode (`0x45d239`).

Two overrides in `0x4cdb40` apply in every mode: category **4** means stop,
and category **2 or 3** forces category selection.

**Category selection** (mode 4, or category 2/3; `0x4cde96`): r = rand()&15.
If "playing" and cat[current]==wanted, keep the current track. Otherwise walk
forward cyclically from the current track and take the max(r,1)-th track whose
category matches. With 7 battle tracks this lands on the same track again
2 times in 16; with 9 building tracks, 1 time in 16. If no track matches, stop.
[S+D]

**When selection happens:**
- On the `MM_MCINOTIFY` success notify (`0x4ce030`, msg 0x3B9, wParam 1, state 1,
  not playing): the next track per mode. This is "track end".
- On category change (`0x4ce690`). It first saves the current track into
  `0x51ff20[old]`, which is written and never read. Then it acts only if
  mode==4 or the new category is 2/3:
  - from category 4: set volume and select at once;
  - if a fade is already running: cancel it and select at once;
  - otherwise **fade out**: step = -vol/18 (truncated) every 2 units (1/15 s),
    via timer callback `0x4ce5e0`. Volume hits 0 after 19 callbacks (about 1.27 s).
    If the new category is **0 (Building)**, it waits a further **120 units (4 s)**
    (`0x4ce5b0`) before selecting. Otherwise it selects at once. There is no
    fade-in: play restores the base volume (`0x4cec56`).
  [S+D `music_emu.py fade`: vol 0x8000, step -1820, 19 steps, 120-unit timer
  for battle→build; in mode 1 a 0→1 change does nothing.]
- At game start, `0x498464`: `setCategory(0)`, then select if not playing.
  In mode 4 this is a random building track.
- After CD insertion, while in game (`0x4911d7`).

**Menus / game end / victory / defeat:**
- The front end sets category 4 (`0x426462`). It also plays the GUI sound `BGM`,
  which is `drone2` in `ALLSOUND.TDF`, through `0x47f210`. That routine takes a
  different sound-API path (`0x4cf540`), so the drone is probably looped [U].
  There is no CD music in menus.
- At game end `0x491b60` stops playback (`0x4ced40`) and sets category 4.
  The same happens on the exit paths at `0x4996be`/`0x49986b`, `0x41ed7e` and `0x460602`.
- **Victory (2) and Defeat (3) are never requested by game code.** Of the 12
  direct `setCategory` callers, none passes 2 or 3. The only other way is the
  `MusicMode <n>` console command (table `0x501e1c`, handler `0x4175e0`). [S]
- The in-game menu pauses the CD (`0x460cc0`→`0x4ce910(1)`, "pause cdaudio").
  Closing it resumes from the saved position (`0x4609a0`→`0x4ce910(0)`). [S]

## 4. GOG `win32.dll` shim (MSVC debug build, audiere)

The only real export is `mciSendStringA` at `0x10002140`. `aux*`, `waveOut*` and
`PlaySoundA` are forwarders to winmm. [S]
- The shim **never calls the real MCI**: the forwarding flag is forced to 0 at
  `0x10002438`. It returns 0 for every command and writes no reply string. So
  "status … mode" never returns "playing" (the game reads an uninitialised
  buffer, [U] for exact contents), "number of tracks" relies on the "17" patch,
  and no `MM_MCINOTIFY` is ever posted.
- `play cdaudio from N…`: N = atoi of the text between "from " and the next
  space. It sets playing=1 and requests a stop of the current stream. If
  `music/N.mp3` opens, track=N and the worker thread starts once
  (`0x10001d80`; the thread first sleeps 1000 ms). Otherwise it flags a resume
  of the previous stream at its saved position.
  `stop cdaudio` is **ignored**. `pause cdaudio` sets playing=0 and the worker
  saves the position and stops.
- Worker loop `0x10001330`, every 250 ms. The audiere `OutputStream` vtable slot
  for each call is shown in parentheses:
  - The first stream opens `music/<track>.mp3` with `setRepeat(cdmode != 1)`.
  - A pending stop request saves the position and stops the stream.
  - `setVolume(musicvol/64)` re-reads registry `musicvol` every iteration
    (`0x10001170`; 1.0 if the key cannot be opened).
  - If playing==1 and the stream has ended: for `cdmode==1`, track++ and wrap
    above 17 back to 2. It then opens `music/<track>.mp3` (new streams do not
    repeat), seeks if a resume is pending, and plays.
  - If `cdmode==2` and a track is playing, it keeps pre-picking the next track
    as `rand()%15+2` (2..16).
  - So with any other cdmode (3 or 4) the current file loops until the game
    sends another play command.
  - Quirk: in `cdmode==1`, a new "play from N" after the first one plays
    **N+1**, because stop → not playing → increment.
- File mapping: `music/N.mp3` = CD track N. So 2..8 are battle and 9..17 are
  building (default table). `0.mp3`≡`9.mp3` and `1.mp3`≡`10.mp3` (identical MD5).
  They are never requested by normal play. Plausibly they are fallbacks for
  garbage-parsed resume strings [U].
- Under GOG the game's fades and volume (`auxSetVolume`) have no effect on the
  MP3s. Volume comes only from registry `musicvol`, so it changes once settings
  are saved.

## 5. Volume

The game computes CD volume as `musicvol << 10`, clamped to 0..0xFFFF, and
sets both channels with `auxSetVolume` (`0x4d00d0`, called from `0x45bf4b`,
`0x45d5eb`, `0x42ff2e` and others). The fade-out acts on this value. The GOG
volume is `musicvol/64` as an audiere gain. Music volume is independent of
`fxvol` (default 27, wave `0x4d0070`). [S]

## Engine implementation spec

Implement retail *game logic* (the real-CD behaviour). Do not copy the GOG
shim's quirks. Wall-clock unit: 1 tick = 1/30 s.

1. **Library**: 16 logical tracks, track *L* is file `music/(L+1).mp3`. Default
   categories: L1-7 Battle, L8-16 Building. Allow a per-track category
   override (Building/Battle/Victory/Defeat/Unused) persisted in settings.
2. **Settings**: `music_enabled` (default on), `music_mode` ∈ {PlayAll=1,
   Random=2, Repeat=3, Custom=4} (default **4**), `repeat_track` (default 1),
   `music_volume` 0..64 (default **32**, gain = vol/64).
3. **Requested category** R ∈ {0 Building, 1 Battle, 2 Victory, 3 Defeat, 4 Silence}.
   Menus and front end: R=4 and stop. Game start: R=0, then select. Game end or
   quit to menu: stop and set R=4. In-game menu open: pause; close: resume from
   the same position.
4. **Battle monitor** (local player only):
   - Keep a 30-slot ring. Add +1 per damage event where the attacker exists
     and either side is the local player, excluding repair/heal. Add +5 per
     kill credited to the local player.
   - Every frame, if more than 30 ticks have passed since the last sample:
     `counter++`.
   - If `counter > 10`, compute `total` (all 30 slots) and `recent` (last 5
     completed slots). Then:
     - R==0 and (total>50 or recent>30) and local unit count >30 → want 1;
     - R==1 and total<10 and recent==0 and counter>60 → want 0.
   - If the wanted value differs from R, call setCategory and set counter=0.
   - Advance and clear the ring slot.
   - Reset the ring **and** counter/last at game start. Retail does not reset
     them (§2 bug); fix it.
5. **setCategory(new)**: record it. Act only if mode==Custom or new∈{2,3}:
   - Fade out linearly over 18 steps of 2 ticks.
   - If new==0, wait a further 120 ticks.
   - Then select a track and start it at full volume. No fade-in.
   - If the old category was 4 or a fade is in progress, select immediately.
6. **Select next track** (at track end, game start, or after a category change):
   - R==4 → stop.
   - R∈{2,3} or mode==Custom → category pick: r=rand()&15, k=max(r,1). Walk
     forward cyclically from the current track and take the k-th track whose
     category is R. If there is none, stop. Do not restart the current track
     if it is still playing and matches.
   - Otherwise by mode: PlayAll = current+1 (wrap 16→1; retail stops at the
     end, recommend wrapping); Random = uniform 1..16, repeats allowed;
     Repeat = repeat_track looped.
   - Each play covers exactly one track. Track end triggers this step again.
7. **Victory/Defeat** categories exist but retail never requests them. Keep them
   out of the default flow, and optionally expose them as a debug/console command.
