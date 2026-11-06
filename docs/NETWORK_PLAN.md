# Network play: plan

Goal: online and LAN play that **feels exactly like the original**, on a **modern, open-source network
stack** (no GameSpy, no proprietary services). Status: agreed 2026-11-06, phases 0-1 under way. Points marked **(decide)** were settled as recommended (section 6).

## 1. How the original works (summary of the research)

Addresses are the disc 2.090 modules (`re/dump/*`) unless noted.

- **Client-server, state replication, distributed control.** Every machine simulates every creature, but
  runs the controlling (`master`) code only for the creatures it owns: its own fighter, or on the server
  the AI bots (`SetupCreatureControl` Session 0x4501a9b0, `IsMaster`, FINDINGS 175).
- **States, never inputs.** When a script calls `Register*State`, the creature's packed state (the
  recordings' `PackState` format, FINDINGS 174) is sent with its timestamp. Clients send only their own
  fighter; the server relays everything (`ShouldReplicate` Core 0x701b1f90).
- **Per-object rollback.** A receiver inserts the state into that creature's history at its past time,
  restores it and re-simulates that one creature up to now, at most 100 ticks (`AddLaggedState`
  0x701ae760, `AddLaggedStateDestIndex` 0x701ae2b0, FINDINGS 184/278).
- **Authority decides conflicts.** Each state carries authority (0-3), significance (0-3) and flags
  (NoInterrupt, TimeCritical, Correction, NoReplicate). A state replaces newer local ones only with
  authority at least theirs and no NoInterrupt in the way, except where `CanStateInterrupt` allows
  (FINDINGS 216). The server adds +1 to everything it handles.
- **Server owns outcomes.** Damage, energy, death, pickups and scores run under `IsServer()` in the
  scripts (725 calls). Clients predict hit reactions; the server's correction wins
  (`RegisterGetHitState` OMF 0x350296b0).
- **Your own fighter is predicted locally.** The server sends a client only corrections for its own
  fighter, never its regular states. That is why your robot responds instantly.
- **Send scheduling.** Each state is re-sent on a ladder (18/30/70/125/200/500 ms × 0.9), slower for
  fighters far from the receiver (1 + distance/180, at most ×4) and for less significant states, within a
  16 KB/s budget (`ReplicateSessionStateManagerStates` Session 0x4500a020).
- **Lag and clock.** Lag = 10 ms + ½ ping × timescale, re-measured every second (Session 0x4500b6d0).
  The client's scene clock slews towards the server's, and is hard-set past a 1 s gap (0x4500b5d0).
- **Replays are this same stream**, with the recording playing the server (`re/specs/replay-playback.md`).
  Our engine already has most of the receiving half for replays.
- **Transport.** One non-blocking UDP socket (WinSock, port 2002), its own framing, fragments and
  acknowledged "guaranteed" packets for session and UI messages. Game states go unreliable and are
  re-sent by the scheduler. No encryption or compression; protocol version 10 in the handshake.
- **Lobby.** GameSpy only listed servers (game name `omfbattle`); LAN uses a UDP broadcast query every
  500 ms; direct IP is supported. No CD keys and no NAT traversal.
- **Sessions.** Up to 16 clients (`Network\Max Clients`), AI bots in empty slots, server-driven pages
  (Choose Pilot / Creature open remotely on clients), a client status machine through loading, chat,
  kick votes and bans, reconnect, Change Fighter, and a `dedicatedserver` switch. Game types: Last Man
  Standing, Demolition, Team Demolition, Last Team Standing.
- **Not deterministic, by design.** The scene clock is real time with variable frames, the AI reads the
  wall clock, `rand()` is per machine and shared with audio, and particles step per frame. Divergence is
  covered by state corrections (FINDINGS 212, 306, 367).

## 2. The approach

**Keep the original's replication model exactly, and replace everything under it.**

Input-based rollback (GGPO style) and lockstep are ruled out. They need a fully deterministic engine,
which the original is not. They would also *change* the feel, because who wins a trade, how a lagging
opponent looks and how hits resolve all come from the state and authority model above. Porting that
model is also the cheaper path: the scripts already call `Register*State`, and our replay code already
packs, inserts and re-simulates states.

| Keep (this is the feel) | Replace (modern stack) |
|---|---|
| Who owns which creature; `master` code only on the owner | WinSock framing, fragments and acks |
| States sent on `Register*State`, original pack contents and flags | Handshake (magic GUID, protocol 10) |
| Authority / significance / NoInterrupt / `CanStateInterrupt` arbitration | No security → encrypted, authenticated sessions |
| Insert in the past, re-simulate the creature up to 100 ticks | GameSpy listing → our own open-source lobby server |
| Server-only outcomes; corrections-only for a client's own fighter | LAN broadcast → LAN discovery on the new transport |
| Lag formula, lag windows, scene-clock sync | NAT traversal and relay (the original had none) |
| 16 ms fighter ticks; the send ladder and distance / significance weights | Wire encoding of each message |
| Session flow: slots, AI bots, remote pages, loading statuses, game types | Silent version mismatch → data and version check |

Interop with original 2.123 clients is **not** a goal: it would mean reproducing the WinSock framing and
about 29 message types byte for byte, for a player base that no longer exists. **(decide)**

## 3. Architecture

```
 client (openbg)                     host / dedicated server (openbg or openbg-server)
 ┌──────────────────────┐            ┌──────────────────────┐
 │ scripts (original)   │            │ scripts (original)   │
 │ session + state mgrs │◄──states──►│ session + state mgrs │   game traffic: direct UDP,
 │ net/ protocol        │◄──session─►│ net/ protocol        │   hole-punched, or via relay
 │ transport (UDP)      │            │ transport (UDP)      │
 └─────────┬────────────┘            └──────────┬───────────┘
           │ list / join / punch                │ heartbeat
           └────────────►  lobby server  ◄──────┘   (open source, self-hostable, in this repo)
```

### 3.1 Engine (C++, `engine/src/net/` + session code)
- **State managers:** authority, significance and flag bits; server-issued numeric IDs with the
  ID-string link; the held-state queue (forced in after 2 × lag); lag windows; the network flavour of
  `AddLaggedState` with `CanStateInterrupt` (the replay flavour skips it, as the original does).
- **Natives made real:** `IsServer`, `IsLocal`, `IsLocallyControlled`, `IsLowAuthoritySimulation`,
  `IsNoReplicate`, `IsJoinable`, `IsReadyToLoad`, `IsFullySequenced`, `RegisterLastState*`,
  `ForceClientsToLoad`, `LoadingComplete`, `IsSinglePlayer` (all stand-ins today, PLACEHOLDERS).
- **Sender:** the original's scheduler, including the ladder, distance and significance weights, and the
  corrections-only rule for a client's own fighter. The budget stays 16 KB/s by default; raising it is a
  setting, since it makes remote fighters slightly fresher than the original. **(decide)**
- **Session protocol:** our own versioned binary messages, one for each original com-interface message.
  They cover session info, slots and clients, remote UI pages, loading statuses, states, chat, kick and
  ban, Change Fighter, reconnect and ping. Each message records the original it mirrors.
- **Clock:** the original's lag measurement and scene-time slewing.
- **Server modes:** a listen server, where the host plays, as the original does. And a headless dedicated
  server (`openbg-server`, from the existing headless mode, Linux/macOS/Windows), the original's
  `dedicatedserver`.

### 3.2 Transport **(decide)**
One UDP socket with a reliable ordered channel (session and UI) and an unreliable channel (states).

| Option | For | Against |
|---|---|---|
| **ENet + libsodium** (recommended) | Small C libraries (MIT / ISC), trivial on macOS and MinGW, same reliable/unreliable model as the original; libsodium adds an X25519 key exchange and ChaCha20-Poly1305 per packet | Encryption and NAT traversal are ours to write (small, well understood) |
| Valve GameNetworkingSockets | Encryption, reliability, ICE/TURN-style P2P built in (BSD) | Heavy build (protobuf, OpenSSL / libsodium), hard on MinGW and universal macOS builds |
| QUIC datagrams (msquic / quiche) | Standard, encrypted, congestion control | TLS certificates for player-hosted servers, larger dependency, more than this game needs |

### 3.3 Lobby server (new, open source, `server/` in this repo)
- A single small service anyone can run (container image plus a static binary). Players can list
  several lobbies in Options; the project runs a default one. **(decide: language. Go is
  recommended for an easy static binary; C++ would share code with the engine.)**
- **Listing:** hosts send a heartbeat with the fields GameSpy carried: name, game type, arena, players
  and maximum, password flag, port and version. Clients query with filters.
- **NAT traversal:** the lobby introduces both sides for UDP hole punching. If that fails, it relays
  traffic itself (a TURN-like relay, optional per lobby). The relay only sees encrypted packets.
- **No accounts at first:** a player name only, as in the original. Optional identity (keypairs, so
  names can be reserved and banned reliably) can come later. **(decide)**
- **LAN:** a broadcast query on the game port, as the original does; direct IP stays in the Join screen.

### 3.4 Compatibility and safety
- The handshake carries the protocol version, the engine version and a hash of the game data's scripts
  and objects. Mismatches are refused with a reason, instead of the original's silent desync.
- The server validates what clients send: only states for their own creatures, and only within the lag
  window (the original's rules already do this), plus size and rate limits.

## 4. Phases

Each phase ends with something testable, merged and pushed.

0. **Close the research gaps** (targeted reverse engineering, FINDINGS entries):
   - the server-side lag-window rules (`CheckForDiscardState` 0x701ae410);
   - whether hit tests run on every machine, and how `IsStateAlterable` behaves online;
   - the remaining com-interface messages and pre-load sequencing (0x4500b9b0);
   - what `DE_CNetworkTransferAgent` sends;
   - the significance multipliers and connection-quality / drop handling (0x4500c290);
   - dedicated server without a local slot;
   - spectators.
   Also fix PLACEHOLDERS: `Network\Max Clients` defaults to 16, not 20.
1. **Two sessions in one process.** Port the state-manager arbitration, roles and sender, and connect a
   server and a client World through an in-memory link with a simulated delay, jitter and loss (the
   original had a built-in delay simulator too). Scripted fights at 0, 50, 100 and 200 ms prove the
   model before any sockets exist. The single-player and replay paths must stay byte-identical.
2. **LAN and direct IP, two players.** Transport, protocol, handshake, the client status machine,
   loading sync, remote Choose Pilot / Choose Creature, and the Host and Join screens (Join is "not
   ported yet" today). Last Man Standing, one against one.
3. **Full sessions.** Up to 16 players, AI bots, all four game types, chat, Change Fighter, kick votes and
   bans, disconnect and reconnect, and the dedicated server.
4. **Internet.** The lobby server, server browser, NAT hole punching, relay fallback and encryption on.
5. **Polish.** A network debug overlay (ping, lag, corrections per second, bytes per second), tuning
   against recorded sessions, cross-platform soak tests (macOS ↔ Windows ↔ Linux server), docs and a
   release.

## 5. Testing
- **Loopback harness** (phase 1 onwards): server and clients headless in one process or several, with
  scripted input (`HOLD=`), seeded conditions and a link simulator. It logs corrections, rollbacks and
  each fighter's position error against the server.
- **Feel checks:** the same scripted exchange at 0, 80 and 150 ms. Compare who wins trades, how often
  the local fighter is corrected, and remote fighters' visual error, at the original's settings.
- **Recordings:** a network match recorded on the server must play back as a normal replay. It is the
  same stream, so this is a strong consistency check.
- **CI:** protocol unit tests and the loopback harness on GitHub-hosted runners (the repository is
  public). Data-dependent runs stay local, like today's tests.

## 6. Decisions (agreed 2026-11-06)
1. Transport: ENet + libsodium.
2. Lobby server in Go, in this repo; who hosts the default instance is still open.
3. No interop with original clients.
4. Names only at first; keypair identity later.
5. Bandwidth: the original's 16 KB/s by default, configurable.
6. First release: LAN plus direct IP (phases 1-2), then the internet lobby.
