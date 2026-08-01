# third_party

Vendored dependencies. Pinned by version and checksum, never edited.

## miniaudio

| | |
|---|---|
| File | `miniaudio.h` |
| Version | **v0.11.22** (2025-02-24) |
| Source | <https://raw.githubusercontent.com/mackron/miniaudio/0.11.22/miniaudio.h> |
| SHA-256 | `9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410` |
| Licence | Public domain (Unlicense) or MIT-0, at your option — see the end of the file |

Verify with:

```sh
sha256sum third_party/miniaudio.h
```

### Why it is here

[`analysis/15`](../analysis/15-platform-audio-and-ptt.md) §2 chose miniaudio for
the cross-platform audio backend: it covers WASAPI, CoreAudio, AAudio, OpenSL ES
and ALSA in one file, it brings device enumeration (which we would otherwise
write four times), and it has no build system of its own — which matters for a
program that must build under MinGW, Xcode, the NDK and gcc.

Android is the constraint that actually decided it. PortAudio's Android support
is an unofficial OpenSL ES port and RtAudio has none.

### The tension, stated plainly

This project holds `core/` to zero mutable globals, zero allocation, and
mechanical proof of both (`make check-pure`). miniaudio is ~93,000 lines that
honour none of that.

The answer is containment, not exception:

- It is included by exactly one translation unit, [`shell/ma_impl.c`](../shell/ma_impl.c),
  which is the only file in the tree compiled without `-Wall -Wextra -Werror`.
- [`shell/backend_ma.c`](../shell/backend_ma.c) sees declarations only and is
  held to the full strict bar like everything else.
- `shell/backend_ma.h` exposes an opaque handle, so no other translation unit
  ever names a miniaudio type.
- `make check-standalone` continues to prove that no `core/` object can reach
  any of it.

If it disappoints, it is one file to replace and one interface to reimplement.

### Updating

1. Download the new tag's `miniaudio.h` and replace this one wholesale.
2. Update the version, URL and SHA-256 above.
3. Run `make test-core` (which includes `test_backend_ma`, exercising the ring,
   both resamplers and the PTT drain path against miniaudio's null backend) and
   `make golden-tx`.
4. Re-check the feature-cut `#define`s at the top of `shell/ma_impl.c` still
   match what the release supports.

Never patch this file in place. A local fix that survives an update is a fix
nobody can find; upstream it or wrap it in `backend_ma.c`.
