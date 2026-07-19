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
- `Notification { source, req_id, oneof event }` — **type-safe**, no opaque
  bytes. The `event` is one of:
  - `SettingEntry entry` — one streamed `list` entry. A relayed `list` sends
    **one entry per notification** (so the peripheral never buffers a page —
    minimal RAM), terminated by `ListDone list_done`.
  - `Response response` — the single result of any non-`list` op
    (read/write/delete/storage_info/gc/clear_all/error).
  - `Complete complete` — a `TARGET_ALL` delete/clear_all finished.
  - `EntryTooLarge entry_too_large` — a `list` entry whose value did not fit one
    relay frame (only reachable over the size-bounded split relay, never on the
    central's stream-encoded synchronous path). Carries the key + value size so
    the web shows the setting with a "value too large" marker and still allows
    deleting it.

## Firmware structure

- `src/setting_expose_dispatch.c`:
  - `setting_expose_dispatch(req, resp)` — the per-op logic (list/read/write/
    delete/storage_info/gc/clear_all), shared by the central handler and the
    peripheral relay path. Compiled whenever `CONFIG_ZMK_SETTING_EXPOSE OR
    CONFIG_ZMK_SETTING_EXPOSE_SPLIT` so the peripheral (no ZMK_STUDIO) can answer.
  - `setting_expose_entry_at(index, out)` — fetch the `index`-th setting into one
    `SettingEntry`. Backs the memory-frugal relayed `list` (one entry per
    notification instead of a buffered page).
- `src/studio/setting_expose_handler.c` — central entry point: decode, then
  `target == 0` → `setting_expose_dispatch` (sync); else → `setting_expose_relay_dispatch`
  (returns `Ack`), or the local fallback when the relay is not built.
- `src/split/setting_expose_relay.c` — the relay glue (both roles). All heavy
  work runs on **ZMK's shared low-priority work queue**
  (`zmk_workqueue_lowprio_work_q()`), **one item per work cycle**: the Studio BLE
  GATT transport drains its TX ring buffer on that same queue, so emitting a
  burst of notifications without yielding would self-deadlock. Re-submitting the
  work item between items yields the queue so the transport can drain.
  - Carriers `se_relay_query` ("SEq", central→peripheral, encoded Request) and
    `se_relay_reply` ("SEr", peripheral→central, encoded Notification event).
  - Peripheral: a non-`list` request → `setting_expose_dispatch` → one
    `Notification{response}` reply. A `list` → stream one `Notification{entry}`
    per cycle via `setting_expose_entry_at`, then `Notification{list_done}`.
  - Central: decode each reply, stamp the real `source`, forward it — one
    notification per cycle — filtered by the pending request's target. The
    central's OWN store is read via the synchronous path (target 0), so there is
    no central-own notification emission.
  - **Delete-all transaction**: broadcast the delete/clear_all, forward each
    peripheral reply, and — when replies reach the expected count or the timeout
    fires — delete the central's own store and emit `Notification{complete}`. The
    transaction state is mutated only on the single-threaded low-priority queue,
    so it needs no locking.

## Buffer sizing

The relay carrier data buffers are **derived** from the relay payload ceiling —
`SE_RELAY_*_DATA_MAX = CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN − header(4)` — so
they track whatever the config resolves to instead of being hard-coded. The
module defaults `DATA_LEN = 512`, which comfortably fits one entry (key + up to
a 256-byte value + framing); the transport chunks a frame across the link
automatically, and because a `list` streams one entry per reply no per-page
buffer is ever allocated. If a value does not fit a (smaller) frame, the
peripheral streams `entry_too_large` instead of dropping it.
`include/zmk/setting_expose/relay.h` `BUILD_ASSERT`s the header offset and a
sane minimum size.

## Web (`web/src/App.tsx`)

- A **target selector** (Central / Peripheral 1..N / All halves) applied to
  load, GC, and clear-all.
- A notification subscription decodes the typed `Notification` and merges
  per-`source` results: `entry` events accumulate into the source's list until
  its `list_done`; `response` events resolve a discrete op; `complete` ends a
  delete-all. Central loads via the reliable synchronous paginated path;
  peripherals stream over the relay. A non-`ack` reply to a targeted request is
  treated as the central-only fallback.
- A **display filter** narrows the table to one half; per-source section
  headers appear once more than one half is loaded.
- `TARGET_ALL` clear waits for the completion notification, then clears the view.

## Testing

- native_sim unit tests (`tests/setting_expose`, `CONFIG_ZMK_SETTING_EXPOSE_UNIT_TEST`):
  the refactored dispatch, the `target != 0` central-only fallback, a
  dispatch→Notification→encode/decode round-trip, and `setting_expose_entry_at`
  streaming (each entry fits the relay reply buffer; end-of-stream reported).
- Build tests (`tests/zmk-config/build.yaml` + `test.py`): split central and
  split peripheral artifacts prove both role paths and the `BUILD_ASSERT`s
  compile (the unibody `tester_xiao` forces the role via cmake-args).
- Web (jest): target selector, notification merge + display filter, delete-all
  completion (via a controllable `onNotification` mock).
- Full split-relay behavior is validated on the two-XIAO hardware rig (a
  hardware-free bsim two-node test is a possible future addition).
