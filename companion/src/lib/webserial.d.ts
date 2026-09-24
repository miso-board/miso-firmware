// Minimal Web Serial typings (not yet in TypeScript's DOM lib).
interface SerialPort {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
}

interface Serial {
  requestPort(options?: {
    filters?: { usbVendorId?: number; usbProductId?: number }[];
  }): Promise<SerialPort>;
}

interface Navigator {
  readonly serial: Serial;
}
