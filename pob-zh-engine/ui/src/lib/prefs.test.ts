import { describe, expect, it } from "vitest";
import { normAccent, normFontFile, normTheme, onColor, ACCENTS } from "./prefs.svelte";

// Mirrors host/launcher_config.cpp NormalizeModern* (--launcher-config-selftest
// T17s-u): the page and the ini must agree on what is a valid value.
describe("appearance prefs", () => {
  it("unknown themes fall back to slate", () => {
    expect(normTheme("light")).toBe("light");
    expect(normTheme("parchment")).toBe("parchment");
    expect(normTheme("neon")).toBe("slate");
    expect(normTheme(undefined)).toBe("slate");
  });
  it("accent is #rrggbb lower-cased, anything else means the theme's own", () => {
    expect(normAccent("#5B9DFF")).toBe("#5b9dff");
    expect(normAccent("red")).toBe("");
    expect(normAccent("#12345")).toBe("");
    expect(normAccent(42)).toBe("");
  });
  it("font is a bare .ttf file name, never a path", () => {
    expect(normFontFile("NotoSansTC-Regular.ttf")).toBe("NotoSansTC-Regular.ttf");
    expect(normFontFile("FZ_ZY.TTF")).toBe("FZ_ZY.TTF");
    expect(normFontFile(String.raw`..\..\x.ttf`)).toBe("");
    expect(normFontFile("C:/Windows/Fonts/x.ttf")).toBe("");
    expect(normFontFile(".hidden.ttf")).toBe("");
    expect(normFontFile("a.otf")).toBe("");
  });
  it("text on an accent button stays readable", () => {
    expect(onColor("#d8aa4b")).toBe("#17120a");
    expect(onColor("#ffc83d")).toBe("#17120a");
    expect(onColor("#1a237e")).toBe("#ffffff");
    expect(onColor("#946510")).toBe("#ffffff");
    for (const c of ACCENTS) expect(["#17120a", "#ffffff"]).toContain(onColor(c));
  });
});
