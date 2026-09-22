// The window's own look: WebView2 zoom (the host applies it), the page's base
// font size (the `--fs-base` token every size derives from), and the settings
// page's "Appearance" card -- theme, accent colour and font.
// Persisted by the host in pob-zh.ini; in the mock they live in localStorage.
import { api, hostInfo, hostUrl, isHosted, type BgLook, type UiPrefs } from "./bridge";

export const ZOOM_MIN = 70;
export const ZOOM_MAX = 200;
export const ZOOM_STEP = 10;
export const FONT_MIN = 11;
export const FONT_MAX = 18;

export const THEMES = ["slate", "light", "contrast", "parchment"] as const;
export type Theme = (typeof THEMES)[number];
/** The accent presets the settings page offers ("" = the theme's own). */
export const ACCENTS = ["#d8aa4b", "#5b9dff", "#4fbf7a", "#a77bff", "#e0645a", "#3fc1c9"] as const;

export const DEFAULTS = { zoom: 100, fontSize: 13, theme: "slate" as Theme, accent: "", font: "" };
/** The launcher's own defaults (launcher_config.h AppearanceConfig): no image, 50 % bright, solid panels. */
export const LOOK_DEFAULT: BgLook = { follow: true, background: "", bgBright: 50, panelOpacity: 100, glassBlur: 0, treeBg: 100 };

const pct = (v: unknown, def: number) => (typeof v === "number" && v >= 0 && v <= 100 ? Math.round(v) : def);
/** Same rule as the host (NormalizeBackgroundFile). */
export function normBgFile(v: unknown): string {
  return typeof v === "string" && /^[^\\/:*?"<>|.][^\\/:*?"<>|]{0,199}\.(png|jpe?g|webp)$/i.test(v) ? v : "";
}
export function normLook(v: Partial<BgLook> | undefined, follow = true): BgLook {
  return {
    follow: v?.follow ?? follow,
    background: normBgFile(v?.background),
    bgBright: pct(v?.bgBright, LOOK_DEFAULT.bgBright),
    panelOpacity: pct(v?.panelOpacity, LOOK_DEFAULT.panelOpacity),
    glassBlur: pct(v?.glassBlur, LOOK_DEFAULT.glassBlur),
    treeBg: pct(v?.treeBg, LOOK_DEFAULT.treeBg),
  };
}
/** The CSS the background set turns into (App's .bgimg layer, app.css, the tree canvas). */
export function lookVars(l: BgLook, url: (file: string) => string): Record<string, string> | null {
  if (!l.background) return null;
  return {
    "--bg-image": `url("${url(l.background)}")`,
    "--bg-bright": String(l.bgBright / 100),
    "--bg-blur": `${Math.round((l.glassBlur / 100) * 24)}px`,
    "--panel-pct": `${l.panelOpacity}%`,
    "--tree-alpha": String(l.treeBg / 100),
  };
}
const LOOK_VARS = ["--bg-image", "--bg-bright", "--bg-blur", "--panel-pct", "--tree-alpha"];


const clampZoom = (z: number) => Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, Math.round(z / ZOOM_STEP) * ZOOM_STEP));
const clampFont = (f: number) => Math.min(FONT_MAX, Math.max(FONT_MIN, Math.round(f)));

/** Same rules as the host (launcher_config NormalizeModern*): unknown = default. */
export function normTheme(v: unknown): Theme {
  return THEMES.includes(v as Theme) ? (v as Theme) : "slate";
}
export function normAccent(v: unknown): string {
  return typeof v === "string" && /^#[0-9a-fA-F]{6}$/.test(v) ? v.toLowerCase() : "";
}
export function normFontFile(v: unknown): string {
  return typeof v === "string" && /^[^\\/:*?"<>|.][^\\/:*?"<>|]{0,115}\.ttf$/i.test(v) ? v : "";
}

/** Text colour that reads on a button filled with `hex` (WCAG relative luminance). */
export function onColor(hex: string): string {
  const n = parseInt(hex.slice(1), 16);
  const lin = (c: number) => {
    const s = c / 255;
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  const l = 0.2126 * lin((n >> 16) & 255) + 0.7152 * lin((n >> 8) & 255) + 0.0722 * lin(n & 255);
  // contrast against black (l+.05)/.05 vs white 1.05/(l+.05): black wins above ~0.18
  return l > 0.18 ? "#17120a" : "#ffffff";
}

class UiPrefsState {
  zoom = $state(DEFAULTS.zoom);
  fontSize = $state(DEFAULTS.fontSize);
  theme = $state<Theme>(DEFAULTS.theme);
  accent = $state(DEFAULTS.accent);
  font = $state(DEFAULTS.font);
  /** What the host offers: Fonts\*.ttf, and the launcher's own choice ("follow the launcher"). */
  fonts = $state<string[]>([]);
  launcherFont = $state("");
  /** Background: the page's own set, the launcher's set it follows by default, the images on disk. */
  look = $state<BgLook>({ ...LOOK_DEFAULT });
  launcherLook = $state<BgLook>({ ...LOOK_DEFAULT });
  backgrounds = $state<string[]>([]);
  /** Bumped whenever colours change; the tree canvas re-reads its palette on it. */
  rev = $state(0);

  /** The file the page actually draws with. */
  get effectiveFont(): string {
    return this.font || this.launcherFont;
  }
  /** What is on screen: the launcher's set while following, the page's own otherwise. */
  get effectiveLook(): BgLook {
    return this.look.follow ? this.launcherLook : this.look;
  }

  init() {
    let p: Partial<UiPrefs> | undefined = hostInfo.prefs;
    if (!p && !isHosted) {
      try {
        p = JSON.parse(localStorage.getItem("pobtools.prefs") ?? "null") ?? undefined;
      } catch {
        p = undefined;
      }
    }
    this.take(p);
    this.apply();
    // The launcher may have switched its font since this window opened.
    if (isHosted)
      api
        .getPrefs()
        .then((r) => {
          if (r) {
            this.take({ fonts: r.fonts, launcherFont: r.launcherFont, launcherLook: r.launcherLook, backgrounds: r.backgrounds });
            this.apply();
          }
        })
        .catch(() => {});
  }

  private take(p: Partial<UiPrefs> | undefined) {
    if (!p) return;
    if (p.zoom != null) this.zoom = clampZoom(p.zoom);
    if (p.fontSize != null) this.fontSize = clampFont(p.fontSize);
    if (p.theme !== undefined) this.theme = normTheme(p.theme);
    if (p.accent !== undefined) this.accent = normAccent(p.accent);
    if (p.font !== undefined) this.font = normFontFile(p.font);
    if (Array.isArray(p.fonts)) this.fonts = p.fonts.filter((f) => normFontFile(f));
    if (p.launcherFont !== undefined) this.launcherFont = normFontFile(p.launcherFont);
    if (p.look) this.look = normLook(p.look);
    if (p.launcherLook) this.launcherLook = normLook(p.launcherLook);
    if (Array.isArray(p.backgrounds)) this.backgrounds = p.backgrounds.filter((f) => normBgFile(f));
  }

  apply() {
    if (typeof document === "undefined") return;
    const root = document.documentElement;
    root.style.setProperty("--fs-base", `${this.fontSize}px`);
    root.dataset.theme = this.theme;
    if (this.accent) {
      root.style.setProperty("--accent-user", this.accent);
      root.style.setProperty("--on-accent-user", onColor(this.accent));
    } else {
      root.style.removeProperty("--accent-user");
      root.style.removeProperty("--on-accent-user");
    }
    applyFontFace(this.effectiveFont);
    const vars = lookVars(this.effectiveLook, (f) => hostUrl("bg", encodeURIComponent(f)));
    for (const k of LOOK_VARS) root.style.removeProperty(k);
    if (vars) {
      for (const [k, v] of Object.entries(vars)) root.style.setProperty(k, v);
      root.dataset.bg = "1";
    } else delete root.dataset.bg;
    this.rev++;
  }

  async set(p: Partial<Pick<UiPrefs, "zoom" | "fontSize" | "theme" | "accent" | "font" | "look">>) {
    this.take(p);
    this.apply();
    const cur = { zoom: this.zoom, fontSize: this.fontSize, theme: this.theme, accent: this.accent, font: this.font, look: this.look };
    if (isHosted) {
      try {
        const r = await api.setPrefs(cur);
        // the host drops a font file it does not have; follow what it kept
        if (r && r.font !== undefined && normFontFile(r.font) !== this.font) {
          this.take({ font: r.font });
          this.apply();
        }
      } catch {
        /* an old host without prefs: the look still applies, only persistence is lost */
      }
    } else {
      try {
        localStorage.setItem("pobtools.prefs", JSON.stringify(cur));
      } catch {
        /* private mode etc. */
      }
    }
  }
  /** A change on the settings page's Background card: from then on the page keeps its own set. */
  setLook(change: Partial<BgLook>) {
    return this.set({ look: { ...this.effectiveLook, ...change, follow: false } });
  }
  followLauncher() {
    return this.set({ look: { ...this.look, follow: true } });
  }
  /** Re-list PobTools\Backgrounds\ (after the user dropped a picture in). */
  async refreshBackgrounds() {
    if (!isHosted) return;
    const r = await api.getPrefs().catch(() => null);
    if (r) {
      this.take({ backgrounds: r.backgrounds, launcherLook: r.launcherLook });
      this.apply();
    }
  }
  zoomBy(steps: number) {
    return this.set({ zoom: this.zoom + steps * ZOOM_STEP });
  }
  reset() {
    return this.set({ ...DEFAULTS });
  }
}

/**
 * The chosen font as the "PobTools UI" family (first in --font-ui). One
 * <style> element, rewritten on change; no file = no rule, and the stack falls
 * through to Noto Sans TC. Korean always has Noto Sans KR behind it, so a font
 * without Hangul (FZ_ZY) still draws a Korean interface.
 */
let faceEl: HTMLStyleElement | null = null;
let faceFor = "\u0000";
function applyFontFace(file: string) {
  if (file === faceFor) return;
  faceFor = file;
  if (!faceEl) {
    faceEl = document.createElement("style");
    faceEl.id = "pobtools-ui-font";
    document.head.appendChild(faceEl);
  }
  faceEl.textContent = file
    ? `@font-face { font-family: "PobTools UI"; src: url("${hostUrl("fonts", encodeURIComponent(file))}") format("truetype"); font-display: swap; }`
    : "";
}

export const prefs = new UiPrefsState();
