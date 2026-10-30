import { describe, expect, it } from "vitest";
import type { CalcCell, CalcRow, CalcSubsection, CalcWidget } from "./bridge";
import { subsectionSummary } from "./calcs-summary";

const cell = (ci: number, c: Partial<CalcCell> = {}): CalcCell => ({ ci, ...c });
const rowOf = (ri: number, ...cells: CalcCell[]): CalcRow => ({ ri, cells });
const sub = (...rows: CalcRow[]): CalcSubsection => ({ ui: 1, collapsed: true, rows });

describe("subsectionSummary 計算子區摘要", () => {
  it("第一格是 control 時,取同列第二個有文字的格(去色碼)", () => {
    const s = sub(rowOf(1, cell(1, { control: "dropdown", text: "不該選這格" }), cell(2, { text: "^x00FF00 1,234 ^7" })));
    expect(subsectionSummary(s)).toBe("1,234");
  });

  it("第一格是 widget 時略過", () => {
    const w = { kind: "check", shown: true, enabled: true } as CalcWidget;
    const s = sub(rowOf(1, cell(1, { widget: w, text: "勾選" }), cell(2, { text: "99" })));
    expect(subsectionSummary(s)).toBe("99");
  });

  it("依列順序,跳過沒有可用格的列", () => {
    const s = sub(rowOf(1, cell(1, { text: "   " }), cell(2, { text: "^7" })), rowOf(2, cell(1, { text: "42%" })), rowOf(3, cell(1, { text: "後面" })));
    expect(subsectionSummary(s)).toBe("42%");
  });

  it("沒有任何可用文字、或沒有列時回空字串", () => {
    expect(subsectionSummary(sub())).toBe("");
    expect(subsectionSummary(sub(rowOf(1, cell(1, {}), cell(2, { control: "edit", text: "x" }))))).toBe("");
  });
});
