# sonview

A remote logic analyzer built on sigrok.

A Raspberry Pi at the bench runs the capture server, `sond`. Your desktop runs
the viewer, `sonview`. They talk over TCP, so the analyzer hardware can sit
next to the target while you work from anywhere on the network.

- `sond` links libsigrok and libsigrokdecode. It drives the capture device,
  runs protocol decoders, and streams samples and decoder annotations to the
  client.
- `sonview` is a Dear ImGui + SDL2 + OpenGL desktop app in the style of the
  Saleae Logic software: a waveform canvas with pan and zoom, cursors, analog
  traces, and decoder annotation lanes.

```
  bench Pi (aarch64)                          local desktop (x86-64)
  ┌───────────────────────────┐   TCP :5620   ┌────────────────────────────┐
  │ sond                      │◀─────────────▶│ sonview (ImGui/SDL2/GL)    │
  │  libsigrok  capture       │  16B header   │  waveform canvas, decoders │
  │  libsigrokdecode  decode  │  JSON ctrl +  │  cursors, analog, markers  │
  └───────────────────────────┘  binary data  └────────────────────────────┘
```

The wire protocol is defined in `proto/son_protocol.h` (binary structs) and
`proto/PROTOCOL.md` (JSON schemas). `docs/ARCHITECTURE.md` explains how the
code fits together.

## Features

### Capture
- Device scan over the network, with a Rescan button for hot-plugged devices.
- Per-channel enable and triggers (edge or level). Triggers set on several
  channels are AND-combined into one stage. Devices without real trigger
  support (such as `ftdi-la`) are labelled as such instead of silently
  ignoring triggers.
- Triggered (fixed length) and continuous (rolling) capture modes.
- Pre-trigger capture (`capture_ratio`), an "armed, waiting for trigger"
  status, a trigger point marker on the canvas, capture progress, and a Repeat
  mode that re-arms automatically.
- Starting a new capture keeps the previous one until new data actually
  arrives.

### Viewing
- Logic and analog channels. Smooth pan and zoom over millions of samples,
  backed by a transition-OR mip pyramid, so short glitches stay visible when
  zoomed far out.
- The timebase rescales with zoom (ns up to s), with a live samples-per-pixel
  and span readout.
- Channels can be renamed and reordered. Rows shrink automatically so every
  channel and decode lane stays visible. Per-channel trace colors. Analog rows
  show min and max scale labels.
- Hover readout shows the time and the value of the channel under the cursor.
- Keyboard: Space starts or stops, F fits, arrow keys pan, + and - zoom,
  Home and End jump to the ends.
- Docked, resizable IDE-style layout with a menu bar.

### Markers and measurements
- Click the waveform to drop named markers and drag them to move. Markers snap
  to signal edges; hold Alt for free placement. Numeric position entry, go-to,
  and right-click delete.
- A Markers table plus an on-canvas readout of the time delta and frequency
  between markers, down to ns as you zoom in.
- Between-marker measurements: edge count, frequency, and duty cycle per logic
  channel; min, max, peak-to-peak, and mean per analog channel, over the last
  two markers.
- Macros: an expression evaluator over marker variables. `m1..mN` are marker
  times in seconds, `s1..sN` are sample indices, `sr` is the samplerate, and
  `abs` and `log10` are available. Example: `1/(m2-m1)`.

### Protocol decoding
- All libsigrokdecode decoders (UART, SPI, I2C, and more; 112 available), with
  an option UI generated from decoder metadata and annotation lanes under the
  signals.
- A searchable decoded-data table with double-click to jump, plus CSV export.
- Post-hoc re-decode: add or reconfigure decoders after a capture (or on a
  loaded file) and press "Re-decode captured data". The client streams the
  stored samples back to the server's decode session.
- A bundled I2S decoder, `i2s_ex`, that handles the formats the stock sigrok
  decoder does not: Philips I2S, left-justified, right-justified, and PCM/DSP,
  with configurable word size, bit order, sample edge, and word-select
  polarity. `tdm_audio` covers TDM.

### Files and export
- Save and load captures as `.son` files (the recorded protocol stream, so a
  file can be viewed fully offline). Markers, macros, and channel renames are
  stored too.
- Export to VCD and sigrok `.sr`, both verified readable by sigrok-cli and
  PulseView. Channel renames are preserved.
- Settings persist across restarts in `~/.config/sonview/config.json`: host,
  device, samplerate, channels, triggers, renames, and macros.

### Reliability
- Non-blocking connect with a timeout and auto-reconnect with visible status.
  Channel and trigger state survives reconnects and rescans.
- The server survives silent network partitions: TCP keepalive plus a
  new-client-wins policy, so a fresh connection replaces a dead one.
- zstd compression on the wire, applied adaptively (skipped when a payload is
  incompressible). It also shrinks saved files.

## Build

### Client `sonview` (native)

Requires SDL2 and OpenGL dev packages. Dear ImGui is a git submodule in
`third_party/imgui` (docking branch); nlohmann/json is vendored.

```
git submodule update --init        # first build only (or clone with --recursive)
cmake -S . -B build -G Ninja -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build            # result: build/client/sonview
```

### Server `sond` (cross-compiled for the Pi from an x86-64 Arch host)

1. Install the toolchain:
   `sudo pacman -S --needed aarch64-linux-gnu-gcc aarch64-linux-gnu-binutils aarch64-linux-gnu-glibc`
2. Pull a sysroot from the Pi (needs SSH access to the Pi host):
   `scripts/make-sysroot.sh`
3. Build: `scripts/xbuild.sh` (result: `build-pi/server/sond`)
4. Deploy the bundled decoder: `scripts/deploy-decoders.sh` copies
   `decoders/i2s_ex` to the Pi.

The toolchain file `cmake/aarch64-pi-toolchain.cmake` uses the Pi rootfs as
the sysroot and links libstdc++ and libgcc statically, so the binary runs on
the Pi without extra runtime packages.

A native build on the Pi should also work with plain CMake, since the server
only needs pkg-config to find libsigrok, libsigrokdecode, and libzstd.

## Run

On the Pi:

```
sond --listen 5620             # or: scripts/deploy-run.sh --listen 5620
```

On the desktop:

```
build/client/sonview --connect <pi-ip>
```

Then press Connect, pick a device, enable channels, add decoders if you want
them, and press Start. Click the waveform to drop markers, add macros in the
Measurements panel, and save or load captures from the Capture panel or the
File menu.

Other modes:

- `sonview --selftest <host> [port]` runs a headless protocol round trip and
  prints a summary.
- `sonview --replay <file.son>` loads a saved capture headlessly and prints a
  summary. Add `--export <base>` to write `<base>.vcd` and `<base>.sr`.
- `sonview --autocapture --connect <host>` auto-connects and runs a demo
  capture (useful for screenshots and smoke tests).
- `sonview --unittest` runs the store and pyramid self-tests.
- `sond --scan` and `sond --decoders` list devices or decoders for debugging.

## Tools

- `tools/son_probe.py <host> [port] [--driver d] [--rate hz] [--samples n]
  [--continuous s] [--decoder id:ch=idx:opt=val]` is a small reference client
  for the protocol, useful for testing the server without the GUI.
- `tools/gen_i2s.py <out.bin> [i2s|left|right]` generates synthetic I2S data
  for decoder testing:
  `sigrok-cli -i out.bin -I binary:numchannels=3 -P i2s_ex:sck=0:ws=1:sd=2:format=i2s -A i2s_ex`

## Repository layout

- `proto/` is the wire protocol: `son_protocol.h` (binary structs),
  `PROTOCOL.md` (JSON schemas), `son_wire.h` (send and receive helpers).
- `server/` is `sond`, the Pi-side capture server.
- `client/` is `sonview`, the desktop viewer.
- `decoders/i2s_ex/` is the bundled multi-format I2S decoder (Python, runs
  inside libsigrokdecode on the server).
- `scripts/` holds the sysroot, cross-build, and deploy helpers.
- `tools/` holds the protocol probe and the test-data generator.
- `third_party/` holds Dear ImGui (git submodule) and vendored nlohmann/json.
- `docs/` holds the architecture notes and the UX audit.

## Known limits

- `fx2lafw` and `ftdi-la` have no hardware trigger, so libsigrok soft-triggers
  in software, which adds latency.
- Live mode relies on normal TCP flow control. There is no credit-based
  backpressure yet.
- One client at a time.
- The panel layout rebuilds to a clean default on each launch. Settings
  persist, the window layout does not.
- A watchdog force-stops a capture that runs far past its expected duration,
  and a stuck capture thread is detached so it never blocks new client
  connections.

## License

sonview is free software, released under the GNU General Public License,
version 3 or later. See [LICENSE](LICENSE).

Why GPL: the server links libsigrok and libsigrokdecode, which are themselves
GPL-3.0-or-later, so any distributed build of `sond` must be GPL anyway. Using
one license for the whole project keeps things simple, and it matches the rest
of the sigrok ecosystem (PulseView is GPLv3 too).

Third-party code in `third_party/` keeps its own license:

- `third_party/imgui` (Dear ImGui, submodule): MIT
- `third_party/json` (nlohmann/json, vendored): MIT

The `i2s_ex` decoder is licensed GPL-2.0-or-later (compatible with GPLv3) so
it can be contributed upstream to libsigrokdecode, which uses that license for
its decoders.
