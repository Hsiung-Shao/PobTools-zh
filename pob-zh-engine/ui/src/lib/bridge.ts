// The page's one connection to the outside: JSON Lines to the host window,
// which forwards anything not "host.*" to the headless POB engine's stdin
// (engine/headless_ipc.h describes the wire format). Requests carry an id the
// page allocates; responses come back with the same id; lines without an id
// are events (hello, gate_result, restarted, error, host.child_exited...).
//
// Without `window.pobtools` (a plain browser during `vite dev`) a mock
// transport serves recorded fixtures so the UI can be iterated on alone.

export interface Transport {
  send(line: string): void;
  onMessage(fn: (line: string) => void): void;
}

export interface HostInfo {
  game: string;
  locale: string;
  exeDir: string;
  pobDir: string;
  version: string;
  hosts: { app: string; pob: string; data: string; fonts: string };
}

export interface BridgeError {
  code: string;
  message: string;
}

declare global {
  interface Window {
    pobtools?: Transport & { info: HostInfo };
  }
}

type Pending = {
  resolve: (v: unknown) => void;
  reject: (e: BridgeError) => void;
  timer: ReturnType<typeof setTimeout>;
};

export class Bridge {
  private pending = new Map<number, Pending>();
  private listeners = new Map<string, Set<(data: unknown) => void>>();
  private seq = 0;
  /** Every raw line received, newest last; for the debug panel and `dump()`. */
  readonly log: string[] = [];

  constructor(private transport: Transport) {
    transport.onMessage((raw) => this.receive(raw));
  }

  call<T>(method: string, params?: unknown, timeoutMs = 30000): Promise<T> {
    const id = ++this.seq;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject({ code: "timeout", message: `${method}: no response within ${timeoutMs} ms` });
      }, timeoutMs);
      this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
      this.transport.send(JSON.stringify({ id, method, params: params ?? {} }));
    });
  }

  on(event: string, fn: (data: unknown) => void): () => void {
    let set = this.listeners.get(event);
    if (!set) {
      set = new Set();
      this.listeners.set(event, set);
    }
    set.add(fn);
    return () => set!.delete(fn);
  }

  private receive(raw: string) {
    if (this.log.length > 200) this.log.shift();
    this.log.push(raw);
    let msg: any;
    try {
      msg = typeof raw === "string" ? JSON.parse(raw) : raw;
    } catch {
      this.emit("host.stray", { line: raw });
      return;
    }
    if (msg && typeof msg.id === "number" && this.pending.has(msg.id)) {
      const p = this.pending.get(msg.id)!;
      this.pending.delete(msg.id);
      clearTimeout(p.timer);
      if (msg.error) p.reject(msg.error as BridgeError);
      else p.resolve(msg.result);
      return;
    }
    if (msg && typeof msg.event === "string") {
      this.emit(msg.event, msg.data);
    }
  }

  private emit(event: string, data: unknown) {
    this.listeners.get(event)?.forEach((fn) => fn(data));
    this.listeners.get("*")?.forEach((fn) => fn({ event, data }));
  }
}

// --- dev-mode mock -----------------------------------------------------------

class MockTransport implements Transport {
  private handler: ((line: string) => void) | null = null;
  send(line: string) {
    const req = JSON.parse(line);
    const url = `./fixtures/${req.method}.json`;
    fetch(url)
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error(`${r.status}`))))
      .then((result) => this.handler?.(JSON.stringify({ id: req.id, result })))
      .catch((e) =>
        this.handler?.(
          JSON.stringify({ id: req.id, error: { code: "no_fixture", message: `${url}: ${e.message}` } }),
        ),
      );
  }
  onMessage(fn: (line: string) => void) {
    this.handler = fn;
    // The real engine announces itself; the mock does too so the app boots.
    setTimeout(() => {
      fn(JSON.stringify({ event: "gate_result", data: { ok: true, failed: [], checked: 0 } }));
      fn(JSON.stringify({ event: "hello", data: { protocol: 1, bridge: "mock", pobVersion: "mock" } }));
    }, 0);
  }
}

export const isHosted = typeof window !== "undefined" && !!window.pobtools;

export const hostInfo: HostInfo = isHosted
  ? window.pobtools!.info
  : {
      game: "poe1",
      locale: "zh-rTW",
      exeDir: "",
      pobDir: "",
      version: "dev",
      hosts: { app: "app.pobtools", pob: "pob.pobtools", data: "data.pobtools", fonts: "fonts.pobtools" },
    };

export const bridge = new Bridge(isHosted ? window.pobtools! : new MockTransport());

// --- typed wrappers ------------------------------------------------------------

export interface GateResult {
  ok: boolean;
  failed: string[];
  checked: number;
}

export interface VersionInfo {
  pobVersion: string;
  pobBranch: string;
  pobPlatform: string;
  bridge: string;
  headless: boolean;
  gate: GateResult;
  buildPath: string | null;
}

export interface UpdateStatus {
  available: string | null;
  checking: boolean;
  progress: string | null;
  error: string | null;
}

export const api = {
  version: () => bridge.call<VersionInfo>("version"),
  selfCheck: () => bridge.call<GateResult>("self_check"),
  getUpdateStatus: () => bridge.call<UpdateStatus>("get_update_status"),
  checkUpdateAsync: () => bridge.call<{ started: boolean }>("check_update_async"),
  applyUpdate: () => bridge.call<{ applied: string }>("apply_update", {}, 120000),
  hostInfo: () => bridge.call<HostInfo>("host.info"),
  setTitle: (text: string) => bridge.call<{ ok: boolean }>("host.set_title", { text }),
};
