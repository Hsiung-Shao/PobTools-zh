<script lang="ts">
  // The builds under POB's own build folder, as POB's list helpers index and
  // sort them. Double-click (or Enter) loads one; folders descend. The bar
  // has POB's New / New Folder; each row's hover actions are Open / Copy /
  // Rename / Delete (BuildListControl's RenameBuild / DeleteBuild).
  import { onMount } from "svelte";
  import { api, bridge, type BuildEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

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
    }
  }
  // the path shown before a search hit that lives in a subfolder (GetRowValue)
  const relPrefix = (e: BuildEntry) => (e.subPath && e.subPath !== subPath && e.subPath.startsWith(subPath) ? e.subPath.slice(subPath.length) : e.subPath !== subPath ? e.subPath : "");

  function open(e: BuildEntry) {
    if (e.isFolder) void refresh(`${e.subPath}${e.folderName}/`);
    else void app.loadBuild(e.fullFileName);
  }

  function up() {
    const parent = subPath.replace(/[^/]+\/$/, "");
    void refresh(parent);
  }

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
    void refresh("");
  });
</script>

<svelte:window onkeydown={onKey} />

<div class="list">
  <div class="bar">
    <span class="label">{t("builds.title")}</span>
    <span class="dim path selectable">{buildPath}{subPath}</span>
    <span class="grow"></span>
    <input class="input search" type="search" placeholder={t("builds.searchHint")} bind:value={filter} oninput={onSearch} />
    <select class="input sort" value={sortMode} onchange={(e) => refresh(subPath, { sortMode: e.currentTarget.value })} title={t("builds.sort")}>
      {#each sortModes as s}<option value={s.sortMode}>{t(`builds.sort.${s.sortMode}`) === `builds.sort.${s.sortMode}` ? s.label : t(`builds.sort.${s.sortMode}`)}</option>{/each}
    </select>
    {#if clip}
      <button class="btn sm" disabled={app.busy > 0 || !canMoveTo(clip.entry, subPath) && !(clip.copy && clip.entry.subPath === subPath)} onclick={paste}
        title={t(clip.copy ? "builds.pasteCopyHint" : "builds.pasteMoveHint", { name: nameOf(clip.entry) })}>{t("builds.paste")}</button>
    {/if}
    <button class="btn primary sm" disabled={app.busy > 0} onclick={() => app.newBuild(undefined, subPath)}>{t("builds.new")}</button>
    <button class="btn sm" disabled={app.busy > 0} onclick={() => (dialog = { kind: "newFolder", name: "" })}>{t("builds.newFolder")}</button>
    <span class="vsep"></span>
    {#if subPath}<button class="btn sm" onclick={up}>↑</button>{/if}
    <button class="btn sm" onclick={() => refresh()}>{t("builds.refresh")}</button>
    <button class="btn sm" onclick={() => bridge.call("host.open_folder", { path: buildPath + subPath })}>{t("builds.openFolder")}</button>
  </div>
  {#if loading && entries.length === 0}
    <div class="dim pad">{t("builds.loading")}</div>
  {:else if entries.length === 0}
    <div class="dim pad">{t("builds.empty", { path: buildPath })}</div>
  {:else}
    <div class="rows">
      {#each entries as e (e.fullFileName)}
        <div
          class="row"
          class:sel={selected === e.fullFileName}
          class:folder={e.isFolder}
          class:cut={clip && !clip.copy && clip.entry.fullFileName === e.fullFileName}
          class:drop={dropOn === e.fullFileName}
          role="button"
          tabindex="0"
          draggable="true"
          ondragstart={(ev) => { dragging = e; ev.dataTransfer?.setData("text/plain", e.fullFileName); }}
          ondragend={() => { dragging = null; dropOn = null; }}
          ondragover={(ev) => { if (e.isFolder && dragging && dragging !== e && canMoveTo(dragging, `${e.subPath}${e.folderName}/`)) { ev.preventDefault(); dropOn = e.fullFileName; } }}
          ondragleave={() => { if (dropOn === e.fullFileName) dropOn = null; }}
          ondrop={(ev) => { ev.preventDefault(); const d = dragging; dragging = null; dropOn = null; if (d && e.isFolder) void moveTo(d, `${e.subPath}${e.folderName}/`, false); }}
          onclick={() => (selected = e.fullFileName)}
          ondblclick={() => open(e)}
          onkeydown={(k) => k.key === "Enter" && open(e)}
        >
          <span class="name">{e.isFolder ? "📁 " : ""}{#if relPrefix(e)}<span class="dim">{relPrefix(e)}</span>{/if}{nameOf(e)}</span>
          <span class="acts">
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); open(e); }}>{t("builds.open")}</button>
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); dialog = { kind: "copy", entry: e, name: nameOf(e) }; }}>{t("builds.copy")}</button>
            <button class="btn ghost sm" title={t("builds.cutHint")} onclick={(ev) => { ev.stopPropagation(); clip = { entry: e, copy: false }; }}>{t("builds.cut")}</button>
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); dialog = { kind: "rename", entry: e, name: nameOf(e) }; }}>{t("builds.rename")}</button>
            <button class="btn ghost sm danger" onclick={(ev) => { ev.stopPropagation(); void remove(e); }}>{t("builds.delete")}</button>
          </span>
          {#if !e.isFolder}
            <span class="cls dim">{e.ascendClassName ?? e.className ?? ""}</span>
            <span class="lvl num dim">{e.level ? `${t("builds.level")} ${e.level}` : ""}</span>
            <span class="date num dim">{fmtDate(e.modified)}</span>
          {/if}
        </div>
      {/each}
    </div>
  {/if}

  {#if dialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t(dialog.kind === "newFolder" ? "builds.newFolder" : dialog.kind === "copy" ? "builds.copyTitle" : "builds.renameTitle")}</div>
        {#if dialog.entry}<p class="dim">{nameOf(dialog.entry)}</p>{/if}
        <!-- svelte-ignore a11y_autofocus -->
        <input class="input" autofocus bind:value={dialog.name} onkeydown={(k) => k.key === "Enter" && dialogOk()} />
        <div class="dlg-actions">
          <button class="btn ghost" onclick={() => (dialog = null)}>{t("tree.cancel")}</button>
          <button class="btn primary" disabled={!nameOk} onclick={dialogOk}>OK</button>
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .list {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 10px;
    padding: 8px 14px;
    border-bottom: 1px solid var(--edge-0);
  }
  .search {
    width: 220px;
  }
  .sort {
    width: auto;
  }
  .row.cut {
    opacity: 0.55;
  }
  .row.drop {
    outline: 1px dashed var(--gold);
  }
  .path {
    font-size: var(--fs-xs);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .grow {
    flex: 1;
  }
  .vsep {
    width: 1px;
    height: 16px;
    background: var(--edge-1);
  }
  .pad {
    padding: 16px;
  }
  .rows {
    flex: 1;
    overflow-y: auto;
    padding: 6px 0;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 6px 16px;
    cursor: default;
  }
  .row:hover {
    background: var(--surface-hover);
  }
  .row.sel {
    background: var(--surface-active, var(--surface-hover));
  }
  .name {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: var(--fs-md);
  }
  .folder .name {
    color: var(--gold);
  }
  .acts {
    display: inline-flex;
    gap: 2px;
    opacity: 0;
  }
  .row:hover .acts,
  .row.sel .acts {
    opacity: 1;
  }
  .danger:hover {
    color: var(--bad);
  }
  .cls {
    width: 180px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .lvl {
    width: 60px;
    text-align: right;
  }
  .date {
    width: 90px;
    text-align: right;
  }
  .modal {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: var(--backdrop);
    z-index: 30;
  }
  .dialog {
    width: 420px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .dialog p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .dlg-actions {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
  }
</style>
