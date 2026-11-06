# Network play: plan

Goal: online and LAN play that **feels exactly like the original**, on a **modern, open-source network
stack** (no GameSpy, no proprietary services). Status: agreed 2026-11-06; phases 0-2 done (LAN and direct IP play). Points marked **(decide)** were settled as recommended (section 6).

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
  scripts (725 calls). Every machine runs every hit test and enters the victim's reaction itself
  (TimeCritical, NoInterrupt); the attacker's machine asks the server for the victim's authoritative
  state (message 0x19), and the server's correction wins (`RegisterGetHitState` OMF 0x350296b0,
  FINDINGS 401).
- **Your own fighter is predicted locally.** The server sends a client only corrections for its own
  fighter, never its regular states. That is why your robot responds instantly.
- **Send scheduling.** A new state goes out on the next pass; until it is acknowledged it is re-sent on
  a slowing ladder (weights 1/18/30/70/125/200/500 against elapsed time over the manager's urgency
  interval), sooner for more significant states and later for fighters far from the receiver
  (1 + distance/180, at most ×4). Bandwidth (`Maximum Outgoing Bytes` 16000) enters only as a quality
  multiplier that stretches the intervals. 2.123 rewrote the sender: urgency classes polled every
  1000/300/100/50 ms or every pass, intervals proportional to the lag (`ReplicateSessionStateManagerStates`
  Session 0x4500a020, 2.123 0x1100a660, FINDINGS 405-406).
- **Lag and clock.** Lag = 10 ms + ½ ping × timescale, re-measured every second (Session 0x4500b6d0);
  each manager derives a discard window 1.3 × lag + 65 ms, a held-state timeout of twice that and a
  NoInterrupt horizon 5 × lag + 2 s (FINDINGS 397). Received states are re-stamped to their arrival
  time unless TimeCritical, so only hit reactions roll back (FINDINGS 398). The client's scene clock
  slews towards the server's, and is hard-set past a 1 s gap (0x4500b5d0, FINDINGS 403).
- **Replays are this same stream**, with the recording playing the server (`re/specs/replay-playback.md`).
  Our engine already has most of the receiving half for replays.
- **Transport.** One non-blocking UDP socket (WinSock, port 2002), its own framing, fragments and
  acknowledged "guaranteed" packets for session and UI messages. Game states go unreliable and are
  re-sent by the scheduler. No encryption or compression; protocol version 10 in the handshake.
- **Lobby.** GameSpy only listed servers (game name `omfbattle`); LAN uses a UDP broadcast query every
  500 ms; direct IP is supported. No CD keys and no NAT traversal.
- **Sessions.** Up to 16 clients by default (`Network\Max Clients`), AI bots in empty slots, server-driven pages
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
  ID-string link; the held-state queue (forced in after 2 × (1.3 × lag + 65 ms)); lag windows; the network flavour of
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

0. **Close the research gaps** (targeted reverse engineering, FINDINGS 397-411). Done 2026-11-06; the
   answers are in section 7.
   - ~~the server-side lag-window rules (`CheckForDiscardState` 0x701ae410)~~: closed (397-400).
   - ~~whether hit tests run on every machine, and how `IsStateAlterable` behaves online~~: closed (401).
   - ~~the remaining com-interface messages and pre-load sequencing (0x4500b9b0)~~: closed (402-404);
     open: the meaning of send flag 0x100000 and message 2's peer-address exchange.
   - ~~what `DE_CNetworkTransferAgent` sends~~: closed, nothing (411).
   - ~~the significance multipliers and connection-quality / drop handling (0x4500c290)~~: closed (405-407).
   - ~~dedicated server without a local slot~~: closed (408).
   - ~~spectators~~: closed, none; mid-match joins work (409).
   - ~~PLACEHOLDERS: `Network\Max Clients` defaults to 16, not 20~~: done, and the engine now uses 16 (411).
1. **Two sessions in one process.** Port the state-manager arbitration, roles and sender, and connect a
   server and a client World through an in-memory link with a simulated delay, jitter and loss (the
   original had a built-in delay simulator too). Scripted fights at 0, 50, 100 and 200 ms prove the
   model before any sockets exist. The single-player and replay paths must stay byte-identical.
2. **LAN and direct IP, two players.** Transport, protocol, handshake, the client status machine,
   loading sync, remote Choose Pilot / Choose Creature, and the Host and Join screens (Join is "not
   ported yet" today). Last Man Standing, one against one.
   Done 2026-11-06 (FINDINGS 413-416): ENet 1.3.18 transport with the delay simulator, our hello
   (protocol, engine version, data hash, password), the status machine with roster, remote Choose
   Pilot / Choose Creature, pre-load at 101 / 103, load authorization, ID requests and clock sync, the
   Join Game session browser (LAN query every 500 ms, typed address) and Host Game on
   `Network\Winsock\Listener Port`; `tools/net-lan/run.sh` plays a two-process match. Approximations
   in PLACEHOLDERS ("Waiting on networking"); not yet: reconnect, remote objects, `AdjustSameState`'s nudge.

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

## 7. Protocol reference (phase 0 results)

Disc 2.090 addresses unless noted; 2.123 changes are called out. Each row is observed in the
decompile unless marked *(inferred)*. Details and evidence: FINDINGS 397-411.

### 7.1 Packet framing (`ProcessPacket` Session 0x45006490)
First byte, low nibble: **0** session message (u16 number follows, table 7.2); **1** the session's
master modifier state (`ReplicateModifierState`, server → client, guaranteed); **2** clock sync.
Clock sync: the client sends {gTime u32, sequence u8, squeezed status} unreliably once per
`AdjustLag` period; the server echoes {that time, its scene `PreciseTime`, sequence}; the client keeps
only the echo of its latest sequence, rejects round trips above an adaptive limit (+25 per reject,
tightened to 50 + 1.5 × rtt), targets server time + rtt/2 × timescale, hard-sets past 1000 ms and
otherwise slews (the difference halved under 120 ms, again under 40, again under 10). A first ping
whose status is 0 makes the server force the client to status 102.

### 7.2 Session messages (header nibble 0)
Flags: 0 unreliable, 2 / 6 guaranteed, 0x100000 / 0x100002 = same plus a bit the disc transport never
reads (meaning unknown).

| # | Dir | Flags | Payload | Receiver |
|---|---|---|---|---|
| 0x01 | S→C | 0x100002 | client roster (per client: ID, name, optional node address) + slot info | creates/renames remote clients, adds slots, unpacks each slot; status 100 → 101 |
| 0x02 | C→S | 0x100002 | count, {client ID, node address} | passes each address to that client's node *(inferred: relayed sub-node addressing)* |
| 0x03 | S→C | 0x100002 | UI manager factory GUID, instance ID, contents | creates or reuses the page and launches it (remote Choose Pilot / Creature) |
| 0x04 | C→S | 0x100002 | GUID, ID, contents | unpacks into the client's remote page; `RemoteUIManagerComplete` |
| 0x05 | C→S | 0x100002 | same | as 4, with the page's +0x60 set first (sent when the local page had +0x60) |
| 0x06 | C→S | 0x100002 | same | `RemoteUIManagerActivated` |
| 0x08 | S→C | 0x100000 | GUID, ID, contents | live update of a remote page |
| 0x09 | C→S | 0x100000 | GUID, ID | server answers 0x08 (client asks while its active page wants updates) |
| 0x0a | S→C | 0x100002 | slot index, slot state | slot `UnpackState` |
| 0x0b | C→S | 0x100002 | player name, ping, kick votes | sets the client's name, ping and votes |
| 0x0c | S→C | 0x100002 | client ID, name, ping, admin level, votes needed to kick | updates that remote client |
| 0x0d | S→C | 0x100002 | count, {server ID, ID string} | links managers (`AddFromServer`) |
| 0x0e | S→C | 0x100002 | BOOL-ended {server ID, ID string, time, flags, newest state} | pre-load: link and insert; status 101 → 102 |
| 0x0f | S→C | 2 | u32 status | sets the session status (server refuses it) |
| 0x10 | S→C | 0x100002 | (none) | load authorized; status 102 → 103 |
| 0x11 | both | 0 | {server ID, time, flags, state}…, 0 | states (7.4) |
| 0x12 | both | 0 | {server ID, raw time}…, 0 | acks: stop re-sending, clear required correction |
| 0x13 | C→S | 0 | server ID | re-send that manager's newest state, or 0x14 if unknown |
| 0x14 | S→C | 0 | server ID | client deletes the manager's owner |
| 0x15 | C→S | 0 | ID string | ID request: queued for 0x0d, or 0x16 if unknown |
| 0x16 | S→C | 0 | ID string | client deletes that object |
| 0x17 | C→S | 6 | squeezed status | status report (100 also requests the roster) |
| 0x18 | any | 2 | 2000, bytes 0..1999 | test packet, sequence checked |
| 0x19 | C→S | 0x100000 | server ID | correction request: forced re-send flagged Correction |
| 0x1a | S→C | 2 | reason, u32 hours | "Banned From Server" with duration; client leaves the session |
| 0x1b | C→S | 2 | admin password | server answers 0x1c "Admin Granted" / "Invalid Password" |
| 0x1c | S→C | 2 | title, text | notification |
| 0x1d | S→C | 2 | (none) | `GH_CLIENT_RESELECT_FIGHTER` on the client's slot (Change Fighter) |

Message 7 has no handler. The pre-session lobby uses the app manager's own com interface
(Core 0x70198130): 1 list request → 3 session infos; 2 join {password, session GUID} → 4 accept
{GUID, node info, name, client ID} (status 2 → 100) or 5 refuse {GUID, reason: unknown, "The server is
full.", wrong password}.

### 7.3 Client status machine
1 → 2 Awaiting Join Granted → 100 Awaiting Client and Slot Update → 101 Awaiting Pre Load Sequencing →
102 Awaiting Load Authorization → 103 Loading → 1000 Sequenced. Server per remote client, by its
reported status: 100 → roster (0x01); 101 and 103 → pre-load (0x0e); 102 → when the slot's pickers
report the user's choices complete, 0x10 (at most once a second); 1000 → upkeep. `ForceClientsToLoad`
sends status 101 to all. States are only exchanged at 1000; a client sending them earlier is forced
to 102. A dedicated server's own session passes 102 by itself.

### 7.4 State handling (receiver)
| Rule | Value |
|---|---|
| Lag (com interface) | 10 + ping × timescale / 2 ms, every 1000 ms |
| Lag per manager +0xa0 | owner units; jumps if the new value is under old + 50, else moves 10 % |
| Discard window +0x98 | 1.3 × lag + 65 ms (scan uses half; TimeCritical the whole) |
| Held timeout | 2 × +0x98 after first held: client forces it in (authority 3); server drops it and re-sends its own |
| NoInterrupt horizon +0x9c | 5 × lag + 2000 ms from the newest state (server only) |
| Stamp | + lag unless TimeCritical (ordinary states land now; hit reactions roll back) |
| Server clamp | future stamps → owner's present |
| Server gate | client not at 1000 → packet ignored; required correction unacked → manager ignores all |
| Client floor | after a local NoInterrupt state on a remote creature: older than its time + 2 × lag dropped unless NoInterrupt/Correction |
| Same prediction | similar state (same method) of higher authority within the window: nudge position, no rollback |
| Authority | newer local state of ≥ authority blocks; Correction beats equal non-correction; server adds +1 (max 3); locally controlled creatures register ≥ 2 |
| Re-simulation | at most 100 ticks (FINDINGS 184) |
| 2.123 vs disc | a client holds an unplaceable TimeCritical state (disc drops it); a received NoInterrupt state is not stored as NoInterrupt; every Correction gets the local-correction flag; a server gives a client's Correction authority 3; merging adopts the incoming authority; `CanStateInterrupt` (FINDINGS 216) |

### 7.5 Sender
Per manager: urgency class (contents) → keep-alive interval 10000 / 5000 / 1000 / 700 / 400 ms.
Distance factor 1 + d/180 (max 4). Ladder weights 1, 18, 30, 70, 125, 200, 500 by resend index.
Significance 1/2/3 multiply the interval by 0.6 / 0.3 / 0.2. Disc: interval = weight × 0.9 × distance
/ (2 × outgoing quality), batched until lag/5 of time × quality or 410 bytes, packet up to ~450 bytes.
2.123: interval = (lag + 50) × weight × distance × 0.003 / (2 × quality), classes polled every 1000 /
300 / 100 / 50 ms / every pass, one packet per pass. Only states of authority ≥ 2 are sent; a client's
own fighter gets only corrections from the server.

### 7.6 Transport (to be replaced)
UDP port 2002, 500-byte fragments, guaranteed packets with a 16-bit sequence, a 16000-byte shared
budget refilling in one second, quality from ping (0 at ≥ 2501 ms), interrupted after 6 s of silence
(`SessionConnectionInterrupted` / `Restored` events), node destroyed after `Kick Time` 60 s. A built-in
delay simulator (`Delay Packet Time` and its variation, 0 by default).

### 7.7 Sessions
- Dedicated server: no local client, the scene is not drawn; slots are all remote players or bots.
- Drops: the fighter freezes while interrupted *(inferred)*; on disconnect the slot resets (fighter
  deleted with a disconnect effect, score zeroed) and auto-populate may add a bot. Reconnect re-sends
  all IDs and newest states and relaunches the current remote page.
- Joining mid-match works (the match server adds the client to the running match, a bot leaves); no
  spectators (slots = Max Clients, joins beyond refused, the flying observer is only a camera).
- AI bots: server-local clients made by `AutoPopulateSession` (`AutoPopulate\Num`), controlled and
  replicated by the server.
- Nothing but session messages and states crosses the wire: `DE_CNetworkTransferAgent` is unused, so
  every machine needs identical data (our data hash check covers it).
