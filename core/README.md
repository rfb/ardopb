# `core/` — the pure core

The rebuild, layer by layer, following [`analysis/06`](../analysis/06-target-architecture.md).

Nothing in here opens a device, reads a clock, binds a socket, draws anything,
or blocks. It consumes data and produces data. The impure shell stays in `src/`
until Stage 4 replaces it.

The old tree keeps working throughout. `core/` is built and tested alongside it,
not in place of it, until a layer is complete and proven against the
[golden vectors](../test/golden/README.md).

---

## The rules, and how each one is enforced

A convention that relies on someone remembering it is a convention that decays.
Every rule below is checked by something that runs on its own — the compiler,
`make check-pure`, or CI. **If a rule here cannot be mechanically enforced, it is
written as guidance and labelled as such, so nobody mistakes it for a guarantee.**

### 1. No mutable global state — *enforced by `make check-pure`*

The single defect that shaped everything else in the old codebase: ~350 mutable
globals, which is why nothing below the leaf modules can be unit tested.

The check is a link-time property, not a code-review habit. It asks which ELF
*section* each defined symbol lands in:

```sh
objdump -t core/**/*.o | grep -E '[[:space:]](\.data|\.bss|\*COM\*)[[:space:]]'
#   must find nothing
```

`.data` and `.bss` are writable for the whole run, so a symbol there is mutable
global state and fails. `.rodata`, `.data.rel.ro` (mapped read-only after
relocation) and `.text` are fine, so a `static const` table passes and a
`static int counter` fails, whether or not anyone reviewed it.

The obvious check — `nm ... | grep ' [BbDd] '` — is *wrong*, and finding out why
is the reason this keys on the section instead. `nm` labels a `static const`
table that contains pointers as `d`, which reads as mutable data. In a
position-independent build such a table is emitted to `.data.rel.ro`, which the
loader maps read-only once its relocations are applied; it is not writable. The
nm-letter check rejects every const table with a string or pointer in it —
including this module's frame table. The section is the truth; the letter is not.

Run against the inherited tree, `.data`/`.bss` symbols number 117 for `ARDOPC.o`
and 0 for `Packed6.o` — it measures exactly the thing that made one testable and
the other not. State lives in caller-owned structs passed explicitly.

### 2. Every warning is an error — *enforced by `-Werror`*

`core/` builds with `-Wall -Wextra -Werror` from its first file, which is the
only moment it is free. The old tree carries 177 warnings and is exempt until
its files are replaced.

### 3. No hidden allocation — *guidance, partially enforced*

The core does not call `malloc`. Callers own storage and pass it in; sizes are
passed with pointers, never inferred. This makes the core usable from an
embedded shell and from another language without a shared allocator, and it
removes leak-shaped bugs by construction.

Partially enforced: `make check-pure` also greps the link map for `malloc`,
`calloc`, `realloc`, `strdup` and `free`. That catches direct calls, not
transitive ones.

### 4. Fixed-width types — *enforced by the compiler*

`<stdint.h>` and `<stdbool.h>` only. No `BOOL`, `UCHAR`, `VOID`, `HANDLE`. The
old typedefs are what produced the GCC 14 build failure in `lib/rawhid`.

### 5. `static` by default — *enforced by `-Wmissing-prototypes`*

Anything not in a header is `static`. `-Wmissing-prototypes` fails the build on
a non-static function with no prototype, so the default is enforced rather than
remembered.

### 6. `const`-correct — *enforced by `-Wdiscarded-qualifiers`*

Inputs are `const`. Lookup tables are `static const` (which is also what makes
rule 1 pass).

### 7. One header per module, self-contained — *enforced by CI*

Each header compiles standalone. CI compiles every header on its own; a header
that forgets an include fails there rather than mysteriously later.

### 8. Return values that must be checked — *enforced by `ARDOP_MUSTUSE`*

Functions that can fail are marked `ARDOP_MUSTUSE` (`warn_unused_result`), so
discarding a status is a build failure. Reused from `src/common/mustuse.h`,
which the old tree already got right.

---

## Style

Settled in [`analysis/08`](../analysis/08-style-and-tooling.md), which records
why each choice was made rather than inherited. Briefly: tabs; K&R braces,
always braced; `snake_case`; `ardop_` prefix on anything public; no Hungarian
notation; `ARDOP_<MODULE>_H_` include guards; Doxygen on exported functions;
comments that explain *why*.

The last one matters more here than usual. Most of
[`analysis/05`](../analysis/05-essential-vs-incidental.md) exists because nobody
recorded why a constant had its value, and separating protocol from accident
meant reverse-engineering a VB translation. **Every normative constant carries a
comment saying where it comes from.**

---

## Layout

```
core/
  codec/     frame tables, RS, CRC, Packed6, StationId   — pure, no state
  modem/     modulate / demodulate / sync / busy detect  — explicit state
  link/      ARQ + FEC + RXO state machines              — explicit state
```

Dependencies point down only: `link` → `modem` → `codec`. Nothing in `core/`
includes anything from `src/`.

Include paths are rooted at `core/`, so a module is included as
`#include "codec/frame.h"` — matching the existing `-Isrc` convention.

---

## Building

```sh
make core          # build core objects with -Werror
make check-pure    # assert no mutable globals, no allocation
make test-core     # run core unit tests
```

`make` still builds `ardopcf` exactly as before. `core/` does not yet
participate in the program.

---

## Status

| Layer | Module | State |
|---|---|---|
| `codec` | `frame` — frame type table | **done**, all 256 types proven equivalent to `FrameInfo()` |
| `codec` | `crc` — CRC-16 and CRC-8 | **done**, proven equivalent to `GenCRC16()`/`GenCRC8()` over a random corpus |
| `codec` | `packed6` — 6-bit callsign/grid packing | **done**, proven equivalent to the original `Packed6` over a random corpus |
| `codec` | `rs` — Reed-Solomon FEC | **done**, 5 globals moved into a caller-owned context; tables and encode/decode proven equivalent to `lib/rockliff/rrs` |
| `codec` | `stationid` — callsign + SSID | **done**, proven equivalent to the original `StationId` (one non-normative `strerror` off-by-one fixed) |
| `codec` | `locator` — Maidenhead grid square | **done**, proven equivalent to the original `Locator` (empty-input now zeroes its output) |
| **`codec`** | **layer complete** | frame, crc, packed6, rs, stationid, locator |
| `modem` | `modulate` — TX (4FSK 50/100 baud) | **in progress**, every sample proven bit-identical to ardopcf; 600-baud 4FSK and PSK/QAM pending |
| `modem` | demodulate / sync / busy | not started |
| `link` | — | not started |

One temporary bridge: `modem/modulate` reaches the carrier-waveform templates
(`int50BaudTwoToneLeaderTemplate` etc.) via `extern` declarations in
`modem/templates.h`, whose definitions still live in the inherited
`src/common/ardopSampleArrays.c`. Those are `const` generated data, so
`check-pure` stays clean; relocating the generated file into `core/` is a
tracked follow-up. It is the one place a core object currently links against a
`src/` definition.

### A note on "these modules already qualify"

[`analysis/05`](../analysis/05-essential-vs-incidental.md) listed nine modules as
carrying across largely as-is because they "already have interfaces, no
globals". Running rule 1's check against them shows that claim was too
generous:

| Module | Mutable globals |
|---|---|
| `Packed6.o`, `FFT.o`, `log_file.o`, `StationId.o`, `Locator.o` | 0 — genuinely clean |
| `wav.o` | 1 — a global header struct; WAV writing is not reentrant |
| `noise.o` | 2 — Box-Muller spare-value cache |
| `sdft.o` | 5 — real module state |
| `lib/rockliff/rrs.o` | 5 — Galois field tables, in the most normative code in the system |
| `log.o` | 21 — a global service by design |

An earlier version of this table listed `StationId.o` and `Locator.o` as having
one global each, "a non-`const` string table". That was the `nm`-letter false
positive that also shaped rule 1's check (see rule 1): the `strerror` message
tables are `const` and land in `.data.rel.ro`, which `nm` mislabels `d`. The
section-based `objdump` check reports them correctly as clean, which the
completed `stationid`/`locator` ports confirm. `sdft.o` and `rrs.o` are the real
module state; `rrs.o` is now moved.

So some of them still need a state handle before they can move, and the
"~2500 lines carry across unchanged" estimate is optimistic — but fewer than the
first pass feared. This is exactly the kind of thing rule 1 exists to surface.
