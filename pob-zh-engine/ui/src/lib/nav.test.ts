import { describe, expect, it } from "vitest";
import { folderCrumbs, NAV_LIMIT, parentFolder, ViewHistory } from "./nav";

describe("view history (mouse back / forward)", () => {
  const all = () => true;
  it("back walks the screens in reverse, forward walks them again", () => {
    const h = new ViewHistory<string>();
    h.visit("builds", "tree");
    h.visit("tree", "items");
    expect(h.goBack("items", all)).toBe("tree");
    expect(h.goBack("tree", all)).toBe("builds");
    expect(h.goBack("builds", all)).toBeUndefined();
    expect(h.goForward("builds", all)).toBe("tree");
    expect(h.goForward("tree", all)).toBe("items");
    expect(h.goForward("items", all)).toBeUndefined();
  });
  it("a new move drops the forward trail", () => {
    const h = new ViewHistory<string>();
    h.visit("builds", "tree");
    h.goBack("tree", all);
    h.visit("builds", "skills");
    expect(h.goForward("skills", all)).toBeUndefined();
    expect(h.goBack("skills", all)).toBe("builds");
  });
  it("screens that cannot be shown now are skipped, the same screen twice is not a step", () => {
    const h = new ViewHistory<string>();
    h.visit("builds", "tree");
    h.visit("tree", "settings");
    expect(h.goBack("settings", (v) => v !== "tree")).toBe("builds");
    h.visit("x", "x");
    expect(h.back.length).toBe(0);
  });
  it("keeps at most NAV_LIMIT steps", () => {
    const h = new ViewHistory<number>();
    for (let i = 0; i < NAV_LIMIT + 10; i++) h.visit(i, i + 1);
    expect(h.back.length).toBe(NAV_LIMIT);
  });
});

describe("build list folders", () => {
  it("parent of a sub-folder, root stays root", () => {
    expect(parentFolder("a/b/")).toBe("a/");
    expect(parentFolder("a/")).toBe("");
    expect(parentFolder("")).toBe("");
  });
  it("breadcrumbs carry the path up to each folder", () => {
    expect(folderCrumbs("測試/子資料夾/")).toEqual([
      { name: "測試", path: "測試/" },
      { name: "子資料夾", path: "測試/子資料夾/" },
    ]);
    expect(folderCrumbs("")).toEqual([]);
  });
});
