import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { SidebarRow } from "./bridge";
import type { SidebarItem } from "./sidebar-groups";
import { loadCollapsed, saveCollapsed, sidebarSummary } from "./sidebar-collapse";

const KEY = "pobtools.sidebar.collapsedGroups";

const row = (r: Partial<SidebarRow>): SidebarRow => ({ h: 16, hasBreakdown: false, ...r });
const pair = (lhs: string, rhs: string): SidebarRow => row({ lhs, rhs });
const items = (rows: SidebarRow[]): SidebarItem[] => rows.map((r, index) => ({ row: r, index }));

// node 環境沒有 localStorage,只 mock 這個外部 I/O
function stubStorage(init: Record<string, string> = {}) {
  const store = new Map<string, string>(Object.entries(init));
  vi.stubGlobal("localStorage", {
    getItem: (k: string) => (store.has(k) ? store.get(k)! : null),
    setItem: (k: string, v: string) => void store.set(k, String(v)),
    removeItem: (k: string) => void store.delete(k),
  });
  return store;
}

afterEach(() => vi.unstubAllGlobals());

describe("sidebarSummary 收合摘要", () => {
  it("組內 5 列 pair 時只取前三個 rhs,以「 · 」連接", () => {
    const s = sidebarSummary(items([pair("A", "1"), pair("B", "2"), pair("C", "3"), pair("D", "4"), pair("E", "5")]));
    expect(s).toBe("1 · 2 · 3");
  });

  it("略過標題、空列與置中訊息,只含 pair 列的值", () => {
    const s = sidebarSummary(
      items([
        row({ lhs: "標題" }), // heading:只有 lhs
        row({}), // gap
        row({ rhs: "孤兒" }), // 只有 rhs,不算 pair
        row({ lhs: "訊息", rhs: "不要出現", align: "CENTER_X" }), // note
        pair("火", "75%"),
        row({}),
        pair("冰", "40%"),
      ]),
    );
    expect(s).toBe("75% · 40%");
  });

  it("去掉 POB 色碼(^xRRGGBB 與 ^0–^9)並 trim", () => {
    const s = sidebarSummary(items([pair("A", "^xFF7C47 1,234 ^7"), pair("B", "^8 50%"), pair("C", "^0x")]));
    expect(s).not.toMatch(/\^/);
    expect(s).toBe("1,234 · 50% · x");
  });

  it("max 參數可調整取用數量", () => {
    const list = items([pair("A", "1"), pair("B", "2"), pair("C", "3")]);
    expect(sidebarSummary(list, 2)).toBe("1 · 2");
    expect(sidebarSummary(list, 10)).toBe("1 · 2 · 3");
  });

  it("沒有任何 pair 列時回空字串", () => {
    expect(sidebarSummary([])).toBe("");
    expect(sidebarSummary(items([row({ lhs: "標題" }), row({})]))).toBe("");
  });
});

describe("收合狀態存取", () => {
  beforeEach(() => stubStorage());

  it("存了之後讀得回來", () => {
    saveCollapsed(new Set(["resistances", "offence"]));
    const got = loadCollapsed();
    expect(got).toBeInstanceOf(Set);
    expect(got.has("resistances")).toBe(true);
    expect(got.has("offence")).toBe(true);
    expect(got.size).toBe(2);
  });

  it("寫進固定鍵,內容是 JSON 陣列", () => {
    const store = stubStorage();
    saveCollapsed(new Set(["resistances"]));
    expect(JSON.parse(store.get(KEY)!)).toEqual(["resistances"]);
  });

  it("沒存過時回空集合", () => {
    expect(loadCollapsed().size).toBe(0);
  });

  it("存檔不是 JSON 時回空集合、不丟錯", () => {
    stubStorage({ [KEY]: "not json" });
    expect(() => loadCollapsed()).not.toThrow();
    expect(loadCollapsed().size).toBe(0);
  });

  it("存檔是 JSON 但不是陣列時回空集合", () => {
    stubStorage({ [KEY]: '{"a":1}' });
    expect(loadCollapsed().size).toBe(0);
  });

  it("localStorage 讀寫丟例外時不丟錯,讀取回空集合", () => {
    vi.stubGlobal("localStorage", {
      getItem: () => {
        throw new Error("denied");
      },
      setItem: () => {
        throw new Error("denied");
      },
    });
    expect(() => saveCollapsed(new Set(["x"]))).not.toThrow();
    expect(loadCollapsed().size).toBe(0);
  });
});
