# ARDOP host-client apps

Small command-line applications built on the ARDOP TCP host interface. They are
thin clients: a modem daemon (`ardopb --host P`, or the inherited `ardopcf`) owns
the sound card and speaks the host protocol; these apps connect to it over TCP.
Because the modem is shared, you can run several at once against one radio.

Build with `make apps` → `apps/ardop-cat`, `apps/ardop-chat`.

All take `--host HOST:PORT` (default `127.0.0.1:8515`), where `PORT` is the
command port and `PORT+1` is the data port. All take `--version`, which prints
the build identifier and exits:

```
$ ardop-cat --version
ardop-cat 4e85005-dirty (2026-08-04), protocol ardopb_1.0.4.1.3-b
```

The first value is the build of this software — the commit, since this project
has issued no release tag yet. The second is the `ardopcf` release whose host
protocol it speaks; a host program such as Pat reads that one. Put the whole
line in any fault report. See the main [README](../README.md#which-build-am-i-running)
for what each part means.

## ardop-cat — a pipe over the radio

netcat's shape: one binary, and the direction is a flag. The sending station's
modem must know its call (set when `ardopb` launches); the receiving station's
modem must be listening (`ardopb --listen`).

```
# receiver (station B):
ardop-cat --host 127.0.0.1:8700 --listen > received.bin

# sender (station A), dialing B's callsign:
ardop-cat --host 127.0.0.1:8600 N0BBB < send.bin
```

Sending dials the target, streams stdin with flow control (so the modem's send
buffer never overflows), waits for it all to drain and be acked, then
disconnects. Listening writes each received `ARQ` payload to stdout and exits
when the peer disconnects, which is end of stream. Exit status is non-zero on
connect failure or a dropped link.

**One direction per invocation.** `nc` is full duplex because TCP is; this link
is half duplex with an explicit turnover, and the completion rule above is
one-directional by nature. A bidirectional pipe would be a feature, not a flag.

### What it does not do, on purpose

No filename, no length, no checksum, no resume, no completion signal — it is
`cat`, and it is named for it. The bytes that arrive are the bytes that were
sent, in order, or it exits non-zero. Check the result yourself, the way you
would with `nc`.

For a transfer that carries a name, verifies a checksum end to end and can be
resumed after a dropped link, both ends run `ardop-station`, which speaks
[ASP](../analysis/17-application-protocol.md).

### It refuses to be pointed at a station application

This is the one place it does *not* follow netcat. `nc` aimed at an HTTPS port
prints binary garbage, and that is correct for `nc` — you are on both ends, and
a terminal is where it lands. Here the far end is a stranger, and what would land
is a file you go on to trust: ASP's framing interleaved with the contents, a
corrupt file, and an exit status of zero.

So the first payload is checked for ASP's greeting (`asp_looks_like_hello`, which
lives beside the encoder so the two cannot drift; `test_asp.c` asserts it against
what a real session actually emits). Receiving one writes nothing and exits 2
with a message naming what to use instead. Sending to one warns and continues,
because the operator may know exactly what they are doing.

It also *reads the tag* now, which the tools it replaces did not: only
`ARQ`-tagged payload is the stream, so an error marker or a station
identification can no longer land in the middle of your file.

## ardop-chat — keyboard-to-keyboard, two ways

Line-oriented, half-duplex. Choose the transport at launch:

```
# ARQ (reliable, one peer): one side calls, the other listens
ardop-chat --host 127.0.0.1:8600 --call N0BBB
ardop-chat --host 127.0.0.1:8700 --listen

# FEC (connectionless broadcast; anyone in FEC receive mode hears it)
ardop-chat --host 127.0.0.1:8515 --fec
ardop-chat --host 127.0.0.1:8515 --fec --fecmode 4FSK.200.50S
```

In ARQ mode, typing turns the link over (`AUTOBREAK`); Ctrl-D drains the last
lines and hangs up. FEC mode broadcasts each line and needs no connection.

**The two transports are framed differently, and that is the specification, not
an inconsistency.**

ARQ chat sends bare lines. There is a session, so the peer is already known and
a callsign on every message would only restate it — and unframed text is what
interoperates with a plain terminal, with `ardopcf`, and with `ardop-station`,
which degrades to raw mode for exactly this case
([analysis/17](../analysis/17-application-protocol.md) §2). So ARQ chat is the
headless equivalent of the station application's Chat screen, and the two talk
to each other.

FEC chat cannot do that. There is no session and no peer, so §1 requires every
message to be **self-contained and idempotent**, and §6's `TEXT_B` carries the
sender's callsign and a message id. `apps/fecchat.c` is that profile — the first
implementation of it in the tree.

### What the framing buys on a broadcast channel

**You can see who spoke.** Every line is prefixed with the sender's callsign,
taken from the modem's `MYCALL` rather than a flag of its own. A broadcast
without one is a column of text with no idea who said any of it.

**Repeats are suppressed properly.** `FECREPEATS` sends every frame several
times. `core/link.c` drops a frame whose type and CRC match the one before it,
and its own comment admits the limit: *"identical consecutive payloads are
indistinguishable from repeats and are dropped."* So today saying the same thing
twice loses the second, and two stations interleaving break the "consecutive"
assumption the other way. `(callsign, msg_id)` over a five-minute window has
neither problem.

**A station running something else is still heard.** §6: a `TEXT_B` that fails
to parse is displayed as raw text rather than discarded, prefixed `?>`. A
broadcast net where only our own messages were visible would be worse than no
framing at all.

### How much you can say

This is the part worth knowing before you start typing, because it is small:

| `--fecmode` | frame | characters per message |
|---|---|---|
| `4PSK.200.100` (default) | 64 B | **54** |
| `4FSK.200.50S` (most robust) | 16 B | **6** |

The header costs ten bytes for a five-character callsign, and a longer callsign
costs more. `ardop-chat` prints your actual figure at startup and **splits a
longer line at a space into two complete messages** rather than truncating it or
fragmenting it — §1 means each piece has to stand alone if the other is lost.

## Trying it without a radio

`tools/loopback.sh` runs two `ardopb` daemons over a virtual audio cable, so you
can exercise the apps end to end on one machine:

```
make ardopb apps
tools/loopback.sh pipe      # transfers a file with ardop-cat, checks it
```

For chat over the loopback, `tools/loopback.sh up`, start two daemons on the
printed device names with `--host 8600` / `--host 8700 --listen`, then run
`ardop-chat` against each; `tools/loopback.sh down` when finished.
