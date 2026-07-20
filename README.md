# ZMK Feature: Zephyr Setting Expose

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-feature-zephyr-setting-expose/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-zephyr-setting-expose/actions/workflows/zmk-module.yml) [![Devcontainer](https://github.com/cormoran/zmk-feature-zephyr-setting-expose/actions/workflows/devcontainer.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-zephyr-setting-expose/actions/workflows/devcontainer.yml)

This ZMK module exposes the Zephyr settings store (NVS key/value pairs) to a connected browser via the **unofficial** custom ZMK Studio RPC protocol. It lets you inspect, edit, or delete any persisted setting on your keyboard without reflashing firmware.

![screenshot](./img/screenshot.png)

## Summary

- **Firmware**: Custom Studio RPC handler (`src/studio/setting_expose_handler.c`) + shared dispatch (`src/setting_expose_dispatch.c`) + split relay (`src/split/`)
- **Protocol**: Protobuf definition (`proto/zmk/setting_expose/setting_expose.proto`)
- **Web UI**: React + TypeScript app (`web/`) at `https://cormoran.github.io/zmk-feature-zephyr-setting-expose/`
- **Split keyboards**: per-request targeting of the central / a peripheral / all halves (see [Split keyboard support](#split-keyboard-support) and [DESIGN.md](./DESIGN.md))
- **Tests**: Firmware unit tests (`tests/studio/`, `tests/setting_expose/`) and build tests (`tests/zmk-config/`)

### Supported operations

| Operation    | Description                                            |
| ------------ | ------------------------------------------------------ |
| List         | Enumerate persisted settings with typed values (paged) |
| Read         | Read a single setting by key                           |
| Write        | Write (persist) a setting value                        |
| Delete       | Delete (reset) a setting by key                        |
| Storage Info | Query NVS storage capacity (total / used / free bytes) |
| GC           | Trigger NVS garbage collection / sector compaction     |
| Clear All    | Delete every persisted setting on the device           |

### Type system

Settings are displayed and edited with type-aware UIs. Firmware code annotates keys with a type via the `ZMK_SETTING_EXPOSE_REGISTER` macro (see [include/zmk/setting_expose.h](include/zmk/setting_expose.h)):

| Type              | Encoding             | Web UI         |
| ----------------- | -------------------- | -------------- |
| `BYTES` (default) | raw bytes            | hex string     |
| `INT32`           | 4-byte little-endian | number input   |
| `BOOL`            | 1 byte               | `true`/`false` |
| `STRING`          | UTF-8                | text input     |

Well-known ZMK settings (BLE profile, output transport, physical layout, behavior local IDs) are pre-registered in `src/zmk_known_settings.c`.

### Split keyboard support

On a split keyboard each half keeps its **own** settings store, and only the
central talks to ZMK Studio. This module lets the Web UI reach any half by
tagging each request with a **target**:

| Target             | Meaning                                                              |
| ------------------ | ------------------------------------------------------------------- |
| `Central` (0)      | The central half only. Answered synchronously (default).            |
| `Peripheral N`     | One peripheral, addressed by its 1-based index.                     |
| `All halves`       | Central + every peripheral.                                         |

A request for a peripheral (or `all`) is relayed over ZMK's split event-relay;
each half runs it against its own store and the reply comes back to the browser
as a **notification** (the `Central` path stays a plain synchronous response).
The Web UI merges the per-half results and can filter the table by half.

`Clear All` (and per-key delete) with the **All halves** target is ordered: the
central deletes the peripherals **first** and its own store **last** — only once
every peripheral has replied (or a timeout elapses) — then reports completion.
This keeps the central (the source of truth) intact until the peripherals are
confirmed cleared.

To enable it, build **both** halves with the module and `CONFIG_ZMK_SETTING_EXPOSE_SPLIT=y`
(on by default for split boards). See the config section below.

### Loading many settings

The RPC transport is comparatively slow (especially over BLE), so returning every
setting in one response can exceed the client's request timeout on devices with a
lot of stored settings. To avoid this, `List` is **paginated**: `ListRequest`
carries an `offset`/`limit` and `ListResponse` reports `next_offset`/`has_more`.
The Web UI fetches successive pages until `has_more` is false and renders entries
progressively, so loading scales to any number of settings without timing out.

## Module User Guide

### 1. Add dependency to your `config/west.yml`

> **Note**: This module requires a patched ZMK with custom Studio RPC support.

```yml
manifest:
    remotes:
        ...
        - name: cormoran
          url-base: https://github.com/cormoran
    projects:
        ...
        - name: zmk-feature-zephyr-setting-expose
          remote: cormoran
          revision: main
        # Required: patched ZMK with custom Studio RPC support
        - name: zmk
          remote: cormoran
          revision: main+custom-studio-protocol
          import:
              file: app/west.yml
```

### 2. Enable the module in your `config/<shield>.conf`

```conf
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_SETTING_EXPOSE=y

# Increase RPC buffers to fit setting payloads
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES=140
```

#### Split keyboards

For split support, build **both** halves with the module. `CONFIG_ZMK_SETTING_EXPOSE_SPLIT`
defaults to `y` on split boards, so usually no extra config is needed — the
central additionally needs `CONFIG_ZMK_SETTING_EXPOSE=y` above (the peripheral
does not, since it has no ZMK Studio). Relayed replies carry full setting values,
so the split relay payload ceiling is raised to 512 bytes on both halves:

```conf
# Both halves (defaulted by the module; set explicitly if the build asserts):
CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=512

# Central only — raising the Studio TX buffer speeds up the (larger)
# peripheral notifications over the transport:
CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256
```

### 3. (Optional) Register type hints for your own settings

```c
#include <zmk/setting_expose.h>

// Exact key: display as an integer
ZMK_SETTING_EXPOSE_REGISTER(my_volume, "mymod/volume", ZMK_SETTING_TYPE_INT32);

// Prefix: all keys starting with "mymod/name/" displayed as strings
ZMK_SETTING_EXPOSE_REGISTER_PREFIX(my_names, "mymod/name/", ZMK_SETTING_TYPE_STRING);
```

### 4. Open the Web UI

Navigate to [https://cormoran.github.io/zmk-feature-zephyr-setting-expose/](https://cormoran.github.io/zmk-feature-zephyr-setting-expose/), connect your keyboard via serial, and unlock the device in ZMK Studio. Then click **Load Settings** to browse and edit your keyboard's settings.

### Web UI

See [web/README.md](./web/README.md) for web UI development instructions.

### Publishing Web UI

**GitHub Pages**: Visit `Actions > Test and Build Web UI > Run workflow` to deploy.

**Cloudflare Workers (PR previews)**: Configure `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` secrets.

## Module Development Guide

### Setup for running tests

#### Option 0: Dev container (recommended)

Open this repository in VS Code with the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers). The container automatically initializes the west workspace.

#### Option 1: Shared west workspace layout

```bash
mkdir west-workspace && cd west-workspace
git clone <this repository>
west init -l . --mf west/west-test-workspace.yml
west update --narrow
west zephyr-export
```

#### Option 2: Isolated layout

```bash
git clone <this repository> && cd <cloned directory>
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

### Pre-commit

```bash
pip install pre-commit
pre-commit install

# Run manually
pre-commit run --all-files
```

### Running Tests

```bash
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```

### Sync changes from template

Run `Actions > Sync Changes in Template > Run workflow` to get the latest template changes as a pull request.

### Coding agent on Actions

- Mention `@copilot`
- Setup `ANTHROPIC_API_KEY` secret and mention `@claude`
