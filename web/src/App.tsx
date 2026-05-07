import { useCallback, useContext, useEffect, useRef, useState } from "react";
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
  StorageInfoResponse,
} from "./proto/zmk/setting_expose/setting_expose";

export const SUBSYSTEM_IDENTIFIER = "zmk__setting_expose";

// ---- Type helpers ---------------------------------------------------------

/** Return a human-readable type label based on which oneof field is set */
function typedValueLabel(entry: SettingEntry): string {
  if (entry.int32Value !== undefined) return "int32";
  if (entry.boolValue !== undefined) return "bool";
  if (entry.stringValue !== undefined) return "string";
  return "bytes";
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

/** Format a SettingEntry's typed value as a human-readable string */
function typedValueDisplay(entry: SettingEntry): string {
  if (entry.int32Value !== undefined) return String(entry.int32Value);
  if (entry.boolValue !== undefined) return entry.boolValue ? "true" : "false";
  if (entry.stringValue !== undefined) return entry.stringValue;
  if (entry.bytesValue && entry.bytesValue.length > 0)
    return bytesToHex(entry.bytesValue);
  return "(empty)";
}

/** Default placeholder for the edit input based on setting type */
function editPlaceholder(entry: SettingEntry): string {
  if (entry.int32Value !== undefined) return "e.g. 42";
  if (entry.boolValue !== undefined) return "true or false";
  if (entry.stringValue !== undefined) return "enter text";
  return "hex bytes, e.g. DE AD BE EF";
}

/**
 * Parse the raw edit string back into typed value fields for WriteRequest.
 * Returns null if the input is invalid for the setting type.
 */
function parseEditValue(
  rawInput: string,
  entry: SettingEntry
): Partial<
  Pick<
    Request["write"] & object,
    "int32Value" | "boolValue" | "stringValue" | "bytesValue"
  >
> | null {
  if (entry.int32Value !== undefined) {
    const n = parseInt(rawInput, 10);
    if (isNaN(n)) return null;
    return { int32Value: n };
  }
  if (entry.boolValue !== undefined) {
    const lower = rawInput.trim().toLowerCase();
    if (lower === "true" || lower === "1") return { boolValue: true };
    if (lower === "false" || lower === "0") return { boolValue: false };
    return null;
  }
  if (entry.stringValue !== undefined) return { stringValue: rawInput };
  const bytes = hexToBytes(rawInput);
  if (!bytes) return null;
  return { bytesValue: bytes };
}

// ---- Grouping helpers -----------------------------------------------------

/** Group sorted entries by their first path segment (before the first '/') */
function groupByPrefix(entries: SettingEntry[]): [string, SettingEntry[]][] {
  const sorted = [...entries].sort((a, b) => a.key.localeCompare(b.key));
  const map = new Map<string, SettingEntry[]>();
  for (const entry of sorted) {
    const slash = entry.key.indexOf("/");
    const prefix = slash >= 0 ? entry.key.slice(0, slash) : "";
    const group = map.get(prefix) ?? [];
    group.push(entry);
    map.set(prefix, group);
  }
  return Array.from(map.entries());
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
  const [storageInfo, setStorageInfo] = useState<StorageInfoResponse | null>(
    null
  );
  const [isGcing, setIsGcing] = useState(false);
  const [isClearing, setIsClearing] = useState(false);

  const inputRef = useRef<HTMLInputElement>(null);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);

  const getService = useCallback(() => {
    if (!zmkApp?.state.connection || !subsystem) return null;
    return new ZMKCustomSubsystem(zmkApp.state.connection, subsystem.index);
  }, [zmkApp, subsystem]);

  /* Auto-focus the edit input whenever a new entry is being edited */
  useEffect(() => {
    if (editEntry) {
      inputRef.current?.focus();
    }
  }, [editEntry]);

  if (!zmkApp) return null;

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

    /* Refresh storage info whenever we reload settings */
    const service2 = getService();
    if (service2) {
      try {
        const infoResp = await callRPC(
          service2,
          Request.create({ storageInfo: {} })
        );
        if (infoResp.storageInfo) {
          setStorageInfo(infoResp.storageInfo);
        }
      } catch {
        /* storage info is best-effort, ignore errors */
      }
    }
  };

  const startEdit = (entry: SettingEntry) => {
    setEditEntry(entry);
    setEditValue(typedValueDisplay(entry));
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

    const typedFields = parseEditValue(editValue, editEntry);
    if (typedFields === null) {
      setEditError("Invalid value for the setting type");
      return;
    }

    setIsSaving(true);
    setEditError(null);
    try {
      const resp = await callRPC(
        service,
        Request.create({
          write: { key: editEntry.key, ...typedFields },
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

  const handleGc = async () => {
    const service = getService();
    if (!service) return;
    setIsGcing(true);
    try {
      await callRPC(service, Request.create({ gc: {} }));
      await loadSettings();
    } catch (e) {
      setError(`GC failed: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setIsGcing(false);
    }
  };

  const handleClearAll = async () => {
    if (
      !confirm("⚠️ Delete ALL settings on the device? This cannot be undone.")
    )
      return;
    const service = getService();
    if (!service) return;
    setIsClearing(true);
    setError(null);
    try {
      const resp = await callRPC(service, Request.create({ clearAll: {} }));
      if (resp.error) {
        setError(`Clear all failed: ${resp.error.message}`);
      } else {
        setSettings([]);
        await loadSettings();
      }
    } catch (e) {
      setError(
        `Clear all failed: ${e instanceof Error ? e.message : String(e)}`
      );
    } finally {
      setIsClearing(false);
    }
  };

  const groups = groupByPrefix(settings);

  return (
    <section className="card">
      <h2>Settings</h2>
      <p>
        List, edit, or delete Zephyr settings stored on the device. The device
        must be unlocked in ZMK Studio first.
      </p>

      {/* ---- Storage capacity bar ---- */}
      {storageInfo && storageInfo.totalBytes > 0 && (
        <div className="storage-info">
          <div className="storage-bar-label">
            Storage: {storageInfo.usedBytes.toLocaleString()} /{" "}
            {storageInfo.totalBytes.toLocaleString()} bytes used (
            {Math.round((storageInfo.freeBytes / storageInfo.totalBytes) * 100)}
            % free)
          </div>
          <div className="storage-bar" role="progressbar">
            <div
              className="storage-bar-used"
              style={{
                width: `${(storageInfo.usedBytes / storageInfo.totalBytes) * 100}%`,
              }}
            />
          </div>
          <div className="storage-actions">
            <button
              className="btn btn-secondary btn-small"
              onClick={handleGc}
              disabled={isGcing}
              title="Trigger NVS sector compaction"
            >
              {isGcing ? "⏳ Running…" : "🗑️ Run GC"}
            </button>
            <button
              className="btn btn-danger btn-small"
              onClick={handleClearAll}
              disabled={isClearing}
              title="Delete all settings on device (irreversible)"
            >
              {isClearing ? "⏳ Clearing…" : "⚠️ Clear All"}
            </button>
          </div>
        </div>
      )}

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

      {/* ---- Edit form (placed above the table) ---- */}
      {editEntry && (
        <div className="edit-form">
          <h3>Edit: {editEntry.key}</h3>
          <label htmlFor="edit-value">
            Value ({typedValueLabel(editEntry)}):
          </label>
          <input
            id="edit-value"
            ref={inputRef}
            type="text"
            value={editValue}
            placeholder={editPlaceholder(editEntry)}
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

      {settings.length === 0 && !isLoading && !error && (
        <p>
          No settings loaded. Click &quot;Load Settings&quot; to fetch from
          device.
        </p>
      )}

      {/* ---- Settings table grouped by prefix ---- */}
      {groups.length > 0 && (
        <div className="settings-groups">
          {groups.map(([prefix, entries]) => (
            <div key={prefix} className="settings-group">
              {prefix !== "" && (
                <div className="settings-group-header">{prefix}</div>
              )}
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
                  {entries.map((entry) => (
                    <tr
                      key={entry.key}
                      className={
                        editEntry?.key === entry.key ? "editing-row" : ""
                      }
                    >
                      <td className="key-cell">{entry.key}</td>
                      <td className="value-cell">{typedValueDisplay(entry)}</td>
                      <td>{typedValueLabel(entry)}</td>
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
            </div>
          ))}
        </div>
      )}
    </section>
  );
}

export default App;
