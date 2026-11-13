  import { untrack } from "svelte";
  import { api, type CompareState, type CompareItemsRows, type CompareSkills, type CompareConfigRows, type CompareTree, type BuildEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import Select from "$lib/ui/Select.svelte";

  type SubView = "summary" | "tree" | "items" | "skills" | "config";

  let st = $state<CompareState | null>(null);
  let view = $state<SubView>("summary");
  let busy = $state(false);
  let err = $state<string | null>(null);
  let code = $state("");
  let picker = $state(false);
  let builds = $state<BuildEntry[]>([]);
  let tree = $state<CompareTree | null>(null);
  let items = $state<CompareItemsRows | null>(null);
  let skills = $state<CompareSkills | null>(null);
  let config = $state<CompareConfigRows | null>(null);
  let loadedRev = "";

  async function reload() {
    try {
      st = await api.compareState();
      loadedRev = app.dataRev;
      if (st.activeIndex) await loadView(view);
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  }
  $effect(() => {
    const rev = app.dataRev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  async function loadView(v: SubView) {
    view = v;
    if (!st?.activeIndex) return;
    busy = true;
    err = null;
    try {
      if (v === "tree") tree = await api.compareTree();
      else if (v === "items") items = await api.compareItems();
      else if (v === "skills") skills = await api.compareSkills();
      else if (v === "config") config = await api.compareConfig();
      else st = await api.compareState();
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
    busy = false;
  }

  async function run(fn: () => Promise<CompareState>) {
    busy = true;
    err = null;
    try {
      st = await fn();
      await loadView(view);
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
    busy = false;
  }

  async function openPicker() {
    picker = true;
    const r = await app.run(() => api.listBuilds("", { filter: "" }));
    if (r) builds = r.entries.filter((e) => !e.isFolder);
  }
  async function loadFile(e: BuildEntry) {
    picker = false;
    await run(() => api.compareLoad({ path: e.fullFileName, label: e.buildName ?? e.fileName }));
  }
  async function use(what: "tree" | "item" | "config", slot?: string) {
    const r = await app.run(() => api.compareUse(what, slot));
    if (r) {
      await app.refresh();
      await reload();
    }
  }
