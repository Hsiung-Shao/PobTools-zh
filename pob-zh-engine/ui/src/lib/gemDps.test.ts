import { describe, expect, it } from "vitest";
import { DPS_DOWN, DPS_SAME, DPS_UP, dpsDeltaColor, dpsDeltaText, formatSignedNumber, formatSignedPct } from "./gemDps";

describe("gem DPS change", () => {
  it("uses POB's three colours", () => {
    expect(dpsDeltaColor(12)).toBe(DPS_UP);
    expect(dpsDeltaColor(-0.5)).toBe(DPS_DOWN);
    expect(dpsDeltaColor(0)).toBe(DPS_SAME);
    expect(dpsDeltaColor(undefined)).toBeUndefined();
    expect(dpsDeltaColor(Number.NaN)).toBeUndefined();
  });
  it("signs and groups the amount", () => {
    expect(formatSignedNumber(45678.4)).toBe("+45,678");
    expect(formatSignedNumber(-1234.6)).toBe("−1,235");
    expect(formatSignedNumber(0.2)).toBe("±0");
  });
  it("one decimal for the percentage", () => {
    expect(formatSignedPct(12.34)).toBe("+12.3%");
    expect(formatSignedPct(-12.36)).toBe("−12.4%");
    expect(formatSignedPct(5)).toBe("+5%");
    expect(formatSignedPct(0.01)).toBe("±0%");
  });
  it("prefers the percentage, falls back to the amount when the base was 0", () => {
    expect(dpsDeltaText({ delta: 500, pct: 25 })).toBe("+25%");
    expect(dpsDeltaText({ delta: 500 })).toBe("+500");
    expect(dpsDeltaText({})).toBe("");
  });
});
