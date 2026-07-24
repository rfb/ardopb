# 01 — Signal chain, end to end

Traces both directions with `file:line` anchors. Every path here was confirmed
against a live run (see [Verification](#verification-notes) at the end), not
inferred from reading alone.

---

## Transmit

### The chain

```
host data (TCP)  or  protocol event (ACK due, ConReq, ID…)
        │
        ▼
  bytDataToSend[]                              ARDOPC.c:~200  (global queue)
        │
        ▼
  MainPoll()                                   ARDOPC.c:1979
   └─ GetNextFrame()                           ARDOPC.c:582
        ├─ GetNextARQFrame()                   ARQ.c:347      (ARQ mode)
        │    └─ GetNextFrameData()             ARQ.c:967      picks frame type + payload
        └─ GetNextFECFrame()                   FEC.c          (FEC mode)
        │
        ▼
  Encode4FSKControl() / EncodeFSKData() / EncodePSKData()     ARDOPC.c
   └─ RSEncode()                               lib/rockliff/rrs.c
   └─ GenCRC16()                               ARDOPC.c
        │  → bytEncodedBytes[1800]  (global)
        ▼
  Mod4FSKDataAndPlay()   Modulate.c:119
  Mod4FSK600BdDataAndPlay()  Modulate.c:263
  ModPSKDataAndPlay()    Modulate.c:471
        │
        ├─ FrameInfo()                         ARDOPC.c:739   modulation params by frame type
        ├─ initFilter(200|500, 1500)           Modulate.c:676 ← ALSO keys PTT, stops capture
        ├─ SendLeaderAndSYNC()                 Modulate.c:66
        └─ per symbol: lookup waveform template
             intFSK50bdCarTemplate[4][240]     ardopSampleArrays.c
             intPSK100bdCarTemplate[9][4][120]
             (or generated at runtime — CalcTemplates.c)
        │
        ▼  one sample at a time
  SampleSink(short)                            Modulate.c:771
   └─ 3–21 section resonator bank (bandwidth-shaping filter)
   └─ scale by DriveLevel, hard-clip at ±32700
   └─ DMABuffer[Number++]
        │  every SendSize (1200) samples
        ▼
  SendtoCard()                                 ALSASound.c:1581
   └─ SoundCardWrite()                         ALSASound.c:1354  ← CLOSES capture handle
        └─ PackSamplesAndSend()                ALSASound.c:1403
             └─ snd_pcm_writei()
        │
        ▼  end of frame
  SoundFlush()                                 ALSASound.c:1765
   └─ AddTrailer()                             Modulate.c:595
   └─ busy-wait until estimated TX end          ALSASound.c:1794
   └─ KeyPTT(FALSE)
   └─ reopen capture, StartCapture()
```

### Things worth noting

**`initFilter()` is misnamed and does far more than filtering.** At
`Modulate.c:676-701` it also keys PTT, records `pttOnTime`, schedules the 10-minute
ID, sets `SoundIsPlaying`, and calls `StopCapture()`. Selecting a filter width
and starting a transmission are the same operation. Any rebuild that separates
"shape this waveform" from "key the radio" has to untangle this first.

**Transmission is synchronous and blocking.** `Mod*DataAndPlay()` returns only
when the whole frame has been played — the comment `// only returns when all
sent` appears at every call site (e.g. `ARQ.c:1163`, `ARDOPC.c:1861`). The main
loop is stalled for the entire frame duration, which for a 1000-byte data frame
is on the order of seconds. Everything that must still happen during that window
(host polling, WebGui) is serviced by re-entering the poll functions from inside
`txSleep()` — see [03](03-timing-model.md).

**Sample-at-a-time filtering.** `SampleSink()` processes one sample per call
through a bank of up to 21 IIR resonators, with a `switch (fWidth)` *inside* the
per-resonator inner loop (`Modulate.c:812`). The comment at `Modulate.c:775-776`
explains why — "we don't have enough RAM in embedded systems to hold a full audio
frame", a constraint inherited from Teensy targets. On the Raspberry Pi Zero this
project actually targets, it is pure cost.

**The TX buffer has two different types.** `Modulate.c:667` declares
`extern unsigned short buffer[2][1200]`; `ALSASound.c:92` defines
`short buffer[2][1200]`. C's separate-compilation model means no diagnostic is
possible. It happens to work because the code only ever moves bits through, but
it is the kind of thing that becomes a real bug the moment anyone does arithmetic
on it. See [04](04-coupling-map.md).

---

## Receive

### The chain

```
  snd_pcm_readi()
        ▼
  SoundCardRead(input, 240)                    ALSASound.c:1496
   └─ mono: memcpy   |   stereo: de-interleave  ALSASound.c:1549-1567  ← DEFECTIVE
        ▼
  PollReceivedSamples()                        ALSASound.c:1622
   └─ add_noise() if INPUTNOISE set            noise.c
   └─ track min/max, report clipping
   └─ write RX WAV if enabled
        ▼
  ProcessNewSamples(short*, nSamples)          SoundInput.c:810   ★ the seam
   └─ CheckMemarqTime()                        SoundInput.c:306
   └─ accumulate into rawSamples[2400] until ≥1024
   └─ UpdateBusyDetector()                     BusyDetect.c
        │
        ▼  State == SearchingForLeader
  SearchFor2ToneLeader3()                      SoundInput.c:1604
   └─ Goertzel bins, spectral peak location
   └─ on success: dblOffsetHz (tuning error), State = AcquireSymbolSync
   └─ on failure: advance 240 samples and retry
      (the 480-sample `SlowCPU` branch is unreachable — see 03)
        │
        ▼
  MixNCOFilter(samples, len, dblOffsetHz)      SoundInput.c:587
   └─ NCO mix to baseband + FSMixFilter2000Hz  SoundInput.c:423
   └─ → intFilteredMixedSamples[5000]
        │
        ▼  State == AcquireSymbolSync
  Acquire2ToneLeaderSymbolFraming()            SoundInput.c:1862
        ▼  State == AcquireFrameSync
  AcquireFrameSyncRSB()                        SoundInput.c:1971
   └─ 1000 ms timeout since leader → give up   SoundInput.c:978
        ▼  State == AcquireFrameType
  Acquire4FSKFrameType()                       SoundInput.c:2360
   └─ DemodFrameType4FSK()                     SoundInput.c:2051
   └─ MinimalDistanceFrameType()               SoundInput.c:2137
        │  frame type known → FrameInfo() gives carriers/baud/lengths
        │
        ├─ short control frame → done, no data phase
        │    └─ ★ IRStoISS turnover happens HERE  SoundInput.c:1070-1090
        │
        ▼  State == AcquireFrame
  DemodulateFrame(intFrameType)                SoundInput.c:3192
   ├─ Demod1Car4FSK()                          SoundInput.c:2431
   │   └─ or Demod1Car4FSK_SDFT()              SoundInput.c:2503  (--sdft)
   ├─ DemodPSK()                               SoundInput.c:4577
   └─ DemodQAM()                               SoundInput.c:5034
        ▼
  DecodeFrame()                                SoundInput.c:3349
   └─ CorrectRawDataWithRS()                   SoundInput.c:692
   └─ CRC check
   └─ Memory ARQ averaging on failure          SoundInput.c:149-150
        ▼
  ProcessRcvdARQFrame()      ARQ.c:1113     (ARQ mode)
  ProcessRcvdFECDataFrame()  FEC.c          (FEC mode)
  ProcessRxoFrame()          RXO.c          (RXO mode)
        ▼
  AddTagToDataAndSendToHost()  →  host over TCP
```

### The receive state machine

`enum _ReceiveState` (`ARDOPC.h:241`) is a clean linear acquisition pipeline:

```
SearchingForLeader → AcquireSymbolSync → AcquireFrameSync
                   → AcquireFrameType  → AcquireFrame → DecodeFramestate
```

Any failure resets to `SearchingForLeader` via the recurring triple
`DiscardOldSamples(); ClearAllMixedSamples(); State = SearchingForLeader;`
(appears ~6 times in `ProcessNewSamples` alone).

This FSM is *well designed*. It is genuinely a demodulator concern and nothing
else — the problem is not this machine but that the link-protocol machine is
interleaved with it.

---

## Where the two chains illegally touch

These are the specific points where a rebuild's layering has to differ.

**1. The demodulator transmits.** `SoundInput.c:1070-1090`, inside
`ProcessNewSamples`, on receiving a `DataACK` while in `IRStoISS`:

```c
if (ProtocolState == IRStoISS && intFrameType >= DataACKmin) {
    txSleep(250);                       // blocking wait, re-enters host poll
    blnEnbARQRpt = FALSE;
    ...
    SetARDOPProtocolState(ISS);
    intLinkTurnovers += 1;
    ARQState = ISSData;
    SendData();                         // ← transmits, from the demodulator
    goto skipDecode;
}
```

The comment at `SoundInput.c:1053` ("See if IRStoISS shortcut can be invoked")
is honest about it: this is a latency optimisation that was easier to implement
where the frame type became known than to plumb as an event. It works. It also
means the demodulator cannot be tested without the transmitter, the protocol
state, and PTT.

**2. The demodulator reads protocol state.** `ProcessNewSamples` returns early
when `ProtocolState == FECSend` (`SoundInput.c:822`, and again at `:864`).

**3. The demodulator drives the GUI.** 25 `wg_send_*` calls in `SoundInput.c`,
interleaved with DSP (e.g. `wg_send_rxframet` at `:1065`, `:1067`, `:1109`).

**4. The protocol blocks on wall-clock inside a receive callback.**
`ProcessRcvdARQFrame` — reached from `ProcessNewSamples` — opens with
`txSleep((250 + extraDelay) - timeSinceDecoded)` (`ARQ.c:1127`). The comment at
`ARQ.c:1130` notes this plainly: *"Note this is called as part of the RX sample
poll routine."*

---

## The seam that already exists

`ProcessNewSamples(short *Samples, int nSamples)` is a genuinely good interface:
push samples in, get protocol effects out. `decode_wav()`
(`ARDOPCommon.c:457-623`) exploits exactly this to drive the **entire receive
chain from a WAV file with no sound card**, substituting a synthetic clock
(`WavNow`) for `CLOCK_MONOTONIC`.

That matters more than anything else in this document. The sans-IO architecture
proposed in [06](06-target-architecture.md) is not speculative — half of it is
already implemented and shipping, in `--decodewav`. The work is generalising it
(to the transmit side, and to time everywhere rather than just in `getTicks`),
not inventing it.

---

## Verification notes

Confirmed on Debian 13 / GCC 14.2 at commit `a7c9228`:

- **Build**: fails out of the box — three `-Wint-conversion` errors in
  `lib/rawhid/rawhid.c` (`CM108Handle` is declared `hid_device *` but on Linux
  is assigned from `rawhid_open()`, which returns `HANDLE` = `int`, and passed
  to POSIX `read`/`write`/`close`). GCC 14 promotes this to an error by default.
  Builds cleanly with `make CFLAGS="-g -MMD -Wno-int-conversion"`. This is a
  toolchain-portability instance of the same missing-platform-abstraction theme.
- **TX trace**: `./ardopcf --nologfile 8515 NOSOUND NOSOUND -H "CONSOLELOG 1;MYCALL N0CALL;TXFRAME IDFrame"`
  produces `Sending Frame Type IDFrame` → `[Main.KeyPTT] PTT-TRUE` →
  `LeaderAndSYNC tones` → `Mod4FSKDataAndPlay 1Car tones` →
  `Tx Time 1770 Time till end = 1770` (the `SoundFlush` estimate, `ALSASound.c:1791`)
  → `[Main.KeyPTT] PTT-FALSE`. Matches the trace above.
- **Round trip**: `test/python/test_wav_io.py` passes for every frame type —
  TX → WAV → `--decodewav` → compare. This is the basis of the golden-vector
  strategy in [07](07-migration-path.md).
- **Unit tests**: not run — `libcmocka-dev` is not installed and installing it
  needs root. `make test` is otherwise wired correctly.
