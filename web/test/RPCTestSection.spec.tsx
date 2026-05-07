import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
  setupZMKMocks,
} from "@cormoran/zmk-studio-react-hook/testing";
import { SettingsSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import { Response } from "../src/proto/zmk/setting_expose/setting_expose";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import App from "../src/App";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

jest.mock("@zmkfirmware/zmk-studio-ts-client/transport/serial", () => ({
  connect: jest.fn(),
}));

// Global confirm mock
global.confirm = jest.fn(() => true);

/** Encode a proto Response to bytes suitable for use as a mock RPC payload */
function encodeResponse(
  resp: Parameters<typeof Response.create>[0]
): Uint8Array {
  return Response.encode(Response.create(resp)).finish();
}

describe("SettingsSection Component", () => {
  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <SettingsSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "zmk__setting_expose" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(/CONFIG_ZMK_SETTING_EXPOSE=y/i)
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<SettingsSection />);
      expect(container.firstChild).toBeNull();
    });
  });

  describe("With Subsystem", () => {
    it("should render load button when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <SettingsSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: /Settings/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: /Load Settings/i })
      ).toBeInTheDocument();
    });

    it("should show empty state before loading", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <SettingsSection />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/No settings loaded/i)).toBeInTheDocument();
    });
  });

  describe("RPC interaction", () => {
    let mocks: ReturnType<typeof setupZMKMocks>;

    beforeEach(() => {
      mocks = setupZMKMocks();
    });

    const renderWithMocks = async (subsystems = [SUBSYSTEM_IDENTIFIER]) => {
      mocks.mockSuccessfulConnection({ deviceName: "Test KB", subsystems });
      (serial_connect as jest.Mock).mockResolvedValue(mocks.mockTransport);
      render(<App />);
      const user = userEvent.setup();
      await user.click(screen.getByText(/Connect Serial/i));
      await waitFor(() =>
        expect(screen.getByText(/Connected to: Test KB/i)).toBeInTheDocument()
      );
      return user;
    };

    it("should display settings after successful list RPC", async () => {
      const payload = encodeResponse({
        list: {
          entries: [
            {
              key: "mymod/volume",
              int32Value: 75,
            },
            {
              key: "mymod/enabled",
              boolValue: true,
            },
          ],
        },
      });

      const user = await renderWithMocks();

      mocks.call_rpc.mockResolvedValueOnce({
        custom: { call: { payload } },
      });

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));

      await waitFor(() => {
        expect(screen.getByText("mymod/volume")).toBeInTheDocument();
        expect(screen.getByText("mymod/enabled")).toBeInTheDocument();
      });

      expect(screen.getByText("75")).toBeInTheDocument(); // int32 value
      expect(screen.getByText("true")).toBeInTheDocument(); // bool value
    });

    it("should sort and group settings by prefix", async () => {
      const payload = encodeResponse({
        list: {
          entries: [
            {
              key: "ble/profile",
              int32Value: 0,
            },
            {
              key: "ble/active",
              int32Value: 1,
            },
            {
              key: "mymod/name",
              stringValue: "kb",
            },
          ],
        },
      });

      const user = await renderWithMocks();

      mocks.call_rpc.mockResolvedValueOnce({
        custom: { call: { payload } },
      });

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));

      await waitFor(() =>
        expect(screen.getByText("ble/active")).toBeInTheDocument()
      );

      // Group headers should be present
      const headers = screen.getAllByText(/^ble$|^mymod$/i);
      expect(headers.length).toBeGreaterThanOrEqual(2);

      // ble/active should appear before ble/profile (sorted)
      const rows = screen.getAllByRole("row");
      const activeIdx = rows.findIndex((r) =>
        r.textContent?.includes("ble/active")
      );
      const profileIdx = rows.findIndex((r) =>
        r.textContent?.includes("ble/profile")
      );
      expect(activeIdx).toBeLessThan(profileIdx);
    });

    it("should show edit form above table when edit button clicked", async () => {
      const payload = encodeResponse({
        list: {
          entries: [
            {
              key: "mymod/name",
              stringValue: "hello",
            },
          ],
        },
      });

      const user = await renderWithMocks();

      mocks.call_rpc.mockResolvedValueOnce({
        custom: { call: { payload } },
      });

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));
      await waitFor(() =>
        expect(screen.getByText("mymod/name")).toBeInTheDocument()
      );

      await user.click(screen.getByText(/✏️ Edit/i));
      await waitFor(() =>
        expect(screen.getByText(/Edit: mymod\/name/i)).toBeInTheDocument()
      );
      expect(screen.getByLabelText(/Value \(string\)/i)).toBeInTheDocument();
    });

    it("should dismiss edit form on cancel", async () => {
      const payload = encodeResponse({
        list: {
          entries: [
            {
              key: "mymod/name",
              stringValue: "hello",
            },
          ],
        },
      });

      const user = await renderWithMocks();

      mocks.call_rpc.mockResolvedValueOnce({
        custom: { call: { payload } },
      });

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));
      await waitFor(() =>
        expect(screen.getByText("mymod/name")).toBeInTheDocument()
      );

      await user.click(screen.getByText(/✏️ Edit/i));
      await waitFor(() =>
        expect(screen.getByText(/Edit: mymod\/name/i)).toBeInTheDocument()
      );

      await user.click(screen.getByText(/Cancel/i));
      expect(screen.queryByText(/Edit: mymod\/name/i)).not.toBeInTheDocument();
    });

    it("should reload settings after a successful write", async () => {
      const listPayload = encodeResponse({
        list: {
          entries: [
            {
              key: "mymod/name",
              stringValue: "hello",
            },
          ],
        },
      });
      const writePayload = encodeResponse({ write: {} });
      const updatedPayload = encodeResponse({
        list: {
          entries: [
            {
              key: "mymod/name",
              stringValue: "world",
            },
          ],
        },
      });

      const user = await renderWithMocks();

      // Initial load + storage info (mock both)
      mocks.call_rpc
        .mockResolvedValueOnce({ custom: { call: { payload: listPayload } } })
        .mockResolvedValueOnce({ custom: { call: { payload: listPayload } } }) // storage info
        .mockResolvedValueOnce({ custom: { call: { payload: writePayload } } })
        .mockResolvedValueOnce({
          custom: { call: { payload: updatedPayload } },
        })
        .mockResolvedValueOnce({
          custom: { call: { payload: updatedPayload } },
        }); // storage info after reload

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));
      await waitFor(() =>
        expect(screen.getByText("mymod/name")).toBeInTheDocument()
      );

      await user.click(screen.getByText(/✏️ Edit/i));
      await waitFor(() =>
        expect(screen.getByLabelText(/Value \(string\)/i)).toBeInTheDocument()
      );

      const input = screen.getByLabelText(/Value \(string\)/i);
      await user.clear(input);
      await user.type(input, "world");
      await user.click(screen.getByText(/💾 Save/i));

      await waitFor(() =>
        expect(screen.getByText("world")).toBeInTheDocument()
      );
      expect(screen.queryByText(/Edit: mymod\/name/i)).not.toBeInTheDocument();
    });

    it("should reload settings after a successful delete", async () => {
      const listPayload = encodeResponse({
        list: {
          entries: [
            {
              key: "mymod/temp",
              boolValue: true,
            },
          ],
        },
      });
      const deletePayload = encodeResponse({ delete: {} });
      const emptyPayload = encodeResponse({ list: { entries: [] } });

      const user = await renderWithMocks();

      mocks.call_rpc
        .mockResolvedValueOnce({ custom: { call: { payload: listPayload } } })
        .mockResolvedValueOnce({ custom: { call: { payload: listPayload } } }) // storage info
        .mockResolvedValueOnce({ custom: { call: { payload: deletePayload } } })
        .mockResolvedValueOnce({ custom: { call: { payload: emptyPayload } } })
        .mockResolvedValueOnce({ custom: { call: { payload: emptyPayload } } }); // storage info

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));
      await waitFor(() =>
        expect(screen.getByText("mymod/temp")).toBeInTheDocument()
      );

      await user.click(screen.getByText(/🗑️ Delete/i));

      await waitFor(() =>
        expect(screen.queryByText("mymod/temp")).not.toBeInTheDocument()
      );
    });

    it("should show error when RPC fails during load", async () => {
      const user = await renderWithMocks();

      mocks.call_rpc.mockRejectedValueOnce(new Error("connection lost"));

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));

      await waitFor(() =>
        expect(screen.getByText(/Failed to load settings/i)).toBeInTheDocument()
      );
    });

    it("should show device error message in edit form", async () => {
      const listPayload = encodeResponse({
        list: {
          entries: [
            {
              key: "mymod/volume",
              int32Value: 0,
            },
          ],
        },
      });
      const errorPayload = encodeResponse({
        error: { message: "storage full" },
      });

      const user = await renderWithMocks();

      mocks.call_rpc
        .mockResolvedValueOnce({ custom: { call: { payload: listPayload } } })
        .mockResolvedValueOnce({ custom: { call: { payload: listPayload } } }) // storage info
        .mockResolvedValueOnce({ custom: { call: { payload: errorPayload } } });

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));
      await waitFor(() =>
        expect(screen.getByText("mymod/volume")).toBeInTheDocument()
      );

      await user.click(screen.getByText(/✏️ Edit/i));
      await waitFor(() =>
        expect(screen.getByLabelText(/Value \(int32\)/i)).toBeInTheDocument()
      );

      await user.click(screen.getByText(/💾 Save/i));

      await waitFor(() =>
        expect(screen.getByText(/storage full/i)).toBeInTheDocument()
      );
    });

    it("should show storage bar when storage info is available", async () => {
      const listPayload = encodeResponse({ list: { entries: [] } });
      const storagePayload = encodeResponse({
        storageInfo: {
          totalBytes: 8192,
          freeBytes: 4096,
          usedBytes: 4096,
          garbageBytes: 0,
        },
      });

      const user = await renderWithMocks();

      mocks.call_rpc
        .mockResolvedValueOnce({ custom: { call: { payload: listPayload } } })
        .mockResolvedValueOnce({
          custom: { call: { payload: storagePayload } },
        });

      await user.click(screen.getByRole("button", { name: /Load Settings/i }));

      await waitFor(() =>
        expect(screen.getByText(/4,096.*\/.*8,192.*bytes/i)).toBeInTheDocument()
      );
      expect(screen.getByRole("progressbar")).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: /Run GC/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: /Clear All/i })
      ).toBeInTheDocument();
    });
  });
});
