import { describe, expect, it } from "vitest";
import { textToTooltipLines } from "./tooltip-lines";

describe("textToTooltipLines", () => {
  it("行首 tab 轉成縮排層級並從文字中移除", () => {
    const lines = textToTooltipLines("控制如何計算:\n\t平均: 依平均\n\t\t僅暴擊");
    expect(lines).toHaveLength(3);
    expect(lines[0].indent ?? 0).toBe(0);
    expect(lines[1].indent).toBe(1);
    expect(lines[1].text).toBe("平均: 依平均");
    expect(lines[2].indent).toBe(2);
    for (const l of lines) {
      expect(l.text).not.toContain("\t");
      expect(l.raw).not.toContain("\t");
      expect(l.size).toBe(14);
      expect(l.center).toBe(false);
    }
  });
});
