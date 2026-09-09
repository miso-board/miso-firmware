// Web Serial transport + Miso wire protocol parser.
// Frame: A5 5A 01 | u32 t_us LE | 31 x u16 LE | u8 checksum (byte sum of payload).
// Anything printable outside a frame is an ASCII status line (INFO / EV ...).

import { FRAME_LEN, NUM_KEYS, onFrame, countBadFrame } from "./engine";

export interface SerialCallbacks {
  onLine(line: string): void;
  onDisconnect(): void;
}

let port: SerialPort | null = null;
let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
let writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
let rxBuf = new Uint8Array(0);
let active = false;
let callbacks: SerialCallbacks | null = null;

export function serialSupported(): boolean {
  return "serial" in navigator;
}

/** Returns true when connected; throws SecurityError upward for the caller to explain. */
export async function connect(cbs: SerialCallbacks): Promise<boolean> {
  try {
    port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: 0x0483 }], // STMicroelectronics
    });
    await port.open({ baudRate: 115200 }); // rate is nominal for CDC
  } catch (e) {
    port = null;
    if (e instanceof DOMException && e.name === "SecurityError") throw e;
    return false; // user dismissed the picker
  }
  callbacks = cbs;
  active = true;
  writer = port.writable!.getWriter();
  void readLoop();
  return true;
}

export async function disconnect(): Promise<void> {
  active = false;
  try {
    if (writer) {
      await send("x");
      writer.releaseLock();
    }
  } catch {}
  try {
    if (reader) await reader.cancel();
  } catch {}
  try {
    if (port) await port.close();
  } catch {}
  port = reader = writer = null;
  rxBuf = new Uint8Array(0);
}

export async function send(s: string): Promise<void> {
  if (!writer) return;
  try {
    await writer.write(new TextEncoder().encode(s));
  } catch {}
}

export async function sendBytes(bytes: Uint8Array): Promise<void> {
  if (!writer) return;
  try {
    await writer.write(bytes);
  } catch {}
}

async function readLoop(): Promise<void> {
  while (port?.readable && active) {
    reader = port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) feed(value);
      }
    } catch {
      break; // device unplugged mid-read
    } finally {
      try {
        reader?.releaseLock();
      } catch {}
    }
  }
  if (active) {
    active = false;
    callbacks?.onDisconnect();
  }
}

function feed(chunk: Uint8Array): void {
  const merged = new Uint8Array(rxBuf.length + chunk.length);
  merged.set(rxBuf);
  merged.set(chunk, rxBuf.length);
  let off = 0;
  while (merged.length - off >= 1) {
    const b = merged[off];
    if (b === 0xa5) {
      if (merged.length - off < FRAME_LEN) break; // wait for a full frame
      if (merged[off + 1] === 0x5a && merged[off + 2] === 0x01) {
        let sum = 0;
        for (let i = off + 3; i < off + FRAME_LEN - 1; i++) sum = (sum + merged[i]) & 0xff;
        if (sum === merged[off + FRAME_LEN - 1]) {
          const dv = new DataView(merged.buffer, merged.byteOffset + off);
          const vals = new Uint16Array(NUM_KEYS);
          for (let i = 0; i < NUM_KEYS; i++) vals[i] = dv.getUint16(7 + 2 * i, true);
          onFrame(dv.getUint32(3, true), vals);
          off += FRAME_LEN;
          continue;
        }
      }
      countBadFrame();
      off++; // resync byte by byte
    } else if (b >= 0x20 && b < 0x7f) {
      let end = off;
      while (end < merged.length && merged[end] !== 0x0a) end++;
      if (end === merged.length && merged.length - off < 200) break; // partial line
      const line = new TextDecoder().decode(merged.subarray(off, end)).trim();
      if (line) callbacks?.onLine(line);
      off = end + 1;
    } else {
      off++;
    }
  }
  rxBuf = merged.slice(off);
}
