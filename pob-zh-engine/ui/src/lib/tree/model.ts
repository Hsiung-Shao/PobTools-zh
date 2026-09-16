// The passive tree as the page draws it, built from what POB already worked
// out (bridge `tree_data`): node positions, connector list, frame names, group
// centres. Nothing geometric is derived here beyond turning POB's "this edge
// curves around its group" into an arc the canvas can stroke.

export type NodeKind = "normal" | "notable" | "keystone" | "socket" | "mastery" | "classStart" | "ascStart";

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
  proxy: boolean;
  linked: number[];
  masteryEffects?: MasteryEffect[];
  flavour?: string;
}

export interface RawGroup {
  id: number;
  x: number;
  y: number;
  oo: number[];
  isProxy: boolean;
  asc?: string;
  ascStart: boolean;
}

export interface RawConnector {
  a: number;
  b: number;
  orbit?: number;
  asc?: string;
}

export interface RawClass {
  id: number;
  name: string;
  nameZh: string;
  startNodeId?: number;
  ascendancies: { id: string; name: string; nameZh: string }[];
  art?: { name: string; x: number; y: number };
}

export interface TreeData {
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

/** Cluster jewel subgraph node from `get_tree_state.dynamicNodes`. */
export interface DynamicNode {
  id: number;
  name: string | null;
  nameZh?: string;
  type: string;
  stats: string[];
  x: number;
  y: number;
  icon: string | null;
  links: number[];
  expansion: boolean;
  allocated: boolean;
}
export interface DynamicGroup {
  x: number;
  y: number;
  orbits: number[];
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
    if (n.size <= 0 || n.kind === "classStart") return;
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

export function buildModel(data: TreeData, dyn: DynamicNode[] = [], dynGroups: DynamicGroup[] = []): TreeModel {
  const groups = new Map<number, RawGroup>();
  for (const g of data.groups) groups.set(g.id, g);

  const nodes = new Map<number, TreeNode>();
  const classStart = new Map<string, number>();
  for (const raw of Object.values(data.nodes)) {
    if (raw.proxy) continue;
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
    if (c.orbit != null && a.raw.group != null) {
      const g = groups.get(a.raw.group);
      const r = data.orbitRadii[c.orbit];
      if (g && r) arc = { cx: g.x, cy: g.y, r };
    }
    edges.push({ a: a.id, b: b.id, asc: c.asc ?? null, arc });
  }

  const rings: GroupRing[] = [];
  const plates: AscPlate[] = [];
  for (const g of data.groups) {
    if (g.isProxy) continue;
    if (g.asc) {
      if (g.ascStart) plates.push({ asc: g.asc, x: g.x, y: g.y, art: `Classes${g.asc}` });
      continue;
    }
    const art = ringArt(g.oo, false);
    if (art) rings.push({ x: g.x, y: g.y, ...art });
  }
  for (const g of dynGroups) {
    const art = ringArt(g.orbits, true);
    if (art) rings.push({ x: g.x, y: g.y, ...art });
  }

  // Cluster jewel nodes: POB generates and positions them per socketed jewel;
  // they link to the parent socket and to each other, all straight lines.
  for (const d of dyn) {
    const kind = kindOf(d.type);
    const raw: RawNode = {
      id: d.id,
      name: d.name ?? "",
      nameZh: d.nameZh ?? d.name ?? "",
      type: d.type,
      stats: d.stats,
      statsZh: d.stats,
      x: d.x,
      y: d.y,
      size: kind === "notable" ? 58 * 1.33 : kind === "socket" ? 58 * 1.33 : 40 * 1.33,
      icon: d.icon ?? undefined,
      frames:
        kind === "socket"
          ? { alloc: "JewelSocketAltActive", path: "JewelSocketAltCanAllocate", unalloc: "JewelSocketAltNormal" }
          : kind === "notable"
            ? { alloc: "NotableFrameAllocated", path: "NotableFrameCanAllocate", unalloc: "NotableFrameUnallocated" }
            : { alloc: "PSSkillFrameActive", path: "PSSkillFrameHighlighted", unalloc: "PSSkillFrame" },
      blighted: false,
      expansion: d.expansion,
      proxy: false,
      linked: d.links,
    };
    nodes.set(d.id, { id: d.id, kind, raw, x: d.x, y: d.y, size: raw.size, asc: null, linked: d.links, dynamic: true });
  }
  const seen = new Set<string>();
  for (const d of dyn) {
    for (const other of d.links) {
      const key = d.id < other ? `${d.id}:${other}` : `${other}:${d.id}`;
      if (seen.has(key) || !nodes.has(other)) continue;
      seen.add(key);
      edges.push({ a: d.id, b: other, asc: null, arc: null });
    }
  }

  const hit = new HitIndex();
  for (const n of nodes.values()) hit.add(n);

  return { version: data.treeVersion, size: data.size, bounds: data.bounds, nodes, edges, rings, plates, classes: data.classes, classStart, hit };
}
