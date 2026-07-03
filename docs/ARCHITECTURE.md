# Architecture

Two programs share one small protocol.

```
  sond (Pi)                                sonview (desktop)
  ┌─────────────────────────┐              ┌──────────────────────────────┐
  │ main.cpp                │              │ net.cpp      TCP client      │
  │  scan, control plane,   │   TCP        │ model.cpp    JSON control    │
  │  accept/dispatch loop   │◀────────────▶│ app.cpp      state + logic   │
  │ capture.cpp             │              │ stores.cpp   sample storage  │
  │  libsigrok datafeed,    │              │ render.cpp   waveform canvas │
  │  libsigrokdecode,       │              │ gui.cpp      SDL2/GL/ImGui   │
  │  streaming, zstd        │              │ export.cpp   VCD / .sr       │
  └─────────────────────────┘              └──────────────────────────────┘
```

## Wire protocol

Every message is a 16-byte little-endian header (`son_hdr`) followed by
`length` bytes of payload. Control messages carry UTF-8 JSON; data-plane
messages carry binary sub-headers plus raw bytes. Data payloads may be zstd
compressed, marked by a flag in the header.

The connection lifecycle is: `HELLO` and `SERVER_INFO`, then any number of
scan or decoder-list requests, then `CONFIG`, `START`, a stream of session
metadata, samples, and annotation batches, and finally `CAPTURE_END`. `STOP`
ends a continuous capture.

The contract lives in `proto/`:

- `son_protocol.h`: message types, header, and binary sub-headers.
- `son_wire.h`: small send and receive helpers used by both sides.
- `PROTOCOL.md`: the JSON schema of every control message.

Both binaries compile against the same headers, so the protocol cannot drift
between them. `tools/son_probe.py` is a third, independent implementation of
the client side, useful for testing the server without the GUI.

## Server (`sond`)

About 1000 lines of C++ in `server/src/`:

- `main.cpp`: control plane. Device scan, decoder enumeration, the TCP
  accept and dispatch loop, and connection policy (keepalive, new client
  replaces a dead one).
- `capture.cpp`: one capture run. Configures the libsigrok device, streams
  logic and analog datafeed packets over the socket, and optionally runs
  libsigrokdecode decoders, batching their annotations back to the client.
  Also serves re-decode sessions, where the client streams stored samples
  back to be decoded again.
- `server.h`: shared structs (device table, capture and decoder config).

A capture runs on its own thread. A watchdog force-stops a capture that runs
far past its expected duration, and a stuck capture thread is detached so it
can never block new client connections.

## Client (`sonview`)

About 4500 lines of C++ in `client/src/`:

| File | Role |
| --- | --- |
| `main.cpp` | Argument parsing, picks GUI or a headless mode. |
| `gui.cpp` | SDL2 + OpenGL + ImGui bootstrap, main loop, docking layout. |
| `app.cpp` / `app.h` | Application state and control logic. Owns the connection, capture state, settings persistence, markers, macros. |
| `net.cpp` / `net.h` | TCP client. Bounded-timeout connect so a wrong IP never hangs the UI. |
| `model.cpp` / `model.h` | Control-plane data model. Parses scan results, decoder lists, and session metadata, and builds `CONFIG`. Shared with `--selftest` so both paths speak identical JSON. |
| `stores.cpp` / `stores.h` | Sample and annotation stores (see below). |
| `chunked.h` | Lock-free, non-reallocating chunked array underlying the stores. |
| `pyramid.h` | Zoom pyramids for fast rendering (see below). |
| `render.cpp` | The waveform canvas: digital traces, analog envelopes, annotation rows, time grid, and cursors, drawn with ImDrawList. |
| `export.cpp` | VCD and sigrok `.sr` export. |
| `zstd_util.h` | Decompresses data-plane frames flagged as zstd. |
| `selftest.cpp` | Headless protocol round trip against a live server. |
| `unittest.cpp` | Store and pyramid self-tests. |

### Threading model

There are two threads that matter: the RX thread, which receives frames from
the socket and appends to the stores, and the render thread, which reads them
every frame. They synchronize without locks on the hot path:

- `chunked.h` is a fixed table of atomic chunk pointers. The single writer
  allocates chunks lazily and publishes each pointer with release semantics;
  readers load with acquire semantics. The table is sized once and chunks
  never move, so a reader may touch any element below the published count.
- Each store publishes its element count with a release store after writing.
  The renderer reads the count with acquire and never looks past it.

So the renderer always sees a consistent prefix of the capture, with no locks
and no copying, while samples continue to arrive.

### Storage and rendering

Logic channels are stored as edges (sample index plus new level), not as raw
per-sample values, so idle signals cost almost nothing. Analog channels are
stored as sample chunks.

Rendering millions of samples per frame works through mip pyramids
(`pyramid.h`, factor 16 per level, 8 levels, covering 2^32 samples):

- Logic uses a transition-OR pyramid. A level-0 flag means "this channel
  changed somewhere in this block of 16 samples"; higher levels OR their
  children. When zoomed out, the renderer walks the pyramid instead of the
  samples, and any block containing a transition draws as an edge. This is
  why single-sample glitches stay visible at any zoom.
- Analog uses a min/max envelope pyramid, so zoomed-out traces draw as
  correct envelopes rather than aliased lines.

Pyramids are grown by the same single writer and read lock-free like the
stores.

### Offline files

A `.son` file is a small magic header (`SONCAP`), the recorded protocol
frames, and one trailing frame that holds markers, macros, and channel
renames. Loading a file replays the frames through the same RX path as a live
capture, which is why files behave exactly like live data, including
re-decoding.

## Cross-compilation

The server targets the Pi (aarch64) but builds on the x86-64 desktop:

- `scripts/make-sysroot.sh` copies the Pi's libraries and headers into
  `.pi-sysroot/` over SSH.
- `cmake/aarch64-pi-toolchain.cmake` points the cross compiler and pkg-config
  at that sysroot and links libstdc++ and libgcc statically.
- `scripts/xbuild.sh` configures and builds into `build-pi/`.
- `scripts/deploy-run.sh` copies the binary to the Pi and runs it;
  `scripts/deploy-decoders.sh` installs `decoders/i2s_ex` there.

## Testing

- `sonview --unittest`: in-process tests of the stores and pyramids.
- `sonview --selftest <host>`: end-to-end protocol round trip against a real
  server, sharing the JSON code with the GUI.
- `sonview --replay <file.son>`: deterministic offline check of the full RX,
  store, and decode-table path.
- `tools/son_probe.py`: independent Python client for probing the server.
- `tools/gen_i2s.py`: synthetic I2S bitstreams for testing `i2s_ex` under
  sigrok-cli, without hardware.
