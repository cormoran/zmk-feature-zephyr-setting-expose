import { render, screen, waitFor, act } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import type {
  NotificationSubscription,
  UseZMKAppReturn,
} from "@cormoran/zmk-studio-react-hook";
import { SettingsSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  Response,
  Notification,
} from "../src/proto/zmk/setting_expose/setting_expose";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

global.confirm = jest.fn(() => true);

const callRpcMock = call_rpc as jest.Mock;

/** Wrap encoded Response bytes the way ZMKCustomSubsystem expects a reply. */
function rpcReply(resp: Parameters<typeof Response.create>[0]) {
  return {
    custom: {
      call: { payload: Response.encode(Response.create(resp)).finish() },
    },
  };
}

/** A CustomNotification carrying an encoded setting_expose Notification. */
function notif(n: Parameters<typeof Notification.create>[0]) {
  return { payload: Notification.encode(Notification.create(n)).finish() };
}

/**
 * Build a connected mock app with a controllable onNotification: the last
 * subscribed callback is exposed so a test can push notifications by hand.
 */
function makeApp(): {
  app: UseZMKAppReturn;
  emit: (n: ReturnType<typeof notif>) => void;
} {
  let cb: ((n: unknown) => void) | null = null;
  const app = createMockZMKApp({
    isConnected: true,
    state: {
      connection: {} as never,
      deviceInfo: null,
      customSubsystems: null,
      isLoading: false,
      error: null,
    },
    findSubsystem: (id: string) =>
      id === SUBSYSTEM_IDENTIFIER ? { index: 0, identifier: id } : null,
    onNotification: (sub: NotificationSubscription) => {
      if (sub.type === "custom") cb = sub.callback as (n: unknown) => void;
      return () => {
        cb = null;
      };
    },
  });
  return {
    app,
    emit: (n) => act(() => cb?.(n)),
  };
}

describe("split target support", () => {
  beforeEach(() => callRpcMock.mockReset());

  it("offers Central / Peripheral / All targets", () => {
    const { app } = makeApp();
    render(
      <ZMKAppProvider value={app}>
        <SettingsSection />
      </ZMKAppProvider>
    );
    const select = screen.getByLabelText(/Target:/i) as HTMLSelectElement;
    const options = Array.from(select.options).map((o) => o.textContent);
    expect(options).toEqual([
      "Central",
      "Peripheral 1",
      "Peripheral 2",
      "Peripheral 3",
      "Peripheral 4",
      "All halves",
    ]);
  });

  it("merges a peripheral reply from a notification and filters by source", async () => {
    const { app, emit } = makeApp();
    const user = userEvent.setup();
    render(
      <ZMKAppProvider value={app}>
        <SettingsSection />
      </ZMKAppProvider>
    );

    // 1) Load the central synchronously.
    callRpcMock
      .mockResolvedValueOnce(
        rpcReply({ list: { entries: [{ key: "c/one", int32Value: 1 }] } })
      )
      .mockResolvedValueOnce(rpcReply({ storageInfo: {} }));
    await user.click(screen.getByRole("button", { name: /Load Settings/i }));
    await waitFor(() => expect(screen.getByText("c/one")).toBeInTheDocument());

    // 2) Switch target to Peripheral 1; the request is acked and the result
    //    arrives as a notification (req_id 1 = the first targeted request).
    await user.selectOptions(screen.getByLabelText(/Target:/i), "1");
    callRpcMock
      .mockResolvedValueOnce(rpcReply({ ack: {} })) // targeted list -> ack
      .mockResolvedValueOnce(rpcReply({ storageInfo: {} }));
    await user.click(screen.getByRole("button", { name: /Load Settings/i }));

    // The peripheral streams one entry per notification, then list_done.
    emit(
      notif({ source: 1, reqId: 1, entry: { key: "p/two", stringValue: "hi" } })
    );
    emit(notif({ source: 1, reqId: 1, listDone: {} }));

    await waitFor(() => expect(screen.getByText("p/two")).toBeInTheDocument());
    // Both halves now present -> source headers + display filter appear.
    expect(screen.getByText("c/one")).toBeInTheDocument();
    expect(
      screen.getByRole("heading", { name: "Peripheral 1" })
    ).toBeInTheDocument();

    // 3) Display filter narrows to Peripheral 1 only.
    await user.selectOptions(screen.getByLabelText(/Show:/i), "1");
    expect(screen.getByText("p/two")).toBeInTheDocument();
    expect(screen.queryByText("c/one")).not.toBeInTheDocument();
  });

  it("shows a marker for a peripheral entry too large to relay", async () => {
    const { app, emit } = makeApp();
    const user = userEvent.setup();
    render(
      <ZMKAppProvider value={app}>
        <SettingsSection />
      </ZMKAppProvider>
    );

    await user.selectOptions(screen.getByLabelText(/Target:/i), "1");
    callRpcMock
      .mockResolvedValueOnce(rpcReply({ ack: {} }))
      .mockResolvedValueOnce(rpcReply({ storageInfo: {} }));
    await user.click(screen.getByRole("button", { name: /Load Settings/i }));

    emit(
      notif({
        source: 1,
        reqId: 1,
        entryTooLarge: { key: "big/blob", valueSize: 300 },
      })
    );
    emit(notif({ source: 1, reqId: 1, listDone: {} }));

    await waitFor(() =>
      expect(screen.getByText("big/blob")).toBeInTheDocument()
    );
    expect(
      screen.getByText(/value too large to transfer.*300 bytes/i)
    ).toBeInTheDocument();
    // Edit is disabled (value unavailable) but Delete remains usable.
    expect(screen.getByRole("button", { name: /Edit/i })).toBeDisabled();
    expect(screen.getByRole("button", { name: /Delete/i })).toBeEnabled();
  });

  it("completes a target=all clear via the completion notification", async () => {
    const { app, emit } = makeApp();
    const user = userEvent.setup();
    render(
      <ZMKAppProvider value={app}>
        <SettingsSection />
      </ZMKAppProvider>
    );

    // Load central once so the storage bar (and Clear All button) render.
    callRpcMock
      .mockResolvedValueOnce(rpcReply({ list: { entries: [] } }))
      .mockResolvedValueOnce(
        rpcReply({
          storageInfo: { totalBytes: 8192, freeBytes: 4096, usedBytes: 4096 },
        })
      );
    await user.click(screen.getByRole("button", { name: /Load Settings/i }));
    await waitFor(() =>
      expect(
        screen.getByRole("button", { name: /Clear All/i })
      ).toBeInTheDocument()
    );

    // target = all
    await user.selectOptions(screen.getByLabelText(/Target:/i), String(0xffff));
    callRpcMock
      .mockResolvedValueOnce(rpcReply({ ack: {} })) // clearAll -> ack (req_id 1)
      .mockResolvedValueOnce(rpcReply({ storageInfo: {} })); // refresh after clear

    await user.click(screen.getByRole("button", { name: /Clear All/i }));
    // Peripheral deletes first, then the central completes the operation.
    emit(notif({ source: 1, reqId: 1, response: { delete: {} } }));
    emit(notif({ source: 0, reqId: 1, complete: {} }));

    await waitFor(() =>
      expect(screen.getByText(/Cleared All halves/i)).toBeInTheDocument()
    );
  });
});
