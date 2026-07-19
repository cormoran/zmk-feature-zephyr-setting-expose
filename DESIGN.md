# Split-keyboard target support — design

## Problem

Only the split **central** runs ZMK Studio, and every setting_expose RPC ran
against the central's local NVS store. Each split **peripheral** has its own
independent settings store that was invisible to the Web UI.

## Goal

Let the Web UI address any half per-request via a `target` field:

- `target = 0` (`TARGET_CENTRAL`) — the central only, answered **synchronously**
  (unchanged, back-compatible, and the only path on a non-split board).
- `target = 1..N` — one peripheral by 1-based source index.
- `target = 0xFFFF` (`TARGET_ALL`) — central + every peripheral.

Peripheral results are delivered to the browser as **notifications**. A
`TARGET_ALL` delete/clear_all deletes the peripherals first and the central
last, then notifies completion.

This mirrors the proven request→relay→peripheral→reply→central→notify pattern in
`zmk-feature-kscan-diagnostics`.

## Key constraints that shaped the design

1. **The relay only broadcasts.** `zmk_split_central_send_relay_event` writes to
   every connected peripheral; a peripheral cannot know its own slot index. So a
   `target = N` request is broadcast to all peripherals (each replies with its
   `source` stamped by the relay-receive path) and the **central filters** the
   replies, forwarding only `source == N`. Correct, mildly wasteful.
2. **No connected-peripheral count.** ZMK exposes no public count of *connected*
   peripherals (`central.c`'s slot table is private). So the delete-all
   completion is **timeout-bounded**: the central finishes when replies reach
   `ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT` **or**
   `CONFIG_ZMK_SETTING_EXPOSE_DELETE_ALL_TIMEOUT_MS` (default 3 s) elapses.
3. **The peripheral has no ZMK_STUDIO.** The per-operation logic and the
   generated proto must therefore build without the Studio subsystem — see the
   module split below.

## Protocol (`proto/zmk/setting_expose/setting_expose.proto`)

- `Request.target` (uint32) + `Request.req_id` (uint32). `req_id` is echoed in
  every notification so the client can drop stale replies.
- `AckResponse` (+ `Response.ack`): the immediate reply to a targeted request on
  a split central (the real results follow as notifications). A build without
  the relay returns the real `Response` instead, so the Web UI treats a
  non-`ack` reply to a targeted request as a central-only fallback.
- `Notification { source, req_id, payload, complete }`: one half's answer.
  `payload` is an **encoded `Response`** (bytes) — carrying it opaquely avoids a
  decode+re-encode of streamed list pages on the central. `complete = true`
  marks the end of a `TARGET_ALL` delete/clear_all.

## Firmware structure

- `src/setting_expose_dispatch.c` (`setting_expose_dispatch(req, resp,
  list_byte_budget)`) — the per-op logic (list/read/write/delete/storage_info/
  gc/clear_all), shared by the central handler and the peripheral relay path.
  Compiled whenever `CONFIG_ZMK_SETTING_EXPOSE OR CONFIG_ZMK_SETTING_EXPOSE_SPLIT`
  so the peripheral (no ZMK_STUDIO) can answer relayed requests.
  - `list_byte_budget`: for a relayed list, the encoder stops before an entry
    would overflow the fixed reply buffer and sets `has_more` (packing as many
    entries as fit, always at least one). `0` = unbounded (the central Studio
    path streams straight to the transport).
- `src/studio/setting_expose_handler.c` — central entry point: decode, then
  `target == 0` → `setting_expose_dispatch` (sync); else → `setting_expose_relay_dispatch`
  (returns `Ack`), or the local fallback when the relay is not built.
- `src/split/setting_expose_relay.c` — the relay glue (both roles):
  - Carriers `se_relay_query` ("SEq", central→peripheral) and `se_relay_reply`
    ("SEr", peripheral→central), each holding an encoded inner Request/Response.
  - A dedicated work queue runs decode/encode and — on the central —
    `raise_zmk_studio_custom_notification`, which is too stack-heavy for the
    system work queue the relay-receive path runs on.
  - Peripheral: decode → `setting_expose_dispatch` (against its own store) →
    encode → `raise_se_relay_reply`.
  - Central: forward each reply as a `Notification`, filtered by the pending
    request's target; for `TARGET_ALL` non-delete ops also emit the central's
    own result as a `source = 0` notification.
  - **Delete-all transaction**: broadcast the delete/clear_all, forward each
    peripheral reply, and — when replies reach the expected count or the timeout
    fires — delete the central's own store and emit
    `Notification{ source: 0, complete: true }`. The transaction state is mutated
    only on the single-threaded relay work queue, so it needs no locking.

## Buffer sizing

Setting values are up to 256 bytes, so one encoded read / single-entry list page
is ~360 bytes. The relay carriers use 480-byte data buffers and the module
defaults `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN = 512` (the relay transport
chunks a frame across the link automatically). `Notification.payload` is capped
to 480 to match. `include/zmk/setting_expose/relay.h` `BUILD_ASSERT`s the fit.

## Web (`web/src/App.tsx`)

- A **target selector** (Central / Peripheral 1..N / All halves) applied to
  load, GC, and clear-all.
- A notification subscription decodes `Notification`s and merges per-`source`
  results. Central loads via the reliable synchronous paginated path;
  peripherals via addressed relay requests (paginated sequentially per source to
  avoid a broadcast pagination race). A non-`ack` reply to a targeted request is
  treated as the central-only fallback.
- A **display filter** narrows the table to one half; per-source section
  headers appear once more than one half is loaded.
- `TARGET_ALL` clear waits for the completion notification, then clears the view.

## Testing

- native_sim unit tests (`tests/setting_expose`, `CONFIG_ZMK_SETTING_EXPOSE_UNIT_TEST`):
  the refactored dispatch, the `target != 0` central-only fallback, an
  encode→dispatch→encode relay round-trip, and the list byte-budget guard.
- Build tests (`tests/zmk-config/build.yaml` + `test.py`): split central and
  split peripheral artifacts prove both role paths and the `BUILD_ASSERT`s
  compile (the unibody `tester_xiao` forces the role via cmake-args).
- Web (jest): target selector, notification merge + display filter, delete-all
  completion (via a controllable `onNotification` mock).
- Full split-relay behavior is validated on the two-XIAO hardware rig (a
  hardware-free bsim two-node test is a possible future addition).
