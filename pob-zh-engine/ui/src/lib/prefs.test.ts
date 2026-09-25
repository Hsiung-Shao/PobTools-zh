import { describe, expect, it } from "vitest";
import { normAccent, normFontFile, normTheme, onColor, ACCENTS, normBgFile, normLook, lookVars, LOOK_DEFAULT, isVideoFile } from "./prefs.svelte";

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
  it("background file is a bare picture name (host NormalizeBackgroundFile)", () => {
    expect(normBgFile("ynQsfhbtoegqAym.webp")).toBe("ynQsfhbtoegqAym.webp");
    expect(normBgFile("背景.JPEG")).toBe("背景.JPEG");
    expect(normBgFile("a.gif")).toBe("");
    expect(normBgFile(String.raw`..\x.png`)).toBe("");
    expect(normBgFile("C:/x.png")).toBe("");
  });
  it("a garbled look falls back to the launcher's defaults, 0 is kept", () => {
    const l = normLook({ follow: false, background: "x.png", bgBright: 250, panelOpacity: 0, glassBlur: -1, treeBg: 40 });
    expect(l).toEqual({ follow: false, background: "x.png", bgBright: 50, panelOpacity: 0, glassBlur: 0, treeBg: 40, bgScope: 0 });
    expect(normLook({ bgScope: 1 }).bgScope).toBe(1);
    expect(normLook({ bgScope: 7 }).bgScope).toBe(0);
    expect(normLook(undefined)).toEqual(LOOK_DEFAULT);
  });
  it("no image: nothing while panels and tree are solid, the plain backdrop once they are not", () => {
    expect(lookVars({ ...LOOK_DEFAULT, glassBlur: 80, bgBright: 10 }, (f) => f)).toBeNull();
    const n = lookVars({ ...LOOK_DEFAULT, panelOpacity: 20, glassBlur: 80 }, (f) => f)!;
    expect(n["--bg-image"]).toBe("none");
    expect(n["--panel-pct"]).toBe("20%");
    expect(n["--bg-blur"]).toBe("0px");
    expect(lookVars({ ...LOOK_DEFAULT, treeBg: 40 }, (f) => f)!["--tree-alpha"]).toBe("0.4");
  });
  it("with an image the percents become CSS values", () => {
    const v = lookVars({ follow: false, background: "a b.png", bgBright: 30, panelOpacity: 40, glassBlur: 50, treeBg: 0 }, (f) => `https://bg.pobtools/${encodeURIComponent(f)}`)!;
    expect(v["--bg-image"]).toBe('url("https://bg.pobtools/a%20b.png")');
    expect(v["--bg-bright"]).toBe("0.3");
    expect(v["--panel-pct"]).toBe("40%");
    expect(v["--bg-blur"]).toBe("12px");
    expect(v["--tree-alpha"]).toBe("0");
  });
  it("a video is its own element: no CSS image; mp4/webm pass, other clips do not (host T17z)", () => {
    expect(normBgFile("loop.MP4")).toBe("loop.MP4");
    expect(normBgFile("a.webm")).toBe("a.webm");
    expect(normBgFile("a.mov")).toBe("");
    expect(isVideoFile("a.webm")).toBe(true);
    expect(isVideoFile("a.png")).toBe(false);
    const v = lookVars({ ...LOOK_DEFAULT, background: "loop.mp4" }, (f) => f)!;
    expect(v["--bg-image"]).toBe("none");
  });
  it("only behind the tree: the panels stay solid whatever their opacity says", () => {
    const v = lookVars({ ...LOOK_DEFAULT, background: "a.png", panelOpacity: 30, bgScope: 1, treeBg: 50 }, (f) => f)!;
    expect(v["--panel-pct"]).toBe("100%");
    expect(v["--tree-alpha"]).toBe("0.5");
  });
});
