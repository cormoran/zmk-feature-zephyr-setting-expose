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

The protocol is built around one value type and one flat result vocabulary,
shared by the synchronous and asynchronous delivery paths.

- **`SettingEntry { key, oneof typed_value }`** is the single key+value carrier —
  a read result, the `write` op's payload, and each list entry are all a
  `SettingEntry`. Its `typed_value` oneof adds `too_large` (a uint32 byte
  length): a value that could not be streamed over the size-bounded split relay
  arrives as a `SettingEntry` with `too_large` set, so the key is still known
  (shown + deletable) while the value is absent. Only a peripheral produces it —
  the central's synchronous path stream-encodes.
- **Shared result pieces**: `Ok` (one success for write/delete/gc/clear_all,
  replacing four empty messages), `Error`, `StorageInfo`.
- `Request { target, req_id, oneof op }` where `op` is `List | Read |
  SettingEntry write | Delete | GetStorageInfo | Gc | ClearAll`. `req_id` is
  echoed in every notification so the client can drop stale replies.
- **Synchronous** `Response { oneof result }` = `Error | Ack | ListPage |
  SettingEntry entry | Ok | StorageInfo`. `Ack` is the immediate reply to a
  targeted request (real results follow as notifications); a build without the
  relay returns the real result instead, so the Web UI treats a non-`ack` reply
  as a central-only fallback. `ListPage` (paged entries) exists only here.
- **Asynchronous** `Notification { source, req_id, oneof event }` = `Error |
  SettingEntry entry | ListDone | Ok | StorageInfo | Complete`. It shares the
  same result vocabulary as `Response` (no nested `Response`), plus the
  streaming markers: a `list` streams one `entry` per notification (so the
  peripheral never buffers a page), terminated by `ListDone`; a `TARGET_ALL`
  delete/clear_all ends with `Complete`.

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
  - Peripheral: a non-`list` request → `setting_expose_dispatch` → the flat
    `Response.result` is mapped 1:1 into the matching `Notification` event
    (`entry`/`ok`/`storage_info`/`error`) and sent as one reply. A `list` →
    stream one `Notification{entry}` per cycle via `setting_expose_entry_at`,
    then `Notification{list_done}`. If an entry does not fit the frame it is
    re-sent as the same `entry` with a `too_large` value.
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

The carriers **serialize themselves** via ZMK's serialize/deserialize relay
macros (`ZMK_RELAY_EVENT_*_SERIALIZE` / `ZMK_RELAY_EVENT_HANDLE_DESERIALIZE`,
cormoran/zmk PR #36): the serialize callback writes ONLY the encoded protobuf's
actual bytes (`data[0..len]`) to the wire and returns that length, so each relay
event is exactly as big as the message — a ~8-byte `list` request is an ~8-byte
event, not a padded buffer. `source` is carried by the relay layer itself (loop
guard on send, stamped to the peripheral index+1 on receive), and `req_id` rides
inside the encoded Request/Notification; neither is serialized into the carrier.
The carrier `data[]`/`len` are therefore just a RAM staging area.

`CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` sets both the serialize `max_size` and
that RAM capacity, and the wire `event_data_size` field is a **uint8 (≤255)**, so
it must stay under 255 — and modest, so even a full-size message still fits the
BLE connection's TX buffers when the relay chunks it across the link (a ~240 B
event was observed to fail with ENOMEM on hardware). The module defaults it to
**192**; a value that would not fit is streamed as a `too_large` SettingEntry
marker instead of being dropped. Because a `list` streams one entry per reply, no
per-page buffer is ever allocated. `include/zmk/setting_expose/relay.h`
`BUILD_ASSERT`s the uint8 (≤255) cap. (An earlier version sent the entire carrier
struct whole — `sizeof`, not the used bytes — which at a 512 `DATA_LEN` made every
event ~508 B, over the uint8 cap and far too large for BLE; per-message
serialization fixes that at the source.)

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
