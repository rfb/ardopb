# 08 — Style and tooling

A **deliberate** style for the rebuild, plus the tooling to enforce it.

Two framing decisions up front.

**This is a new style, not an inherited one.** Where a choice happens to match
existing practice it is because the reasoning leads there, not because the code
already does it. Each decision below states what it changes and why. The rebuild
rewrites these files anyway ([07](07-migration-path.md)), so the usual objection
to restyling — destroying `git blame` on inherited code — does not apply to new
work. Inherited files keep their existing style until the stage that replaces
them; there is no big-bang reformat.

**Build tooling and development tooling are different things.** `CONTRIBUTING.md`
says pull requests requiring "additional external libraries or build tools are
unlikely to be accepted", and that constraint is right — the audience is amateur
operators on Raspberry Pis, and `apt install build-essential libasound2-dev &&
make` is a genuine feature.

Nothing here changes that. Every tool below is either **already in GCC** or
**CI/development-only**. The build stays `make` + gcc + libasound.

---

## Part 1 — Style

### Indentation: tabs, width-agnostic — **kept, with reasoning**

The existing code is 100% tab-indented (15,881 tab-indented lines, 0
space-indented) — the one convention that is already perfectly consistent.

Keeping it, for two reasons rather than inertia: tab width is a *reader*
preference, which matters for a project whose stated audience is hobbyists on
whatever editor they already have, and it is the accessible choice for anyone who
needs large indentation to track nesting.

The **new** rule is the important half: **tabs for indentation, never for
alignment.** No aligning continuation lines, trailing comments, or struct members
with tabs — that is what breaks under a different tab width. Wrap and indent
instead of aligning.

*Alternative considered:* spaces (LLVM, Google, WebKit). Renders identically
everywhere and removes the tab/space mixing hazard entirely. Defensible; the
accessibility argument decided it.

### Braces: K&R, always braced — **changed**

Currently mixed: inherited files are predominantly Allman (`SoundInput.c` 251
Allman vs 47 K&R), while the newest and best-factored file, `StationId.c`, is
100% K&R.

```c
if (condition) {
    do_thing();
} else {
    do_other();
}
```

**Braces are mandatory, even for single statements.** This is the load-bearing
part. `-Wdangling-else` currently fires three times in `KeyPTT()`
(`ALSASound.c:1880-1892`) — PTT control, where a misbinding keys or unkeys the
radio wrongly:

```c
if (PTTMode & PTTRTS)
    if (PTTState)
        COMSetRTS(hPTTDevice);
    else                        /* binds to inner if — correct here, but only by luck */
        COMClearRTS(hPTTDevice);
```

Mandatory braces removes this class outright.

### Naming: `snake_case`, module-prefixed — **changed**

Retire Hungarian notation (`bln`, `int`, `dbl`, `byt`, `str`, `tmr`, `dtt`). Not
on taste grounds — **the prefixes now actively lie**. `dbl`-prefixed variables
are `float` (`dblOffsetHz`, `dblCarPh`); `int`-prefixed ones are `short` arrays
(`intPhases`, `intToneMags`) or `float` (`intCarFreq`, `SoundInput.c:84`, whose
comment reads `// (was int)`). A reader must learn a mapping that no longer
holds. That is a direct cost against the project's own goal of being approachable
to newcomers.

| Kind | Convention | Example |
|---|---|---|
| Exported function | `ardop_<module>_<verb>` | `ardop_link_step()` |
| Exported type | `ardop_<noun>` | `ardop_frame_spec` |
| Internal (file-static) | `snake_case` | `compute_tone_mags()` |
| Macro / constant | `ARDOP_SCREAMING_CASE` | `ARDOP_SAMPLE_RATE` |
| Struct member | `snake_case`, no prefix | `spec.baud` |

Every non-`static` symbol gets an `ardop_` prefix — the codebase currently exports
~350 unprefixed globals into a single namespace.

Do **not** mass-rename inherited code. Names change when a module is rewritten.

### Types: `<stdint.h>` and `<stdbool.h>` — **changed**

Delete the typedef layer at `ARDOPC.h:104-118`:

```c
typedef int BOOL;         →  bool          (<stdbool.h>)
typedef unsigned char UCHAR;  →  uint8_t
#define VOID void         →  void
#define HANDLE int        →  a real platform handle type
#define TRUE 1 / FALSE 0  →  true / false
#define True 1 / False 0  →  (delete — four spellings of two values)
```

`#define HANDLE int` is what produced the GCC 14 build failure: `lib/rawhid`
declares `hid_device *CM108Handle` and assigns it from a function returning
`HANDLE` ([04](04-coupling-map.md)). A platform handle needs to be a real type,
not a macro that means different things per OS.

Fixed-width types throughout: `int16_t` for samples, `uint8_t` for frame bytes,
`uint64_t` for the sample clock ([06](06-target-architecture.md) Rule 2),
`size_t` for lengths.

### Headers — **changed**

- One header per module, exposing the minimum. Not `ARDOPC.h`'s 171 externs.
- Include guards named `ARDOP_<MODULE>_H`. **Not** leading-underscore-capital —
  `_TEST_ARDOP_SETUP_H` (`test/ardop/setup.h`) is a reserved identifier under
  C11 §7.1.3. Harmless today, non-conforming.
- No `extern` variable declarations in headers. State lives behind opaque handles.
- No function prototypes in `.c` files. The ad-hoc `wg_send_*` declarations
  repeated across `ARQ.c:31-34`, `RXO.c:12-13`, `Modulate.c:29` become one header.
- Headers self-contained: each compiles standalone.

### Comments — **kept, deliberately**

`CONTRIBUTING.md` asks for "plentiful comments... that explain both what the code
is doing and why", citing `sdft.c` as the model. That is a real project value
tied to its audience, and the rebuild should keep it — this is the one place
where the existing project is ahead of typical practice, not behind it.

Additions: Doxygen `/** */` on exported functions (`StationId.h` already does
this well), and a comment on every normative constant citing the spec section it
comes from. Much of [05](05-essential-vs-incidental.md) exists because that
provenance was never recorded.

### Other

- **Line length 100.** Soft limit; log-message strings may exceed.
- **`static` by default.** Anything not in a header is `static`.
- **`const` correctness**, especially on the normative tables.
- **Declare at first use**, not at function top (C99). Narrows the scope of the
  `intPtr`-style aliasing mistakes below.
- **No VLAs**, no large stack arrays (see `-Wstack-usage` below).

---

## Part 2 — Compiler warnings

The primary tool, because it costs nothing and needs no new dependency.

### Current debt

177 warnings under `-Wall -Wextra` (`make CFLAGS="-g -MMD -Wno-int-conversion -Wall -Wextra"`):

| Count | Warning | Character |
|---:|---|---|
| 52 | `-Wunused-variable` | tidiness |
| 51 | `-Wpointer-sign` | **the `short`/`unsigned short` confusion of [04](04-coupling-map.md)** |
| 22 | `-Wunused-parameter` | tidiness |
| 14 | `-Wunused-but-set-variable` | often dead logic |
| 10 | `-Wswitch` | unhandled enum cases |
| 10 | `-Wsign-compare` | real hazard around buffer lengths |
| 6 | `-Wparentheses` | |
| 3 | `-Wdangling-else` | **PTT control** (above) |
| 2 | `-Wunused-value` | **found a real bug** (below) |
| 2 | `-Wtype-limits` | always-true comparisons |
| 2 | `-Wabsolute-value` | **`fabsf()` on a `double`** — precision loss in DSP |
| 1 | `-Wreturn-type` | falls off the end of a non-void function (`lib/rockliff/rrs.c:521`) |

Two thirds is tidiness. The remaining third contains genuine defects.

### `-Wunused-value` found a real, dormant bug

`Track1Car4FSK()` (`SoundInput.c:4063`) — 4FSK symbol-timing tracking:

```c
if (dblMagEarly > dblMag && dblMagEarly > dblMagLate)
{
    *intPtr --;        /* SoundInput.c:4081 */
```

Postfix `--` binds tighter than unary `*`, so this parses as `*(intPtr--)`:
dereference and **discard** the value, then decrement the *pointer* — which is a
by-value parameter, so the change is lost at return. The intent is `(*intPtr)--`.
Same at `:4088`. The function's entire timing correction is a no-op, while
`Corrections` and the stats counters still update, so logs would look healthy.

**It is dead code** — the only call site, `SoundInput.c:2724`, is commented out.
So this is latent, not a live decoder fault.

But note what it *was* for, from its own comment (`SoundInput.c:4067-4068`):

> *"This should handle sample rate offsets (sender to receiver) up to about 2000 ppm"*

A mechanism to absorb exactly the sample-rate mismatch that [03](03-timing-model.md)
identifies as the root timing problem — written, silently broken, and disabled.
Worth understanding before the rebuild decides how to handle rate offsets.

The point for this document: a compiler flag available since the 1990s finds this
in under a second, and it has been sitting there through two upstream forks.

### Recommended flags

**Tier 1 — adopt now, fix incrementally, then `-Werror` in CI**

```
-Wall -Wextra
```

**Tier 2 — each justified by a specific finding in this review**

| Flag | Catches |
|---|---|
| `-flto` | **Cross-TU type mismatches** — verified below |
| `-Wstack-usage=16384` | The 512 KB and 128 KB stack arrays (`ALSASound.c:1405`, `:1498`) |
| `-Wcast-align` | Unaligned access — **traps on ARM**, i.e. the Pi Zero this project targets |
| `-Wshadow` | Shadowing, likely with ~350 globals in scope everywhere |
| `-Wmissing-prototypes -Wstrict-prototypes` | The ad-hoc prototypes scattered in `.c` files |
| `-Wformat=2` | The many `sprintf`/`snprintf` call sites |
| `-Wwrite-strings` | String literal mutation |
| `-Wvla` | Variable-length arrays |
| `-Wnull-dereference`, `-Wduplicated-cond`, `-Wlogical-op` | General |

**Tier 3 — evaluate, do not adopt blindly**

`-Wconversion` / `-Wsign-conversion` would catch the `short`/`unsigned short`
class directly, but is extremely noisy in DSP code doing deliberate
float↔int narrowing. Consider per-module once the core is separated.

### LTO catches the cross-TU mismatches — verified

The type mismatches in [04](04-coupling-map.md) are invisible to normal
compilation because C matches across translation units by name only. **LTO gives
GCC whole-program visibility and diagnoses them.** Confirmed:

```
$ make CFLAGS="... -flto" LDFLAGS="-flto ..."
src/common/RXO.c:6:14:      type of 'bytFrameData1' does not match original declaration
src/common/Modulate.c:671:  type of 'SendtoCard' does not match original declaration
src/common/ARDOPC.h:375:    type of 'intFSK100bdCarTemplate' does not match original declaration
src/common/ARDOPC.c:72:     type of 'HostCommands' does not match original declaration
```

Four real mismatches, including `SendtoCard` — declared `unsigned short *(unsigned
short *, int)` in `Modulate.c:671` and defined `short *(short *, int)` in
`ALSASound.c:1581`. That is the function-signature form of the `buffer` problem.

`-flto` is a GCC flag, not a new tool. **Adding it to CI is the single
highest-value one-line change available**, because it makes an entire class of
currently-undiagnosable defect visible.

### `-fanalyzer` — static analysis with no new tool

GCC 10+ ships a static analyser: `-fanalyzer` finds use-after-free, double-free,
NULL dereference, leaks, and some buffer issues. It is slow and has false
positives, so run it in CI rather than in the default build — but it needs
**nothing installed** beyond the compiler already required, which makes it the
right first static-analysis step for this project specifically.

---

## Part 3 — Sanitizers

`test/python/README.md` already contemplates this and cites
[issue #37](https://github.com/pflarue/ardop/issues/37):

> *"Running these tests with ardopcf which has been compiled with ASAN/UBSAN may
> be useful."*

Worth doing, and cheap:

```sh
make CFLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
     LDFLAGS="-fsanitize=address,undefined"
cd test/python && python3 test_wav_io.py
```

The round trip exercises every frame type through the full modulate/demodulate
path with no hardware, so it is an excellent sanitizer workload. ASAN would flag
the `SoundCardRead` de-interleave overrun class directly. Built into GCC — no new
dependency.

---

## Part 4 — Formatting and static analysis (development-only)

These *are* new tools. They must be optional, never required to build.

**`.editorconfig`** — adopt now. Not a tool; a config file that most editors
honour natively. Encodes tabs, final newline, trailing-whitespace trimming, and
UTF-8 with zero cost to anyone who does not have it.

**`clang-format`** — adopt with a `.clang-format` encoding the decisions in
Part 1. Apply **per-file as files are rewritten** during migration, not as a
big-bang reformat. Optional for contributors; enforced in CI only on changed
files.

**`clang-tidy`** — valuable for the naming conventions above
(`readability-identifier-naming` can enforce them mechanically) and for
`bugprone-*` checks. CI-only.

**`cppcheck`** — good complement, distinct check set, no compilation database
needed. CI-only.

None are installed on this machine, so all of them are genuinely new
dependencies — which is exactly why they belong in CI and not in `make`.

---

## Part 5 — CI

**There is currently no CI** — no `.github/`, no config of any kind. Given that
the project's stated priority is stability across varied hardware and toolchains,
this is the largest single gap.

A minimal GitHub Actions workflow:

| Job | Purpose |
|---|---|
| Linux build, GCC | catches the GCC 14 `-Wint-conversion` failure |
| Linux build, Clang | different diagnostics |
| Windows cross-build (`mingw-w64`) | the `Makefile` already documents this |
| `make test` | the six cmocka suites |
| `test/python/test_wav_io.py` | the full round trip — the real regression net |
| Same, under ASAN/UBSAN | memory and UB |
| `-flto` build | cross-TU type mismatches |
| `-fanalyzer` | static analysis |

Everything except the mingw cross-compiler is already required to build, and that
is one `apt` line in the workflow. **Contributors' build instructions do not
change at all.**

---

## Adoption order

Sequenced so nothing blocks on a large cleanup, and mapped onto
[07](07-migration-path.md):

| Step | Action | Stage |
|---|---|---|
| 1 | CI building both platforms + round-trip test | Stage 0 |
| 2 | `-Wall -Wextra` non-fatal; record the 177 as a baseline | Stage 0 |
| 3 | `-flto` and ASAN/UBSAN jobs in CI | Stage 0 |
| 4 | `.editorconfig`; `.clang-format` for new files only | Stage 1 |
| 5 | Fix warnings by category, tidiness first | Stage 1 |
| 6 | Add Tier 2 flags as their categories reach zero | Stage 1–2 |
| 7 | `-Werror` once the baseline is clean | Stage 2 |
| 8 | `-fanalyzer`, `clang-tidy`, `cppcheck` in CI | Stage 3 |
| 9 | New style applied to each module as it is rewritten | Stages 1–4 |

Steps 1–3 are worth doing regardless of whether the rebuild proceeds, for the
same reason as Stage 0's golden vectors: they are pure safety net, and they
change no code.
