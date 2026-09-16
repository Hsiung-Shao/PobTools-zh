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
  /** A build .xml to open as soon as the engine is up ("" = none). */
  open?: string;
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
  /** POB reopened its last build by itself at startup. */
  buildLoaded: boolean;
}

export interface UpdateStatus {
  available: string | null;
  checking: boolean;
  progress: string | null;
  error: string | null;
}

export interface SidebarRow {
  h: number;
  lhs?: string;
  rhs?: string;
  lhsRaw?: string;
  rhsRaw?: string;
  stat?: string;
  actor?: "player" | "minion";
  align?: string;
  hasBreakdown: boolean;
}

export interface Sidebar {
  rows: SidebarRow[];
  warnings: { text: string; raw: string }[];
  rev: number;
}

export type BreakdownSection =
  | { type: "text"; size: number; lines: string[] }
  | { type: "table"; label?: string; footer?: string; cols: { label: string; key: string; right: boolean }[]; rows: Record<string, string>[] }
  | { type: "radius"; radius: number };

export interface BuildEntry {
  isFolder: boolean;
  folderName?: string;
  fileName?: string;
  fullFileName: string;
  subPath: string;
  buildName?: string;
  level?: number;
  className?: string;
  ascendClassName?: string;
  modified?: number;
}

export interface BuildInfo {
  buildName: string;
  dbFileName?: string;
  unsaved: boolean;
  level: number;
  classId?: number;
  className?: string;
  classNameZh?: string;
  ascendClassId?: number;
  ascendClassName?: string;
  ascendClassNameZh?: string;
  treeVersion?: string;
  points: {
    used: number; ascUsed: number; secondaryAscUsed: number; sockets: number;
    usedMax?: number; ascMax?: number; display?: string; req?: string;
  };
  rev: number;
}

export interface LoadedBuild {
  buildName: string;
  dbFileName: string;
  outputRevision: number;
  className?: string;
  ascendClassName?: string;
  level?: number;
}

export type TreeData = import("./tree/model").TreeData;

export interface TreeOverride {
  why: "mastery" | "conquered" | "tattoo" | "renamed";
  name: string;
  nameZh: string;
  icon?: string;
  effect?: string;
  stats: string[];
  statsZh: string[];
}

export interface TreeState {
  treeVersion: string;
  classId?: number;
  className?: string;
  ascendClassId?: number;
  ascendClassName?: string;
  allocatedNodes: number[];
  allocCount: number;
  overrides: Record<string, TreeOverride>;
  dynamicNodes: import("./tree/model").DynamicNode[];
  dynamicGroups: import("./tree/model").DynamicGroup[];
  sockets: { nodeId: number; itemId?: number; name?: string; title?: string; baseName?: string }[];
  points: BuildInfo["points"];
  rev: number;
}

export interface MasteryChoice {
  effect: number;
  stats: string[];
  statsZh: string[];
  takenBy?: number;
}

/** tree_click: either the new state, or a question the page has to answer. */
export type TreeClickResult =
  | (TreeState & { needsMastery?: undefined; needsConfirm?: undefined })
  | { needsMastery: true; id: number; name: string; nameZh: string; effects: MasteryChoice[]; selected?: number }
  | { needsConfirm: "class_change"; id: number; className: string; classNameZh: string; ascendClassName?: string; connectFailed?: boolean };

export interface NodeHover {
  id: number;
  allocated: boolean;
  path: number[];
  depends: number[];
  cost?: number;
}

export type TooltipLine = { size: number; text: string; raw: string; center: boolean; font?: string } | { sep: number };

export interface NodeInfo {
  id: number;
  name: string;
  nameZh: string;
  type?: string;
  allocated: boolean;
  header?: string;
  lines: TooltipLine[];
  stats: string[];
  statsZh: string[];
  masteryEffects?: { effect: number; stats: string[] }[];
  masterySelected?: number;
  pathDist?: number;
}

export interface DdEntry {
  val?: number;
  label: string;
  labelZh?: string;
  minionId?: string;
  itemSetId?: number;
}
export interface DdField {
  index: number;
  list: DdEntry[];
  enabled?: boolean;
}
/** The top bar as POB fills it (Build.lua's level + main-skill controls). */
export interface BuildHeader {
  buildName: string;
  dbFileName?: string;
  unsaved: boolean;
  level: number;
  levelAuto: boolean;
  mainSocketGroup: DdField;
  mainSkill?: DdField;
  mainSkillPart?: DdField;
  mainSkillStageCount?: number;
  mainSkillMineCount?: number;
  mainSkillMinion?: DdField;
  mainSkillMinionSkill?: DdField;
  rev: number;
}
export interface Committed {
  rev: number;
  unsaved: boolean;
}
export interface SaveResult {
  dbFileName: string;
  buildName: string;
  subPath?: string;
  unsaved: boolean;
  rev: number;
}
export interface CodeInfo {
  sections: string[];
  className?: string;
  classNameZh?: string;
  ascendClassName?: string;
  ascendClassNameZh?: string;
  level?: number;
  targetVersion?: string;
  itemCount?: number;
  skillCount?: number;
  hasTree?: boolean;
  xmlBytes: number;
}
export interface CharImportParams {
  items?: string;
  passives?: string;
  importTree?: boolean;
  importItems?: boolean;
  deleteJewels?: boolean;
  clearItems?: boolean;
  clearSkills?: boolean;
  ignoreWeaponSwap?: boolean;
}

export const api = {
  version: () => bridge.call<VersionInfo>("version"),
  treeData: (version?: string) => bridge.call<TreeData>("tree_data", version ? { version } : {}, 120000),
  treeAssets: (version?: string) => bridge.call<import("./tree/assets").SpriteManifest>("tree_assets", version ? { version } : {}, 60000),
  getTreeState: () => bridge.call<TreeState>("get_tree_state"),
  nodeHover: (id: number) => bridge.call<NodeHover>("node_hover", { id }),
  nodeInfo: (id: number) => bridge.call<NodeInfo>("node_info", { id }),
  treeClick: (id: number, extra: { effect?: number; confirm?: "reset" | "connect" } = {}) =>
    bridge.call<TreeClickResult>("tree_click", { id, ...extra }, 60000),
  selectMastery: (id: number, effect: number) => bridge.call<TreeState>("select_mastery", { id, effect }, 60000),
  treeUndo: () => bridge.call<TreeState>("tree_undo", {}, 60000),
  treeRedo: () => bridge.call<TreeState>("tree_redo", {}, 60000),
  listBuilds: (subPath = "") => bridge.call<{ buildPath: string; subPath: string; entries: BuildEntry[] }>("list_builds", { subPath }),
  loadBuildFile: (path: string) => bridge.call<LoadedBuild>("load_build_file", { path }, 120000),
  getSidebar: () => bridge.call<Sidebar>("get_sidebar"),
  sidebarBreakdown: (rowIndex: number) => bridge.call<{ sections: BreakdownSection[]; rev: number }>("sidebar_breakdown", { rowIndex }),
  getBuildInfo: () => bridge.call<BuildInfo>("get_build_info"),
  selfCheck: () => bridge.call<GateResult>("self_check"),
  getUpdateStatus: () => bridge.call<UpdateStatus>("get_update_status"),
  checkUpdateAsync: () => bridge.call<{ started: boolean }>("check_update_async"),
  applyUpdate: () => bridge.call<{ applied: string }>("apply_update", {}, 120000),
  getBuildHeader: () => bridge.call<BuildHeader>("get_build_header"),
  setBuildField: (field: string, value: unknown) => bridge.call<Committed>("set_build_field", { field, value }, 60000),
  saveBuild: () => bridge.call<SaveResult>("save_build", {}, 60000),
  saveBuildAs: (path: string) => bridge.call<SaveResult>("save_build_as", { path }, 60000),
  revertBuild: () => bridge.call<LoadedBuild>("revert_build", {}, 120000),
  exportCode: () => bridge.call<{ code: string; bytes: number }>("export_code", {}, 60000),
  decodeCode: (code: string) => bridge.call<CodeInfo>("decode_code", { code }, 60000),
  importCode: (code: string, mode: "replace" | "new") => bridge.call<LoadedBuild>("import_code", { code, mode }, 180000),
  importCharacter: (p: CharImportParams) => bridge.call<Committed & { imported: string[] }>("import_character", p, 180000),
  hostInfo: () => bridge.call<HostInfo>("host.info"),
  setTitle: (text: string) => bridge.call<{ ok: boolean }>("host.set_title", { text }),
};
