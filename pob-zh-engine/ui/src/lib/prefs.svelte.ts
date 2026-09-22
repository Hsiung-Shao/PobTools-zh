// The window's own look: WebView2 zoom (the host applies it), the page's base
// font size (the `--fs-base` token every size derives from), and the settings
// page's "Appearance" card -- theme, accent colour and font.
// Persisted by the host in pob-zh.ini; in the mock they live in localStorage.
import { api, hostInfo, hostUrl, isHosted, type UiPrefs } from "./bridge";

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
  /** Bumped whenever colours change; the tree canvas re-reads its palette on it. */
  rev = $state(0);

  /** The file the page actually draws with. */
  get effectiveFont(): string {
    return this.font || this.launcherFont;
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
            this.take({ fonts: r.fonts, launcherFont: r.launcherFont });
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
    this.rev++;
  }

  async set(p: Partial<Pick<UiPrefs, "zoom" | "fontSize" | "theme" | "accent" | "font">>) {
    this.take(p);
    this.apply();
    const cur = { zoom: this.zoom, fontSize: this.fontSize, theme: this.theme, accent: this.accent, font: this.font };
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
