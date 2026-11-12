import { describe, expect, it } from "vitest";
import { fitTransform } from "./minimap";

describe("fitTransform", () => {
  it("所有點等比例落在畫布留邊內且置中", () => {
    const pts = [
      { x: -1000, y: -500 },
      { x: 3000, y: 500 },
    ];
    const f = fitTransform(pts, 200, 200, 10);
    const sx = pts.map((p) => p.x * f.s + f.ox);
    const sy = pts.map((p) => p.y * f.s + f.oy);
    expect(sx[0]).toBeCloseTo(10);
    expect(sx[1]).toBeCloseTo(190);
    expect((sy[0] + sy[1]) / 2).toBeCloseTo(100);
  });
  it("沒有點時不產生 NaN", () => {
    const f = fitTransform([], 200, 100);
    expect(Number.isFinite(f.s + f.ox + f.oy)).toBe(true);
  });
});
