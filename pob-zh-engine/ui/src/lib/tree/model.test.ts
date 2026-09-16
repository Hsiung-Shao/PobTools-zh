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
