import { describe, expect, it } from "vitest";
import { FLOAT_MARGIN, placeAxis, placeFloat, pointAnchor } from "./floatFit";

describe("floating panel placement", () => {
  const view = { w: 1000, h: 700 };

  it("goes right of and below the cursor when it fits there", () => {
    expect(placeFloat(pointAnchor(100, 100), { w: 400, h: 300 }, view)).toEqual({ x: 116, y: 114 });
  });

  it("flips to the left / above when the right / bottom side is too small", () => {
    // right: 916 + 400 > 990 → left: 900 - 16 - 400; below: 614 + 300 > 690 → above: 600 - 14 - 300
    expect(placeFloat(pointAnchor(900, 600), { w: 400, h: 300 }, view)).toEqual({ x: 484, y: 286 });
  });

  it("is pushed inside the window when neither side fits", () => {
    // 800 wide from a cursor in the middle: neither 516+800 nor 484-800 fits
    expect(placeFloat(pointAnchor(500, 350), { w: 800, h: 600 }, view)).toEqual({
      x: view.w - FLOAT_MARGIN - 800,
      y: view.h - FLOAT_MARGIN - 600,
    });
  });

  it("starts at the margin when the panel is as large as the window", () => {
    const big = { w: view.w - 2 * FLOAT_MARGIN, h: view.h - 2 * FLOAT_MARGIN };
    expect(placeFloat(pointAnchor(500, 350), big, view)).toEqual({ x: FLOAT_MARGIN, y: FLOAT_MARGIN });
    expect(placeFloat(pointAnchor(500, 350), { w: 5000, h: 5000 }, view)).toEqual({ x: FLOAT_MARGIN, y: FLOAT_MARGIN });
  });

  it("never starts before the margin, even with an anchor off-screen", () => {
    // the sidebar anchors its panel 36px above the cursor; near the top that is outside
    expect(placeAxis({ after: -20, before: 50 }, 300, 700)).toBe(FLOAT_MARGIN);
    // a pinned panel whose cursor point left the window after a resize
    expect(placeFloat(pointAnchor(1400, 900), { w: 300, h: 200 }, view)).toEqual({ x: 690, y: 490 });
  });

  it("keeps the panel fully inside for any anchor", () => {
    for (const ax of [-50, 0, 10, 300, 600, 990, 1200]) {
      for (const w of [50, 400, 980]) {
        const x = placeAxis({ after: ax + 16, before: ax - 16 }, w, view.w);
        expect(x).toBeGreaterThanOrEqual(FLOAT_MARGIN);
        expect(x + w).toBeLessThanOrEqual(view.w - FLOAT_MARGIN);
      }
    }
  });
});
