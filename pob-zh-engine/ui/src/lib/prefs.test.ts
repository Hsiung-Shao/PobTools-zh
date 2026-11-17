import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { normAccent, normFontFile, normTheme, onColor, normBgFile, normLook, lookVars, LOOK_DEFAULT, isVideoFile, prefs } from "./prefs.svelte";
import { ACCENTS as ACCENTS1 } from "../theme1/accents";
import { ACCENTS } from "../theme2/accents";

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
    for (const c of [...ACCENTS1, ...ACCENTS]) expect(["#17120a", "#ffffff"]).toContain(onColor(c));
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

describe("ACCENTS", () => {
  it("強調色預設值是新的靛藍", () => {
    expect(ACCENTS[0]).toBe("#6366f1");
    for (const c of ACCENTS) expect(c).toMatch(/^#[0-9a-f]{6}$/);
  });
});

// Exercise the effective selection, not just a filename predicate: following the
// launcher and switching away must update the same root that owns the CSS tokens.
describe("Pico throughout the interface", () => {
  let root: { style: { setProperty: ReturnType<typeof vi.fn>; removeProperty: ReturnType<typeof vi.fn> }; dataset: Record<string, string> };
  const face = { textContent: "", id: "" };
  beforeEach(() => {
    root = { style: { setProperty: vi.fn(), removeProperty: vi.fn() }, dataset: {} };
    vi.stubGlobal("document", { documentElement: root, createElement: () => face, head: { appendChild: vi.fn() } });
    prefs.font = "";
    prefs.launcherFont = "NotoSansTC-Regular.ttf";
    prefs.apply();
  });
  afterEach(() => {
    prefs.font = "";
    prefs.launcherFont = "";
    prefs.apply();
    vi.unstubAllGlobals();
  });
  it("explicit Pico selection enables the shared normal and monospace family", async () => {
    await prefs.set({ font: "Pico-Regular.ttf" });
    expect(root.dataset.font).toBe("pico");
    expect(face.textContent).toContain("Pico-Regular.ttf");
  });
  it("following the launcher also enables Pico, case insensitively", async () => {
    prefs.launcherFont = "PICO-REGULAR.TTF";
    await prefs.set({ font: "" });
    expect(root.dataset.font).toBe("pico");
  });
  it("an explicit other font overrides launcher Pico and restores the usual mono stack", async () => {
    prefs.launcherFont = "Pico-Regular.ttf";
    prefs.apply();
    expect(root.dataset.font).toBe("pico");
    await prefs.set({ font: "NotoSansTC-Regular.ttf" });
    expect(root.dataset.font).toBe("");
    expect(face.textContent).toContain("NotoSansTC-Regular.ttf");
    await prefs.set({ font: "" });
    expect(root.dataset.font).toBe("pico");
    prefs.launcherFont = "NotoSansTC-Regular.ttf";
    prefs.apply();
    expect(root.dataset.font).toBe("");
  });
  it("reset follows the launcher rather than leaving a stale Pico override", async () => {
    await prefs.set({ font: "Pico-Regular.ttf" });
    await prefs.reset();
    expect(prefs.effectiveFont).toBe("NotoSansTC-Regular.ttf");
    expect(root.dataset.font).toBe("");
  });
});
