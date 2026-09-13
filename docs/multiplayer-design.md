# Multiplayer Design: Client–Server, 8 Players, Teams

Status: **proposed** (v1 scope agreed 2026-09-04; revised after adversarial
design review). Supersedes the 2-player peer-to-peer lockstep in
`src/net/lockstep.{h,cpp}` as the multiplayer story; the deterministic-sim
substrate underneath it is kept.

## 1. Goals

- **Client–server topology.** All clients make one outbound TCP connection to a
  central server (`takserver`) reachable on the LAN or internet. Everything —
  lobby, game traffic — is proxied through it. No NAT traversal or port
  forwarding on any player's side.
- **Up to 8 players per game**, any mix of humans and AIs.
- **Up to 8 teams.** Default: every player/AI on its own team (free-for-all).
  Players on the same team share vision and radar (fog of war and minimap).
- **Own color per player** (the retail art ships exactly 10 color variants).
- **AI players run on the server**, not on the host's machine. The host picks
  them in the lobby; the server simulates and commands them.
- **Control isolation:** a player commands only their own units — allies
  included cannot be commanded.
- **Named games with optional passwords**, chosen by the host.
- **Game browser:** the server lists joinable (lobby-state) games and live
  (running, unjoinable) games.
- **Durable sessions:** a dropped player's slot is held; they can rejoin and
  the client catches up automatically.

Non-goals for v1 (tracked in §10): spectators/watch mode, in-lobby map file
transfer, allied mana sharing / unit gifting, mid-game diplomacy, persistent
accounts or ratings, wire encryption (deploy behind a TLS proxy — and on
untrusted networks that proxy is *not* optional, see resume tokens in §4).

## 2. What exists today (and what survives)

The engine already has the hard part: a **deterministic 30 Hz sim** where only
commands cross the wire, verified in 2-player lockstep with periodic FNV-1a
state hashes. The sim proper is RNG-free, the COB VM fixed-seeded, and
`stateHash()` already iterates all players generically.

| Keep | Replace |
|---|---|
| Deterministic `World::tick` at fixed 1/30s | `net::Session` 2-peer host/client class |
| 35-byte `Command` wire struct + serialization | `--host`/`--join` out-of-band setup (identical command lines) |
| `issue()` → `apply()` command funnel with per-unit `owns()` check | Trusting the wire-supplied `Command.player` byte |
| Length-prefixed framing (u32 len + u8 kind) | The 3-message protocol (Hello is sent but never checked) |
| Periodic state hashing (every 30 ticks) | Desync-is-fatal handling (becomes referee + auto-resync) |

Known command-path leaks that must be fixed regardless (they desync even the
current 2-player mode): **Ctrl+D** (self-destruct writes `hp=0` directly) and
the **F9/F11** stress-spawn keys. Self-destruct becomes a real `Cmd::Destroy`;
the stress keys are disabled in networked games.

## 3. Architecture overview

```
   takclient (client)  ─┐
   takclient (client)  ─┼─ TCP ──►  takserver ── per-game: GameRoom
   takclient (client)  ─┘             │  ├─ command sequencer (tick authority)
                                    │  ├─ headless World (referee + AI host)
                                    │  ├─ ai::Controller per AI slot
                                    │  └─ bundle log (reconnect + replay)
                                    └─ lobby: game registry, browser, chat
```

**Server-sequenced deterministic lockstep.** Every participant that needs game
state — each client, and the server itself — runs the identical sim. Clients
send their commands to the server; the server is the *only* clock: it assigns
every command (human or AI) to a tick, closes each tick on a fixed 30 Hz wall
schedule, and broadcasts one `TickBundle` per tick to all clients. A client may
only advance its sim through tick T once it has bundle T, and **clients never
apply their own commands locally** — every command takes the round trip and
executes only when its bundle arrives (the current `issue()` funnel already has
this shape).

Causality note: a command issued against the client's view of tick T executes
at server tick S ≥ T (latency ≈ RTT + at most one bundle interval). This is
standard relay-sequenced lockstep, and safe because commands are *intent*
(move-to, attack-id) validated at execution time — a stale reference (unit died
in between) degrades to a no-op via the existing `owns()`/unit-lookup checks in
`apply()`. The server additionally pre-validates `owns()` against the referee
sim at the closing tick, so accept/reject is identical on every sim.

**Sequenced server events.** Anything that affects sim state but originates
from wall-clock server decisions — a player forfeiting when their reconnect
grace expires, a player leaving voluntarily, (later) an alliance change — is
**not** a side-band notice: it is an *event record inside the TickBundle*,
executing at that bundle's tick on every sim, and therefore part of the log.
This is what keeps three properties simultaneously true: the log replays to
the same end state, a rejoiner reconstructs the same world as live clients,
and the referee's hash stays comparable. (Purely informational notices —
connected/dropped indicators, ping — remain side-band `PlayerStatus`
messages; they don't touch the sim.) The bundle format carries its (possibly
empty) event section **from the first server milestone** so later features
never break the wire or log format.

Why this model (vs. authoritative state replication):

- Bandwidth is tiny and flat: ~35 bytes per command, one small frame per tick.
- The deterministic sim already exists and is verified; replication would mean
  serializing full World state and rewriting interpolation.
- **AI-on-server falls out for free**: the server already runs a sim per game
  (as referee), so an AI player is just another command source feeding the
  sequencer — exactly like a human, subject to the same validation.
- **Reconnect nearly falls out for free**: the sequenced bundle log *is* the
  game; a rejoining client can replay it headless to catch up (§4 for the
  honest cost model and the snapshot fallback). A log written to disk is a
  replay file.

Clients never gate on *each other*. A slow or stalled client only delays
itself — the server keeps closing ticks on schedule and other clients are
unaffected. Honestly stated, though: that client's own command-to-effect
latency equals its lag (10 s behind ⇒ its orders visibly land ~10 s later on
its own screen), so the client HUD shows a "behind by N s" indicator, and past
a threshold (default ~10 s of unacknowledged ticks) the server drops it into
the reconnect flow rather than let it play unplayably.

**Client jitter buffer + adaptive playout.** Bundles are sequenced by the server
but *arrive* unevenly (network jitter, TCP retransmits). Draining "play every
bundle the instant it lands" turns each late bundle into a visible micro-stall.
So the client keeps a small receive buffer and paces playout on its own wall
clock, staying `netDelay_` bundles behind the newest received; a late bundle is
covered from that reserve, and only a gap deeper than the reserve stalls. This
is pure pacing — the same bundles play in the same order, so every sim and hash
stays byte-identical (`mpStep()` in `src/client/main.cpp`).

Two refinements make it self-tuning:
- **Auto-sizing** (the default; `TAK_NET_DELAY` overrides — `0` disables the
  buffer, a positive integer pins a fixed depth): an active RTT probe (one
  ping/sec) plus a decaying-max of the measured bundle-arrival jitter size the
  reserve each frame — `clamp(2 + max(kJit, kRtt), 2, 16)` bundles — so a clean
  LAN sits at 2 and a jittery WAN grows the cushion only as needed.
- **Servo playout** holds the reserve *at* that target instead of drifting: the
  sim clock runs slightly fast when the buffer is deep (shed latency) and slightly
  slow when it is shallow (rebuild the reserve) — `rate = 30·clamp(1 + 0.06·(buffered
  − target), 0.7, 1.3)` Hz. Equilibrium is exactly `buffered == target`, so added
  latency stays tight rather than accreting. A backlog deeper than target+60 (a
  rejoin replay, or a client that fell behind) is fast-forwarded, not paced.

Only game ticks are gated by this buffer; camera, zoom, and edge-scroll run every
render frame in `cameraFrame(dt)`, independent of the sim, so input feel is never
coupled to link latency.

*Test harness.* A synthetic link model injects receive delay on the client with
no server needed: `TAK_NET_BASE_MS` (constant one-way), `TAK_NET_JITTER_MS`
(uniform 0..N added), `TAK_NET_LOSS_PCT` (probability of a ~2·base retransmit
spike). `TAK_NETBENCH=1` runs the headless client at a fixed 60 fps and reports
`delay`, measured `rtt`, frames, and starved-frame `stalls`. Measured over 15 s
of an `--mpai` game, the servo cuts stalls roughly 10–12× on lossy links
(base60+jit60+loss5%: 28.2% → 2.3%; base80+jit120+loss10%: 61.2% → 6.0%) while
holding delay at 4–5 bundles.

### Trust model

The server stamps `Command.player` from the connection it arrived on and
discards whatever the client claimed — closing today's spoof hole (any client
could set `player=0` and command the opponent). Sim-level `owns()` checks
remain in every client as defense in depth. The server's own sim gives it a
canonical state hash for attribution — with one humility rule (§4, referee
suspicion): when everyone disagrees with the referee but agrees with each
other, the referee suspects *itself*.

### Game-data agreement + override policy

Every peer reads its game files through a VFS over a retail install directory
(`hpi::mountRetailRoot`: root `*.hpi` + `Maps/*.kmp` + `Music/` + `overrides/`,
resolved by the retail newest-date rule). Lockstep needs every sim to feed on
byte-identical *gameplay* data, so that's fingerprinted and agreed:

- **`hpi::gameplayHash(vfs)`** — a content hash over just the files the sim reads
  (unit `.fbi`, weapon/side/game `.tdf`, build lists, features; **not** maps, which
  are per-game, and **not** cosmetics — art, models, animation, sound, music, GUI).
- Each game carries an **override tier** (`GameOptions.overridePolicy`): `none`
  (pure retail), `cosmetic` (art/sound overrides allowed, never hashed, may differ
  per player), or `full` (gameplay overrides allowed but everyone must have the
  same ones). The host picks it; joiners **adopt** it, remounting their VFS to the
  tier at game start so their registry matches the referee's.
- Enforcement is two-stage. The **`Hello`** carries the client's *pure-retail*
  gameplay hash (mounted with no overrides), so the base game files must match for
  everyone regardless of tier — a modified retail file is rejected at connect. At
  **`Loaded`** the client sends its gameplay hash *at the room's tier*, and the
  referee (mounted at that tier) rejects a mismatch — so a `full`-tier player
  lacking or differing on a gameplay override is caught before tick 0 rather than
  as a mid-game desync. A pure relay (no `--data`) has no referee hash and falls
  back to the live `StateHash` cross-check.

### Determinism contract (explicit)

Lockstep at 8 players lives or dies on bit-identical simulation, so the
contract is stated, enforced, and narrow:

- **Identical binary required.** The `Hello` build id identifies the exact
  binary (content hash), not a version string. Cross-platform / cross-build
  play is **unsupported** until the sim's floating-point math is made
  implementation-independent. The practical deployment is: everyone runs the
  same release build for their platform *and* the sim is hardened as below.
- The sim (and anything it calls) is compiled without `-ffast-math` and with
  `-ffp-contract=off` pinned in CMake for those targets.
- The sim's transcendentals (`sin`/`cos`/`atan2`/`sqrt` in movement/turning)
  route through a small deterministic math shim (`sim/detmath`) rather than
  raw libm — libm results differ across implementations and versions even on
  the same architecture. Until the shim lands, same-binary-same-libm is the
  requirement and the doc says so honestly.
- Data gate: `Hello` carries a checksum over the loaded unit registry + build
  tree + map; the server refuses mixed-data games with a readable error.
- The AI's RNG is outside the sim and cannot desync the world — only its
  *commands* matter, and those are sequenced like everyone else's.
- *(Historical — the flow fields are gone; the invariant is not.)* The
  flow-field pathfinding cache stays a **pure memo**: field content must
  remain a function of (nav grid, goal, domain) only — never of when or
  whether it was built — because the server's AI queries build cache entries
  clients never build. `pathExists` stays query-only; cache access is
  single-threaded per room (debug-asserted). This invariant is what makes the
  `mutable` cache safe; breaking it desyncs only server-vs-client
  configurations, the nastiest kind of bug, so it's load-bearing and
  documented in the code.
- Fog of war is excluded from `stateHash` (render-only), so per-client team
  fog cannot desync; the referee runs with **no vis player set** and skips the
  visibility pass entirely (the AI is omniscient and nothing on the server
  renders).

## 4. The server (`takserver`)

A new headless binary. Needs the same game data as clients (it runs the sim
and the AI): point it at the extracted data directory. No SDL, no rendering —
link `sim`, `tdf`, `net`, and the new `ai` module only.

- **Lobby service.** Holds the game registry. Handles create / list / join /
  leave / slot changes / chat / start. One TCP port (default 7677) for
  everything; a connection is in lobby state or attached to one game. If the
  host disconnects **pre-start**, host powers pass to the next human in the
  lobby (the game dissolves if none remain).
- **GameRoom.** One per game: player slots (up to 8), the sequencer, the
  headless referee sim, one `ai::Controller` per AI slot, the bundle log, and
  per-game settings (map, Crusades balance, gods on/off).
- **Tick loop.** Fixed 30 Hz per room. Each tick: collect queued client
  commands (stamped, validated, capped — §5 hardening), run each due AI
  controller (they emit commands into the same queue), append due sequenced
  events (forfeits, leaves), close the bundle, append it to the log, broadcast,
  advance the referee sim. AI controllers think once per 30 ticks (1 s cadence,
  matching today's AI), staggered across AI slots. On `Resume` after a pause
  the tick closer rebases its wall-clock schedule — no burst of catch-up ticks.
- **Desync referee.** Clients send `StateHash` every 30 ticks. Clients lag the
  referee by RTT + buffering, so the server keeps a bounded ring of its own
  per-tick hashes to compare against late-arriving client hashes. A
  mismatching client gets a `Desynced` notice and enters the reconnect flow —
  which *is* the recovery, since replay reconstructs correct state.
  **Referee suspicion rule:** if a hash round shows the clients agreeing with
  each other and disagreeing with the referee, the server suspects its own sim
  (e.g. a server-only AI code path bug), logs loudly, stops dropping clients,
  and flags the room for investigation instead of drop-looping every innocent client.
- **Hash breadth.** To attribute desyncs quickly, the extended `stateHash`
  (§6) also folds in projectile owner + quantized position and order targets —
  counts alone let same-count divergences hide until damage lands.
- **Disconnect & pause policy.** On disconnect the slot is held and the
  player's empire runs itself — this needs **no sim work** (all unit behavior,
  auto-acquire, repeat-train, economy already lives in `World::tick`; the
  client only ever issued commands). Consequence owned: a dropped player's
  repeat-train keeps spending their mana during the grace window. The game
  **auto-pauses** on a human drop with a per-player pause budget (default
  2 min of pause per player per game) and a host "resume now" override — so a
  flaky client cannot repeatedly freeze seven other people, and past the budget the
  game continues without them. Reconnect grace (slot held, default 5 min,
  host-configurable) keeps counting during pause. Grace expiry emits a
  sequenced **Forfeit** event (§3) — from that tick, on every sim, the
  player's units become inert or self-destruct per lobby setting, and defeat
  logic proceeds.
- **Reconnect.** A rejoining client authenticates with the game id + a resume
  token: **≥128-bit CSPRNG**, single-use (rotated on every rejoin), and the
  server kills any prior half-open socket for that slot on success. It then
  receives the log and fast-forwards headless while buffering live bundles.
  **Honest cost model:** replay time ≈ elapsed ticks × average tick cost;
  late-game ticks measurably reach several ms (the sim's own stall telemetry
  triggers at 15 ms), so a 30-minute game may replay in minutes, not seconds —
  measured in M5, and mitigated by the planned fallback: **periodic
  server-side state snapshots** (World serialization written only by the
  referee, read only by rejoiners — a far smaller ask than full state
  replication, and invisible to the protocol). Non-convergence is detected: a
  client whose replay throughput stays below ~2× live rate is told plainly
  "machine too slow to catch up" instead of drop-reconnect-looping forever.
- **Log format.** Bundles including events, prefixed by a header: format
  version, build id, data checksum, and the full `GameStarting` payload (slot
  table, map, options, per-AI seeds) — that header is exactly what makes a log
  a self-contained replay file. Logs are memory-bounded per room (command data
  is ~KB/min; caps stated in config anyway) and optionally persisted.
- **Scaling.** One thread per GameRoom plus one I/O thread (poll/epoll).
  Late-game rooms cost real CPU (double-digit-ms ticks were the reason the
  sprite auto-tuner exists), so capacity claims wait for the M4 measurement;
  rejoin fast-forwards run niced/capped so they can't starve live rooms.

## 5. Protocol

Framing (kept): `u32 LE length` + `u8 kind` + payload, over one TCP stream
with `TCP_NODELAY`. All multi-byte integers little-endian. The `Command` wire
format (35 bytes) is kept, with the caveat that the `player` byte is
server-assigned on receipt.

**Handshake — actually enforced this time.** `Hello` carries protocol version,
engine build id (binary content hash), and the data checksum. The server
rejects mismatches with a human-readable reason. (Today's kHello is sent and
silently skipped.)

### Lobby messages (client ⇄ server)

| Msg | Payload | Notes |
|---|---|---|
| `Hello` / `Welcome` | proto ver, build id, data hash / assigned session id | version + data gate |
| `SetName` | display name | no accounts in v1 |
| `ListGames` / `GameList` | — / entries: id, name, map, players/capacity, state (lobby/running), passworded, uptime | running games listed but unjoinable |
| `CreateGame` | name, password?, map id, options (crusades, gods, speed, unit cap, forfeit rule) | creator becomes host |
| `JoinGame` / `JoinResult` | game id, password? / slot or error | error: full, bad password, running |
| `SlotUpdate` | slot #, {human/AI/closed}, faction, color, team, ready | players set their own; host sets any (host resolves conflicts, assigns AIs) |
| `LobbyState` | full slot table + settings | broadcast on every change |
| `SetGameOptions` | full `GameOptions` | host only; in the lobby the server adopts them and rebroadcasts `LobbyState`; in-game only `speed` takes effect (if `speedUnlock`), pushed via `SpeedUpdate` |
| `Chat` | text | lobby and in-game |
| `Kick` | slot # | host only |
| `StartGame` | — | host only; server validates: every human ready, colors unique, players ≤ map start positions |
| `GameStarting` | final slot table, map id, options, per-AI RNG seeds, resume token | clients load and report `Loaded`; server starts when all humans are loaded; a `Loaded` timeout kicks that player back to the lobby and re-opens the slot (host decides: re-fill or start without) |

### Game messages

| Msg | Payload | Notes |
|---|---|---|
| `PlayerCommands` | n × Command | client → server; player byte ignored/overwritten |
| `TickBundle` | tick u32, n × Command (ascending player, then arrival order), m × Event | server → all clients, 30/s; empty bundles are the clock; the event section exists from day one (§3) |
| `StateHash` | tick u32, hash u64 | client → server, every 30 ticks |
| `Desynced` | tick, reason | server → one client; client auto-reconnects |
| `Pause` / `Resume` | cause (host / drop-grace), pausing player | server stops closing ticks; budgets in §4 |
| `SpeedUpdate` | speed (tenths, 10 = 1×) | server → all; host changed speed mid-game (needs `speedUnlock`); re-cadences tick pacing, not the per-tick `dt`, so the sim stays deterministic |
| `PlayerStatus` | slot, {connected, dropped, left} + ping | informational only — never sim-affecting (sim-affecting lifecycle is an Event in the bundle) |
| `Rejoin` | game id, resume token | then `LogChunk`* (compressed) → live bundles |
| `Bye` | reason | clean shutdown both ways |

Bundle **Event** records (u8 kind + payload): `Forfeit{player}` (grace expired
or explicit surrender), `Leave{player}` (voluntary, units per forfeit rule),
reserved: `AllianceChange` (v2 diplomacy). Defeat itself is *not* an event —
it's derived deterministically inside the sim from unit state, so every sim
concludes it independently on the same tick.

New `Cmd` kind: `Destroy` (self-destruct via the command path). No other new
kinds are required — the AI's four world mutations (train, 2× startBuild,
attackMove) all map onto existing `Train` / `Build` / `AttackMove` commands.

### Server hardening (v1, not later)

- Frame-length cap (e.g. 64 KB) — the inherited parser trusts a u32 length up
  to 4 GB and grows its buffer unbounded; reject oversized frames, drop the
  connection.
- Per-player per-tick command cap (e.g. 64) so a spammer can't inflate every
  bundle for eight clients; excess commands are dropped with a notice.
- Full validation of remote input: `Cmd` kind range, `type` strings looked up
  against the registry, unit/target ids sane. Malformed ⇒ disconnect.
- Lobby rate limits (CreateGame, Chat, ListGames floods).
- Keepalive with numbers: client pings every 2 s; either side times out after
  15 s of silence. Load-bearing during pause, when no bundles flow.
- `--server <host>` resolves via `getaddrinfo` — IPv6 and hostnames (today's
  `join()` is IPv4-literal-only).

## 6. Sim changes (engine-wide, benefits single-player too)

1. **8 players.** `players_` grows from the hard-coded 4 to 8 (sized from
   game setup); add a bounds guard in `World::spawn` — today every
   `players_[u.player]` index is unchecked. Grow the F4 panel, god-check
   loops, and scenario arrays that assume 4. Colors need **no structural
   work**: `colorSlot_[8]` already covers 8 players, the `& 7` masks become
   correct bounds guards, and each slot's value spans the 10 retail palette
   variants — every player picks any of the 10 colors, and with 8 players
   lobby-enforced uniqueness is always satisfiable.
2. **Team layer.** `Player::team` (0–7, set at game setup; default = player
   index) + `World::allied(a, b)`. Convert friend/foe tests to `allied()`:
   splash damage, target auto-acquire, auras, resurrect, cloak proximity,
   capture, and the viewer's attack-click/AI target scans. `owns()` stays
   per-player — allies cannot command each other's units. `team` is immutable
   in v1; if diplomacy ever makes it mutable, the change must be a sequenced
   event **and `team` must join `stateHash`** (noted in §10 so we don't paint
   into that corner).
3. **Shared team vision.** The single fog grid design survives: it is
   render-only and excluded from `stateHash`, so each client computing *its
   own team's* fog is desync-safe. The one-line core change is the visibility
   pass filter (`u.player != visPlayer_` → `!allied(u.player, visPlayer_)`),
   plus converting every viewer-side `u.player != localPlayer_ &&
   !cellVisible` render/minimap gate to "not on my team" so allied units draw
   through fog. Radar is already merged into the reveal radius, so shared team
   radar is free. The referee runs with no vis player and skips the pass.
4. **Win condition: last team standing — and defined defeat.** Replace the
   hardcoded 2-player check with per-team alive counts, computed **inside the
   sim** so every sim (and the referee) concludes it independently on the same
   tick. A player is defeated when they have no living units; a *forfeited*
   player's units are disposed per the lobby's forfeit rule (inert or
   self-destruct). A defeated-but-connected human stays in the game with their
   **team's** vision until the game ends (their team fights on) or leaves via
   `Bye`; in an FFA their defeat ends their game — no free spectating
   (spectators are v2). A team is out when all its players are; the game ends
   when one team remains. Ally vision shrink on defeat needs no work — fog is
   recomputed from living units every pass.
5. **N-player spawn.** Generalize the 2-monarch setup: assign map start
   positions (`.ota` `StartPosN` specials) to players — allies clustered on
   adjacent starts when possible — with per-player faction kits and starting
   mana.
6. **Hash coverage.** Extend `stateHash` with veterancy, reload timers, god
   favor, projectile (owner, quantized position), and order targets — counts
   alone hide same-count divergences, and fast attribution is the referee's
   whole job.

## 7. AI extraction (`src/ai/`)

The skirmish AI (~200 lines in `GameView`, `main.cpp:2926–3131`) moves to a
new module, one `ai::Controller` per AI player:

```cpp
namespace tak::ai {
  Profile loadProfile(path);              // parses ai/default.txt weight/limit
  class Controller {
    Controller(int player, const sim::TypeRegistry&, const Profile&, uint32_t seed);
    void tick(sim::World&, uint32_t simTick, const CommandSink&);  // emits net::Commands
  };
}
```

- **Commands, not mutations.** The AI's four direct `world_` calls become
  emitted `Command`s through the sink — on the server they enter the sequencer
  like any client's; offline they feed straight into `apply()`. Same code both
  places. (This also eliminates the class of server-only direct-mutation bugs
  that would make the referee itself desync — see §4's suspicion rule for the
  backstop.)
- **Sim-tick cadence.** Today's `aiTimer_ -= dt` is wall-clock (frame-rate
  dependent — not replay-safe). The controller thinks every 30 sim ticks.
- **Seeded RNG.** The private LCG keeps the AI deterministic, but seed it per
  game + player (from `GameStarting`) instead of the constant, so eight AIs
  don't mirror each other. AI RNG stays outside the sim.
- **World access.** Add const accessors the AI needs (`player()` const, the
  mana-spot list — the AI currently reads the *viewer's* copy) and make the
  flow-field cache `mutable` under the purity invariant of §3 (memo-only,
  single-threaded per room, debug-asserted).
- **Omniscience stays.** The AI ignores fog (positional intel only; its
  economy is honest). That matches retail-era AI and avoids per-AI fog grids.
- Vestigial members from the pre-rewrite AI (`aiKeepId_`, `aiCycle_`, …) are
  dropped during the move.

Single-player keeps working identically: `GameView` instantiates local
Controllers feeding `apply()` — same behavior, and it proves the sink
interface before the server exists.

## 8. Client changes (`takclient`)

- **Server browser** (new screen or `--server <addr>` entry): game list with
  name, map, players/capacity, lobby/running state, lock icon; create-game
  dialog (name, password, map picker showing start-position capacity,
  options).
- **Lobby screen:** slot rows (name/AI/closed, faction, color swatch, team
  number, ready check, ping), chat, host controls (add AI, kick, close slot,
  start). Players edit their own row; host edits any.
- **In-game:** allied units render through shared fog and show on the minimap;
  score/status panel grows to 8 players with team grouping; net status HUD
  (tick, ping, **behind-by indicator**, paused-by, reconnecting-player
  banners); chat overlay.
- The `--host`/`--join` peer mode is removed once the server path works
  (`net::Session` retired); `--server`, `--name` added. LAN play = run
  `takserver` on any machine on the LAN.

## 9. Maps, capacity, colors (retail facts)

- Capacity comes from counting `StartPosN` specials in the map's `.ota` — not
  `numplayers`, which is list-valued ("2, 4, 6, 8") or empty. The lobby caps
  human+AI slots at that count.
- Retail ships 55 skirmish maps: 6 support 2 starts, 9×3, 22×4, 4×5, 5×6, and
  **9 maps support the full 8** (e.g. Angvir's Maze, Black Heart Jungle,
  Sewers of Elam). The 8-player cap is chosen to match this retail ceiling —
  no shipped map has more than 8 starts, so no synthetic spawn-point
  fallback is needed and every player count has real maps.
- 10 colors confirmed in the retail art (10-frame GAF color sequences
  throughout; the engine already samples `playerColors_[10]`) — 8 players
  picking among 10 colors means uniqueness never runs out of options.

## 10. Retail parity backlog (post-v1)

The retail binary confirms these existed; all are natural extensions of this
design and none block v1:

- **Spectators** ("Watching" lobby checkbox, host-deniable) — a watcher is a
  bundle subscriber with no slot; needs per-team fog toggling.
- **In-lobby map transfer** to joiners missing the map.
- **Alliance economy:** `AutoShareMana` with limit/percentage, unit gifting
  (`ShareUnits`).
- **Mid-game diplomacy:** requires mutable `Player::team` as a sequenced
  bundle Event *and* `team` entering `stateHash` (reserved in the Event enum
  now precisely so the wire format doesn't break later).
- **God-game chance:** retail rolled a 10% chance a multiplayer game is
  god-enabled (`gods.tdf GameChance=0.1`) — ours is a lobby toggle with an
  optional "retail random" mode.
- **Rejoin snapshots:** referee-written World snapshots to cut rejoin time for
  long games (the log header/format anticipates them).
- Host migration is a non-issue by construction (the server owns the game; the
  "host" is a player with lobby permissions — pre-start they pass to the next
  human, in-game they matter only for pause/kick powers).

## 11. Milestones

Each lands independently, keeps single-player green, and is verifiable.

- **M0 — Command-path hygiene.** `Cmd::Destroy`; disable stress spawns in net
  games; enforce the Hello version check in the existing session (small,
  immediate). *Verify: 2-player lockstep still hash-clean.*
- **M1 — Sim foundation.** 8 players, bounds guards, team layer + `allied()`,
  shared team vision, sim-side team win/defeat, N-player spawn, 10 colors,
  extended hash (incl. projectiles + order targets), determinism flags
  (`-ffp-contract=off`, no fast-math) pinned in CMake. *Verify: local
  4-player FFA (3 local AIs) and a 2v2 with shared fog, via new `--players`
  dev flags.*
- **M2 — AI extraction.** `src/ai/` Controller emitting commands via sink into
  `apply()`; sim-tick cadence; seeded RNG; delete GameView AI. *Verify: fixed
  seed ⇒ bit-identical `stateHash` sequence across two runs; behavior parity
  with pre-extraction AI.*
- **M3 — Server + protocol, humans only.** `takserver` lobby (create/list/
  join/password/slots/chat/kick/start), sequencer, **TickBundle with the event
  section and the log header from day one**, referee sim + suspicion rule,
  hash ring, server hardening (§5), client browser + lobby screens; retire
  `--host`/`--join`. *Known gap until M5: a mid-game disconnect ends that
  player's game (no rejoin yet).* *Verify: 3 humans on LAN, full game, hashes
  clean; password/full/running join rejections; malformed-frame fuzz doesn't
  kill the server.*
- **M4 — Server-hosted AI.** Controllers in the GameRoom; mixed games up to 8
  slots. *Verify: 1 human + 7 AIs through the server; per-room CPU measured
  (grounds the §4 scaling claim); 8-start map cap enforced.*
- **M5 — Durability.** Reconnect via log replay + rotating resume tokens, drop
  auto-pause with budgets, grace-expiry **Forfeit events**, keepalive + ping,
  clean `Bye`, replay-to-disk, replay-throughput guard ("machine too slow").
  *Verify: kill -9 a client mid-game, rejoin, finish hash-clean; measure
  replay time vs. game length (decides the snapshot backlog item's priority);
  forfeit fires as a logged event and replays identically.*
- **M6 — Polish.** In-game team/status UI at 8 players, chat overlay, det-math
  shim (`sim/detmath`) for cross-build play, spectator/map-transfer
  groundwork, docs + server deployment notes (systemd unit, TLS proxy
  example).

## 12. Testing strategy

- **Headless client mode** (`takclient --headless-net`) so CI can run N scripted
  clients + server on one machine and assert every hash matches to game end.
- **Replay determinism:** every CI game's log re-run must reproduce the final
  hash — this also guards single-player AI determinism.
- **Fault injection:** socket kill, delayed delivery, malformed/oversized
  frames, command floods, and a deliberately desynced client (mutated unit
  hp) must produce: referee flags exactly that client, reconnect restores it,
  game completes. A deliberately desynced *referee* (test hook) must trigger
  the suspicion rule, not a mass drop.
- The existing `--shot`/`--time` smoke harness keeps covering the offline
  path.
