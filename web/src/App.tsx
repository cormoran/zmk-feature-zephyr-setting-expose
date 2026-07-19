import { useCallback, useContext, useEffect, useRef, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import type { NotificationSubscription } from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  Notification,
  SettingEntry,
  StorageInfoResponse,
} from "./proto/zmk/setting_expose/setting_expose";

export const SUBSYSTEM_IDENTIFIER = "zmk__setting_expose";

// ---- Split targeting ------------------------------------------------------

/** Request.target values (mirror proto Target). */
const TARGET_CENTRAL = 0;
const TARGET_ALL = 0xffff;

/** How many peripheral slots the target selector offers. */
const MAX_PERIPHERALS = 4;

/** Display source values used by the display filter ("all" == every source). */
type DisplaySource = number | "all";

/** Human label for a reply source (0 = central, 1.. = peripheral index). */
function sourceLabel(source: number): string {
  return source === TARGET_CENTRAL ? "Central" : `Peripheral ${source}`;
}

/** Human label for a request target. */
function targetLabel(target: number): string {
  if (target === TARGET_CENTRAL) return "Central";
  if (target === TARGET_ALL) return "All halves";
  return `Peripheral ${target}`;
}

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

/**
 * Per-call RPC timeout. The default in the client library is 5s, which is too
 * short for slow transports (BLE) or devices with many settings. Give every
 * call a generous ceiling so a large transfer completes instead of timing out.
 */
const RPC_TIMEOUT_MS = 20000;

/** Number of entries to request per list page. */
const LIST_PAGE_SIZE = 32;

/** Hard cap on list pages to avoid an infinite loop on a misbehaving device. */
const MAX_LIST_PAGES = 10000;

/**
 * How long to collect asynchronous peripheral notifications for a broadcast
 * (target = all) before assuming every connected half has answered. ZMK gives
 * no reliable connected-peripheral count, so this is time-boxed.
 */
const NOTIFY_COLLECT_MS = 1500;

/** Per-target notification wait for a single addressed half. */
const NOTIFY_ONE_MS = 4000;

/**
 * Wait for a target = all delete/clear_all to finish. Slightly longer than the
 * firmware's CONFIG_ZMK_SETTING_EXPOSE_DELETE_ALL_TIMEOUT_MS (default 3s).
 */
const DELETE_ALL_WAIT_MS = 6000;

async function callRPC(
  service: ZMKCustomSubsystem,
  request: Request,
  options?: { timeout?: number }
): Promise<Response> {
  const payload = Request.encode(request).finish();
  const responsePayload = await service.callRPC(payload, {
    timeout: options?.timeout ?? RPC_TIMEOUT_MS,
  });
  if (!responsePayload) {
    throw new Error("No response from device");
  }
  return Response.decode(responsePayload);
}

/** Results gathered from the notifications of one targeted request. */
type TargetedResult = {
  /** Streamed `list` entries, grouped by reply source. */
  entriesBySource: Record<number, SettingEntry[]>;
  /** Single Response per source for non-list ops (read/write/delete/gc/...). */
  responseBySource: Record<number, Response>;
  /** A TARGET_ALL delete/clear_all reported completion. */
  completed: boolean;
};

/** Registered collector for the notifications of one in-flight targeted request. */
type Collector = { onEvent: (source: number, n: Notification) => void };

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
  /* Loaded settings keyed by reply source (0 = central, 1.. = peripheral). */
  const [settingsBySource, setSettingsBySource] = useState<
    Record<number, SettingEntry[]>
  >({});
  const [target, setTarget] = useState<number>(TARGET_CENTRAL);
  const [displayFilter, setDisplayFilter] = useState<DisplaySource>("all");
  const [isLoading, setIsLoading] = useState(false);
  const [status, setStatus] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [editEntry, setEditEntry] = useState<SettingEntry | null>(null);
  const [editSource, setEditSource] = useState<number>(TARGET_CENTRAL);
  const [editValue, setEditValue] = useState("");
  const [editError, setEditError] = useState<string | null>(null);
  const [isSaving, setIsSaving] = useState(false);
  const [storageInfo, setStorageInfo] = useState<StorageInfoResponse | null>(
    null
  );
  const [isGcing, setIsGcing] = useState(false);
  const [isClearing, setIsClearing] = useState(false);

  const inputRef = useRef<HTMLInputElement>(null);
  /* Monotonic request id (kept in the firmware's 1-byte echo range). */
  const reqCounter = useRef(0);
  /* In-flight notification collectors keyed by request id. */
  const collectors = useRef<Map<number, Collector>>(new Map());

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);

  const getService = useCallback(() => {
    if (!zmkApp?.state.connection || !subsystem) return null;
    return new ZMKCustomSubsystem(zmkApp.state.connection, subsystem.index);
  }, [zmkApp, subsystem]);

  /* Subscribe to firmware notifications carrying peripheral (and central-own)
   * replies. Each notification is routed to the collector for its request id. */
  useEffect(() => {
    if (!zmkApp?.onNotification || !subsystem) return;
    const sub: Extract<NotificationSubscription, { type: "custom" }> = {
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (notif) => {
        let n;
        try {
          n = Notification.decode(notif.payload);
        } catch {
          return;
        }
        collectors.current.get(n.reqId)?.onEvent(n.source, n);
      },
    };
    const unsub = zmkApp.onNotification(sub);
    return typeof unsub === "function" ? unsub : undefined;
  }, [zmkApp, subsystem]);

  /* Auto-focus the edit input whenever a new entry is being edited */
  useEffect(() => {
    if (editEntry) {
      inputRef.current?.focus();
    }
  }, [editEntry]);

  const nextReqId = () => {
    reqCounter.current = (reqCounter.current % 255) + 1;
    return reqCounter.current;
  };

  /*
   * Send a targeted request and gather the asynchronous per-half replies.
   *
   *   - On a split central the request returns an Ack and the actual results
   *     arrive as notifications; we collect them per `mode`.
   *   - On a build without split relay the request returns the real Response
   *     directly (central-only fallback); we surface it as source 0.
   *
   * mode: "first" resolves on the first non-list Response (one addressed half);
   * "list" streams entries and resolves on the addressed source's list_done;
   * "collect" waits a fixed window for every half's stream; "complete" waits for
   * the completion notification (target = all delete/clear_all).
   */
  const sendTargeted = useCallback(
    async (
      service: ZMKCustomSubsystem,
      fields: Partial<Request>,
      reqTarget: number,
      mode: "first" | "list" | "collect" | "complete",
      timeout: number
    ): Promise<TargetedResult> => {
      const reqId = nextReqId();
      const result: TargetedResult = {
        entriesBySource: {},
        responseBySource: {},
        completed: false,
      };

      const collected = new Promise<void>((resolve) => {
        const done = () => {
          collectors.current.delete(reqId);
          clearTimeout(timer);
          resolve();
        };
        const timer = setTimeout(done, timeout);
        collectors.current.set(reqId, {
          onEvent: (source, n) => {
            if (n.entry) {
              (result.entriesBySource[source] ??= []).push(n.entry);
            } else if (n.listDone) {
              result.entriesBySource[source] ??= [];
              if (mode === "list" && source === reqTarget) done();
            } else if (n.response) {
              result.responseBySource[source] = n.response;
              if (mode === "first") done();
            } else if (n.complete) {
              result.completed = true;
              if (mode === "complete") done();
            }
          },
        });
      });

      const ack = await callRPC(
        service,
        Request.create({ ...fields, target: reqTarget, reqId }),
        { timeout }
      );
      if (ack.ack === undefined) {
        /* Central-only fallback (or an error): the sync response IS the result. */
        collectors.current.delete(reqId);
        result.responseBySource[0] = ack;
        if (ack.list) result.entriesBySource[0] = ack.list.entries;
        return result;
      }
      await collected;
      return result;
    },
    []
  );

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

  /* Fetch every page of the central's own store synchronously. */
  const loadCentralPages = async (
    service: ZMKCustomSubsystem
  ): Promise<SettingEntry[]> => {
    const collected: SettingEntry[] = [];
    let offset = 0;
    for (let page = 0; page < MAX_LIST_PAGES; page++) {
      const resp = await callRPC(
        service,
        Request.create({
          list: { offset, limit: LIST_PAGE_SIZE },
          target: TARGET_CENTRAL,
        })
      );
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      if (!resp.list) break;
      collected.push(...resp.list.entries);
      if (!resp.list.hasMore || resp.list.entries.length === 0) break;
      offset = resp.list.nextOffset || offset + resp.list.entries.length;
    }
    return collected;
  };

  /*
   * Load one peripheral's store. The peripheral streams one entry per
   * notification and terminates with list_done, so we just collect the stream
   * addressed to that source. Returns null if the source reported an error.
   */
  const loadPeripheralEntries = async (
    service: ZMKCustomSubsystem,
    source: number
  ): Promise<SettingEntry[] | { error: string } | null> => {
    const r = await sendTargeted(
      service,
      { list: { offset: 0, limit: 0 } },
      source,
      "list",
      NOTIFY_ONE_MS
    );
    const err = r.responseBySource[source]?.error;
    if (err) return { error: err.message };
    if (r.entriesBySource[source]) return r.entriesBySource[source];
    return null; // no reply (e.g. disconnected peripheral)
  };

  const refreshStorageInfo = async () => {
    const service = getService();
    if (!service) return;
    try {
      const infoResp = await callRPC(
        service,
        Request.create({ storageInfo: {}, target: TARGET_CENTRAL })
      );
      if (infoResp.storageInfo) setStorageInfo(infoResp.storageInfo);
    } catch {
      /* storage info is best-effort */
    }
  };

  const loadSettings = async () => {
    const service = getService();
    if (!service) return;

    setIsLoading(true);
    setError(null);
    setStatus(null);
    try {
      const next: Record<number, SettingEntry[]> = { ...settingsBySource };

      const loadCentral = async () => {
        next[TARGET_CENTRAL] = await loadCentralPages(service);
        setSettingsBySource({ ...next });
      };

      if (target === TARGET_CENTRAL) {
        await loadCentral();
      } else if (target === TARGET_ALL) {
        /* Central via the reliable sync path; peripherals stream over one
         * broadcast, grouped by source (each source ends with list_done). */
        await loadCentral();
        const r = await sendTargeted(
          service,
          { list: { offset: 0, limit: 0 } },
          TARGET_ALL,
          "collect",
          NOTIFY_COLLECT_MS
        );
        for (const s of Object.keys(r.entriesBySource).map(Number)) {
          if (s === TARGET_CENTRAL) continue; // central handled above
          next[s] = r.entriesBySource[s];
          setSettingsBySource({ ...next });
        }
      } else {
        /* A single addressed peripheral. */
        const entries = await loadPeripheralEntries(service, target);
        if (entries === null) {
          setStatus(`No reply from ${targetLabel(target)}.`);
        } else if ("error" in entries) {
          setError(`Device error: ${entries.error}`);
        } else {
          next[target] = entries;
          setSettingsBySource({ ...next });
        }
      }
    } catch (e) {
      setError(
        `Failed to load settings: ${e instanceof Error ? e.message : String(e)}`
      );
    } finally {
      setIsLoading(false);
    }

    await refreshStorageInfo();
  };

  const startEdit = (entry: SettingEntry, source: number) => {
    setEditEntry(entry);
    setEditSource(source);
    setEditValue(typedValueDisplay(entry));
    setEditError(null);
  };

  const cancelEdit = () => {
    setEditEntry(null);
    setEditValue("");
    setEditError(null);
  };

  /* Reload just the source that was mutated, so the table reflects the change. */
  const reloadSource = async (source: number) => {
    const service = getService();
    if (!service) return;
    if (source === TARGET_CENTRAL) {
      const entries = await loadCentralPages(service);
      setSettingsBySource((prev) => ({ ...prev, [source]: entries }));
    } else {
      const entries = await loadPeripheralEntries(service, source);
      if (Array.isArray(entries)) {
        setSettingsBySource((prev) => ({ ...prev, [source]: entries }));
      }
    }
    await refreshStorageInfo();
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
      const key = editEntry.key;
      const source = editSource;
      let deviceError: string | undefined;
      if (source === TARGET_CENTRAL) {
        const resp = await callRPC(
          service,
          Request.create({
            write: { key, ...typedFields },
            target: TARGET_CENTRAL,
          })
        );
        deviceError = resp.error?.message;
      } else {
        const r = await sendTargeted(
          service,
          { write: { key, ...typedFields } },
          source,
          "first",
          NOTIFY_ONE_MS
        );
        deviceError = r.responseBySource[source]?.error?.message;
      }
      if (deviceError) {
        setEditError(`Device error: ${deviceError}`);
      } else {
        cancelEdit();
        await reloadSource(source);
      }
    } catch (e) {
      setEditError(
        `Save failed: ${e instanceof Error ? e.message : String(e)}`
      );
    } finally {
      setIsSaving(false);
    }
  };

  const deleteSetting = async (key: string, source: number) => {
    if (!confirm(`Delete setting "${key}" on ${sourceLabel(source)}?`)) return;
    const service = getService();
    if (!service) return;

    setError(null);
    try {
      if (source === TARGET_CENTRAL) {
        const resp = await callRPC(
          service,
          Request.create({ delete: { key }, target: TARGET_CENTRAL })
        );
        if (resp.error) {
          setError(`Delete failed: ${resp.error.message}`);
          return;
        }
      } else {
        const r = await sendTargeted(
          service,
          { delete: { key } },
          source,
          "first",
          NOTIFY_ONE_MS
        );
        const err = r.responseBySource[source]?.error;
        if (err) {
          setError(`Delete failed: ${err.message}`);
          return;
        }
      }
      await reloadSource(source);
    } catch (e) {
      setError(`Delete failed: ${e instanceof Error ? e.message : String(e)}`);
    }
  };

  const handleGc = async () => {
    const service = getService();
    if (!service) return;
    setIsGcing(true);
    try {
      if (target === TARGET_CENTRAL) {
        await callRPC(
          service,
          Request.create({ gc: {}, target: TARGET_CENTRAL })
        );
      } else {
        await sendTargeted(
          service,
          { gc: {} },
          target,
          target === TARGET_ALL ? "collect" : "first",
          NOTIFY_ONE_MS
        );
      }
      await refreshStorageInfo();
    } catch (e) {
      setError(`GC failed: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setIsGcing(false);
    }
  };

  const handleClearAll = async () => {
    if (
      !confirm(
        `⚠️ Delete ALL settings on ${targetLabel(
          target
        )}? This cannot be undone.`
      )
    )
      return;
    const service = getService();
    if (!service) return;
    setIsClearing(true);
    setError(null);
    setStatus(null);
    try {
      if (target === TARGET_CENTRAL) {
        const resp = await callRPC(
          service,
          Request.create({ clearAll: {}, target: TARGET_CENTRAL })
        );
        if (resp.error) {
          setError(`Clear all failed: ${resp.error.message}`);
          return;
        }
      } else {
        /* target = all: the central deletes peripherals first, then itself,
         * and finishes with a completion notification. */
        setStatus(`Clearing ${targetLabel(target)}…`);
        await sendTargeted(
          service,
          { clearAll: {} },
          target,
          target === TARGET_ALL ? "complete" : "first",
          DELETE_ALL_WAIT_MS
        );
      }
      /* Everything on the targeted half/halves is gone: clear the display
       * optimistically rather than issuing a slow reload. */
      if (target === TARGET_ALL) {
        setSettingsBySource({});
      } else {
        setSettingsBySource((prev) => {
          const next = { ...prev };
          delete next[target];
          return next;
        });
      }
      setStatus(`Cleared ${targetLabel(target)}.`);
      await refreshStorageInfo();
    } catch (e) {
      setError(
        `Clear all failed: ${e instanceof Error ? e.message : String(e)}`
      );
    } finally {
      setIsClearing(false);
    }
  };

  const loadedSources = Object.keys(settingsBySource)
    .map(Number)
    .filter((s) => settingsBySource[s] !== undefined)
    .sort((a, b) => a - b);
  const hasMultipleSources = loadedSources.length > 1;
  const visibleSources =
    displayFilter === "all"
      ? loadedSources
      : loadedSources.filter((s) => s === displayFilter);
  const totalEntries = loadedSources.reduce(
    (n, s) => n + settingsBySource[s].length,
    0
  );

  return (
    <section className="card">
      <h2>Settings</h2>
      <p>
        List, edit, or delete Zephyr settings stored on the device. The device
        must be unlocked in ZMK Studio first.
      </p>

      {/* ---- Target selector ---- */}
      <div className="target-controls">
        <label htmlFor="target-select">
          <strong>Target:</strong>
        </label>{" "}
        <select
          id="target-select"
          value={target}
          onChange={(e) => setTarget(Number(e.target.value))}
        >
          <option value={TARGET_CENTRAL}>Central</option>
          {Array.from({ length: MAX_PERIPHERALS }, (_, i) => i + 1).map((n) => (
            <option key={n} value={n}>
              Peripheral {n}
            </option>
          ))}
          <option value={TARGET_ALL}>All halves</option>
        </select>{" "}
        <button
          className="btn btn-primary"
          disabled={isLoading}
          onClick={loadSettings}
        >
          {isLoading ? "⏳ Loading..." : "🔄 Load Settings"}
        </button>
      </div>

      {/* ---- Storage capacity bar (central) ---- */}
      {storageInfo && storageInfo.totalBytes > 0 && (
        <div className="storage-info">
          <div className="storage-bar-label">
            Central storage: {storageInfo.usedBytes.toLocaleString()} /{" "}
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
              title={`Trigger NVS sector compaction on ${targetLabel(target)}`}
            >
              {isGcing ? "⏳ Running…" : "🗑️ Run GC"}
            </button>
            <button
              className="btn btn-danger btn-small"
              onClick={handleClearAll}
              disabled={isClearing}
              title={`Delete all settings on ${targetLabel(target)} (irreversible)`}
            >
              {isClearing ? "⏳ Clearing…" : "⚠️ Clear All"}
            </button>
          </div>
        </div>
      )}

      {status && (
        <div className="info-message">
          <p>{status}</p>
        </div>
      )}

      {error && (
        <div className="error-message" role="alert">
          <p>🚨 {error}</p>
        </div>
      )}

      {/* ---- Display filter (only when more than one half is loaded) ---- */}
      {hasMultipleSources && (
        <div className="display-filter">
          <label htmlFor="display-filter-select">Show:</label>{" "}
          <select
            id="display-filter-select"
            value={String(displayFilter)}
            onChange={(e) =>
              setDisplayFilter(
                e.target.value === "all" ? "all" : Number(e.target.value)
              )
            }
          >
            <option value="all">All halves</option>
            {loadedSources.map((s) => (
              <option key={s} value={s}>
                {sourceLabel(s)}
              </option>
            ))}
          </select>
        </div>
      )}

      {/* ---- Edit form (placed above the table) ---- */}
      {editEntry && (
        <div className="edit-form">
          <h3>Edit: {editEntry.key}</h3>
          <p className="edit-source">On {sourceLabel(editSource)}</p>
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

      {totalEntries === 0 && !isLoading && !error && (
        <p>
          No settings loaded. Choose a target and click &quot;Load
          Settings&quot; to fetch from device.
        </p>
      )}

      {/* ---- Settings tables grouped by source, then key prefix ---- */}
      {visibleSources.map((source) => (
        <div key={source} className="source-section">
          {hasMultipleSources && (
            <h3 className="source-header">{sourceLabel(source)}</h3>
          )}
          <div className="settings-groups">
            {groupByPrefix(settingsBySource[source]).map(
              ([prefix, entries]) => (
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
                            editEntry?.key === entry.key &&
                            editSource === source
                              ? "editing-row"
                              : ""
                          }
                        >
                          <td className="key-cell">{entry.key}</td>
                          <td className="value-cell">
                            {typedValueDisplay(entry)}
                          </td>
                          <td>{typedValueLabel(entry)}</td>
                          <td>
                            <button
                              className="btn btn-secondary btn-small"
                              onClick={() => startEdit(entry, source)}
                            >
                              ✏️ Edit
                            </button>{" "}
                            <button
                              className="btn btn-danger btn-small"
                              onClick={() => deleteSetting(entry.key, source)}
                            >
                              🗑️ Delete
                            </button>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )
            )}
          </div>
        </div>
      ))}
    </section>
  );
}

export default App;
