# 13 — Completing the rebuild: the full remaining scope

[07](07-migration-path.md) laid out the stages before any `core/` code existed.
Most of it is now done: Stage 0 (golden vectors), Stage 1 (`codec`), Stage 2
(`modem`), and Stage 3's hard core — the `link` FSM — are built and proven, the
last capped by the **two-instance loopback** (`test/core/test_loopback.c`), where
two `ardop_link`s run a full connect → data → disconnect ARQ session through the
real modulator and demodulator with no hardware.

This document assembles everything that remains into one plan. It supersedes the
forward-looking sketch in [07](07-migration-path.md) §Stage 3–5 with the concrete
detail now available, because the core exists and the shape of the finish is no
longer guesswork.

**The definition of done is unchanged:** the `core/` tree *becomes* the running
`ardopcf` — over-the-air interoperable with ARDOP_Win/ardopc per the 2017 spec,
and speaking the existing TCP host interface so Pat/WoAD keep working — with the
inherited `src/` protocol/DSP code deleted, not merely bypassed.

There are three workstreams left, plus a deferred decision:

- **W1 — Finish the link protocol.** The parts of the ARQ/FEC/RXO machine the
  loopback does not yet exercise.
- **W2 — The platform shell (Stage 4).** Wire the core into a running program:
  one main loop, a platform interface, the host interface on core events/actions.
  This is where the core replaces `src/`.
- **W3 — Validation & cutover.** Prove the assembled program on the golden
  corpus and against a real peer, then remove the old tree.
- **W4 — Language reassessment (Stage 5).** Deferred, evidence-based, optional.

W1 and W2 are largely independent and can proceed in parallel; W3 depends on
both; W4 waits for W3.

---

## W1 — Finish the link protocol

The FSM drives connect / bidirectional data / gear-shift / timers / disconnect
today. What remains are the protocol corners the happy-path loopback skips. Each
is a scripted-test-then-loopback increment in the established `core/link` style.

### W1.1 — BREAK / link turnover (the IRS→ISS handover)
The most significant gap, and the one [02](02-protocol-fsm.md) singles out: the
`IRStoISS` / `IRSfromISS` transition that currently (in the inherited tree) lives
*inside the demodulator* (`SoundInput.c:1070`, the maintainer's own `TODO` at
`ARQ.c:1759`). In the core it becomes ordinary FSM transitions:
- host `BREAK` command (and `AutoBreak`) → the IRS sends `BREAK` instead of an
  ACK when it has data (rule 3.4), entering `IRS_TO_ISS`;
- the ISS, on `BREAK`, ACKs and yields the link → `IRS_FROM_ISS`;
- the substate settles to the steady data state on the first frame after the
  handover (rule 3.5).
The `ARDOP_LINK_IRS_TO_ISS` state already exists as a placeholder. **Exit:** the
loopback test extended with a turnover — A sends, B breaks, B sends, A receives.

### W1.2 — Station ID frames, and the host commands that need them
- Port `EncodeARQIDFrame` / `SendID` (Packed6 callsign + Maidenhead grid + RS)
  as a codec/link builder — the last frame type the core cannot yet *send*.
- Wire the deferred timers that fire it: the closing ID after `END`
  (`final_id_deadline`, already tracked) and the 10-minute regulatory ID.
- Remaining host commands: `SEND_ID`, `SET_MODE` (ARQ/FEC/RXO), and the config
  setters (`MYCALL`, aux calls, grid, bandwidth, `LISTEN`, `FSKONLY`, `BUSYDET`).

### W1.3 — FEC mode
`FEC.c` (~393 lines): broadcast, no ACKs, each frame optionally repeated
`FECRepeats` times; `ProtocolState` becomes `FEC_SEND`/`FEC_RCV`. As FSM: a
`FEC_SEND` path that emits `SEND_FRAME`s from the queue with repeats and no wait
for a reply, and a `FEC_RCV` path that delivers every good data frame to the host.
No handshake, so it is smaller than ARQ. **Exit:** loopback FEC broadcast — one
station sends, the other delivers, no ACKs.

### W1.4 — RXO mode
`RXO.c` (~332 lines): decode everything, participate in nothing. The demod's RXO
frame-type path is already ported (`--decodewav` uses it, and it is how
`golden-core` runs). What remains is the link-side RXO handler: route every
`FRAME_DECODED` to the host (with the session-id grouping `RXO.c` does), never
transmit. Small.

### W1.5 — Memory ARQ
`SaveFSKSamples`/`SavePSKSamples` averaging + re-decode of failed carriers across
retransmissions (`SoundInput.c:295-340`, the `intCarPhaseAvg`/`intCarMagAvg`
accumulators), aged out by `CheckMemarqTime`, reset by `ResetMemoryARQ`. This is
demod-domain DSP that improves low-SNR decode; it is only *testable* now that the
loopback can re-feed a degraded, repeated frame — so it lands after W2/W3 give it
a noise-injecting harness, or against the golden `snrNNdb` corpus.

### W1.6 — Protocol loose ends (small, each noted in code)
- The overall send-timeout give-up (`tmrSendTimeout`) ending a stalled session.
- ~~`ARQTIMEOUT`'s own stall-abort~~ **Done.** `ARQ.c` rule 1.7, ported: `ardop_link::arq_timeout`
  (`core/link/link.h`, default 120 s, range 30-240, set by `shell/host.c`'s
  `ARQTIMEOUT` -- the command existed before this, since Pat's TNC client blocks
  on its reply during handshake; the gap was that nothing read it). Armed on
  connect and rearmed on every decoded frame while connected -- matching the
  reference's own ~14 reset sites in spirit, checked directly rather than
  replicated one by one -- so an active IDLE/ACK exchange never trips it, only
  genuine silence does. Distinct from `tmrSendTimeout` above, which is still
  open. Motivated by a live capture ([20](20-field-results.md) session 3) that
  turned out *not* to be what this fixes: VA7DEP idled for 88 s continuously
  exchanging IDLE/ACK, which a faithful port correctly leaves alone -- the
  session was blocked at the application layer (Pat), not a dead link.
- The repeated-`ConReq` re-ack in `IRS_CON_ACK` (ISS missed our `ConAck`).
- The busy-block guard (`ConRejBusy` when the channel was busy just before the
  leader) — needs `BUSY_CHANGED` events threaded from the demod's busy detector
  (`core/modem/busy` is built; the event plumbing is not).
- `event.quality` bit-exactness: reconcile the demod's `phases_len` with ardopcf
  so the 3/17 off-by-one golden-core quality values match (heuristic; low
  priority).

---

## W2 — The platform shell (Stage 4)

The core is a pure library; W2 is the impure program around it. It is the
largest remaining piece by consequence, and the payoff of the whole rebuild: it
deletes the `FixTiming`/`SlowCPU`/`txSleep`/reentrancy class of bugs by
construction ([03](03-timing-model.md), [06](06-target-architecture.md)).

### W2.1 — The main loop
The single-clocked loop from [10](10-modem-link-design.md) §7: read audio → `t +=
n` → `ardop_demod_push` → for each event `ardop_link_step` → `perform(action)` →
`ardop_mod_pull` on transmit. One clock (the sample counter), no wall-clock in
the core path, no blocking. Replaces the divergent poll/flush spines in
`ALSASound.c` (2289 ln) and `Waveout.c` (1292 ln).

### W2.2 — The platform interface
One narrow interface behind which ALSA and WinMM sit as thin backends: audio
in/out, PTT (key/unkey — the `ARDOP_ACT_SET_PTT` the core already models), serial
(CAT/GPIO), the wall clock (logging + the regulatory ID only), and files (WAV
capture/replay). Collapses the duplicated device juggling; fixes the
[04](04-coupling-map.md) divergences (`-L`/`-R` broken on Linux / no-op on
Windows; stereo-capture half-garbage; `extraDelay` missing on Windows).

### W2.3 — The host (TCP) interface on core events/actions
`ProcessCommandFromHost` (the ~1300-line `strcmp` ladder in `HostInterface.c`)
becomes: parse a host command → build an `ardop_link_input` (`ARDOP_IN_HOST`) →
`ardop_link_step`; and core `ARDOP_ACT_NOTIFY_HOST` / `ARDOP_ACT_DELIVER_DATA`
actions → host protocol replies. The wire format is frozen (Pat/WoAD depend on
it); only the plumbing changes. This is also where IDFrame/ConReq **host-string
formatting** lands (the app-layer step the demod deliberately stops short of —
see `golden-core`).

### W2.4 — Presentation out of the core
Move the `wg_send_*` WebGui calls (25 in `SoundInput.c`, 15 in `ARQ.c`, 7 in
`Modulate.c`) out; the shell observes core events and updates the GUI. The core
already emits `LEADER_DETECTED`/`FRAME_DECODED`/quality as data.

### W2.5 — Types and build hygiene
Replace `BOOL`/`UCHAR`/`VOID`/`HANDLE` with `<stdint.h>` types and a real handle
abstraction (the `lib/rawhid` GCC-14 failure). **Exit for W2:** the core links and
tests with no ALSA / no sockets / no WebGui; both platforms build clean at
`-Wall -Wextra -Werror`; GCC 14 builds without `-Wno-int-conversion`.

---

## W3 — Validation & cutover

1. **Golden corpus through the assembled program.** `--writetxwav` / `--decodewav`
   on the new shell must reproduce the frozen `test/golden` corpus:
   `tx_sha256` bit-identical, every frame decoding to its recorded result. The
   modulator and demodulator are already golden-clean in isolation; this proves
   the *shell* wires them correctly.
2. **In-process ARQ regression.** The loopback test, extended across W1
   (turnover, FEC, RXO), as the protocol regression suite.
3. **On-air interop.** The one thing no in-process test can cover: a real
   `ardopcf` (old tree) ↔ new-shell ARDOP session over a channel/simulator, both
   directions, connect through disconnect, across bandwidths and gear-shifts.
   Against the 2017 spec, which is the authority ([05](05-essential-vs-incidental.md)).
4. **Delete `src/`.** The inherited protocol/DSP files are removed once the shell
   drives the core; `make check-pure` and the golden corpus are the safety net.
   The `compat` vs `improved` two-mode plan ([12](12-normative-accidents.md))
   becomes actionable here — every preserved accident has a fix waiting.

---

## W4 — Language reassessment (Stage 5, deferred)

With the core pure, explicitly-stated, and covered by the golden corpus and the
loopback, the Rust question ([06](06-target-architecture.md), [09](09-embedding-and-bindings.md))
is now a low-risk, separable experiment: port one module, run the vectors, keep
or revert. Take it with evidence after W3, not before.

---

## Sequencing

```
W1 (link protocol)  ─┐
                     ├─▶  W3 (validate + cutover)  ─▶  W4 (language, optional)
W2 (platform shell) ─┘
```

- **W1 and W2 are independent** — one is `core/link` protocol work with the
  loopback as its rig; the other is `src/` shell work against the frozen core
  API. They can run concurrently.
- **W2 is the critical path** to a shippable program; W1.1 (turnover), W1.3 (FEC)
  and W1.4 (RXO) are the protocol features a real deployment needs beyond the
  ARQ happy path.
- **W1.5 (MemARQ)** and the **quality bit-exactness** loose end are the two items
  that genuinely want W2/W3's harness first; everything else in W1 is doable now.
- Recommended start: **W1.1 (turnover)** to close the last conspicuous protocol
  gap and complete Stage 3's exit criterion, then **W2.1–W2.3** (the shell spine +
  host interface), which is where the rebuild stops being a library and starts
  being the program.

## Effort, roughly

| | Workstream | Size |
|---|---|---|
| W1.1 | BREAK / turnover | M |
| W1.2 | ID frames + host commands | S–M |
| W1.3 | FEC mode | M |
| W1.4 | RXO link mode | S |
| W1.5 | Memory ARQ | M (needs a harness) |
| W1.6 | Protocol loose ends | S each |
| W2.1 | Main loop | M |
| W2.2 | Platform interface + backends | L |
| W2.3 | Host (TCP) interface | M |
| W2.4 | Presentation out of core | S |
| W2.5 | Types / build hygiene | S |
| W3 | Validation & cutover | M–L |

## Open design decisions to settle in-flight

1. **Where the shell keeps its arenas.** The core is caller-owned, no-alloc; the
   shell decides static vs heap for `ardop_demod`/`ardop_link` (each ~100–200 KB)
   and the TX/RX buffers.
2. **PTT and TX completion.** `ARDOP_ACT_SET_PTT` + observing `ardop_mod_pull`
   drain to zero replaces `SoundFlush`'s busy-wait; the backend contract for
   "transmission finished" needs pinning (§2 of [10](10-modem-link-design.md)).
3. **Host-command surface parity.** An audit of `HostInterface.c`'s full command
   set against `ardop_host_cmd` — some commands are pure config (direct setters),
   some drive transitions (inputs); a few may need new action kinds.
4. **`compat` vs `improved` activation.** How the two-mode plan is exposed (build
   flag? host command?) once `src/` is gone and the accidents have live fixes.
