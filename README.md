# Infinity Link Direct

An [Infinity Link](https://github.com/tekchansin/Infinity-link) endpoint for Windows that plays through a DAC maker's
own ASIO® driver — the maker's path to the DAC, and **native DSD** where that driver offers it (Windows' own audio,
WASAPI, has no DSD: only DoP).

<img src="third_party/asiosdk/logo/ASIO-compatible-logo-Steinberg-TM-BW.svg" alt="ASIO Compatible" width="120"/>

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

## Use

```
Infinity-Link-Direct --list                        the ASIO drivers on this PC and what each one plays
Infinity-Link-Direct --driver "iBasso DX340 ASIO"  an endpoint on the network, found by any Infinity Link sender
Infinity-Link-Direct --driver NAME --local         this PC only (127.0.0.1, not announced): prints "port N", ends with its stdin
Infinity-Link-Direct --driver NAME --panel         the driver's own settings window
```

Options: `--name S` (what senders show), `--port N`, `--buffer MS`, `--volume fixed|software` (fixed: nothing touches
the samples, the volume is the DAC's; software: the sender's volume is applied here, TPDF dither below 100 %, and DSD is
then sent as PCM), `--any-address` (senders from outside private networks too).

Infinity Core starts it by itself (Settings › Audio): there is nothing to run by hand.

## How it plays

- Offers the sender only what the driver plays as it is (`canSampleRate`, PCM 44.1–768 kHz; native DSD64–512 through
  `kAsioSetIoFormat` / `kASIODSDFormat`, DSD MSB- or LSB-first).
- Every ASIO call on one thread (COM single-threaded apartment + a hidden window); the driver's `bufferSwitch` reads a
  lock-free FIFO and never waits. Sample types: Int16/24/32 LSB, Int32 LSB 16/18/20/24, Float32/64 LSB.
- Pause = silence at once (DSD: 0x69, the DAC stays in DSD). A driver's reset request reopens it. No `bufferSwitch` for
  1 s while playing = the DAC went away: the sender is told.
- The driver is loaded only while something plays (and 10 s after), so other programs can use the DAC otherwise.
- x64 only: a 32-bit-only driver cannot load into it.

## Build

`./tools/build-windows.sh` — llvm-mingw in Docker (on the Mac it runs on the build server) → `dist/`.

## Licence

**GPLv3** (LICENSE) — it is built with Steinberg's ASIO SDK 2.3.4 under the SDK's GPLv3 option (`third_party/asiosdk`,
its LICENSE.txt). The Infinity Link library in `third_party/infinitylink` is MIT (its LICENSE; VERSION says which
commit, and the one local change).
