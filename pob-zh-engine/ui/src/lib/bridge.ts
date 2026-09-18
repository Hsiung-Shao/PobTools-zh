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
  /** Tab to show once a build is loaded (POB_ZH_UI_VIEW, a developer knob). */
  view?: string;
  /** The window's remembered scale (pob-zh.ini ModernZoom / ModernFontSize). */
  prefs?: UiPrefs;
  hosts: { app: string; pob: string; data: string; fonts: string; cache?: string };
  /** Served by the system-browser fallback (Wine / CrossOver): hosts are full URLs. */
  browser?: boolean;
}
export interface UiPrefs {
  zoom: number;
  fontSize: number;
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
      if (msg.error) {
        // the engine appends a Lua stack trace to a raised error; the page shows the message
        const e = msg.error as BridgeError;
        if (typeof e.message === "string") e.message = e.message.split(/\s*stack traceback:/)[0].trim();
        p.reject(e);
      } else p.resolve(msg.result);
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
      hosts: { app: "app.pobtools", pob: "pob.pobtools", data: "data.pobtools", fonts: "fonts.pobtools", cache: "cache.pobtools" },
    };

/**
 * URL of `path` under one of the host's folders. The WebView2 window names a
 * virtual host ("pob.pobtools"); the browser fallback hands over a full URL
 * ("http://127.0.0.1:port/t/<token>/~pob").
 */
export function hostUrl(host: keyof HostInfo["hosts"], path: string, info: HostInfo = hostInfo): string {
  const h = info.hosts[host] ?? `${host}.pobtools`;
  const base = h.includes("://") ? h : `https://${h}`;
  return `${base}/${path}`;
}

export const bridge = new Bridge(isHosted ? window.pobtools! : new MockTransport());

// --- typed wrappers ------------------------------------------------------------

export interface GateResult {
  ok: boolean;
  failed: string[];
  checked: number;
}

/** What this POB offers beyond the gate (bridge capabilities()); a false one hides a feature. */
export interface Caps {
  sidebarBreakdown?: boolean;
  siteImport?: boolean;
  itemEnchant?: boolean;
  itemCrucible?: boolean;
  itemInfluence?: boolean;
  itemModLineToggle?: boolean;
  abyssJewels?: boolean;
  /** PoE1's SkillsTab imbued support picker and Optimise Sockets. */
  skillImbued?: boolean;
  /** PoE1's tattoos (TreeTab:ModifyNodePopup). */
  tattoos?: boolean;
  /** PoE1's tree link import/export dialogs. */
  treeLinks?: boolean;
  /** PoE1's Add Mod browser for custom modifiers. */
  configModBrowser?: boolean;
  /** PoE1's "Find a Timeless Jewel" dialog. */
  timelessJewel?: boolean;
  /** POB's trade pane ("Trade for these items"). */
  tradeQuery?: boolean;
  /** POB's Compare tab. */
  compareTab?: boolean;
}

/** TreeTab's passive trees (list_specs). */
export interface SpecList {
  specs: {
    index: number;
    title?: string;
    treeVersion: string;
    versionLabel: string;
    latest: boolean;
    className: string;
    classNameZh?: string;
    ascendClassName?: string;
    ascendClassNameZh?: string;
    points: number;
    active: boolean;
  }[];
  activeSpec: number;
  treeVersion: string;
  versions: { value: string; label: string }[];
  showConvert: boolean;
  /** PoE1: the tree link import/export dialogs. */
  treeLinks: boolean;
  /** PoE1: Reset also offers Remove All Tattoos. */
  tattoos: boolean;
}

/** POB's two shared lists (main.sharedItemList / sharedItemSetList). */
export interface SharedItems {
  items: { index: number; name: string; nameZh?: string; rarity?: string; raw: string }[];
  sets: { index: number; title?: string; slots: Record<string, { name: string; nameZh?: string; rarity?: string }> }[];
}

/** One minion in POB's spectre/beast library. */
export interface MinionEntry {
  id: string;
  name: string;
  nameZh?: string;
  category?: string;
  recommended?: boolean;
}

/** POB's Compare tab: the loaded comparison builds and the Summary table. */
export interface CompareState {
  builds: { index: number; label?: string; buildName?: string; className?: string; ascendClassName?: string; level?: number; active: boolean }[];
  activeIndex: number;
  stats?: { gap?: boolean; stat?: string; label?: string; labelZh?: string; primary?: string; compare?: string; diff?: string; diffPercent?: string; better?: boolean }[];
  sets?: {
    specs: { index: number; title?: string }[];
    activeSpec: number;
    itemSets: { id: number; title?: string }[];
    activeItemSetId?: number;
    skillSets: { id: number; title?: string }[];
    activeSkillSetId?: number;
    configSets: { id: number; title?: string }[];
    activeConfigSetId?: number;
  };
}
export interface CompareTree {
  treeVersion?: string;
  allocatedNodes: number[];
  onlyInCompare: number[];
  onlyInPrimary: number[];
  points?: number;
}
export interface CompareItemsRows {
  rows: {
    slot: string;
    slotZh?: string;
    same: boolean;
    primary?: { name: string; nameZh?: string; rarity?: string; raw: string };
    compare?: { name: string; nameZh?: string; rarity?: string; raw: string };
  }[];
}
export interface CompareSkills {
  primary: { index: number; label?: string; labelZh?: string; slot?: string; enabled: boolean; isMain: boolean; gems: { name: string; nameZh?: string; level?: number; quality?: number; enabled: boolean }[] }[];
  compare: CompareSkills["primary"];
}
export interface CompareConfigRows {
  rows: { var: string; label?: string; labelZh?: string; primary?: string; compare?: string }[];
}

/** POB's price-builder pane (TradeQuery:PriceItem). */
export interface TradeState {
  authenticated: boolean;
  authLabel?: string;
  realm?: TjField;
  league?: TjField;
  tradeType?: TjField;
  sort?: TjField;
  itemSet?: TjField;
  fetchPages?: string;
  notice?: string;
  totalPrice?: string;
  rows?: {
    index: number;
    name: string;
    nameZh?: string;
    unique: boolean;
    url: string;
    validUrl: boolean;
    searching: boolean;
    canPrice: boolean;
    canFindBest: boolean;
    hasResults: boolean;
    selected: number;
    whisper?: string;
    results: { index: number; label: string; labelZh?: string; amount?: number; currency?: string }[];
  }[];
}

/** One of the timeless jewel dialog's drop-downs. */
export interface TjField {
  options: { label: string; labelZh?: string }[];
  sel: number;
  shown: boolean;
}
/** TreeTab's "Find a Timeless Jewel" as the bridge drives it. */
export interface TimelessState {
  jewel?: TjField;
  conqueror?: TjField;
  socket?: TjField;
  node?: TjField;
  fallbackWeights?: TjField;
  abyssAscendancy?: TjField;
  devotion1?: TjField;
  devotion2?: TjField;
  filterNodes?: { state: boolean; shown: boolean };
  socketJewel?: { state: boolean; shown: boolean };
  protectAllocated?: { state: boolean; shown: boolean };
  nodeDistance?: { value?: number; shown: boolean };
  weights?: { primary?: number; secondary?: number; minimum?: number; primaryLabel?: string; secondaryLabel?: string; minimumLabel?: string; distanceLabel?: string };
  totalMinimumWeight?: string;
  searchList?: string;
  searchListFallback?: string;
  results?: { index: number; label: string; seed: number; total: number; socketLabel?: string }[];
  resultCount?: number;
}

/** TreeTab's Show Node Power: the heat map's numbers and the Power Report. */
export interface NodePower {
  enabled: boolean;
  stat?: string;
  stats: { index: number; stat?: string; label: string; labelZh?: string }[];
  maxDepth?: number;
  theme?: string;
  powerMax?: { singleStat?: number; offence?: number; defence?: number };
  nodes?: Record<string, { offence?: number; defence?: number; singleStat?: number }>;
  report?: {
    id: number;
    name: string;
    nameZh?: string;
    type?: string;
    power: number;
    powerStr: string;
    pathPower: number;
    pathPowerStr: string;
    pathDist?: number;
    allocated: boolean;
    sd: string[];
    sdZh: string[];
  }[];
  rev?: number;
}

/** Build.lua's loadout drop-down (list_loadouts). */
export interface LoadoutList {
  entries: { index: number; label: string; labelZh?: string; kind: "header" | "action" | "loadout" }[];
  selIndex: number;
  activeLoadout?: number;
  rev?: number;
}

export interface GemOptionChoice {
  value: string;
  label: string;
  labelZh?: string;
  description?: string;
}
/** SkillsTab's "Gem Options" section. */
export interface GemOptions {
  sortGemsByDPS: boolean;
  sortGemsByDPSField: string;
  sortFields: GemOptionChoice[];
  defaultGemLevel: string;
  defaultGemLevels: GemOptionChoice[];
  defaultGemQuality: number;
  showSupportGemTypes: string;
  supportGemTypes: GemOptionChoice[];
  showLegacyGems: boolean;
}
/** What a group's detail shows beyond set_group's fields. */
export interface GroupExtras {
  index: number;
  count: number;
  countShown: boolean;
  imbued?: { shown: boolean; enabled: boolean; name?: string; nameZh?: string };
  optimiseSockets?: { shown: boolean; enabled: boolean };
}

/** bridge parse_item_text: the pasted text after reverse translation, and what POB's parser made of it. */
export interface ParsedItemText {
  reversed: boolean;
  text: string;
  /** Lines still holding Chinese after the reverse translation (POB turns them into '?'). */
  untranslated: string[];
  parsed: boolean;
  rarity?: string;
  title?: string;
  baseName?: string;
  lines?: { kind: string; line: string; lineZh: string; unsupported: boolean }[];
}

export interface VersionInfo {
  /** "poe1" or "poe2": which Path of Building the engine runs. */
  game?: "poe1" | "poe2";
  caps?: Caps;
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
/** POB's "Update Ready" dialog: the changelog entries newer than this version. */
export interface PobUpdateInfo {
  available?: string | null;
  version?: string;
  branch?: string;
  lines: { text?: string; height?: number }[];
  truncated?: boolean;
}
/** One control of POB's Options dialog, as the dialog built it. */
export interface PobOption {
  name: string;
  kind: "dropdown" | "edit" | "check" | "slider";
  section: "app" | "build";
  label?: string;
  labelZh?: string;
  tooltip?: string;
  tooltipZh?: string;
  /** drawn on the same row as this control (the proxy URL next to its scheme) */
  anchoredTo?: string;
  options?: { label: string; labelZh: string }[];
  sel?: number;
  text?: string;
  state?: boolean;
  value?: number;
}
export interface PobOptions {
  sections: { id: "app" | "build"; title?: string; titleZh?: string }[];
  options: PobOption[];
  branch?: string;
  version?: string;
  saved?: boolean;
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

export interface BuildListResult {
  buildPath: string;
  subPath: string;
  entries: BuildEntry[];
  /** POB's search box text in effect (main.filterBuildList). */
  filter?: string;
  /** main.buildSortMode and the choices of POB's sort drop-down. */
  sortMode?: string;
  sortModes?: { sortMode: string; label: string }[];
}

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
  /** TreeTab's Compare: the other tree, when it is ticked. */
  compare?: { index: number; title?: string; allocatedNodes: number[] };
  overrides: Record<string, TreeOverride>;
  dynamicNodes: import("./tree/model").DynamicNode[];
  dynamicGroups: import("./tree/model").DynamicGroup[];
  dynamicConnectors?: import("./tree/model").DynamicConnector[];
  sockets: TreeSocket[];
  /** POB's data.jewelRadius for this tree version (col is a ^xRRGGBB code). */
  jewelRadius?: JewelRadius[];
  points: BuildInfo["points"];
  /** PoE2: where new points go (0 main tree, 1/2 weapon set) and each allocated weapon-set node's set. */
  allocMode?: number;
  nodeModes?: Record<string, number>;
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
  | { needsConfirm: "class_change"; id: number; className: string; classNameZh: string; ascendClassName?: string; connectFailed?: boolean }
  | { needsAttribute: true; id: number; options: { index: number; name: string; nameZh: string }[]; last?: number }
  | { blocked: "weapon_set_global"; id: number };

/** TreeTab:ModifyNodePopup for one node (PoE1 tattoos). */
export interface TattooOptions {
  id: number;
  allowed: boolean;
  name?: string;
  nameZh?: string;
  isTattoo?: boolean;
  selected?: number;
  showLegacy?: boolean;
  count?: string;
  options?: { index: number; id: string | number; name: string; nameZh?: string; lines: string[]; linesZh: string[] }[];
}

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
  /** PoE2: which stat set of the skill (and of the minion's skill) to use. */
  statSet?: DdField;
  minionStatSet?: DdField;
  /** PoE1: the minion drop-down has POB's spectre library next to it. */
  minionLibrary?: string;
  /** PoE2 also has a beast library. */
  beastLibrary?: boolean;
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

// --- account import (ImportTab's OAuth and account-name flows) --------------

export interface AccountChar {
  name: string;
  league?: string;
  class?: string;
  classZh?: string;
  level?: number;
  realm?: string;
}
export interface ImportStatus {
  authorized: boolean;
  oauth: { loading: boolean; errCode?: string; timer?: number; rateLimitEnd?: number; now: number; url?: string };
  site: { mode: string; status?: string; statusZh?: string; accountName?: string; characters: AccountChar[] };
  realms: { id: string; label: string; realmCode: string }[];
  lastRealm?: string;
  lastLeague?: string;
  characters: Record<string, AccountChar[]>;
  lastAccountName?: string;
  accountHistory: string[];
  hasPoints: boolean;
  /** Count of POB import calls that ran (tree or items); moves when a download finished. */
  imported: number;
  lastImport?: "tree" | "items";
  recalculated: boolean;
  rev: number;
}
export interface AccountImportParams {
  source: "oauth" | "site";
  realm: string;
  name: string;
  league?: string;
  what: "tree" | "items";
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
  /** list_items: where the item is used (absent = unused), ItemListControl's own rule */
  usedIn?: ItemUsedIn | null;
  /** item_db with a stat sort: ListBuilder's measured power */
  measuredPower?: number;
}
export interface ItemUsedIn {
  kind: "slot" | "abyss" | "jewel";
  slot?: string;
  label?: string;
  labelZh?: string;
  setId?: number;
  setTitle?: string;
  specTitle?: string;
  /** used, but in another item set / tree than the active one */
  otherSet: boolean;
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
  /** the slot the stat-difference block compared against */
  compareSlot?: string;
  header?: string;
  color?: string;
  lines: TooltipLine[];
  summary: ItemSummary;
  /** The pasted text was Chinese and went through the reverse translator. */
  reversed?: boolean;
}
export interface ItemDbPage {
  kind: "unique" | "rare";
  total: number;
  page: number;
  size: number;
  items: ItemSummary[];
  /** a stat sort was asked for but more items match than the bridge will calculate */
  tooMany?: boolean;
  max?: number;
  statSort?: boolean;
}
export interface DdOption {
  label: string;
  labelZh: string;
}
export interface ItemDbOptions {
  kind: "unique" | "rare";
  slot: DdOption[];
  type: DdOption[];
  searchMode: DdOption[];
  league?: DdOption[];
  requirement?: DdOption[];
  obtainable?: DdOption[];
  sort?: (DdOption & { sortMode: string; stat?: string })[];
}
export interface ItemDbQuery {
  kind: "unique" | "rare";
  slot?: number;
  type?: number;
  league?: number;
  requirement?: number;
  obtainable?: number;
  searchMode?: number;
  query?: string;
  sortMode?: string;
  page?: number;
  size?: number;
}

// --- item editing (POB's displayItem session) ---------------------------------

export interface ItemEditAffix {
  index: number;
  table: "prefixes" | "suffixes";
  slot: number;
  kind: string;
  kindZh: string;
  options: (DdOption & { tiers?: number; haveRange: boolean })[];
  sel: number;
  roll?: number;
  rollShown: boolean;
  tiers?: number;
}
export interface ItemEditState {
  id?: number;
  isNew: boolean;
  raw: string;
  summary: ItemSummary;
  tooltip: { header?: string; color?: string; lines: TooltipLine[] };
  sockets: { color: string; group: number }[];
  socketShown: boolean[];
  socketColors: string[];
  links: { shown: boolean; on: boolean }[];
  canAddSocket: boolean;
  quality: { shown: boolean; value?: number };
  catalyst: { shown: boolean; options: DdOption[]; sel: number; qualityShown: boolean; quality?: number };
  influence: { shown: boolean; options: DdOption[]; sel: [number, number]; keys: string[]; current: string[] };
  variants: { control: string; options: DdOption[]; sel: number; enabled: boolean }[];
  versions?: { options: DdOption[]; sel: number };
  affixes: ItemEditAffix[];
  crafted: boolean;
  /** PoE1: how the affix drop-downs are sorted (ItemsTab craftingSorting). */
  affixSort?: { options: { label: string; labelZh?: string }[]; sel: number };
  /** PoE2: the item's rune and jewel socket counts. */
  runeSockets?: { count: number };
  jewelSockets?: { count: number };
  ranges: { index: number; label: string; labelZh: string; range?: number; showSlider: boolean; mutable: boolean; mutated: boolean }[];
  modLines: { index: number; text: string; textZh: string; disabled: boolean; kind?: "crafted" | "custom" | "crucible"; remove?: number }[];
  cluster?: { options: DdOption[]; sel: number; nodeCount?: number; minNodes?: number; maxNodes?: number };
  actions: Record<"enchant" | "enchant2" | "anoint" | "anoint2" | "anoint3" | "anoint4" | "corrupt" | "addImplicit" | "custom" | "crucible", boolean>;
  popup?: string | null;
}
export type ItemEditPopupKind = "enchant" | "anoint" | "corrupt" | "custom" | "crucible" | "text" | "implicit";
export interface ItemEditPopupControl {
  name: string;
  kind: "dropdown" | "edit" | "slider" | "check" | "button" | "nodes";
  enabled: boolean;
  options?: (DdOption & { id?: number })[];
  sel?: number;
  text?: string;
  value?: number;
  state?: boolean;
  label?: string;
  labelZh?: string;
  search?: string;
  /** notable lists: what the selected node does (POB's own anoint tooltip) */
  tooltip?: TooltipLine[];
}
export interface ItemEditPopup {
  popup: ItemEditPopupKind | null;
  title?: string;
  controls: ItemEditPopupControl[];
}
export interface CraftOptions {
  rarities: (DdOption & { rarity: string })[];
  types: { type: string; typeZh: string; bases: (DdOption & { name: string })[] }[];
  defaults: { rarity: number; type: number; base: number };
}
export type ItemEditSetParams =
  | { quality: number }
  | { catalyst: number }
  | { catalystQuality: number }
  | { influence: [number, number] }
  | { variant: { control: string; sel: number } }
  | { version: number }
  | { socket: { index: number; color: string } }
  | { link: { index: number; on: boolean } }
  | { addSocket: true }
  | { range: { index: number; value?: number; mutate?: boolean } }
  | { modLine: { index: number; enabled: boolean } }
  | { removeModLine: number }
  | { cluster: { sel?: number; nodeCount?: number } }
  | { affixSort: number }
  | { runeSockets: number }
  | { jewelSockets: number }
  | { raw: string };

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
  /** Sorted by DPS (POB's gem picker): its estimate and the colour POB gives it. */
  dps?: number;
  dpsColor?: string;
  canSupport?: boolean;
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

/** PartyTab's import row. */
export interface PartyImportState {
  destinations: { index: number; label: string; labelZh?: string }[];
  destination: number;
  append: boolean;
  valid: boolean;
  fetching: boolean;
  detail: string;
  detailZh?: string;
  rev: number;
}

/** main:OpenAboutPopup's lists: rows of columns with POB colour codes. */
export interface AboutData {
  version?: string;
  changelog: { height: number; cols: string[] }[];
  help: { height: number; cols: string[] }[];
}

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
  nodeInfo: (id: number, diff = false) => bridge.call<NodeInfo>("node_info", { id, diff }, 60000),
  tattooOptions: (id: number, showLegacy?: boolean) => bridge.call<TattooOptions>("tattoo_options", { id, showLegacy }, 60000),
  tattooApply: (p: { id: number; tattoo?: string | number; reset?: boolean; showLegacy?: boolean }) => bridge.call<TreeState>("tattoo_apply", p, 60000),
  setCompareSpec: (index?: number) => bridge.call<TreeState>("set_compare_spec", index == null ? {} : { index }, 60000),
  nodePower: (p: { enabled?: boolean; stat?: string; maxDepth?: number | null }) => bridge.call<NodePower>("node_power", p, 600000),
  compareState: () => bridge.call<CompareState>("compare_state", {}, 60000),
  compareLoad: (p: { code?: string; xml?: string; path?: string; label?: string }) => bridge.call<CompareState>("compare_load", p, 180000),
  compareSelect: (index: number) => bridge.call<CompareState>("compare_select", { index }, 60000),
  compareRemove: (index: number) => bridge.call<CompareState>("compare_remove", { index }, 60000),
  compareSet: (p: Record<string, unknown>) => bridge.call<CompareState>("compare_set", p, 120000),
  compareTree: () => bridge.call<CompareTree>("compare_tree", {}, 60000),
  compareItems: () => bridge.call<CompareItemsRows>("compare_items", {}, 60000),
  compareSkills: () => bridge.call<CompareSkills>("compare_skills", {}, 60000),
  compareConfig: () => bridge.call<CompareConfigRows>("compare_config", {}, 60000),
  compareUse: (what: "tree" | "item" | "config", slot?: string) => bridge.call<Committed>("compare_use", { what, slot }, 120000),
  tradeOpen: () => bridge.call<TradeState>("trade_open", {}, 120000),
  tradeSet: (p: Record<string, unknown>) => bridge.call<TradeState>("trade_set", p, 60000),
  tradeFindBest: (row: number) => bridge.call<TradeState>("trade_find_best", { row }, 300000),
  tradePrice: (row: number) => bridge.call<TradeState>("trade_price", { row }, 300000),
  tradePick: (row: number, index: number) => bridge.call<TradeState>("trade_pick", { row, index }, 60000),
  tradeImport: (row: number) => bridge.call<Committed & { state: TradeState }>("trade_import", { row }, 120000),
  tradeReset: (row: number) => bridge.call<TradeState>("trade_reset", { row }, 60000),
  tradeWhisper: (row: number) => bridge.call<{ text: string }>("trade_whisper", { row }, 60000),
  tradeAuth: () => bridge.call<TradeState>("trade_auth", {}, 120000),
  tradeRefresh: (seconds = 1) => bridge.call<TradeState>("trade_refresh", { seconds }, 60000),
  tradeClose: () => bridge.call<{ ok: boolean }>("trade_close", {}, 30000),
  tjOpen: () => bridge.call<TimelessState>("tj_open", {}, 120000),
  tjSet: (p: Record<string, unknown>) => bridge.call<TimelessState>("tj_set", p, 120000),
  tjSearch: () => bridge.call<TimelessState>("tj_search", {}, 900000),
  tjResult: (index: number, action?: "socket") => bridge.call<{ raw: string; ok?: boolean }>("tj_result", { index, action }, 120000),
  tjClose: () => bridge.call<{ ok: boolean }>("tj_close", {}, 30000),
  treeClick: (id: number, extra: { effect?: number; confirm?: "reset" | "connect"; attribute?: number; trace?: number[] } = {}) =>
    bridge.call<TreeClickResult>("tree_click", { id, ...extra }, 60000),
  selectMastery: (id: number, effect: number) => bridge.call<TreeState>("select_mastery", { id, effect }, 60000),
  treeAttribute: (id: number, attribute: number) => bridge.call<TreeState>("tree_attribute", { id, attribute }, 60000),
  setAllocMode: (mode: number) => bridge.call<TreeState>("set_alloc_mode", { mode }, 60000),
  listSpecs: () => bridge.call<SpecList>("list_specs", {}, 60000),
  listLoadouts: () => bridge.call<LoadoutList>("list_loadouts", {}, 60000),
  minionLibrary: (kind: "spectre" | "beast") => bridge.call<{ kind: string; inBuild: MinionEntry[]; available: MinionEntry[] }>("minion_library", { kind }, 60000),
  setMinionLibrary: (kind: "spectre" | "beast", ids: string[]) => bridge.call<Committed>("set_minion_library", { kind, ids }, 60000),
  listBuildSites: () => bridge.call<{ sites: { id: string; label: string; canImport: boolean; canShare: boolean }[]; lastExport?: string }>("list_build_sites", {}, 60000),
  importFromUrl: (url: string) => bridge.call<{ site: string; label: string; code: string }>("import_from_url", { url }, 120000),
  shareBuild: (site: string) => bridge.call<{ site: string; url: string }>("share_build", { site }, 120000),
  tabUndo: (tab: string, redo = false) => bridge.call<Committed & { canUndo: boolean; canRedo: boolean }>("tab_undo", { tab, redo }, 60000),
  undoState: () => bridge.call<{ tabs: Record<string, { canUndo: boolean; canRedo: boolean }> }>("undo_state", {}, 60000),
  selectLoadout: (index: number, title?: string) => bridge.call<LoadoutList>("select_loadout", { index, title }, 120000),
  configModSearch: (block: number, query: string, limit = 100) => bridge.call<{ mods: { text: string; textZh?: string; sources: string[] }[]; total: number }>("config_mod_search", { block, query, limit }, 60000),
  configModAdd: (block: number, text: string) => bridge.call<ConfigList>("config_mod_add", { block, text }, 60000),
  setActiveSpec: (index: number) => bridge.call<SpecList>("set_active_spec", { index }, 120000),
  specOp: (p: { op: "new" | "copy" | "rename" | "delete" | "move"; index?: number; title?: string; to?: number }) => bridge.call<SpecList>("spec_op", p, 120000),
  convertTree: (version: string, o: { copy?: boolean; all?: boolean }) => bridge.call<SpecList>("convert_tree", { version, ...o }, 180000),
  resetTree: (tattoos: boolean) => bridge.call<TreeState>("reset_tree", { tattoos }, 120000),
  exportTreeUrl: () => bridge.call<{ url: string }>("export_tree_url", {}, 60000),
  importTreeUrl: (url: string, title: string) => bridge.call<SpecList>("import_tree_url", { url, title }, 120000),
  treeUndo: () => bridge.call<TreeState>("tree_undo", {}, 60000),
  treeRedo: () => bridge.call<TreeState>("tree_redo", {}, 60000),
  listClasses: () => bridge.call<ClassList>("list_classes"),
  setClass: (classId: number, confirm?: "reset" | "connect") => bridge.call<SetClassResult>("set_class", { classId, confirm }, 60000),
  setAscendancy: (ascendClassId: number) => bridge.call<TreeState>("set_ascendancy", { ascendClassId }, 60000),
  setSecondaryAscendancy: (ascendClassId: number) => bridge.call<TreeState>("set_secondary_ascendancy", { ascendClassId }, 60000),
  listBuilds: (subPath = "", opts: { filter?: string; sortMode?: string } = {}) =>
    bridge.call<BuildListResult>("list_builds", { subPath, ...opts }),
  moveBuild: (p: { path: string; subPath: string; isFolder: boolean; name: string; targetSubPath: string; copy?: boolean }) =>
    bridge.call<{ path: string }>("move_build", p),
  loadBuildFile: (path: string) => bridge.call<LoadedBuild>("load_build_file", { path }, 120000),
  getSidebar: () => bridge.call<Sidebar>("get_sidebar"),
  sidebarBreakdown: (rowIndex: number) => bridge.call<{ sections: BreakdownSection[]; rev: number }>("sidebar_breakdown", { rowIndex }),
  getBuildInfo: () => bridge.call<BuildInfo>("get_build_info"),
  selfCheck: () => bridge.call<GateResult>("self_check"),
  getUpdateStatus: () => bridge.call<UpdateStatus>("get_update_status"),
  checkUpdateAsync: () => bridge.call<{ started: boolean }>("check_update_async"),
  pobUpdateInfo: () => bridge.call<PobUpdateInfo>("pob_update_info", {}, 60000),
  pobOptions: () => bridge.call<PobOptions>("pob_options", {}, 60000),
  setPobOptions: (values: Record<string, string | number | boolean>) => bridge.call<PobOptions>("set_pob_options", { values }, 60000),
  applyUpdate: (mode?: string, timeoutMs = 120000) => bridge.call<{ applied: string }>("apply_update", mode ? { mode } : {}, timeoutMs),
  getBuildHeader: () => bridge.call<BuildHeader>("get_build_header"),
  setBuildField: (field: string, value: unknown) => bridge.call<Committed>("set_build_field", { field, value }, 60000),
  saveBuild: () => bridge.call<SaveResult>("save_build", {}, 60000),
  saveBuildAs: (path: string) => bridge.call<SaveResult>("save_build_as", { path }, 60000),
  revertBuild: () => bridge.call<LoadedBuild>("revert_build", {}, 120000),
  exportCode: () => bridge.call<{ code: string; bytes: number }>("export_code", {}, 60000),
  decodeCode: (code: string) => bridge.call<CodeInfo>("decode_code", { code }, 60000),
  importCode: (code: string, mode: "replace" | "new") => bridge.call<LoadedBuild>("import_code", { code, mode }, 180000),
  importCharacter: (p: CharImportParams) => bridge.call<Committed & { imported: string[] }>("import_character", p, 180000),
  importStatus: () => bridge.call<ImportStatus>("import_status", {}, 60000),
  oauthStart: () => bridge.call<{ started?: boolean; authorized?: boolean; url?: string }>("oauth_start", {}, 30000),
  oauthLogout: () => bridge.call<{ authorized: boolean }>("oauth_logout"),
  fetchCharacters: (p: { source: "oauth" | "site"; realm: string; accountName?: string }) => bridge.call<{ started: boolean }>("fetch_characters", p, 30000),
  importAccountCharacter: (p: AccountImportParams) => bridge.call<{ started: boolean; what: string }>("import_account_character", p, 30000),
  importSiteReset: () => bridge.call<{ mode: string }>("import_site_reset"),
  listItems: () => bridge.call<ItemsList>("list_items", {}, 60000),
  itemTooltip: (p: { id?: number; raw?: string; rarity?: string; slotName?: string; dbMode?: boolean; compare?: boolean; slotOnly?: boolean }) => bridge.call<ItemTooltip>("item_tooltip", p, 60000),
  itemRaw: (id: number) => bridge.call<{ raw: string }>("item_raw", { id }),
  parseItemText: (raw: string) => bridge.call<ParsedItemText>("parse_item_text", { raw }, 60000),
  addItem: (raw: string, opts: { equip?: boolean; slotName?: string } = {}) => bridge.call<Committed & { item: ItemSummary; reversed?: boolean }>("add_item", { raw, ...opts }, 60000),
  deleteItem: (id: number) => bridge.call<Committed>("delete_item", { id }, 60000),
  equipItem: (id: number, slotName: string) => bridge.call<Committed>("equip_item", { id, slotName }, 60000),
  unequipSlot: (slotName: string) => bridge.call<Committed>("unequip_slot", { slotName }, 60000),
  setSlotActive: (slotName: string, active: boolean) => bridge.call<Committed>("set_slot_active", { slotName, active }, 60000),
  setItemSet: (id: number) => bridge.call<Committed>("set_item_set", { id }, 60000),
  newItemSet: (title: string, copyCurrent: boolean) => bridge.call<Committed & { id: number }>("new_item_set", { title, copyCurrent }, 60000),
  renameItemSet: (id: number, title: string) => bridge.call<Committed>("rename_item_set", { id, title }, 60000),
  deleteItemSet: (id: number) => bridge.call<Committed>("delete_item_set", { id }, 60000),
  setWeaponSwap: (on: boolean) => bridge.call<Committed>("set_weapon_swap", { on }, 60000),
  itemDb: (p: ItemDbQuery) => bridge.call<ItemDbPage>("item_db", p, 300000),
  itemDbOptions: (kind: "unique" | "rare") => bridge.call<ItemDbOptions>("item_db_options", { kind }, 180000),
  sortItems: () => bridge.call<Committed>("sort_items", {}, 60000),
  deleteUnusedItems: () => bridge.call<Committed & { deleted: number }>("delete_unused_items", {}, 60000),
  deleteAllItems: () => bridge.call<Committed>("delete_all_items", {}, 60000),
  equipPrimary: (id: number, alt: boolean) => bridge.call<Committed & { slotName: string; equipped: boolean }>("equip_primary", { id, alt }, 60000),
  craftItemOptions: () => bridge.call<CraftOptions>("craft_item_options", {}, 60000),
  itemEditBegin: (p: { id?: number; raw?: string; craft?: { rarity: string; type: string; base: string; title?: string } }) => bridge.call<ItemEditState>("item_edit_begin", p, 60000),
  itemEditState: () => bridge.call<ItemEditState>("item_edit_state", {}, 60000),
  itemEditSet: (p: ItemEditSetParams) => bridge.call<ItemEditState>("item_edit_set", p, 60000),
  itemEditAffix: (p: { index: number; sel?: number; roll?: number }) => bridge.call<ItemEditState>("item_edit_affix", p, 60000),
  itemEditPopup: (p: { kind?: ItemEditPopupKind; action: "open" | "pick" | "apply" | "cancel"; slot?: number; name?: string; sel?: number; text?: string; state?: boolean; value?: number; button?: string }) =>
    bridge.call<ItemEditPopup & Partial<ItemEditState>>("item_edit_popup", p, 120000),
  itemEditPopupTip: (name: string, value: number) => bridge.call<{ id: number; tooltip: TooltipLine[] }>("item_edit_popup", { action: "tip", name, value }, 120000),
  itemEditCommit: (equip: boolean) => bridge.call<Committed & { id: number; added: boolean; item: ItemSummary }>("item_edit_commit", { equip }, 60000),
  itemEditCancel: () => bridge.call<{ ok: boolean }>("item_edit_cancel", {}, 60000),
  listSkills: () => bridge.call<SkillsList>("list_skills", {}, 60000),
  addGroup: (p: { label?: string; slot?: string; gems?: { nameSpec: string; level?: number; quality?: number }[] }) => bridge.call<Committed & { index: number }>("add_group", p, 60000),
  deleteGroup: (index: number) => bridge.call<Committed>("delete_group", { index }, 60000),
  setGroup: (index: number, p: { label?: string; enabled?: boolean; includeInFullDPS?: boolean; slot?: string; mainActiveSkill?: number }) => bridge.call<Committed>("set_group", { index, ...p }, 60000),
  moveGroup: (from: number, to: number) => bridge.call<Committed>("move_group", { from, to }, 60000),
  groupExtras: (index: number) => bridge.call<GroupExtras>("group_extras", { index }, 60000),
  setGroupCount: (index: number, count: number) => bridge.call<Committed>("set_group_count", { index, count }, 60000),
  setImbuedSupport: (index: number, gemId?: string) => bridge.call<Committed>("set_imbued_support", { index, gemId }, 60000),
  optimiseSockets: (index: number) => bridge.call<Committed>("optimise_sockets", { index }, 60000),
  copyGroup: (index: number) => bridge.call<{ text: string }>("copy_group", { index }, 60000),
  pasteGroup: (text: string) => bridge.call<Committed & { index: number }>("paste_group", { text }, 60000),
  getGemOptions: () => bridge.call<GemOptions>("get_gem_options", {}, 60000),
  setGemOptions: (p: Partial<Pick<GemOptions, "sortGemsByDPS" | "sortGemsByDPSField" | "defaultGemLevel" | "defaultGemQuality" | "showSupportGemTypes" | "showLegacyGems">>) =>
    bridge.call<GemOptions>("set_gem_options", p, 60000),
  addGem: (group: number, p: { nameSpec?: string; gemId?: string; level?: number; quality?: number; index?: number }) => bridge.call<Committed & { gem: GemInstance }>("add_gem", { group, ...p }, 60000),
  setGem: (group: number, index: number, p: GemPatch) => bridge.call<Committed & { gem: GemInstance }>("set_gem", { group, index, ...p }, 60000),
  deleteGem: (group: number, index: number) => bridge.call<Committed>("delete_gem", { group, index }, 60000),
  moveGem: (group: number, from: number, to: number) => bridge.call<Committed>("move_gem", { group, from, to }, 60000),
  gemTooltip: (group: number, index: number) => bridge.call<{ lines: TooltipLine[]; header?: string }>("gem_tooltip", { group, index }, 60000),
  groupTooltip: (index: number) => bridge.call<{ lines: TooltipLine[] }>("group_tooltip", { index }, 60000),
  gemSearch: (p: { query: string; limit?: number; supportOnly?: boolean; activeOnly?: boolean; group?: number; index?: number; byDps?: boolean }) =>
    bridge.call<{ gems: GemHit[]; byDps?: boolean; baseDps?: number; dpsField?: string }>("gem_search", p, 600000),
  sharedItems: () => bridge.call<SharedItems>("shared_items", {}, 60000),
  shareItem: (id: number) => bridge.call<SharedItems>("share_item", { id }, 60000),
  shareItemSet: (id: number) => bridge.call<SharedItems>("share_item_set", { id }, 60000),
  unshare: (kind: "item" | "set", index: number) => bridge.call<SharedItems>("unshare", { kind, index }, 60000),
  useSharedSet: (index: number) => bridge.call<Committed & { id: number }>("use_shared_set", { index }, 60000),
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
  getNotes: () => bridge.call<{ text: string; unsaved: boolean; rev: number; colours?: { code: string; name: string }[] }>("get_notes"),
  setNotes: (text: string) => bridge.call<Committed>("set_notes", { text }, 60000),
  getParty: () => bridge.call<PartyData>("get_party", {}, 60000),
  partyImportState: () => bridge.call<PartyImportState>("party_import_state", {}, 60000),
  partyImport: (code: string, destination: number, append: boolean) => bridge.call<PartyImportState>("party_import", { code, destination, append }, 120000),
  partyAction: (action: "clear" | "disable" | "rebuild") => bridge.call<Committed>("party_action", { action }, 60000),
  about: () => bridge.call<AboutData>("about", {}, 60000),
  removeAccountHistory: (name: string) => bridge.call<ImportStatus>("remove_account_history", { name }, 60000),
  setParty: (field: string, text: string) => bridge.call<Committed>("set_party", { field, text }, 60000),
  setPartyExport: (value: boolean) => bridge.call<Committed>("set_party", { field: "enableExportBuffs", value }, 60000),
  hostInfo: () => bridge.call<HostInfo>("host.info"),
  setTitle: (text: string) => bridge.call<{ ok: boolean }>("host.set_title", { text }),
  getPrefs: () => bridge.call<UiPrefs>("host.get_prefs"),
  setPrefs: (p: Partial<UiPrefs>) => bridge.call<UiPrefs>("host.set_prefs", p),
  // build list management (POB's New / New Folder / Copy / Rename / Delete)
  newBuild: (name?: string, subPath?: string) => bridge.call<LoadedBuild>("new_build", { name, subPath }, 120000),
  newFolder: (subPath: string, name: string) => bridge.call<{ path: string; subPath: string }>("new_folder", { subPath, name }),
  renameBuild: (p: { path: string; subPath: string; isFolder: boolean; newName: string; copy?: boolean }) => bridge.call<{ path: string }>("rename_build", p),
  deleteBuild: (p: { path: string; isFolder: boolean; recursive?: boolean }) => bridge.call<{ deleted: string }>("delete_build", p),
};
