// The window's own scale knobs: WebView2 zoom (the host applies it) and the
// page's base font size (the `--fs-base` token every size derives from).
// Persisted by the host in pob-zh.ini; in the mock they live in localStorage.
import { api, hostInfo, isHosted, type UiPrefs } from "./bridge";

export const ZOOM_MIN = 70;
export const ZOOM_MAX = 200;
export const ZOOM_STEP = 10;
export const FONT_MIN = 11;
export const FONT_MAX = 18;
export const DEFAULTS: UiPrefs = { zoom: 100, fontSize: 13 };

const clampZoom = (z: number) => Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, Math.round(z / ZOOM_STEP) * ZOOM_STEP));
const clampFont = (f: number) => Math.min(FONT_MAX, Math.max(FONT_MIN, Math.round(f)));

class UiPrefsState {
  zoom = $state(DEFAULTS.zoom);
  fontSize = $state(DEFAULTS.fontSize);
  open = $state(false);

  init() {
    let p: Partial<UiPrefs> | undefined = hostInfo.prefs;
    if (!p && !isHosted) {
      try {
        p = JSON.parse(localStorage.getItem("pobtools.prefs") ?? "null") ?? undefined;
      } catch {
        p = undefined;
      }
    }
    if (p?.zoom != null) this.zoom = clampZoom(p.zoom);
    if (p?.fontSize != null) this.fontSize = clampFont(p.fontSize);
    this.applyFont();
  }

  applyFont() {
    if (typeof document !== "undefined") document.documentElement.style.setProperty("--fs-base", `${this.fontSize}px`);
  }

  async set(p: Partial<UiPrefs>) {
    if (p.zoom != null) this.zoom = clampZoom(p.zoom);
    if (p.fontSize != null) this.fontSize = clampFont(p.fontSize);
    this.applyFont();
    const cur: UiPrefs = { zoom: this.zoom, fontSize: this.fontSize };
    if (isHosted) {
      try {
        await api.setPrefs(cur);
      } catch {
        /* an old host without prefs: the font still applies, only the zoom is lost */
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

export const prefs = new UiPrefsState();
