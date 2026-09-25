// App-wide state: engine status, the loaded build, sidebar, view.
// Svelte 5 runes in a module so every component reads the same object.
import { api, bridge, hostInfo, type BuildHeader, type BuildInfo, type GateResult, type Sidebar, type VersionInfo } from "./bridge";
import { setEnglish, t } from "./i18n";
import { parentFolder, ViewHistory } from "./nav";

export type ViewId = "builds" | "tree" | "items" | "skills" | "config" | "calcs" | "notes" | "party" | "import" | "compare" | "settings";

class AppState {
  engine = $state<"booting" | "ready" | "gone">("booting");
  gate = $state<GateResult | null>(null);
  version = $state<VersionInfo | null>(null);
  childExit = $state<number | null>(null);
  /**
   * Browser mode only: the page's line to the PobTools process. "lost" = no
   * connection for a few seconds (it ended or crashed; it may come back),
   * "closed" = it was ended (the page's End button or the launcher).
   */
  link = $state<"ok" | "lost" | "closed">("ok");
  error = $state<string | null>(null);
  notice = $state<string | null>(null);
  busy = $state(0);

  #view = $state<ViewId>("builds");
  /** Every tab switch goes through here, so the back / forward history sees it. */
  get view(): ViewId {
    return this.#view;
  }
  set view(v: ViewId) {
    this.history.visit(this.#view, v);
    this.#view = v;
  }
  /** The mouse's back / forward buttons (App.svelte); see lib/nav. */
  readonly history = new ViewHistory<ViewId>();
  /**
   * The build list's folder ("" = POB's build folder, else "a/b/"). Lives here,
   * not in BuildList, so coming back to the list lands where it was left.
   */
  buildsSubPath = $state("");
  /** A screen that can be shown right now: the build tabs need a loaded build. */
  private canShow = (v: ViewId) => v === "builds" || v === "settings" || this.loaded;
  /** Back button: up one folder in the build list first, then the previous screen. */
  goBack(): boolean {
    if (this.#view === "builds" && this.buildsSubPath) {
      this.buildsSubPath = parentFolder(this.buildsSubPath);
      return true;
    }
    const v = this.history.goBack(this.#view, this.canShow);
    if (v === undefined) return false;
    this.#view = v;
    return true;
  }
  goForward(): boolean {
    const v = this.history.goForward(this.#view, this.canShow);
    if (v === undefined) return false;
    this.#view = v;
    return true;
  }
  info = $state<BuildInfo | null>(null);
  sidebar = $state<Sidebar | null>(null);
  header = $state<BuildHeader | null>(null);
  /** "HH:MM" of the last successful save in this session. */
  savedAt = $state<string | null>(null);
  /** POB's outputRevision after the last refresh; everything cached is keyed on it. */
  rev = $state(0);
  /** F2: showing POB's original English instead of the translation. */
  english = $state(false);
  /**
   * Bumped by the F2 toggle. App keys the whole page on it, which is what makes
   * every t() call re-evaluate (t reads a plain table, not a rune) and every
   * view re-fetch (POB's outputRevision does NOT move when only the display
   * language changed, so `rev` alone would leave the old strings on screen).
   */
  langRev = $state(0);
  loaded = $derived(this.info !== null);
  /** PoE2's Path of Building (bridge version.game; the host's game before the engine answers). */
  poe2 = $derived((this.version?.game ?? hostInfo.game) === "poe2");
  /** A capability POB lacks is false; unknown (older bridge) counts as present. */
  has(cap: keyof NonNullable<VersionInfo["caps"]>): boolean {
    return this.version?.caps?.[cap] !== false;
  }

  /** Runs one engine call with the busy counter and error surfacing. */
  async run<T>(fn: () => Promise<T>): Promise<T | undefined> {
    this.busy++;
    try {
      const r = await fn();
      this.error = null;
      return r;
    } catch (e: any) {
      this.error = `${e?.code ?? "error"}: ${e?.message ?? e}`;
      return undefined;
    } finally {
      this.busy--;
    }
  }

  /** Re-reads what depends on the build's calculation state. Returns true. */
  async refresh(): Promise<boolean> {
    const [info, side, header] = await Promise.all([api.getBuildInfo(), api.getSidebar(), api.getBuildHeader()]);
    this.info = info;
    this.sidebar = side;
    this.header = header;
    this.rev = side.rev;
    return true;
  }

  /**
   * F2, the new interface's half of it: flip the engine's display-translation
   * switch (the same one the classic window's F2 flips), swap our own strings,
   * then re-read. POB's text arrives already translated -- there is no English
   * copy of a sidebar breakdown line -- so the only way back to English is to
   * ask the engine again with the switch off.
   */
  async toggleEnglish(): Promise<void> {
    if (!this.has("translateToggle")) return;
    const want = !this.english;
    const r = await this.run(() => api.setTranslate({ enabled: !want }));
    if (!r) return;
    this.english = r.enabled === false;
    setEnglish(this.english);
    this.langRev++;
    if (this.version) this.version = { ...this.version, translate: r.enabled };
    if (this.loaded) await this.run(() => this.refresh());
    this.notice = this.english ? t("app.englishOn") : t("app.englishOff");
  }

  /** Build:Init ran again (import): every cached view is stale, like a fresh load. */
  async afterReinit() {
    this.rev = 0;
    await this.run(() => this.refresh());
  }

  private stamp() {
    const d = new Date();
    this.savedAt = `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
  }

  async save() {
    const r = await this.run(() => api.saveBuild());
    if (r) {
      this.stamp();
      await this.refresh();
    }
  }

  /**
   * Saves under POB's build folder: `name` may carry a sub-folder ("dir/name").
   * "exists" = another build already has that file and nothing was written; the
   * caller asks, then calls again with `overwrite`.
   */
  async saveAs(name: string, overwrite = false): Promise<"saved" | "exists" | "failed"> {
    const buildPath = this.buildPath;
    if (!buildPath) return "failed";
    const rel = name.replace(/\\/g, "/").replace(/^\/+/, "");
    const path = `${buildPath}${rel}${rel.toLowerCase().endsWith(".xml") ? "" : ".xml"}`;
    const r = await this.run(() => api.saveBuildAs(path, overwrite));
    if (!r) return "failed";
    if (r.exists) return "exists";
    this.stamp();
    await this.refresh();
    const after = this.afterSaveAs;
    this.afterSaveAs = null;
    after?.();
    return "saved";
  }

  /** The window is being closed with unsaved changes: App asks Save / Don't save / Cancel. */
  closeAsk = $state(false);
  /** A tree jewel socket to bring into view on the Items tab (right-click on the tree). */
  focusSocketNode = $state<number | null>(null);
  /** Bumped to ask BuildBar for its Save As dialog (a build that has no file yet). */
  saveAsRequest = $state(0);
  /** Runs once after the next successful Save As (closing the window after "Save"). */
  afterSaveAs: (() => void) | null = null;
  /**
   * Ctrl+S and every "save first?" answer. A build with no file yet (new, or
   * imported as a new build) has nothing to save to: open Save As instead of
   * letting save_build error. True when the build is saved by the time it returns.
   */
  async saveOrAsk(then?: () => void): Promise<boolean> {
    if (!this.header?.dbFileName) {
      if (this.view === "builds" || this.view === "settings") this.view = this.landingView();
      this.afterSaveAs = then ?? null;
      this.saveAsRequest++;
      return false;
    }
    await this.save();
    const ok = !this.info?.unsaved;
    if (ok) then?.();
    return ok;
  }

  /** POB's build folder with a trailing separator (from version / list_builds). */
  buildPath = $state<string>("");

  /**
   * The tree changed in the engine (a click, an undo): POB recalculated in
   * the same call, so pull the sidebar and header again. The tree view keys
   * its own refresh on `rev`.
   */
  async afterTreeChange() {
    await this.refresh();
  }

  /** The tab a freshly loaded build opens on: POB_ZH_UI_VIEW (developer knob) or the tree. */
  landingView(): ViewId {
    const v = hostInfo.view as ViewId | undefined;
    const ids: ViewId[] = ["tree", "items", "skills", "config", "calcs", "notes", "party", "import", "settings"];
    return v && ids.includes(v) ? v : "tree";
  }

  async loadBuild(path: string) {
    if (this.info?.unsaved && !confirm(t("builds.unsavedOpen"))) return;
    const ok = await this.run(async () => {
      await api.loadBuildFile(path);
      return this.refresh();
    });
    if (ok) this.view = this.landingView();
  }

  /** POB's "New" button: an unnamed, unsaved build (Ctrl+S then asks where). */
  async newBuild(name?: string, subPath?: string) {
    if (this.info?.unsaved && !confirm(t("builds.unsavedPrompt"))) return;
    const ok = await this.run(async () => {
      await api.newBuild(name, subPath);
      this.rev = 0;
      return this.refresh();
    });
    if (ok) this.view = "tree";
  }

  reset() {
    this.info = null;
    this.sidebar = null;
    this.header = null;
    this.rev = 0;
    this.#view = "builds";
    this.history.clear();
  }
}

export const app = new AppState();

// Engine events. `hello` arrives once per Lua state: at boot and again after a
// POB self-update restarted it, when every cached build fact is stale.
bridge.on("gate_result", (d) => {
  app.gate = d as GateResult;
});
let openedOnce = false;
bridge.on("hello", async () => {
  app.engine = "ready";
  app.childExit = null;
  app.reset();
  try {
    app.version = await api.version();
    if (app.version?.buildPath) app.buildPath = app.version.buildPath;
  } catch (e: any) {
    app.error = String(e?.message ?? e);
  }
  // The host was asked to open a build (command line); once per page life.
  if (!openedOnce && hostInfo.open) {
    openedOnce = true;
    void app.loadBuild(hostInfo.open);
  } else if (app.version?.buildLoaded) {
    // POB reopened its last build itself (Settings.xml): show it.
    const ok = await app.run(() => app.refresh());
    if (ok) app.view = app.landingView();
  }
});
bridge.on("restarted", () => {
  app.engine = "booting";
  app.notice = t("status.restarted");
});
// POB is applying its own update: "normal" restarts the Lua state in place
// (restarted + hello follow); "basic" ends the child and the host closes this
// window so Update.exe can reopen it.
bridge.on("update_applying", () => {
  app.engine = "booting";
  app.notice = t("status.updating");
});
bridge.on("host.updating", () => {
  app.engine = "booting";
  app.notice = t("status.updating");
});
bridge.on("host.child_exited", (d: any) => {
  app.engine = "gone";
  app.childExit = typeof d?.exitCode === "number" ? d.exitCode : -1;
});
bridge.on("host.disconnected", () => {
  if (app.link === "ok") app.link = "lost";
});
bridge.on("host.reconnected", () => {
  if (app.link === "lost") app.link = "ok";
});
// The window's X (WebView2 host): answer at once, then close, or ask about an
// unsaved build first (App.svelte shows the question).
bridge.on("host.close_requested", () => {
  void bridge.call("host.close_ack").catch(() => {});
  if (app.info?.unsaved && app.engine === "ready") app.closeAsk = true;
  else void bridge.call("host.close").catch(() => {});
});
bridge.on("host.closed", () => {
  app.link = "closed";
});
bridge.on("error", (d: any) => {
  app.error = String(d?.message ?? d);
});
