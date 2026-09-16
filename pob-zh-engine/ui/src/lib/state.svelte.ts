// App-wide state: engine status, the loaded build, sidebar, view.
// Svelte 5 runes in a module so every component reads the same object.
import { api, bridge, hostInfo, type BuildInfo, type GateResult, type Sidebar, type VersionInfo } from "./bridge";

export type ViewId = "builds" | "tree";

class AppState {
  engine = $state<"booting" | "ready" | "gone">("booting");
  gate = $state<GateResult | null>(null);
  version = $state<VersionInfo | null>(null);
  childExit = $state<number | null>(null);
  error = $state<string | null>(null);
  notice = $state<string | null>(null);
  busy = $state(0);

  view = $state<ViewId>("builds");
  info = $state<BuildInfo | null>(null);
  sidebar = $state<Sidebar | null>(null);
  /** POB's outputRevision after the last refresh; everything cached is keyed on it. */
  rev = $state(0);
  loaded = $derived(this.info !== null);

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
    const [info, side] = await Promise.all([api.getBuildInfo(), api.getSidebar()]);
    this.info = info;
    this.sidebar = side;
    this.rev = side.rev;
    return true;
  }

  /**
   * The tree changed in the engine (a click, an undo): POB recalculated in
   * the same call, so pull the sidebar and header again. The tree view keys
   * its own refresh on `rev`.
   */
  async afterTreeChange() {
    await this.refresh();
  }

  async loadBuild(path: string) {
    const ok = await this.run(async () => {
      await api.loadBuildFile(path);
      return this.refresh();
    });
    if (ok) this.view = "tree";
  }

  reset() {
    this.info = null;
    this.sidebar = null;
    this.rev = 0;
    this.view = "builds";
  }
}

export const app = new AppState();

// Engine events. `hello` arrives once per Lua state: at boot and again after a
// POB self-update restarted it, when every cached build fact is stale.
bridge.on("gate_result", (d) => (app.gate = d as GateResult));
let openedOnce = false;
bridge.on("hello", async () => {
  app.engine = "ready";
  app.childExit = null;
  app.reset();
  try {
    app.version = await api.version();
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
    if (ok) app.view = "tree";
  }
});
bridge.on("restarted", () => {
  app.engine = "booting";
  app.notice = "POB restarted";
});
bridge.on("host.child_exited", (d: any) => {
  app.engine = "gone";
  app.childExit = typeof d?.exitCode === "number" ? d.exitCode : -1;
});
bridge.on("error", (d: any) => (app.error = String(d?.message ?? d)));
