<script lang="ts">
  // The builds under POB's own build folder, as POB's list helpers index and
  // sort them. Double-click (or Enter) loads one; folders descend.
  import { onMount } from "svelte";
  import { api, bridge, type BuildEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  let entries = $state<BuildEntry[]>([]);
  let buildPath = $state("");
  let subPath = $state("");
  let loading = $state(false);
  let selected = $state<string | null>(null);

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

  onMount(() => {
    void refresh("");
  });
</script>

<div class="list">
  <div class="bar">
    <span class="label">{t("builds.title")}</span>
    <span class="dim path selectable">{buildPath}{subPath}</span>
    <span class="grow"></span>
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
          <span class="name">{e.isFolder ? "📁 " : ""}{e.isFolder ? e.folderName : e.buildName}</span>
          {#if !e.isFolder}
            <span class="cls dim">{e.ascendClassName ?? e.className ?? ""}</span>
            <span class="lvl num dim">{e.level ? `${t("builds.level")} ${e.level}` : ""}</span>
            <span class="date num dim">{fmtDate(e.modified)}</span>
          {/if}
        </div>
      {/each}
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
    border-bottom: 1px solid var(--line-0);
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
  .pad {
    padding: 16px;
  }
  .rows {
    flex: 1;
    overflow-y: auto;
    padding: 6px 8px;
  }
  .row {
    display: grid;
    grid-template-columns: 1fr 160px 60px 90px;
    gap: 12px;
    align-items: center;
    padding: 5px 8px;
    border-radius: var(--r-1);
    font-size: var(--fs-sm);
    cursor: default;
  }
  .row:hover {
    background: var(--bg-hover);
  }
  .row.sel {
    background: var(--bg-active);
  }
  .row.folder .name {
    color: var(--fg-1);
  }
  .name {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .lvl,
  .date {
    text-align: right;
  }
</style>
