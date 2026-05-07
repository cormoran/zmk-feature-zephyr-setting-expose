// jest-dom adds custom jest matchers for asserting on DOM nodes.
import "@testing-library/jest-dom";
import { TextEncoder, TextDecoder } from "util";

// Polyfill TextEncoder/TextDecoder for jsdom environment
Object.defineProperty(globalThis, "TextEncoder", {
  value: TextEncoder,
  writable: false,
});
Object.defineProperty(globalThis, "TextDecoder", {
  value: TextDecoder,
  writable: false,
});
