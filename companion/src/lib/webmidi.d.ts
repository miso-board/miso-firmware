// Minimal Web MIDI typings (not in TypeScript's DOM lib).
interface MisoMIDIMessageEvent extends Event {
  data: Uint8Array;
}

interface MisoMIDIInput extends EventTarget {
  id: string;
  name: string | null;
  manufacturer: string | null;
  state: string;
  onmidimessage: ((e: MisoMIDIMessageEvent) => void) | null;
}

interface MisoMIDIAccess extends EventTarget {
  inputs: Map<string, MisoMIDIInput>;
  outputs: Map<string, unknown>;
  onstatechange: ((e: Event) => void) | null;
}

interface Navigator {
  requestMIDIAccess(options?: { sysex?: boolean }): Promise<MisoMIDIAccess>;
}
