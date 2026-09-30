# Licensing

This repository is a **fork of FxSound**, which is licensed under the
**GNU Affero General Public License, version 3 (AGPL-3.0)**. See [`LICENSE`](LICENSE).

Because the AGPL is a copyleft licence, the project as a whole — and any build
you make from it — remains under the AGPL-3.0. Forking, modifying and
redistributing it is permitted, but the AGPL's conditions apply:

- if you distribute a modified version, you must publish the complete
  corresponding source under the AGPL-3.0 as well;
- if you run a modified version as a network service, users interacting with it
  over a network must be offered the corresponding source (AGPL section 13);
- the licence notices and copyright notices must be preserved.

The original copyright is held by **FxSound LLC**. The upstream project is
<https://github.com/fxsound2/fxsound-app>.

## Linux-specific changes in this fork

The Linux port and the fixes in this fork were written by
**AlexC1991** (2026) and are contributed under the terms of the AGPL-3.0, to
match the rest of the project.

## File-level exception: MIT

One file in this fork is an original work and is additionally offered under the
**MIT License**:

| File | Licence |
| --- | --- |
| `audiopassthru/include/VoxLimiter.h` | MIT — Copyright (c) 2026 AlexC1991 |

It carries an `SPDX-License-Identifier: MIT` tag and the full MIT text in its
header. This grants permissive terms for that file on its own; it does **not**
relicense the project, and any distributed build that links it still falls under
the AGPL-3.0 as described above.

### Provenance of VoxLimiter.h

`VoxLimiter.h` is not a new invention written for this fork: it is a port of the
author's own Rust audio engine, written as the mastering backend for their
website (the `vox-auto-master-v3` processing recipe, used to master the audio
tracks users create on that site). The design and every limiter constant
(threshold −2.0 dB, target −1.0 dB, knee 2.0 dB, look-ahead 1 ms, release
20 ms) come from that engine, and were carried over verbatim.

### Why it exists on Linux

On Windows, FxSound's volume is applied by the operating system at the audio
endpoint (`IAudioEndpointVolume`, used by
`sndDevices/sndDevicesVolCallbacks.cpp`), which is *after* the DSP — so a volume
boost lands where it can be heard. The Linux/PipeWire port has no equivalent
volume stage, so the only loudness control available (FxSound's own volume) is
applied inside the DSP, where the engine's normalization absorbs it. Gaining
volume on Linux was therefore much harder than on Windows.

This limiter restores the missing behaviour, sitting after the DSP and after
FxSound's own volume. It is the reason a hard boost now produces louder sound
instead of clipping.

## Using this fork in your own project

- **As a whole application** (or any binary linking the FxSound code): the
  AGPL-3.0 applies. Your changes must be released under the AGPL-3.0.
- **The limiter alone**: `VoxLimiter.h` is a self-contained header with no
  FxSound dependencies, so it can be copied into another project under the MIT
  terms.

If you are unsure whether your intended use is compatible, the AGPL-3.0 is
copyleft and deliberately strict — check with the FSF's guidance or your own
legal advice before distributing.
