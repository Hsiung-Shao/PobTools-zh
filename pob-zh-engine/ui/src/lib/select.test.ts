import { describe, expect, it } from "vitest";
import { filterOptions, moveIndex, placeList, typeahead, type SelOption } from "./select";

const opts = (labels: string[], disabled: number[] = []): SelOption[] =>
  labels.map((label, i) => ({ value: i, label, disabled: disabled.includes(i) || undefined }));

describe("placeList 下拉清單開啟方向", () => {
  it("下方空間足夠時往下開", () => {
    expect(placeList({ below: 400, above: 100, listH: 200 })).toEqual({ dir: "down", maxH: 200 });
  });

  it("下方不足上方足夠時往上開", () => {
    // below 120 - 8 = 112 < 300;above 500 - 8 = 492 ≥ 300
    expect(placeList({ below: 120, above: 500, listH: 300 })).toEqual({ dir: "up", maxH: 300 });
  });

  it("上下都不足時取較大側並限高", () => {
    // 兩側都 < 600;下方 200 較大 → maxH = 200 - 8
    expect(placeList({ below: 200, above: 150, listH: 600, margin: 8 })).toEqual({ dir: "down", maxH: 192 });
    // 反向:上方較大時往上,確認不是寫死 down
    expect(placeList({ below: 150, above: 200, listH: 600, margin: 8 })).toEqual({ dir: "up", maxH: 192 });
  });
});

describe("moveIndex 鍵盤移動", () => {
  it("鍵盤下移跳過停用選項", () => {
    const o = opts(["A", "B", "C"], [1]);
    expect(moveIndex(o, 0, 1)).toBe(2);
    // 上移同樣跳過
    expect(moveIndex(o, 2, -1)).toBe(0);
  });

  it("首尾不循環", () => {
    const o = opts(["A", "B", "C"]);
    expect(moveIndex(o, 2, 1)).toBe(2);
    expect(moveIndex(o, 0, -1)).toBe(0);
  });
});

describe("typeahead 首字跳選", () => {
  it("首字跳選找下一個符合項", () => {
    const o = opts(["Mace", "Axe", "Maul", "Sword"]);
    expect(typeahead(o, 0, "m")).toBe(2);
    expect(typeahead(o, 2, "m")).toBe(0);
  });
});

describe("filterOptions 搜尋過濾", () => {
  it("搜尋過濾忽略色碼且中英皆可", () => {
    const o = opts(["^x88FF88扭曲項鍊 Twisted Amulet", "^7Gold Amulet 黃金護身符", "Ruby Ring"]);
    const labels = (q: string) => filterOptions(o, q).map((x) => x.label);
    expect(labels("扭曲")).toEqual([o[0].label]);
    expect(labels("twisted")).toEqual([o[0].label]);
    expect(labels("amulet")).toEqual([o[0].label, o[1].label]);
    // 色碼本身不可被當成內容比對到
    expect(labels("x88")).toEqual([]);
    expect(labels("")).toEqual(o.map((x) => x.label));
  });
  it('搜尋可比對選項的英文別名', () => {
    const o: SelOption[] = [
      { value: 1, label: "增加 10% 生命", search: "10% increased Life" },
      { value: 2, label: "增加 10% 魔力", search: "^x8888FF10% increased Mana" },
    ];
    expect(filterOptions(o, "increased life").map((x) => x.value)).toEqual([1]);
    expect(filterOptions(o, "INCREASED MANA").map((x) => x.value)).toEqual([2]);
    expect(filterOptions(o, "x8888").map((x) => x.value)).toEqual([]);
  });
});
