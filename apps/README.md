# ARDOP host-client apps

Small command-line applications built on the ARDOP TCP host interface. They are
thin clients: a modem daemon (`ardopb --host P`, or the inherited `ardopcf`) owns
the sound card and speaks the host protocol; these apps connect to it over TCP.
Because the modem is shared, you can run several at once against one radio.

Build with `make apps` → `apps/ardop-tx`, `apps/ardop-rx`, `apps/ardop-chat`.

All take `--host HOST:PORT` (default `127.0.0.1:8515`), where `PORT` is the
command port and `PORT+1` is the data port.

## ardop-tx / ardop-rx — a pipe over the radio

Reliable one-way byte stream over an ARQ connection. The sending station's modem
must know its call (set when `ardopb` launches); the receiving station's modem
must be listening (`ardopb --listen`).

```
# receiver (station B):
ardop-rx --host 127.0.0.1:8700 > received.bin

# sender (station A), dialing B's callsign:
cat send.bin | ardop-tx --host 127.0.0.1:8600 N0BBB
```

`ardop-tx` dials the target, streams stdin with flow control (so the modem's
send buffer never overflows), waits for it all to drain and be acked, then
disconnects. `ardop-rx` writes each received payload to stdout and exits when the
peer disconnects (end of stream). Exit status is non-zero on connect failure or a
dropped link.

## ardop-chat — basic two-way chat

Line-oriented, half-duplex. Each line you type is sent; each received line prints
as `peer> …`. Choose the transport at launch:

```
# ARQ (reliable, one peer): one side calls, the other listens
ardop-chat --host 127.0.0.1:8600 --call N0BBB
ardop-chat --host 127.0.0.1:8700 --listen

# FEC (connectionless broadcast; anyone in FEC receive mode hears it)
ardop-chat --host 127.0.0.1:8515 --fec
```

In ARQ mode, typing turns the link over (`AUTOBREAK`); Ctrl-D drains the last
lines and hangs up. FEC mode broadcasts each line and needs no connection.

## Trying it without a radio

`tools/loopback.sh` runs two `ardopb` daemons over a virtual audio cable, so you
can exercise the apps end to end on one machine:

```
make ardopb apps
tools/loopback.sh pipe      # transfers a file with ardop-tx/ardop-rx, checks it
```

For chat over the loopback, `tools/loopback.sh up`, start two daemons on the
printed device names with `--host 8600` / `--host 8700 --listen`, then run
`ardop-chat` against each; `tools/loopback.sh down` when finished.
