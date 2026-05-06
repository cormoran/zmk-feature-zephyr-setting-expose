import { useCallback, useContext, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  SettingEntry,
  SettingType,
} from "./proto/zmk/setting_expose/setting_expose";

export const SUBSYSTEM_IDENTIFIER = "zmk__setting_expose";

// ---- Type helpers ---------------------------------------------------------

function settingTypeLabel(type: SettingType): string {
  switch (type) {
    case SettingType.INT32:
      return "int32";
    case SettingType.BOOL:
      return "bool";
    case SettingType.STRING:
      return "string";
    default:
      return "bytes";
  }
}

/** Format a byte array as a hex string for display */
function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join(" ");
}

/** Parse a hex string (space-separated or continuous) into bytes */
function hexToBytes(hex: string): Uint8Array | null {
  const clean = hex.replace(/\s+/g, "");
  if (clean.length % 2 !== 0) return null;
  const result = new Uint8Array(clean.length / 2);
  for (let i = 0; i < result.length; i++) {
    const byte = parseInt(clean.slice(i * 2, i * 2 + 2), 16);
    if (isNaN(byte)) return null;
    result[i] = byte;
  }
  return result;
}

/** Encode a typed value to bytes for writing */
function typedValueToBytes(
  rawInput: string,
  type: SettingType
): Uint8Array | null {
  switch (type) {
    case SettingType.INT32: {
      const n = parseInt(rawInput, 10);
      if (isNaN(n)) return null;
      const buf = new ArrayBuffer(4);
      new DataView(buf).setInt32(0, n, /* littleEndian= */ true);
      return new Uint8Array(buf);
    }
    case SettingType.BOOL: {
      const lower = rawInput.trim().toLowerCase();
      if (lower === "true" || lower === "1") return new Uint8Array([1]);
      if (lower === "false" || lower === "0") return new Uint8Array([0]);
      return null;
    }
    case SettingType.STRING:
      return new TextEncoder().encode(rawInput);
    default:
      return hexToBytes(rawInput);
  }
}

/** Format bytes as a human-readable value based on type */
function bytesToTypedDisplay(bytes: Uint8Array, type: SettingType): string {
  if (bytes.length === 0) return "(empty)";
  switch (type) {
    case SettingType.INT32: {
      if (bytes.length < 4) return bytesToHex(bytes) + " (too short for int32)";
      const v = new DataView(
        bytes.buffer,
        bytes.byteOffset,
        bytes.byteLength
      ).getInt32(0, true);
      return String(v);
    }
    case SettingType.BOOL:
      return bytes[0] !== 0 ? "true" : "false";
    case SettingType.STRING:
      try {
        return new TextDecoder().decode(bytes);
      } catch {
        return bytesToHex(bytes);
      }
    default:
      return bytesToHex(bytes);
  }
}

/** Default edit placeholder/example based on type */
function editPlaceholder(type: SettingType): string {
  switch (type) {
    case SettingType.INT32:
      return "e.g. 42";
    case SettingType.BOOL:
      return "true or false";
    case SettingType.STRING:
      return "enter text";
    default:
      return "hex bytes, e.g. DE AD BE EF";
  }
}

// ---- RPC helpers ----------------------------------------------------------

async function callRPC(
  service: ZMKCustomSubsystem,
  request: Request
): Promise<Response> {
  const payload = Request.encode(request).finish();
  const responsePayload = await service.callRPC(payload);
  if (!responsePayload) {
    throw new Error("No response from device");
  }
  return Response.decode(responsePayload);
}

// ---- App ------------------------------------------------------------------

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>⚙️ ZMK Setting Expose</h1>
        <p>Read, write, and delete Zephyr settings on your ZMK keyboard</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>⏳ Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>🚨 {error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                🔌 Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>✅ Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <SettingsSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>ZMK Setting Expose</strong> – Manage Zephyr settings on your
          keyboard via ZMK Studio RPC. Requires firmware unlock.
        </p>
      </footer>
    </div>
  );
}

// ---- Settings section -----------------------------------------------------

export function SettingsSection() {
  const zmkApp = useContext(ZMKAppContext);
  const [settings, setSettings] = useState<SettingEntry[]>([]);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [editEntry, setEditEntry] = useState<SettingEntry | null>(null);
  const [editValue, setEditValue] = useState("");
  const [editError, setEditError] = useState<string | null>(null);
  const [isSaving, setIsSaving] = useState(false);

  if (!zmkApp) return null;

  const subsystem = zmkApp.findSubsystem(SUBSYSTEM_IDENTIFIER);

  const getService = useCallback(() => {
    if (!zmkApp.state.connection || !subsystem) return null;
    return new ZMKCustomSubsystem(zmkApp.state.connection, subsystem.index);
  }, [zmkApp, subsystem]);

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem &quot;{SUBSYSTEM_IDENTIFIER}&quot; not found. Make sure
            your firmware includes the setting_expose module with{" "}
            <code>CONFIG_ZMK_SETTING_EXPOSE=y</code>.
          </p>
        </div>
      </section>
    );
  }

  const loadSettings = async () => {
    const service = getService();
    if (!service) return;

    setIsLoading(true);
    setError(null);
    try {
      const resp = await callRPC(service, Request.create({ list: {} }));
      if (resp.error) {
        setError(`Device error: ${resp.error.message}`);
      } else if (resp.list) {
        setSettings(resp.list.entries);
      }
    } catch (e) {
      setError(
        `Failed to load settings: ${e instanceof Error ? e.message : String(e)}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const startEdit = (entry: SettingEntry) => {
    setEditEntry(entry);
    setEditValue(bytesToTypedDisplay(entry.value, entry.type));
    setEditError(null);
  };

  const cancelEdit = () => {
    setEditEntry(null);
    setEditValue("");
    setEditError(null);
  };

  const saveEdit = async () => {
    if (!editEntry) return;
    const service = getService();
    if (!service) return;

    const bytes = typedValueToBytes(editValue, editEntry.type);
    if (bytes === null) {
      setEditError("Invalid value for the setting type");
      return;
    }

    setIsSaving(true);
    setEditError(null);
    try {
      const resp = await callRPC(
        service,
        Request.create({
          write: { key: editEntry.key, value: bytes },
        })
      );
      if (resp.error) {
        setEditError(`Device error: ${resp.error.message}`);
      } else {
        cancelEdit();
        await loadSettings();
      }
    } catch (e) {
      setEditError(
        `Save failed: ${e instanceof Error ? e.message : String(e)}`
      );
    } finally {
      setIsSaving(false);
    }
  };

  const deleteSetting = async (key: string) => {
    if (!confirm(`Delete setting "${key}"?`)) return;
    const service = getService();
    if (!service) return;

    setError(null);
    try {
      const resp = await callRPC(service, Request.create({ delete: { key } }));
      if (resp.error) {
        setError(`Delete failed: ${resp.error.message}`);
      } else {
        await loadSettings();
      }
    } catch (e) {
      setError(`Delete failed: ${e instanceof Error ? e.message : String(e)}`);
    }
  };

  return (
    <section className="card">
      <h2>Settings</h2>
      <p>
        List, edit, or delete Zephyr settings stored on the device. The device
        must be unlocked in ZMK Studio first.
      </p>

      <button
        className="btn btn-primary"
        disabled={isLoading}
        onClick={loadSettings}
      >
        {isLoading ? "⏳ Loading..." : "🔄 Load Settings"}
      </button>

      {error && (
        <div className="error-message" role="alert">
          <p>🚨 {error}</p>
        </div>
      )}

      {settings.length === 0 && !isLoading && !error && (
        <p>
          No settings loaded. Click &quot;Load Settings&quot; to fetch from
          device.
        </p>
      )}

      {settings.length > 0 && (
        <table className="settings-table">
          <thead>
            <tr>
              <th>Key</th>
              <th>Value</th>
              <th>Type</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody>
            {settings.map((entry) => (
              <tr key={entry.key}>
                <td className="key-cell">{entry.key}</td>
                <td className="value-cell">
                  {bytesToTypedDisplay(entry.value, entry.type)}
                </td>
                <td>{settingTypeLabel(entry.type)}</td>
                <td>
                  <button
                    className="btn btn-secondary btn-small"
                    onClick={() => startEdit(entry)}
                  >
                    ✏️ Edit
                  </button>{" "}
                  <button
                    className="btn btn-danger btn-small"
                    onClick={() => deleteSetting(entry.key)}
                  >
                    🗑️ Delete
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {editEntry && (
        <div className="edit-form">
          <h3>Edit: {editEntry.key}</h3>
          <label htmlFor="edit-value">
            Value ({settingTypeLabel(editEntry.type)}):
          </label>
          <input
            id="edit-value"
            type="text"
            value={editValue}
            placeholder={editPlaceholder(editEntry.type)}
            onChange={(e) => setEditValue(e.target.value)}
          />
          {editError && (
            <div className="error-message" role="alert">
              <p>{editError}</p>
            </div>
          )}
          <div className="edit-form-actions">
            <button
              className="btn btn-primary"
              disabled={isSaving}
              onClick={saveEdit}
            >
              {isSaving ? "⏳ Saving..." : "💾 Save"}
            </button>
            <button className="btn btn-secondary" onClick={cancelEdit}>
              Cancel
            </button>
          </div>
        </div>
      )}
    </section>
  );
}

export default App;
