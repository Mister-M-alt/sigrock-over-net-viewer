# sonview UX audit — 2026-07-02

> **STATUS (same day):** ALL findings implemented and verified on hardware —
> tiers A, B, D, and C in full: C13 incl. VCD + .sr export (validated with
> sigrok-cli), C14 post-hoc re-decode (DECODE_REQ/END protocol), C16
> between-marker logic/analog statistics, and finding 46 (channel reorder +
> auto row-height fit). Bonus fix found during verification: ftdi-la silently
> ignores triggers — trigger capability is now queried per device (SR_CONF_
> TRIGGER_MATCH) and reflected in the UI.

Six-agent audit (5 lenses + adversarial critic), 51 raw findings, deduplicated and
verified against the code. Ordered by what most improves the daily bench experience.

## A. Correctness bugs that masquerade as UX problems — fix first

1. **Reconnect after server death crashes the client** (`std::terminate`).
   `rx_loop` self-exits when sond dies, leaving `rx_thread_` joinable; clicking
   Connect again move-assigns into it → abort. One-line fix: join the old thread at
   the top of `do_connect`. `client/src/app.cpp` (do_connect / rx_loop). VERIFIED.
2. **Watchdog kills any triggered capture waiting >30 s for its trigger, then
   reports `complete`**. Budget is computed from post-trigger duration and starts at
   `sr_session_start`; soft-triggered devices (fx2lafw/ftdi-la) waiting for a rare
   event get force-stopped and the empty result looks successful. Fix: start the
   timer at the first datafeed packet, report reason `timeout`, make it configurable.
   `server/src/capture.cpp` watchdog block. VERIFIED (design flaw).
3. **Continuous capture corrupts memory after 2^32 samples** (~3 min @ 24 MS/s).
   Write paths `LogicStore::ensure_word` (stores.cpp:48) and `AnalogStore` (185)
   index `chunks_[slot]` without the `slot >= SLOTS` bounds check the read paths
   have; rolling window frees chunks but never rebases indices. Also: pyramid +
   analog memory is never reclaimed by the rolling window. VERIFIED.
4. **FPS collapses as decode results grow**: `draw_ann_rows` deep-copies every
   annotation of every visible row every frame (`anns_.get()` returns a full vector
   copy incl. strings) + O(N) cull. And `AnnotationStore` is unbounded in continuous
   mode. Fix: sorted visit(lo,hi,cb) with binary search; trim with rolling window.
   `client/src/render.cpp:205`, `stores.cpp:258`.
5. **Silent network partition wedges the server**: single-client accept loop blocks
   forever in `son_recv` on a dead socket (no SO_KEEPALIVE anywhere, listen backlog 1);
   client reconnect handshakes into the backlog and is never serviced → ssh + restart
   sond. Fix: keepalive (~15 s) on accepted fd; poll listen socket, new-client-wins.
   `server/src/main.cpp` run_server/handle_client.
6. **Start irreversibly wipes the previous capture before any new data exists**
   (stores + save buffer cleared on SESSION_META, which precedes the trigger). A
   mis-click destroys captured glitch evidence. Fix: defer wipe to first DATA frame
   and/or warn when unsaved.

## B. Daily-friction eliminators (highest UX return)

7. **Persist settings across restarts** — host/port, device selection, samplerate,
   mode/limit, channel enables+triggers, decoder stacks (id+ch_map+options), renames,
   save path → `~/.config/sonview/config.json`. The serializer already exists
   (`client_state_dump`). Today every morning starts with ~10 repeated setup steps.
8. **Device rescan** — server caches the first scan forever (`if (g_devices.empty())`)
   and the client only scans at connect; hot-plugging the Saleae requires restarting
   sond. Add a Rescan button + server-side re-enumeration when idle.
9. **Reconnect without losing state** — `do_connect` clears devices/channels/triggers;
   SCAN_RESULT should merge enables/triggers by driver+conn identity. Plus: run
   connect on a worker thread with a short timeout ('connecting…'), auto-reconnect
   with backoff, log 'connection lost' from rx_loop, make `--connect` actually connect.
10. **Trigger state feedback** — armed/waiting vs capturing is indistinguishable
    (and today waiting >30 s gets killed by A2). Show 'armed — waiting for trigger'
    (server can send a state event at SR_DF_TRIGGER / first data), draw a trigger
    marker at the trigger sample.

## C. Measurement-tool credibility (Saleae parity)

11. **Marker snap-to-edge** (hold-modifier to disable). Pixel-accurate markers make
    Δt measurements carry error today; the transition pyramid makes nearest-edge
    lookup O(log N).
12. **Pre-trigger capture** — captures start AT the trigger, so the cause is lost.
    libsigrok has SR_CONF_CAPTURE_RATIO (%) — plumb it through CONFIG + UI.
13. **Decoded-data table + export** — annotations exist only as lane rectangles; add
    a searchable table (time, decoder, row, text) with CSV export; add capture export
    to VCD/.sr/CSV so results are shareable with sigrok tooling/colleagues.
14. **Post-hoc decode** — decoders only run during acquisition; adding/repairing a
    decoder after capture (or on a loaded .son) yields nothing. Either client-side
    re-send to server decode session, or persist raw logic and re-run.
15. **Hover readout** — time + per-channel logic value / analog voltage under the
    cursor (Saleae's most-used affordance). Analog rows also need min/max y labels.
16. **Between-marker measurements** — Vmin/Vmax/Vpp/mean for analog, edge count /
    frequency / duty for logic. **Edge/pulse search** ("next pulse < 1 µs").
17. **Repeat/auto-re-arm capture mode** for intermittent events.

## D. Quick wins (small diffs, big feel)

- Show **capture duration** next to sample count (`100 k @ 1 MHz = 100 ms`) and
  live progress (x/N, % during capture).
- **Zoom-to-fit** button + keyboard: space=start/stop, F=fit, arrows=pan, +/-=zoom,
  Home/End. Clamp zoom-out to capture span.
- **Per-channel colors** (and use them in labels + decoder pickers).
- **Annotation tooltip** on hover (full text) + double-click annotation → zoom to it.
- **Start-disabled reason** as tooltip; validate decoder required pins with a clear
  message; 'enable all channels' button; sensible default samplerate (fastest ≤ 24M).
- **Don't yank the view on CAPTURE_END** if the user had zoomed/panned during capture.
- **Persist imgui.ini** (user layout) with a View→Reset-layout item instead of
  discarding customization every launch.
- Marker polish: numeric position entry, 'go to marker', canvas delete (right-click),
  don't drop markers on every bare click (require modifier or double-click).
- Decoder picker: text filter + longname (130-entry flat combo today).
- Deduplicate 'Follow newest' checkbox (Device + Capture panels).
- .son save: timestamped default name, overwrite confirmation, load-file picker.

## Suggested order
1) A1–A6 (bugs; A1/A2/A5 are small), 2) B7–B10, 3) D batch (one pass), 4) C11–C13,
then C14–C17 as follow-ups.
