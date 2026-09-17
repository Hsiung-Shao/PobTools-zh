import { describe, expect, it } from "vitest";
import { buildModel, HitIndex, type RawNode, type TreeData, type TreeNode } from "./model";

const node = (p: Partial<RawNode> & { id: number; x: number; y: number }): RawNode => ({
  name: `n${p.id}`,
  nameZh: `節${p.id}`,
  type: "Normal",
  stats: [],
  statsZh: [],
  size: 40,
  linked: [],
  blighted: false,
  expansion: false,
  proxy: false,
  ...p,
});

const data: TreeData = {
  treeVersion: "3_29",
  size: 100,
  bounds: { minX: -1000, minY: -1000, maxX: 1000, maxY: 1000 },
  orbitRadii: [0, 82, 162, 335],
  nodes: {
    "1": node({ id: 1, x: 0, y: 0, type: "ClassStart", classStartIndex: 3, linked: [2] }),
    "2": node({ id: 2, x: 100, y: 0, group: 7, orbit: 1, linked: [1, 3] }),
    "3": node({ id: 3, x: 200, y: 0, type: "Notable", group: 7, orbit: 1, linked: [2] }),
    "4": node({ id: 4, x: 900, y: 900, type: "Mastery", proxy: true }),
  },
  nodeCount: 4,
  groups: [{ id: 7, x: 150, y: 0, oo: [1], isProxy: false, ascStart: false }],
  connectors: [
    { a: 1, b: 2 },
    { a: 2, b: 3, orbit: 1 },
    { a: 3, b: 99 },
  ],
  classes: [{ id: 3, name: "Witch", nameZh: "女巫", startNodeId: 1, ascendancies: [] }],
  alternateAscendancies: [],
};

describe("buildModel", () => {
  const m = buildModel(data);
  it("keeps POB's coordinates and kinds, drops proxy nodes", () => {
    expect(m.nodes.size).toBe(3);
    expect(m.nodes.get(3)!.kind).toBe("notable");
    expect(m.nodes.get(1)!.kind).toBe("classStart");
    expect(m.nodes.get(2)!.x).toBe(100);
  });
  it("turns connectors into edges, arcs on an orbit around the group centre, and skips unknown ids", () => {
    expect(m.edges).toHaveLength(2);
    const straight = m.edges.find((e) => e.a === 1 && e.b === 2)!;
    const curved = m.edges.find((e) => e.a === 2 && e.b === 3)!;
    expect(straight.arc).toBeNull();
    expect(curved.arc).toEqual({ cx: 150, cy: 0, r: 82 });
  });
  it("records class start nodes and a ring for the group", () => {
    expect(m.classStart.get("Witch")).toBe(1);
    expect(m.rings).toHaveLength(1);
    expect(m.rings[0].art).toBe("PSGroupBackground1");
  });
  it("applies PassiveSpec's filter: nodes in proxy groups and parented expansion sockets are dropped", () => {
    const d: TreeData = {
      ...data,
      nodes: {
        ...data.nodes,
        "5": node({ id: 5, x: 500, y: 500, group: 9 }),
        "6": node({ id: 6, x: 600, y: 600, type: "Socket", expansion: true, expansionParent: true }),
        "7": node({ id: 7, x: 700, y: 700, type: "Socket", expansion: true }),
      },
      groups: [...data.groups, { id: 9, x: 500, y: 500, oo: [1], isProxy: true, ascStart: false }],
    };
    const mm = buildModel(d);
    expect(mm.nodes.has(5)).toBe(false);
    expect(mm.nodes.has(6)).toBe(false);
    expect(mm.nodes.has(7)).toBe(true);
    expect(mm.rings).toHaveLength(1); // no ring for the proxy group
  });
});

describe("buildModel with cluster subgraphs", () => {
  // The socket 7 is a large expansion socket on the static tree; POB's
  // subgraph hangs off it with its own ids and its own connectors.
  const d: TreeData = { ...data, nodes: { ...data.nodes, "7": node({ id: 7, x: 700, y: 700, type: "Socket", expansion: true }) } };
  const dyn = [
    { id: 66576, name: "e", type: "Normal", stats: [], x: 750, y: 700, size: 53.2, icon: null, links: [7, 66579], group: 66500, expansion: false, allocated: true, frames: { alloc: "A", path: "P", unalloc: "U" } },
    { id: 66579, name: "n", type: "Notable", stats: ["x"], statsZh: ["叉"], x: 800, y: 700, icon: null, links: [66576], group: 66500, expansion: false, allocated: false },
  ];
  const groups = [{ id: 66500, x: 775, y: 700, orbits: [2], parentSocket: 7 }];
  const conns = [
    { a: 66576, b: 66579, orbit: 2, group: 66500 },
    { a: 66576, b: 7, group: 66500 },
  ];
  const m = buildModel(d, dyn, groups, conns);
  it("keeps POB's node ids: nothing static is overwritten", () => {
    expect(m.nodes.size).toBe(3 + 1 + 2);
    expect(m.nodes.get(1)!.kind).toBe("classStart");
    expect(m.nodes.get(66576)!.dynamic).toBe(true);
    expect(m.nodes.get(66576)!.size).toBe(53.2);
    expect(m.nodes.get(66576)!.raw.frames).toEqual({ alloc: "A", path: "P", unalloc: "U" });
    expect(m.nodes.get(66579)!.raw.statsZh).toEqual(["叉"]);
    expect(m.nodes.get(66579)!.raw.frames!.alloc).toBe("NotableFrameAllocated");
  });
  it("turns POB's subgraph connectors into edges, arcs around the subgraph's group, and adds the cluster ring", () => {
    const arc = m.edges.find((e) => e.a === 66576 && e.b === 66579)!;
    expect(arc.arc).toEqual({ cx: 775, cy: 700, r: 162 });
    const toSocket = m.edges.find((e) => (e.a === 66576 && e.b === 7) || (e.a === 7 && e.b === 66576))!;
    expect(toSocket.arc).toBeNull();
    expect(m.edges.filter((e) => e.a === 66576 || e.b === 66576)).toHaveLength(2); // link list adds no duplicates
    expect(m.rings.some((r) => r.art === "GroupBackgroundMediumAlt")).toBe(true);
  });
});

describe("HitIndex", () => {
  const mk = (id: number, x: number, y: number, size = 40, kind: TreeNode["kind"] = "normal"): TreeNode => ({
    id,
    kind,
    raw: node({ id, x, y }),
    x,
    y,
    size,
    asc: null,
    linked: [],
    dynamic: false,
  });
  it("returns the nearest node within its hit radius and nothing outside", () => {
    const h = new HitIndex();
    h.add(mk(1, 0, 0));
    h.add(mk(2, 50, 0));
    h.add(mk(3, 5000, 5000, 40, "classStart"));
    expect(h.at(10, 5)?.id).toBe(1);
    expect(h.at(45, 0)?.id).toBe(2);
    expect(h.at(300, 300)).toBeNull();
    expect(h.at(5000, 5000)).toBeNull(); // class starts are not hit targets
  });
});

describe("buildModel (PoE2)", () => {
  const poe2: TreeData = {
    ...data,
    game: "poe2",
    treeVersion: "0_5",
    nodes: {
      ...data.nodes,
      "5": node({ id: 5, x: 150, y: 0, type: "OnlyImage", group: 7, size: 0 }),
    },
    nodeCount: 5,
    groups: [
      { id: 7, x: 150, y: 0, oo: [1], isProxy: false, ascStart: false },
      { id: 8, x: 500, y: 500, oo: [2], isProxy: false, ascStart: false, bg: { image: "PSGroupBackground2", half: false } },
    ],
    // POB's own arc centre (BuildArc's first vertex), not the group centre
    connectors: [
      { a: 1, b: 2 },
      { a: 2, b: 3, orbit: 1, cx: 150, cy: 40 },
    ],
  };
  const m = buildModel(poe2);
  it("uses the arc centre POB computed", () => {
    expect(m.poe2).toBe(true);
    expect(m.edges.find((e) => e.a === 2)!.arc).toEqual({ cx: 150, cy: 40, r: 82 });
  });
  it("draws group art only where the group names it", () => {
    expect(m.rings).toEqual([{ x: 500, y: 500, art: "PSGroupBackground2", mirrored: false }]);
  });
  it("keeps group centre images out of hit testing", () => {
    expect(m.nodes.get(5)!.kind).toBe("image");
    expect(m.hit.at(150, 0)).toBeNull();
  });
});
