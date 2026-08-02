# 17 — The application protocol: chat and file transfer

A wire specification, written before implementation, because there is nothing to
extend. What exists today is not a protocol:

- `ardop-tx` is `cat` over ARQ. No framing, no filename, no length, no checksum,
  no resume, no completion signal. It infers "done" by polling until `BUFFER 0`
  and `STATE IDLE` (`apps/ardop_tx.c:157-172`), and on a dropped link prints
  `"link dropped during transfer"` and exits 1.
- `ardop-rx` writes every received payload to stdout and **ignores the
  `ARQ`/`FEC`/`ERR`/`IDF` tag entirely** — it receives it into `tag[]` and never
  reads it (`apps/ardop_rx.c:94-104`), so an error marker or a station ID would
  land in the middle of your file.
- `ardop_chat` is `fgets` in, `printf` out.

This document specifies a protocol precise enough that a second implementer could
write an interoperable client from it, per [14](14-station-application.md)'s
constraint that it be a public spec rather than an internal format.

Working name: **ASP** (ARDOP Station Protocol), version 1.

---

## 1. What the layer below provides

| | ARQ | FEC |
|---|---|---|
| Reliable | yes — retried until acked | no |
| Ordered | yes | no |
| Duplicated | no | **yes** — `FECREPEATS` sends each frame N times |
| Addressed | yes — connected peer known | no — broadcast |
| Interruptible | yes — link can drop at any point | n/a |

That gives two profiles with genuinely different designs, and conflating them is
the main way this could go wrong.

- **ARQ profile** — a reliable, ordered, interruptible byte stream. Carries files
  and directed chat. Full framing.
- **FEC profile** — unreliable, unordered, duplicating datagrams. Carries
  broadcast chat only. Every message must be self-contained and idempotent.

**No file transfer over FEC.** Without acknowledgement there is no way to know
what arrived, and a partial file with no way to find the holes is worse than no
file.

Only `ARQ`-tagged payload feeds the ARQ profile; `ERR` and `IDF` tagged data is
never parsed as protocol.

---

## 2. Coexistence with Pat and with plain ARDOP stations

Two different problems, with two different answers.

**A guest on our own TNC ports** is handled at the modem, not here.
[14](14-station-application.md) Decision 4 gives the link a single session owner —
there is one 16 KB transmit queue (`shell/runtime.h:107`), and two writers would
interleave into mutual garbage. So when Pat holds the link, ASP is not running,
and vice versa. Nothing in this document needs to accommodate it.

**A remote peer that does not speak ASP** is this document's problem, and the
answer is to detect it on the first byte received and degrade.

Every connection opens with `HELLO` in both directions. The first byte a peer
sends is either `0x01` followed by a well-formed HELLO carrying the magic
`"ASP/1"`, or it is not. If it is not:

> **Raw mode.** Everything received is displayed as chat text. Everything sent
> goes out as unframed UTF-8. File transfer is unavailable and the UI says so.

This is not a fallback grudgingly tolerated — it means the app rag-chews
correctly with anyone running plain `ardopcf`, `ardop-chat`, or a terminal, which
is most of the people on the air. The decision is made on one byte and never
revisited within a connection.

Our own HELLO on a plain peer's screen renders as
`\x01\x0eASP/1 N0CALL…` — mostly legible, which is deliberate.

---

## 3. Message framing (ARQ profile)

Every message:

```
+--------+------------------+--------------------+
| type   | length           | payload            |
| u8     | varint (1..4 B)  | `length` bytes     |
+--------+------------------+--------------------+
```

`varint` is unsigned LEB128: 7 bits of value per byte, least-significant group
first, high bit set on all but the last. **Capped at 4 bytes**; a fifth
continuation byte is a protocol error, so a decoder's bound is static.

**Maximum payload is 4096 bytes.** A larger declared length is a protocol error.
This keeps every receiver's buffer fixed-size and non-allocating, consistent with
the rest of the tree.

**An unknown type is skipped by its length**, exactly as `ardop_tlm_parse` skips
an unknown record kind (`shell/telemetry.c:248-251`). That single rule is what
lets version 1 and version 2 stations talk without a flag day, and it is why
length precedes payload for every type including ones that have none.

### Types

| | Type | Profile | Payload |
|---|---|---|---|
| `0x01` | HELLO | both | magic, version, callsign, capability flags |
| `0x02` | BYE | ARQ | optional UTF-8 reason |
| `0x10` | TEXT | ARQ | UTF-8 message |
| `0x11` | TEXT_B | FEC | callsign + message id + UTF-8 (see §6) |
| `0x20` | OFFER | ARQ | id, size, crc32, name, content type |
| `0x21` | ACCEPT | ARQ | id, have, crc32 of the prefix held |
| `0x22` | REJECT | ARQ | id, reason code |
| `0x23` | START | ARQ | id, offset the sender will begin at |
| `0x24` | DATA | ARQ | raw bytes of the active transfer |
| `0x25` | DONE | ARQ | id |
| `0x26` | RESULT | ARQ | id, status code |
| `0x27` | CANCEL | ARQ | id, reason code |

`0x00` is reserved so that a stream of zero bytes is never a valid message.

---

## 4. Why there are no per-chunk offsets

The obvious design puts an offset in every `DATA` chunk. It is wrong here.

The ARQ layer already delivers reliably and in order, so within one connection an
offset is redundant — the receiver knows where it is. Offsets matter only for
**resume across connections**, which happens once per connection, not once per
kilobyte. Negotiating it once in `ACCEPT`/`START` costs a few bytes total;
carrying it per chunk costs 3–4 bytes every 1024, forever, on a link that moves a
few hundred bytes per second.

`DATA` therefore carries no id and no offset. It belongs to the one active
outbound transfer in that direction, and it continues where the last one stopped.

**One active transfer per direction.** Concurrency at 300 B/s makes both
transfers slower and neither more likely to finish. Additional offers queue.

---

## 5. File transfer

```
A                                          B
OFFER(id, size, crc32, name, type)  ──▶
                                    ◀──  ACCEPT(id, have=N, crc32_prefix)
                                          or REJECT(id, reason)
START(id, from=M)                   ──▶    M = N if the prefix verified, else 0
DATA(…) × many                      ──▶
DONE(id)                            ──▶
                                    ◀──  RESULT(id, OK | CRC_MISMATCH)
```

### Resume, and why `ACCEPT` carries a prefix CRC

The receiver holding N bytes is not evidence that those N bytes are the *same*
file's first N bytes — the offer may be a different file with a coincidentally
equal name, or a partial from a truncated earlier attempt. So `ACCEPT` carries
CRC-32 of the exact prefix the receiver holds, the sender checks it against its
own first N bytes, and `START` says where it will actually begin. `M = 0` means
"your prefix did not match, starting over", and the receiver truncates.

Without this the mismatch is only discovered by the whole-file CRC after the
entire remainder has been sent — minutes of airtime spent to learn the transfer
was doomed at the start.

### Transfer identity

`id` is a `u16` scoped to one connection. **Resume identity across connections is
`(peer callsign, name, size, crc32)`** — the id is meaningless once the link
drops. Both ends persist partial transfers keyed on that tuple.

### Integrity: CRC-32, not SHA-256

CRC-32 (IEEE 802.3, the zlib polynomial) over the whole file, 4 bytes in `OFFER`.

The threat is accidental corruption, not forgery — and the link below already
applies per-frame CRC and Reed–Solomon, so ASP's check is a backstop against
framing and resume errors rather than against the channel. CRC-32 is ~30 lines
and a table; SHA-256 is ~200 lines and 32 bytes on the wire for a property nobody
here needs. A `SHA256` capability flag in `HELLO` leaves the door open.

It lives in the app layer. **Not in `core/`** — `core/codec/crc.c` holds the
CRCs that are normative to ARDOP itself, and adding a non-protocol checksum
beside them would blur exactly the line that document 12 exists to keep sharp.

### Filenames are hostile input

`name` is UTF-8, at most 255 bytes, and is **not a path**. A receiver must:

- strip every directory component — `../`, `..\`, absolute prefixes, drive
  letters, and anything after the last separator;
- reject or replace control characters, and reject empty names;
- refuse reserved device names on Windows (`CON`, `PRN`, `AUX`, `NUL`, `COM1`…);
- write into a single fixed receive directory chosen by the operator, never a
  path derived from the peer.

This is stated in the spec, not left to implementations, because "accept a
filename from a stranger over the radio and write it to disk" is exactly the
shape of bug that is embarrassing to have.

### Auto-accept is off by default

A station that automatically accepts arbitrary files from any caller is a station
that will eventually receive something its operator did not want. Default:
prompt. Allow a per-callsign allowlist for operators running unattended.

---

## 6. Chat

**ARQ profile (`TEXT`, 0x10).** Payload is UTF-8, nothing else. The sender is the
connected peer, which is already known; the receive time is local. A per-message
timestamp or callsign would cost 8–12 bytes to restate what the session already
establishes.

Interleaving during a file transfer is legal at message boundaries. With 1024-byte
`DATA` chunks the worst-case delay for a chat line is one chunk — a few seconds —
which is the right trade against the per-chunk cost of anything finer.

**FEC profile (`TEXT_B`, 0x11).** No session, so each message must stand alone,
and `FECREPEATS` means it will arrive more than once.

```
+---------+------------------+----------+-------------------+
| calllen | callsign         | msg_id   | UTF-8 text        |
| u8      | calllen bytes    | u16      | rest of payload   |
+---------+------------------+----------+-------------------+
```

Receivers deduplicate on `(callsign, msg_id)` over a sliding window — five
minutes is ample, since a repeat arrives within seconds. `msg_id` is a per-sender
counter, and wrap is harmless at that window.

Broadcast chat is where the app is most likely to meet a station running
something else, so a `TEXT_B` that fails to parse is displayed as raw text rather
than discarded.

---

## 7. Turnover and flow control

**Turnover is the link's job.** ARQ is half-duplex; the receiver of a file needs
to send `ACCEPT` promptly. `AUTOBREAK TRUE` already does this in core — on an
`IDLE` keep-alive from the ISS, an IRS with queued data sends `BREAK`
(`core/link/link.c:875-878`). The app sets it and does not reimplement it.

**Flow control needs no polling.** `apps/ardop_tx.c` polls `BUFFER` with an
8192-byte window and 2048-byte chunks (`:25-26`) because it is a socket client.
Embedded, the app receives `ARDOP_OBS_BUFFER` events whenever the queue depth
changes (`shell/runtime.h:57`). Keep at most ~8 KB of the 16 KB `tx_queue`
outstanding and refill on the event.

The `DONE` → `RESULT` exchange also replaces `ardop-tx`'s "poll until `BUFFER 0`
and `STATE IDLE`" heuristic with an actual end-to-end acknowledgement, which is
the difference between knowing a file arrived and hoping it did.

---

## 8. Failure semantics

| Event | Behaviour |
|---|---|
| Link drops mid-transfer | Both ends persist partial state keyed on `(peer, name, size, crc32)`. Resume on the next connection via `ACCEPT`/`START`. |
| Prefix CRC mismatch | `START(from=0)`; receiver truncates and restarts. |
| Whole-file CRC mismatch | `RESULT(CRC_MISMATCH)`. Receiver discards; sender may re-offer. Do not auto-retry more than once — a repeatable mismatch is a bug, not a channel event. |
| Unknown message type | Skip by length. Never an error. |
| Malformed varint, oversize length, or a length that cannot be satisfied | Protocol error: log, disconnect. Do not attempt resynchronisation — a reliable stream that has desynchronised is a bug in one of the two implementations. |
| Peer does not speak ASP | Raw mode (§2). |
| Duplicate delivery | Cannot occur on ARQ. On FEC, deduplicated by `(callsign, msg_id)`. |
| Transfer refused | `REJECT(id, reason)`: NO_SPACE, REFUSED, UNSUPPORTED, TOO_LARGE. |
| Either side cancels | `CANCEL(id, reason)`. Partial state is kept, so a cancel is resumable. |

---

## 9. Regulatory

**No encryption.** Amateur radio rules in most jurisdictions prohibit messages
encoded to obscure their meaning (in the US, 47 CFR §97.113(a)(4)). ASP carries
no encryption and no obfuscation, and must not grow any. Compression is a
different thing — a published algorithm applied for efficiency, not concealment —
and is permitted, but it is deliberately **not** in version 1: it adds a
dependency and a decompression bomb surface for a gain that matters mainly on
text, which is already small.

Station identification is the modem's obligation, not this layer's — the
regulatory ID timer lives in the link ([13](13-completing-the-rebuild.md) W1.2).

---

## 10. Testing

This layer is testable with no radio and no UI, which is the reason it can be
built in parallel with everything else in [14](14-station-application.md).

`test/core/test_loopback.c` already runs two `ardop_link` instances against each
other through the real modulator and demodulator, in-process and deterministic;
its `hop()` (`test_loopback.c:83-119`) modulates one station's frame, pads to
flush the demodulator's look-ahead, and pushes it into the other. ASP sits on top
of exactly that.

The tests that matter:

1. A file transferred between two in-process stations arrives byte-identical.
2. The same transfer, with the link torn down at 40% and re-established, arrives
   byte-identical — this is the whole point of the protocol.
3. A resume whose prefix does not match restarts from zero and still arrives
   correct.
4. Chat interleaved into a file transfer arrives in order, and the file is
   unaffected.
5. A peer sending plain text triggers raw mode, and nothing is misparsed.
6. An unknown message type inserted mid-stream is skipped and the transfer
   completes.
7. Filenames `../../etc/passwd`, `C:\Windows\x`, `CON`, and one containing a
   newline all land as inert names in the receive directory.

Note that `apps/` currently has **zero automated tests** — no `test/apps/`, no
Makefile target, no CI job — and `apps/hostclient.c` reimplements `shell/host.c`'s
framing by hand with nothing checking the two agree. ASP should not repeat that.

---

## Open decisions

1. **Whether `apps/ardop-tx`/`ardop-rx` adopt ASP** or stay as the raw pipe. They
   are useful precisely because they are `cat`; a `--asp` flag is probably the
   answer, but then the framing has a second implementation and needs the
   round-trip test that `test_telemetry.c` does for the telemetry format.
2. **Compression.** Deferred out of version 1 above, but a capability flag and a
   single well-defined algorithm (deflate) would help on text-heavy links.
3. **Whether `TEXT` needs a message id on ARQ** for UI-level delivery receipts.
   Not needed for correctness; nice for a chat that shows "delivered".
4. **A directory/announce message** — "what files do you have", or a beacon of
   station capability on FEC. Obvious next feature, and the reason `HELLO`
   carries capability flags now.
5. **Interop profiles.** [14](14-station-application.md) leaves B2F (Winlink, via
   Pat) and ARIM as possible alternate profiles. Nothing here precludes them, but
   nothing here anticipates them either; that is a separate document if it
   happens.
6. **`msg_id` scope on FEC** when two stations share a callsign with different
   SSIDs. `ardop_stationid` carries the SSID; the spec above says "callsign" and
   should say which.

## Exit criteria

- Tests 1–7 of §10 pass in-process against the loopback harness.
- A transfer interrupted and resumed produces a byte-identical file, verified by
  CRC and by comparison, across at least two link drops.
- Two independent implementations of the framing — the app's and the test
  harness's — agree, in the manner `test/core/test_telemetry.c` establishes for
  the telemetry wire format.
- The spec in this document is sufficient to write the second implementation
  without reading the first one's source. That is the actual test of whether it
  is a specification.

---

## Amendments made during implementation

1. **The session owns no transport and no storage.** Not stated above, and it is
   what makes §10's claim -- that this layer needs no radio, no filesystem and no
   clock -- actually true rather than aspirational. `asp_io` is a table of nine
   functions; the application supplies one that writes to the spine and to disk,
   and the test supplies one that writes to arrays. A "file" in the test suite is
   an array, the "link" is a byte queue, and time is the loop counter.

   That is the difference between the interesting cases being things a test can
   simply *do* and things somebody has to reproduce on the air. Dropping a link at
   40% and resuming it is four lines.

2. **`asp_io::send` returns a count, and the session honours it.** The obvious
   shape is a `void` send that always succeeds. It would have looked correct
   against any test that gave it room, and lost bytes on the air the first time
   the 16 kB transmit queue filled -- §7 puts admission in the spine's hands, so a
   partial send is the normal case, not an error path. `test_transfer_survives_a_
   stingy_link` runs a whole transfer through a link that refuses four calls in
   five.

3. **`DATA` arriving with no transfer in progress is skipped, not an error.**
   §8's table does not cover it. Treating it as a protocol error would turn a
   harmless race -- a cancel crossing a chunk in flight -- into a dropped link.

4. **A second `OFFER` while one is being received is refused, not queued.** §4
   says additional offers queue, and they do, but *above* this layer: the session
   holds one inbound and one outbound transfer and says no to a second. Queueing
   inside the session would mean holding an offer whose sender may have given up.

5. **The prefix-CRC check reads the sender's own file.** Worth stating because it
   is the only place the sender re-reads what it has already sent. §5 explains why
   it is worth it: the alternative discovers the mismatch from the whole-file CRC
   after the entire remainder has been sent -- minutes of airtime to learn the
   transfer was doomed at the start.

6. **Open decision 6 is answered by construction.** `msg_id` scope on FEC is
   per-callsign-string, and the callsign carried in `TEXT_B` is whatever
   `ardop_stationid` renders -- which includes the SSID. Two stations sharing a
   callsign with different SSIDs therefore have separate `msg_id` spaces, which is
   the behaviour the open decision was asking for.

7. **A file transferred over real ARQ arrives corrupt, and the fault is below
   ASP.** *(This entry replaces an earlier version of amendment 7 that blamed
   `AUTOBREAK` and the discarded transmit queue. That diagnosis was wrong and is
   corrected below; the note is left as an amendment rather than deleted because
   a wrong diagnosis that was acted on is worth being able to find again.)*

   Driving ASP over the harness's real link -- two spines, the real modulator,
   the real demodulator -- corrupts the file. `test_asp.c` passes, because it
   drives the protocol over a byte queue that never loses anything, which is what
   makes it a good test of the protocol and no test at all of the stack beneath
   it. This is the argument for `ardop-spine --asp` existing.

   ### What it is not

   Ruled out by instrumentation, each with a measurement:

   - **Not the transmit queue discarded on BREAK.** `iss_yield_on_break()`
     (`core/link/link.c:667`) does throw the queue away, and its own comment says
     `SaveQueueOnBreak` was deliberately dropped in the port -- but `tx_len` is
     **0** at every call in this session. Nothing is lost there. This was the
     first diagnosis and it was wrong.
   - **Not the link's enqueue limit.** `step_host_send_data` drops bytes past
     capacity silently; it never fires here, because the transmit credit makes it
     unreachable and `app/spine.c` asserts the count anyway.
   - **Not the spine's queues.** `event_lost` stays 0 and no payload is
     truncated: `ARDOP_DEMOD_MAX_PAYLOAD` is 1024 and no delivery exceeds it.
   - **Not ASP's framing.** Every message the sender puts on the wire is a
     complete, correctly framed 1027 bytes, verified by capturing the sender's
     byte stream.

   ### What it is

   **The IRS silently discards genuinely new data frames as duplicates.**
   `core/link/link.c:896`:

   ```c
   if ((int)ev->frame_type != l->last_data_to_host) {
           deliver_data(...);
           l->last_data_to_host = ev->frame_type;
   }
   ```

   The duplicate test is one bit -- the parity of the data frame type -- and
   during this transfer the receiver sees the same type twice in a row carrying
   *different* payload three times, and drops all three. Confirmed by comparing
   each suppressed payload against the last one actually delivered, with the
   comparison buffer held per station. (An earlier pass at this used a
   file-scope buffer shared by both link instances and was therefore worthless;
   worth recording, because it produced a confident-looking wrong answer.)

   Three frames, 1024 + 1024 + 863 bytes, gone with no NAK, no fault and no
   counter. The sender sees each one ACKed and drops it from its queue. ASP then
   desynchronises at the next message header, since §4 correctly gives `DATA` no
   offsets and there is nothing for the receiver to notice the hole with.

   The sender's toggle is re-anchored by `iss_begin_sending()` at every turnover
   (`last_data_acked = 1`, so the first frame after a turnover is always even),
   and the receiver's `last_data_to_host` is re-armed by two different events
   (`iss_yield_on_break` and `step_irs_to_iss_rx`). **Which of those gets out of
   step, and when, is not yet established.**

   ### Why it is not fixed here

   A one-bit sequence number is part of the on-air format and cannot be widened
   without breaking interoperability, so the fix has to be in how the two ends
   keep their toggles in step -- which means establishing what the reference
   implementation's rule actually is, not inventing one.

   Two tempting fixes are both wrong:

   - *Suppress only when the payload is byte-identical.* A retransmission is
     byte-identical by construction, so this looks exact -- until a file contains
     two consecutive identical 1024-byte blocks, which a file of zeros does
     immediately, and real data is dropped again.
   - *Stop suppressing after a turnover.* Delivers duplicates into a file, which
     is the same corruption with the opposite sign and is quieter.

   `core/link` is the part validated against the golden corpus, and a wrong
   change to it corrupts real traffic in a way that is harder to see than this
   one. So this is written down rather than guessed at.

   **Reproduction:** `test/app/asp.script`, deliberately not in CI.

   ### Update: one of the two causes is fixed

   Hunting this down found **two** independent defects, not one.

   **Fixed.** `iss_yield_on_break()` discarded the ISS's transmit queue when the
   IRS took the link -- see [12](12-normative-accidents.md). That is a block of
   host data lost per turnover, silently, and every ARQ file transfer turns over
   because the receiver has to acknowledge at its own level. It is now reproduced
   in isolation by `test_loopback_turnover_loses_nothing` (which fails without
   the fix) and corrected.

   **Not fixed.** The duplicate suppression above survives that fix: the same
   three frames are still discarded, still carrying new data. What is now known
   about it:

   - It is **not** the transmit queue, the enqueue limit, the spine's queues,
     payload truncation, or ASP's framing -- each ruled out by measurement.
   - It does **not** reproduce at the link level on a clean channel, even with
     repeated turnovers driven deliberately
     (`test_loopback_turnover_loses_nothing` passes).
   - It is **not** the loopback's air buffer overflowing: zero dropped samples.
   - Modelling the loopback as half duplex -- a transmitting station is deaf,
     which is true of every real radio and is *not* true of this harness -- does
     not fix it either. That change was reverted rather than kept: with a
     *queued* air buffer, discarding audio while PTT is up invents loss a live
     channel would not have, and shipping a plausible-but-unproven change to the
     channel model would make the next investigation harder, not easier.

   So the remaining difference between the harness that fails and the harness
   that passes is that `app/loopback.c` steps both stations concurrently, and
   `hop()` delivers one frame at a time to completion. That is where to look
   next, and the question to answer first is whether the failure is in the link
   or in the harness -- because the evidence no longer points clearly at the
   link.

8. **Still to build:** the Chat and Files screens. The protocol is complete, its
   own tests pass, and the application-side `asp_io` is written. **Chat is not
   blocked** -- a lost chat line is a lost chat line, and the raw-mode path does
   not use ARQ data framing at all. **Files are blocked on amendment 7**, because
   a transfer UI over a transfer that silently corrupts is the wrong thing to
   build next.
