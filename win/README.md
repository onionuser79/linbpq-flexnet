# Windows build (LinBPQ.exe)

Cross-compiles **LinBPQ for Windows** — the 32-bit console program upstream
distributes as `LinBPQ.exe` — with FlexNet, using
[mingw-w64](https://www.mingw-w64.org/) on Linux or macOS. No Windows
machine or Visual Studio is needed to build it.

> **Status: experimental.** The Windows build compiles the same FlexNet
> code as the Linux build and starts with FlexNet initialised. It has not
> yet carried FlexNet links to live peers on Windows; the Linux build
> remains the tested platform.

## Requirements

| Host | Install |
|---|---|
| macOS | `brew install mingw-w64` |
| Debian / Ubuntu | `sudo apt install gcc-mingw-w64-i686 binutils-mingw-w64-i686 curl perl` |

Plus `git`, and a clone of this repository with upstream LinBPQ as the
`upstream` remote:

```bash
git clone https://github.com/onionuser79/linbpq-flexnet.git
cd linbpq-flexnet
git remote add upstream https://github.com/g8bpq/LinBPQ.git
git fetch upstream
```

## Build

```bash
win/build-win.sh                                    # standard
EXTRA_CFLAGS=-DFLEXNET_PROD=1 win/build-win.sh      # silent (recommended for production)
EXTRA_CFLAGS=-DFLEXNET_DEBUG=1 win/build-win.sh     # debug trace
```

The result is `win/build/LinBPQ.exe` (about 2.4 MB). The script prints
the LinBPQ and FlexNet versions it contains, and the number of
`FlexNet: ` console strings — at most 5 on a silent build, as on Linux.

An optional argument selects the LinBPQ revision (default
`upstream/master`). It must be the LinBPQ version named at the top of the
main [README](../README.md); the overlay replaces whole upstream files.

Every run builds from a fresh export, so switching flavour needs no
clean. The first run also downloads and builds three libraries
(about 1 MB of source, checksums pinned); later runs reuse them.

## Install

`LinBPQ.exe` needs only DLLs that ship with Windows 10 and later. Put it
in its own directory next to `bpq32.cfg`, as with upstream's
`LinBPQ.exe`, and run it from there. The FlexNet directives are the same
as on Linux — see the main [README](../README.md#configuration).

Check the version on the node console:

```
V
Version 6.0.25.41 and FlexNet v2.5.0
```

## How it differs from upstream's LinBPQ.exe

Upstream builds `LinBPQ.exe` with Microsoft Visual C++ from
`MailNode.vcxproj`. This build compiles the same source files with the
same definitions (`LINBPQ`, `NOMQTT`, `_USE_32BIT_TIME_T`), with three
differences:

- **No structured exception handling.** Visual C++ lets upstream catch a
  crash in some routines, log it and carry on; GCC has no equivalent. A
  fault there ends the process — as it does on Linux, where those
  handlers do not exist either.
- **zlib, libconfig and miniupnpc are rebuilt** from source (1.3.1,
  1.7.3 and 2.2.6, matching the headers upstream bundles) because the
  Visual C++ libraries upstream ships do not link with mingw-w64.
- **A handful of declarations are corrected** in the exported copy of
  the upstream sources — places where Visual C++ accepts code that GCC
  rejects. Neither upstream nor this repository is changed.

## Files

| File | Purpose |
|---|---|
| `build-win.sh` | Exports upstream, overlays the FlexNet files, compiles and links |
| `build-deps.sh` | Downloads (checksum-verified) and builds the three libraries |
| `fixups.sh` | Declaration fixes for GCC; each must match exactly once, so a changed upstream file stops the build with the name of the fix |
| `winshim.h` | Included in every file: maps the exception-handling keywords and one name clash |
| `stdexcept.c` | Replaces upstream's crash handler, which uses Visual C++ inline assembly |

## Known limitations

- The debug build writes its trace to `/tmp/flexnet_axudp.log`, which on
  Windows means `\tmp` on the current drive; create that directory to
  get the log.
- MQTT is not available, as in upstream's `LinBPQ.exe`.
