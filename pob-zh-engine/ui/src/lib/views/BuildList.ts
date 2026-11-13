  // The builds under POB's own build folder, as POB's list helpers index and
  // sort them. Double-click (or Enter) loads one; folders descend. The bar
  // has POB's New / New Folder; each row's hover actions are Open / Copy /
  // Rename / Delete (BuildListControl's RenameBuild / DeleteBuild).
  // The folder being shown is app.buildsSubPath, so it survives leaving the list
  // and the mouse's back button (app.goBack) can walk up it.
  import { onMount, untrack } from "svelte";
  import { api, bridge, type BuildEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { folderCrumbs, parentFolder } from "$lib/nav";
  import { app } from "$lib/state.svelte";
  import Select from "$lib/ui/Select.svelte";

  let entries = $state<BuildEntry[]>([]);
  let buildPath = $state("");
  let subPath = $state("");
  let loading = $state(false);
  let selected = $state<string | null>(null);
  let dialog = $state<{ kind: "newFolder" | "rename" | "copy"; entry?: BuildEntry; name: string } | null>(null);
  // POB's search box and sort drop-down (BuildList.lua); both live in POB
  // (main.filterBuildList / buildSortMode), the page only mirrors them.
  let filter = $state("");
  let sortMode = $state("");
  let sortModes = $state<{ sortMode: string; label: string }[]>([]);
  // Ctrl+X / Ctrl+C then Ctrl+V (or the Paste button) in another folder.
  let clip = $state<{ entry: BuildEntry; copy: boolean } | null>(null);
  let dragging = $state<BuildEntry | null>(null);
  let dropOn = $state<string | null>(null);

  async function refresh(path = subPath, opts: { filter?: string; sortMode?: string } = {}) {
    loading = true;
    const r = await app.run(() => api.listBuilds(path, { filter, ...opts }));
    loading = false;
    if (!r) return;
    entries = r.entries;
    buildPath = r.buildPath;
    subPath = r.subPath;
    app.buildsSubPath = r.subPath;
    if (typeof r.sortMode === "string") sortMode = r.sortMode;
    if (r.sortModes) sortModes = r.sortModes;
  }

  let searchTimer: ReturnType<typeof setTimeout> | undefined;
  function onSearch() {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => void refresh(subPath), 200);
  }

  const entryName = (e: BuildEntry) => (e.isFolder ? e.folderName : e.fileName) ?? "";
  // BuildListHelpers.CanMoveToSubPath: not where it already is, a folder not into itself
  function canMoveTo(e: BuildEntry, target: string) {
    if (e.subPath === target) return false;
    if (e.isFolder && target.startsWith(`${e.subPath}${e.folderName}/`)) return false;
    return true;
  }
  async function moveTo(e: BuildEntry, target: string, copy: boolean) {
    if (!canMoveTo(e, target)) return false;
    const r = await app.run(() =>
      api.moveBuild({ path: e.fullFileName, subPath: e.subPath, isFolder: e.isFolder, name: entryName(e), targetSubPath: target, copy }),
    );
    if (r) await refresh();
    return !!r;
  }
  async function paste() {
    if (!clip) return;
    const c = clip;
    if (c.entry.subPath === subPath) {
      // pasting a copy into its own folder is POB's "Copy" (rename) dialog
      if (c.copy) dialog = { kind: "copy", entry: c.entry, name: nameOf(c.entry) };
      clip = null;
      return;
    }
    if (await moveTo(c.entry, subPath, c.copy)) clip = null;
  }
  const selEntry = $derived(entries.find((e) => e.fullFileName === selected) ?? null);

  function onKey(k: KeyboardEvent) {
    const tgt = k.target as HTMLElement | null;
    if (dialog || (tgt && (tgt.tagName === "INPUT" || tgt.tagName === "TEXTAREA"))) return;
    const key = k.key.toLowerCase();
    if (k.ctrlKey && key === "n") {
      k.preventDefault();
      void app.newBuild(undefined, subPath);
    } else if (k.ctrlKey && (key === "x" || key === "c") && selEntry) {
      k.preventDefault();
      clip = { entry: selEntry, copy: key === "c" };
    } else if (k.ctrlKey && key === "v" && clip) {
      k.preventDefault();
      void paste();
      // F2 belongs to the language toggle, the way the engine takes it in the
      // classic window; renaming moved to Ctrl+R (every row also has a button).
    } else if (k.ctrlKey && key === "r" && selEntry) {
      k.preventDefault();
      dialog = { kind: "rename", entry: selEntry, name: nameOf(selEntry) };
    } else if (k.key === "Delete" && selEntry) {
      k.preventDefault();
      void remove(selEntry);
    } else if (k.key === "Backspace" && !k.ctrlKey && !k.altKey && subPath) {
      k.preventDefault();
      up();
    }
  }
  // the path shown before a search hit that lives in a subfolder (GetRowValue)
  const relPrefix = (e: BuildEntry) => (e.subPath && e.subPath !== subPath && e.subPath.startsWith(subPath) ? e.subPath.slice(subPath.length) : e.subPath !== subPath ? e.subPath : "");

  function open(e: BuildEntry) {
    if (e.isFolder) void refresh(`${e.subPath}${e.folderName}/`);
    else void app.loadBuild(e.fullFileName);
  }

  function up() {
    void refresh(parentFolder(subPath));
  }
  const crumbs = $derived(folderCrumbs(subPath));
  const parentPath = $derived(parentFolder(subPath));

  function fmtDate(ts?: number) {
    if (!ts) return "";
    // The engine hands POB a Windows FILETIME (100 ns ticks since 1601).
    const d = new Date(ts / 10000 - 11644473600000);
    if (Number.isNaN(d.getTime())) return "";
    return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
  }

  const nameOf = (e: BuildEntry) => (e.isFolder ? e.folderName : e.buildName) ?? "";
  const nameOk = $derived(!!dialog && dialog.name.trim().length > 0 && !/[\\/:*?"<>|]/.test(dialog.name));

  async function dialogOk() {
    if (!dialog || !nameOk) return;
    const d = dialog;
    dialog = null;
    const name = d.name.trim();
    let r: unknown;
    if (d.kind === "newFolder") r = await app.run(() => api.newFolder(subPath, name));
    else if (d.entry) r = await app.run(() => api.renameBuild({ path: d.entry!.fullFileName, subPath: d.entry!.subPath, isFolder: d.entry!.isFolder, newName: name, copy: d.kind === "copy" }));
    if (r) await refresh();
  }
  async function remove(e: BuildEntry) {
    if (!confirm(t(e.isFolder ? "builds.confirmDeleteFolder" : "builds.confirmDelete", { name: nameOf(e) }))) return;
    const r = await app.run(() => api.deleteBuild({ path: e.fullFileName, isFolder: e.isFolder, recursive: e.isFolder }));
    if (r) {
      if (selected === e.fullFileName) selected = null;
      await refresh();
    }
  }

  onMount(() => {
    void refresh(app.buildsSubPath);
  });
  // app.goBack() (the mouse's back button) moves the folder from outside
  $effect(() => {
    const want = app.buildsSubPath;
    untrack(() => {
      // only once the first listing is in (onMount does that one)
      if (buildPath && want !== subPath && !loading) void refresh(want);
    });
  });
