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

  async function refresh(path = subPath) {
    loading = true;
    const r = await app.run(() => api.listBuilds(path));
    loading = false;
    if (!r) return;
    entries = r.entries;
    buildPath = r.buildPath;
    subPath = r.subPath;
  }

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

<div class="list">
  <div class="bar">
    <span class="label">{t("builds.title")}</span>
    <span class="dim path selectable">{buildPath}{subPath}</span>
    <span class="grow"></span>
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
          role="button"
          tabindex="0"
          onclick={() => (selected = e.fullFileName)}
          ondblclick={() => open(e)}
          onkeydown={(k) => k.key === "Enter" && open(e)}
        >
          <span class="name">{e.isFolder ? "📁 " : ""}{nameOf(e)}</span>
          <span class="acts">
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); open(e); }}>{t("builds.open")}</button>
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); dialog = { kind: "copy", entry: e, name: nameOf(e) }; }}>{t("builds.copy")}</button>
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
    align-items: center;
    gap: 10px;
    padding: 8px 14px;
    border-bottom: 1px solid var(--edge-0);
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
