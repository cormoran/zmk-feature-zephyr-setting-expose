# ZMK Setting Expose – Web Frontend

A React + TypeScript web app for browsing and editing Zephyr settings stored on a ZMK keyboard, using the custom Studio RPC protocol.

## Features

- **Device Connection**: Connect via Serial (WebSerial API)
- **List Settings**: Enumerate all persisted NVS settings, grouped by key prefix
- **Type-aware display**: `int32`, `bool`, `string`, and raw `bytes` rendered appropriately
- **Edit / Write**: Inline editor with type validation
- **Delete**: Remove individual settings (with confirmation)
- **Storage Info**: Visual capacity bar (total / used / free bytes)
- **GC**: Trigger NVS sector garbage collection
- **Clear All**: Wipe every setting on the device (with confirmation)

## Quick Start

```bash
# Install dependencies
npm install

# Generate TypeScript types from proto
npm run generate

# Run development server
npm run dev

# Build for production
npm run build

# Run tests
npm test
```

## Project Structure

```
src/
├── main.tsx              # React entry point
├── App.tsx               # Main application component
├── App.css               # Styles
└── proto/                # Generated protobuf TypeScript types
    └── zmk/setting_expose/
        └── setting_expose.ts

test/
├── App.spec.tsx              # Tests for App component
└── RPCTestSection.spec.tsx   # Tests for RPC functionality
```

## How It Works

### 1. Protocol Definition

The protobuf schema is defined in `../proto/zmk/setting_expose/setting_expose.proto`.
Messages support a `oneof typed_value` field (`bytes`, `int32`, `bool`, `string`) so the
UI can render an appropriate editor per entry.

### 2. Code Generation

TypeScript types are generated using `ts-proto`:

```bash
npm run generate
```

This runs `buf generate` using the configuration in `buf.gen.yaml`.

### 3. Using react-zmk-studio

The app uses the `@cormoran/zmk-studio-react-hook` library:

```typescript
import { useZMKApp, ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";

const { state, findSubsystem } = useZMKApp();

const subsystem = findSubsystem("zmk__setting_expose");
const service = new ZMKCustomSubsystem(state.connection, subsystem.index);

// Example: list all settings
const payload = Request.encode(Request.create({ list: {} })).finish();
const raw = await service.callRPC(payload);
const response = Response.decode(raw);
```

The device must be **unlocked in ZMK Studio** before the subsystem accepts requests.

## Testing

```bash
npm test             # run all tests
npm run test:watch   # watch mode
npm run test:coverage
```

### Writing Tests

```typescript
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";

const mockZMKApp = createConnectedMockZMKApp({
  deviceName: "Test Device",
  subsystems: ["zmk__setting_expose"],
});

render(
  <ZMKAppProvider value={mockZMKApp}>
    <SettingsSection />
  </ZMKAppProvider>
);
```
