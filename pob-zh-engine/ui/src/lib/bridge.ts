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
  dynamicConnectors?: import("./tree/model").DynamicConnector[];
  sockets: TreeSocket[];
  /** POB's data.jewelRadius for this tree version (col is a ^xRRGGBB code). */
  jewelRadius?: JewelRadius[];
  points: BuildInfo["points"];
  rev: number;
}

/** A jewel socket as PassiveTreeView draws it: the jewel in it, its overlay art and the radius it rings. */
export interface TreeSocket {
  nodeId: number;
  expansion: boolean;
  expansionSize?: number;
  charm: boolean;
  itemId?: number;
  name?: string;
  nameZh?: string;
  title?: string;
  baseName?: string;
  rarity?: string;
  /** JewelSocketActive* / CharmSocketActive* (GetJewelSocketOverlay). */
  overlay?: string;
  /** 1-based index into jewelRadius; absent = no radius art. */
  radiusIndex?: number;
  radiusLabel?: string;
  /** Timeless ring pair key (maraketh/eternal/vaal/karui/templar/kalguur). */
  ringKey?: string;
}
export interface JewelRadius {
  inner: number;
  outer: number;
  col: string;
  label: string;
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
  /** Build.lua's classDrop / ascendDrop / secondaryAscendDrop lists and selection. */
  classes?: ClassEntry[];
  secondaryAscendancies?: AscEntry[];
  classPick?: ClassPick;
  rev: number;
}
export interface AscEntry {
  id: number;
  name: string;
  nameZh: string;
}
export interface ClassEntry {
  id: number;
  name: string;
  nameZh: string;
  ascendancies: AscEntry[];
}
export interface ClassPick {
  classId: number;
  className: string;
  classNameZh: string;
  ascendClassId: number;
  ascendClassName?: string;
  secondaryAscendClassId: number;
}
export interface ClassList {
  classes: ClassEntry[];
  secondary: AscEntry[];
  current: ClassPick;
}
/** set_class: the new tree state, or POB's "Class Change" question. */
export type SetClassResult =
  | (TreeState & { needsConfirm?: undefined })
  | { needsConfirm: "class_change"; classId: number; className: string; classNameZh: string; connectFailed?: boolean };
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

// --- items -------------------------------------------------------------------

export interface ItemSummary {
  id?: number;
  name: string;
  nameZh: string;
  title?: string;
  titleZh?: string;
  baseName?: string;
  baseNameZh?: string;
  rarity: "NORMAL" | "MAGIC" | "RARE" | "UNIQUE" | "RELIC" | string;
  type?: string;
  typeZh?: string;
  primarySlot?: string;
  unsupported: boolean;
  corrupted: boolean;
  quality?: number;
  itemLevel?: number;
  sockets: { color: string; group: number }[];
  influences: string[];
  clusterJewel: boolean;
  league?: string;
  source?: string;
  /** item_db only: the text to add_item with. */
  raw?: string;
}
export interface ItemSlot {
  name: string;
  label: string;
  labelZh: string;
  selItemId: number;
  weaponSet?: number;
  nodeId?: number;
  /** tree jewel sockets: POB's running number ("Socket #n") */
  socketIndex?: number;
  shown: boolean;
  inactive: boolean;
  parent?: string;
  isFlask: boolean;
  active: boolean;
  /** ids of the build's items POB accepts in this slot */
  valid: number[];
}
export interface ItemsList {
  items: ItemSummary[];
  slots: ItemSlot[];
  itemSets: { id: number; title?: string }[];
  activeItemSetId: number;
  useSecondWeaponSet: boolean;
  rev: number;
}
export interface ItemTooltip {
  id?: number;
  header?: string;
  color?: string;
  lines: TooltipLine[];
  summary: ItemSummary;
}
export interface ItemDbPage {
  kind: "unique" | "rare";
  total: number;
  page: number;
  size: number;
  items: ItemSummary[];
  types: { type: string; typeZh: string }[];
}

// --- skills ------------------------------------------------------------------

export interface GemInstance {
  index: number;
  nameSpec: string;
  name?: string;
  nameZh?: string;
  gemId?: string;
  skillId?: string;
  level: number;
  quality: number;
  enabled: boolean;
  enableGlobal1: boolean;
  enableGlobal2: boolean;
  count?: number;
  errMsg?: string;
  color?: string;
  support: boolean;
  hasGlobalEffect: boolean;
  naturalMaxLevel?: number;
  reqLevel?: number;
  matchesSocket: boolean;
  fromItem: boolean;
  fromNode: boolean;
}
export interface SocketGroup {
  index: number;
  label: string;
  displayLabel?: string;
  displayLabelZh?: string;
  enabled: boolean;
  includeInFullDPS: boolean;
  slot?: string;
  slotEnabled: boolean;
  source: boolean;
  sourceName?: string;
  mainActiveSkill: number;
  isMain: boolean;
  gems: GemInstance[];
  skills: { index: number; name?: string; nameZh?: string }[];
}
export interface SkillsList {
  groups: SocketGroup[];
  skillSets: { id: number; title?: string }[];
  activeSkillSetId: number;
  mainSocketGroup: number;
  slotOptions: { name: string; label: string; labelZh: string }[];
  defaultGemLevel?: string;
  defaultGemQuality?: number;
  rev: number;
}
export interface GemHit {
  gemId: string;
  name: string;
  nameZh: string;
  support: boolean;
  color?: number;
  tags: string[];
  naturalMaxLevel?: number;
  exceptional: boolean;
}
export interface GemPatch {
  nameSpec?: string;
  gemId?: string;
  level?: number;
  quality?: number;
  enabled?: boolean;
  enableGlobal1?: boolean;
  enableGlobal2?: boolean;
  count?: number;
}

// --- config / calcs ------------------------------------------------------------

export type ConfigValue = boolean | number | string | null | undefined;
export interface ConfigItem {
  var: string;
  type: "check" | "count" | "countAllowZero" | "integer" | "float" | "list" | "text" | string;
  label?: string;
  labelZh?: string;
  value?: ConfigValue;
  placeholder?: number | string;
  visible: boolean;
  tooltip?: string;
  tooltipZh?: string;
  list?: { val: ConfigValue; label: string; labelZh: string }[];
}
export interface ConfigSection {
  name: string;
  nameZh: string;
  col: number;
  items: ConfigItem[];
}
export interface CustomModBlock {
  title?: string;
  text: string;
  enabled: boolean;
}
export interface ConfigList {
  sections: ConfigSection[];
  customMods: CustomModBlock[];
  configSets: { id: number; title?: string }[];
  activeConfigSetId: number;
  rev: number;
}
export interface CalcCell {
  ci: number;
  text?: string;
  raw?: string;
  hasBreakdown?: boolean;
  control?: string;
}
export interface CalcRow {
  ri: number;
  label?: string;
  labelZh?: string;
  color?: string;
  textSize?: number;
  cells: CalcCell[];
}
export interface CalcSubsection {
  ui: number;
  label?: string;
  labelZh?: string;
  collapsed: boolean;
  extra?: string;
  rows: CalcRow[];
}
export interface CalcSection {
  si: number;
  id: string;
  group: number;
  colour?: string;
  widthCols: number;
  enabled: boolean;
  subsections: CalcSubsection[];
}
export interface CalcsData {
  sections: CalcSection[];
  input: { skill_number: number; misc_buffMode: string; showMinion: boolean };
  selectors: { mainSocketGroup?: DdField; mainSkill?: DdField; mainSkillPart?: DdField };
  hasMinion: boolean;
  rev: number;
}

// --- notes / party -------------------------------------------------------------

export interface PartyData {
  fields: Record<"partyMemberStats" | "aura" | "curse" | "warcry" | "link" | "enemyCond" | "enemyMods", string>;
  enableExportBuffs: boolean;
  exports: Record<string, string>;
  unsaved: boolean;
  rev: number;
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
  listClasses: () => bridge.call<ClassList>("list_classes"),
  setClass: (classId: number, confirm?: "reset" | "connect") => bridge.call<SetClassResult>("set_class", { classId, confirm }, 60000),
  setAscendancy: (ascendClassId: number) => bridge.call<TreeState>("set_ascendancy", { ascendClassId }, 60000),
  setSecondaryAscendancy: (ascendClassId: number) => bridge.call<TreeState>("set_secondary_ascendancy", { ascendClassId }, 60000),
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
  listItems: () => bridge.call<ItemsList>("list_items", {}, 60000),
  itemTooltip: (p: { id?: number; raw?: string; rarity?: string; slotName?: string; dbMode?: boolean }) => bridge.call<ItemTooltip>("item_tooltip", p, 60000),
  itemRaw: (id: number) => bridge.call<{ raw: string }>("item_raw", { id }),
  addItem: (raw: string, opts: { equip?: boolean; slotName?: string } = {}) => bridge.call<Committed & { item: ItemSummary }>("add_item", { raw, ...opts }, 60000),
  deleteItem: (id: number) => bridge.call<Committed>("delete_item", { id }, 60000),
  equipItem: (id: number, slotName: string) => bridge.call<Committed>("equip_item", { id, slotName }, 60000),
  unequipSlot: (slotName: string) => bridge.call<Committed>("unequip_slot", { slotName }, 60000),
  setSlotActive: (slotName: string, active: boolean) => bridge.call<Committed>("set_slot_active", { slotName, active }, 60000),
  setItemSet: (id: number) => bridge.call<Committed>("set_item_set", { id }, 60000),
  newItemSet: (title: string, copyCurrent: boolean) => bridge.call<Committed & { id: number }>("new_item_set", { title, copyCurrent }, 60000),
  renameItemSet: (id: number, title: string) => bridge.call<Committed>("rename_item_set", { id, title }, 60000),
  deleteItemSet: (id: number) => bridge.call<Committed>("delete_item_set", { id }, 60000),
  setWeaponSwap: (on: boolean) => bridge.call<Committed>("set_weapon_swap", { on }, 60000),
  itemDb: (p: { kind: "unique" | "rare"; query?: string; type?: string; page?: number; size?: number }) => bridge.call<ItemDbPage>("item_db", p, 120000),
  listSkills: () => bridge.call<SkillsList>("list_skills", {}, 60000),
  addGroup: (p: { label?: string; slot?: string; gems?: { nameSpec: string; level?: number; quality?: number }[] }) => bridge.call<Committed & { index: number }>("add_group", p, 60000),
  deleteGroup: (index: number) => bridge.call<Committed>("delete_group", { index }, 60000),
  setGroup: (index: number, p: { label?: string; enabled?: boolean; includeInFullDPS?: boolean; slot?: string; mainActiveSkill?: number }) => bridge.call<Committed>("set_group", { index, ...p }, 60000),
  moveGroup: (from: number, to: number) => bridge.call<Committed>("move_group", { from, to }, 60000),
  addGem: (group: number, p: { nameSpec?: string; gemId?: string; level?: number; quality?: number; index?: number }) => bridge.call<Committed & { gem: GemInstance }>("add_gem", { group, ...p }, 60000),
  setGem: (group: number, index: number, p: GemPatch) => bridge.call<Committed & { gem: GemInstance }>("set_gem", { group, index, ...p }, 60000),
  deleteGem: (group: number, index: number) => bridge.call<Committed>("delete_gem", { group, index }, 60000),
  moveGem: (group: number, from: number, to: number) => bridge.call<Committed>("move_gem", { group, from, to }, 60000),
  gemTooltip: (group: number, index: number) => bridge.call<{ lines: TooltipLine[]; header?: string }>("gem_tooltip", { group, index }, 60000),
  groupTooltip: (index: number) => bridge.call<{ lines: TooltipLine[] }>("group_tooltip", { index }, 60000),
  gemSearch: (p: { query: string; limit?: number; supportOnly?: boolean; activeOnly?: boolean }) => bridge.call<{ gems: GemHit[] }>("gem_search", p, 60000),
  setSkillSet: (id: number) => bridge.call<Committed>("set_skill_set", { id }, 60000),
  newSkillSet: (title: string, copyCurrent: boolean) => bridge.call<Committed & { id: number }>("new_skill_set", { title, copyCurrent }, 60000),
  renameSkillSet: (id: number, title: string) => bridge.call<Committed>("rename_skill_set", { id, title }, 60000),
  deleteSkillSet: (id: number) => bridge.call<Committed>("delete_skill_set", { id }, 60000),
  listConfig: () => bridge.call<ConfigList>("list_config", {}, 60000),
  setConfig: (v: string, value: ConfigValue) => bridge.call<Committed>("set_config", { var: v, value }, 60000),
  setConfigPlaceholder: (v: string, value: number | string | null) => bridge.call<Committed>("set_config_placeholder", { var: v, value }, 60000),
  resetConfig: (v: string) => bridge.call<Committed>("reset_config", { var: v }, 60000),
  setCustomMods: (list: CustomModBlock[]) => bridge.call<Committed>("set_custom_mods", { list }, 60000),
  setConfigSet: (id: number) => bridge.call<Committed>("set_config_set", { id }, 60000),
  newConfigSet: (title: string, copyCurrent: boolean) => bridge.call<Committed & { id: number }>("new_config_set", { title, copyCurrent }, 60000),
  renameConfigSet: (id: number, title: string) => bridge.call<Committed>("rename_config_set", { id, title }, 60000),
  deleteConfigSet: (id: number) => bridge.call<Committed>("delete_config_set", { id }, 60000),
  getCalcs: () => bridge.call<CalcsData>("get_calcs", {}, 60000),
  setCalcsInput: (v: string, value: unknown) => bridge.call<Committed>("set_calcs_input", { var: v, value }, 60000),
  calcsBreakdown: (si: number, ui: number, ri: number, ci: number) => bridge.call<{ sections: BreakdownSection[]; rev: number }>("calcs_breakdown", { si, ui, ri, ci }, 60000),
  getNotes: () => bridge.call<{ text: string; unsaved: boolean; rev: number }>("get_notes"),
  setNotes: (text: string) => bridge.call<Committed>("set_notes", { text }, 60000),
  getParty: () => bridge.call<PartyData>("get_party", {}, 60000),
  setParty: (field: string, text: string) => bridge.call<Committed>("set_party", { field, text }, 60000),
  setPartyExport: (value: boolean) => bridge.call<Committed>("set_party", { field: "enableExportBuffs", value }, 60000),
  hostInfo: () => bridge.call<HostInfo>("host.info"),
  setTitle: (text: string) => bridge.call<{ ok: boolean }>("host.set_title", { text }),
};
