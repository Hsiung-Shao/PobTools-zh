// The passive tree as the page draws it, built from what POB already worked
// out (bridge `tree_data`): node positions, connector list, frame names, group
// centres. Nothing geometric is derived here beyond turning POB's "this edge
// curves around its group" into an arc the canvas can stroke.

export type NodeKind = "normal" | "notable" | "keystone" | "socket" | "mastery" | "classStart" | "ascStart" | "image";

export interface FrameSet {
  alloc?: string;
  path?: string;
  unalloc?: string;
}

export interface MasteryEffect {
  effect: number;
  stats: string[];
  statsZh: string[];
}

/** One node from `tree_data.nodes` (POB's PassiveTree node, the fields we use). */
export interface RawNode {
  id: number;
  name: string;
  nameZh: string;
  type: string;
  stats: string[];
  statsZh: string[];
  x: number;
  y: number;
  size: number;
  group?: number;
  orbit?: number;
  asc?: string;
  classStartIndex?: number;
  startArt?: string;
  icon?: string;
  activeIcon?: string;
  inactiveIcon?: string;
  effectImage?: string;
  frames?: FrameSet;
  blighted: boolean;
  expansion: boolean;
  /** An expansion socket that lives inside a cluster proxy group (PassiveSpec drops these). */
  expansionParent?: boolean;
  proxy: boolean;
  linked: number[];
  masteryEffects?: MasteryEffect[];
  flavour?: string;
  /** PoE2: PassiveTree:GetNodeTargetSize half extents (base w/h, overlay ow/oh, effect ew/eh). */
  draw?: { w?: number; h?: number; ow?: number; oh?: number; ew?: number; eh?: number };
  /** PoE2 attribute node (Strength/Dexterity/Intelligence chosen on allocation). */
  attribute?: boolean;
  /**
   * PoE2 (the Oracle's hidden passives, node.unlockConstraint): every node listed
   * must be allocated before this one is drawn, hoverable or searchable.
   */
  unlock?: number[];
}

/** POB2's node whose unallocated hover previews the hidden passives waiting on it (PassiveTreeView unseenPathHover). */
export const UNSEEN_PATH_NODE = 5571;

/**
 * PassiveTreeView:checkUnlockConstraints, inverted: true while the node is still
 * hidden. `preview` = the unallocated UNSEEN_PATH_NODE is under the mouse, which
 * shows the nodes that wait on it first.
 */
export function isLocked(raw: Pick<RawNode, "unlock">, allocated: ReadonlySet<number>, preview = false): boolean {
  const u = raw.unlock;
  if (!u?.length) return false;
  if (preview && u[0] === UNSEEN_PATH_NODE) return false;
  return u.some((id) => !allocated.has(id));
}

export interface RawGroup {
  id: number;
  x: number;
  y: number;
  oo: number[];
  isProxy: boolean;
  asc?: string;
  ascStart: boolean;
  /** PoE2: the group's own background art (renderGroup), when it names one. */
  bg?: { image: string; half: boolean; offsetX?: number; offsetY?: number };
}

export interface RawConnector {
  a: number;
  b: number;
  orbit?: number;
  asc?: string;
  /** Arc centre POB computed (PassiveTree:BuildArc); PoE2 arcs need not centre on the group. */
  cx?: number;
  cy?: number;
}

/** PoE2 class plate: image at x,y (half extents w,h), BGTreeActive/BGTree sizes, the ascendancy plates. */
export interface ClassBackground {
  image: string;
  x: number;
  y: number;
  w: number;
  h: number;
  active?: { w: number; h: number };
  center?: { w: number; h: number };
  ascendancies: { id: number; key: string; image: string; x: number; y: number; w: number; h: number; replace?: string; replaceBy?: string }[];
}

export interface RawClass {
  id: number;
  name: string;
  nameZh: string;
  startNodeId?: number;
  ascendancies: { id: string; name: string; nameZh: string }[];
  art?: { name: string; x: number; y: number };
  background?: ClassBackground;
}

export interface TreeData {
  game?: "poe1" | "poe2";
  /** PassiveTree.scaleImage (PoE2 draws group art at sheet size x this). */
  artScale?: number;
  treeVersion: string;
  size: number;
  bounds: { minX: number; minY: number; maxX: number; maxY: number };
  orbitRadii: number[];
  nodes: Record<string, RawNode>;
  nodeCount: number;
  groups: RawGroup[];
  connectors: RawConnector[];
  classes: RawClass[];
  alternateAscendancies: { id: string; name: string; nameZh: string }[];
}

/** Cluster jewel subgraph node from `get_tree_state.dynamicNodes` (POB's own id, position, frames). */
export interface DynamicNode {
  id: number;
  name: string | null;
  nameZh?: string;
  type: string;
  stats: string[];
  statsZh?: string[];
  x: number;
  y: number;
  size?: number;
  orbit?: number;
  icon: string | null;
  links: number[];
  /** Subgraph id (the key of `spec.subGraphs`), same as its DynamicGroup.id. */
  group?: number;
  frames?: FrameSet;
  expansion: boolean;
  expansionSkill?: boolean;
  allocated: boolean;
}
export interface DynamicGroup {
  id?: number;
  x: number;
  y: number;
  orbits: number[];
  parentSocket?: number;
}
/** One of `subGraph.connectors` (PassiveTree:BuildConnector), like RawConnector but with the subgraph it belongs to. */
export interface DynamicConnector {
  a: number;
  b: number;
  orbit?: number;
  group?: number;
}

export interface TreeNode {
  id: number;
  kind: NodeKind;
  raw: RawNode;
  x: number;
  y: number;
  /** POB's hit and frame half-size (nodeOverlay.size = artWidth × 1.33). */
  size: number;
  asc: string | null;
  linked: number[];
  dynamic: boolean;
}

export interface Edge {
  a: number;
  b: number;
  asc: string | null;
  /** Curves around the group centre at this radius; null = straight. */
  arc: { cx: number; cy: number; r: number } | null;
}

export interface GroupRing {
  x: number;
  y: number;
  /** PSGroupBackground1..3 or the cluster (Alt) variants. */
  art: string;
  /** PoB draws the large ring as one half image mirrored. */
  mirrored: boolean;
}

export interface AscPlate {
  asc: string;
  x: number;
  y: number;
  art: string;
}

export interface TreeModel {
  poe2: boolean;
  version: string;
  size: number;
  bounds: TreeData["bounds"];
  nodes: Map<number, TreeNode>;
  edges: Edge[];
  rings: GroupRing[];
  plates: AscPlate[];
  classes: RawClass[];
  classStart: Map<string, number>;
  hit: HitIndex;
}

function kindOf(type: string): NodeKind {
  switch (type) {
    case "Notable":
      return "notable";
    case "Keystone":
      return "keystone";
    case "Socket":
      return "socket";
    case "Mastery":
      return "mastery";
    case "ClassStart":
      return "classStart";
    case "AscendClassStart":
      return "ascStart";
    case "OnlyImage":
      return "image";
    default:
      return "normal";
  }
}

function ringArt(oo: number[], cluster: boolean): { art: string; mirrored: boolean } | null {
  // PassiveTreeView.renderGroup: the largest orbit in use picks the ring.
  if (oo.includes(3)) return { art: cluster ? "GroupBackgroundLargeHalfAlt" : "PSGroupBackground3", mirrored: true };
  if (oo.includes(2)) return { art: cluster ? "GroupBackgroundMediumAlt" : "PSGroupBackground2", mirrored: false };
  if (oo.includes(1)) return { art: cluster ? "GroupBackgroundSmallAlt" : "PSGroupBackground1", mirrored: false };
  return null;
}

/** Coarse grid for "which node is under the cursor". Cell = 600 tree units. */
export class HitIndex {
  private cell = 600;
  private cells = new Map<string, TreeNode[]>();
  add(n: TreeNode) {
    if (n.size <= 0 || n.kind === "classStart" || n.kind === "image") return;
    const k = `${Math.floor(n.x / this.cell)}:${Math.floor(n.y / this.cell)}`;
    let list = this.cells.get(k);
    if (!list) this.cells.set(k, (list = []));
    list.push(n);
  }
  at(x: number, y: number): TreeNode | null {
    const gx = Math.floor(x / this.cell);
    const gy = Math.floor(y / this.cell);
    let best: TreeNode | null = null;
    let bestD = Infinity;
    for (let i = -1; i <= 1; i++) {
      for (let j = -1; j <= 1; j++) {
        const list = this.cells.get(`${gx + i}:${gy + j}`);
        if (!list) continue;
        for (const n of list) {
          const d = Math.hypot(n.x - x, n.y - y);
          if (d <= n.size && d < bestD) {
            bestD = d;
            best = n;
          }
        }
      }
    }
    return best;
  }
}

const FALLBACK_FRAMES: Record<string, FrameSet> = {
  socket: { alloc: "JewelSocketAltActive", path: "JewelSocketAltCanAllocate", unalloc: "JewelSocketAltNormal" },
  notable: { alloc: "NotableFrameAllocated", path: "NotableFrameCanAllocate", unalloc: "NotableFrameUnallocated" },
  keystone: { alloc: "KeystoneFrameAllocated", path: "KeystoneFrameCanAllocate", unalloc: "KeystoneFrameUnallocated" },
  normal: { alloc: "PSSkillFrameActive", path: "PSSkillFrameHighlighted", unalloc: "PSSkillFrame" },
};

export function buildModel(data: TreeData, dyn: DynamicNode[] = [], dynGroups: DynamicGroup[] = [], dynConnectors: DynamicConnector[] = []): TreeModel {
  const groups = new Map<number, RawGroup>();
  for (const g of data.groups) groups.set(g.id, g);

  const nodes = new Map<number, TreeNode>();
  const classStart = new Map<string, number>();
  for (const raw of Object.values(data.nodes)) {
    // PassiveSpec's own node filter (PassiveSpec.lua:54): proxies, anything
    // in a proxy group, and expansion sockets that belong to a proxy.
    if (raw.proxy || raw.expansionParent) continue;
    const g = raw.group != null ? groups.get(raw.group) : undefined;
    if (g?.isProxy) continue;
    const kind = kindOf(raw.type);
    nodes.set(raw.id, { id: raw.id, kind, raw, x: raw.x, y: raw.y, size: raw.size, asc: raw.asc ?? null, linked: raw.linked, dynamic: false });
  }
  for (const c of data.classes) if (c.startNodeId != null) classStart.set(c.name, c.startNodeId);

  const edges: Edge[] = [];
  for (const c of data.connectors) {
    const a = nodes.get(c.a);
    const b = nodes.get(c.b);
    if (!a || !b) continue;
    let arc: Edge["arc"] = null;
    if (c.orbit != null && c.cx != null && c.cy != null) {
      const r = data.orbitRadii[c.orbit];
      if (r) arc = { cx: c.cx, cy: c.cy, r };
    } else if (c.orbit != null && a.raw.group != null) {
      const g = groups.get(a.raw.group);
      const r = data.orbitRadii[c.orbit];
      if (g && r) arc = { cx: g.x, cy: g.y, r };
    }
    edges.push({ a: a.id, b: b.id, asc: c.asc ?? null, arc });
  }

  const poe2 = data.game === "poe2";
  const rings: GroupRing[] = [];
  const plates: AscPlate[] = [];
  for (const g of data.groups) {
    if (g.isProxy) continue;
    if (poe2) {
      // PoE2 renderGroup: art only where the group names it; ascendancy plates come with the classes
      if (g.bg) rings.push({ x: g.x + (g.bg.offsetX ?? 0), y: g.y + (g.bg.offsetY ?? 0), art: g.bg.image, mirrored: g.bg.half });
      continue;
    }
    if (g.asc) {
      if (g.ascStart) plates.push({ asc: g.asc, x: g.x, y: g.y, art: `Classes${g.asc}` });
      continue;
    }
    const art = ringArt(g.oo, false);
    if (art) rings.push({ x: g.x, y: g.y, ...art });
  }
  const dynGroupById = new Map<number, DynamicGroup>();
  for (const g of dynGroups) {
    if (g.id != null) dynGroupById.set(g.id, g);
    const art = ringArt(g.orbits, true);
    if (art) rings.push({ x: g.x, y: g.y, ...art });
  }

  // Cluster jewel nodes: POB generates and positions them per socketed jewel
  // (PassiveSpec:BuildSubgraph); their ids are POB's own, so a socket inside a
  // cluster keeps the id of the tree's expansion socket it stands in for.
  for (const d of dyn) {
    const kind = kindOf(d.type);
    const raw: RawNode = {
      id: d.id,
      name: d.name ?? "",
      nameZh: d.nameZh ?? d.name ?? "",
      type: d.type,
      stats: d.stats,
      statsZh: d.statsZh ?? d.stats,
      x: d.x,
      y: d.y,
      size: d.size && d.size > 0 ? d.size : kind === "notable" || kind === "socket" ? 58 * 1.33 : 40 * 1.33,
      group: d.group,
      orbit: d.orbit,
      icon: d.icon ?? undefined,
      frames: d.frames ?? FALLBACK_FRAMES[kind] ?? FALLBACK_FRAMES.normal,
      blighted: false,
      expansion: d.expansion,
      proxy: false,
      linked: d.links,
    };
    nodes.set(d.id, { id: d.id, kind, raw, x: d.x, y: d.y, size: raw.size, asc: null, linked: d.links, dynamic: true });
  }
  const seen = new Set<string>();
  const dynEdge = (a: number, b: number, orbit: number | undefined, group: number | undefined) => {
    const key = a < b ? `${a}:${b}` : `${b}:${a}`;
    if (seen.has(key) || !nodes.has(a) || !nodes.has(b)) return;
    seen.add(key);
    let arc: Edge["arc"] = null;
    if (orbit != null && group != null) {
      const g = dynGroupById.get(group);
      const r = data.orbitRadii[orbit];
      if (g && r) arc = { cx: g.x, cy: g.y, r };
    }
    edges.push({ a, b, asc: null, arc });
  };
  // POB's own connectors (arcs on the cluster's orbit); the link list is the
  // fallback for a bridge that did not send them.
  for (const c of dynConnectors) dynEdge(c.a, c.b, c.orbit, c.group);
  for (const d of dyn) for (const other of d.links) dynEdge(d.id, other, undefined, undefined);

  const hit = new HitIndex();
  for (const n of nodes.values()) hit.add(n);

  return { poe2, version: data.treeVersion, size: data.size, bounds: data.bounds, nodes, edges, rings, plates, classes: data.classes, classStart, hit };
}
