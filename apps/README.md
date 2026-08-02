# ARDOP host-client apps

Small command-line applications built on the ARDOP TCP host interface. They are
thin clients: a modem daemon (`ardopb --host P`, or the inherited `ardopcf`) owns
the sound card and speaks the host protocol; these apps connect to it over TCP.
Because the modem is shared, you can run several at once against one radio.

Build with `make apps` → `apps/ardop-cat`, `apps/ardop-chat`.

All take `--host HOST:PORT` (default `127.0.0.1:8515`), where `PORT` is the
command port and `PORT+1` is the data port.

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

## ardop-chat — basic two-way chat

Line-oriented, half-duplex. Each line you type is sent; each received line prints
as `peer> …`. Choose the transport at launch:

```
# ARQ (reliable, one peer): one side calls, the other listens
ardop-chat --host 127.0.0.1:8600 --call N0BBB
ardop-chat --host 127.0.0.1:8700 --listen

# FEC (connectionless broadcast; anyone in FEC receive mode hears it)
ardop-chat --host 127.0.0.1:8515 --fec
ardop-chat --host 127.0.0.1:8515 --fec --fecmode 4FSK.200.50S
```

In ARQ mode, typing turns the link over (`AUTOBREAK`); Ctrl-D drains the last
lines and hangs up. FEC mode broadcasts each line and needs no connection;
`--fecmode` picks the frame type it broadcasts with (default `4PSK.200.100`,
any name the `FECMODE` host command accepts). Slower, more robust modes carry
less per frame but survive worse conditions.

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
