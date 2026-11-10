<!-- 頂列:品牌格(對齊導覽列)、目前建置名稱與檔案、配裝組合、復原/重做、
     語言切換、儲存。儲存走 POB 自己的回呼(save_build*)。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, bridge, hostInfo, type LoadoutList } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import { layout } from "$lib/layout.svelte";
  import Select from "./Select.svelte";

  const h = $derived(app.header);

  // The OS window title carries the build name for the taskbar.
  $effect(() => {
    const name = app.info?.buildName;
    const title = name ? `${name}${app.info?.unsaved ? " •" : ""}` : "";
    void api.setTitle(title);
    // in the system browser the tab title is the page's own
    if (hostInfo.browser) document.title = title ? `${title} - PobTools` : "PobTools";
  });

  // In the system browser closing the tab does not end the program right away
  // (it waits to see whether the page comes back), so the page has its own End.
  function endProgram() {
    if (app.info?.unsaved && !confirm(t("title.endUnsaved"))) return;
    app.link = "closed";
    void bridge.call("host.close").catch(() => {});
  }

  // Build.lua's loadout drop-down: one pick switches tree, items, skills and config
  let loadouts = $state<LoadoutList | null>(null);
  let loadoutRev = "";
  let newLoadout = $state<string | null>(null);
  $effect(() => {
    const rev = app.dataRev;
    if (!app.loaded) return;
    if (rev === loadoutRev) return;
    loadoutRev = rev;
    untrack(async () => {
      try {
        loadouts = await api.listLoadouts();
      } catch {
        loadouts = null;
      }
    });
  });
  async function pickLoadout(v: string) {
    const i = Number(v);
    const e = loadouts?.entries.find((x) => x.index === i);
    if (!e) return;
    if (e.label === "New Loadout") {
      newLoadout = "";
      return;
    }
    const r = await app.run(() => api.selectLoadout(i));
    if (r) {
      loadouts = r;
      await app.refresh();
    }
  }
  async function makeLoadout() {
    const title = (newLoadout ?? "").trim();
    const i = loadouts?.entries.find((x) => x.label === "New Loadout")?.index;
    newLoadout = null;
    if (!title || !i) return;
    const r = await app.run(() => api.selectLoadout(i, title));
    if (r) {
      loadouts = r;
      await app.refresh();
    }
  }

  let saveAsOpen = $state(false);
  let saveAsName = $state("");
  // the folder the build goes into, relative to POB's build folder ("" = top)
  let saveAsDir = $state("");
  let saveAsFolders = $state<string[]>([]);
  let saveAsNewFolder = $state<string | null>(null);
  async function openSaveAs() {
    saveAsName = h?.buildName ?? "";
    const root = app.buildPath.split(String.fromCharCode(92)).join("/").replace(/[/]+$/, "").toLowerCase();
    const file = (h?.dbFileName ?? "").split(String.fromCharCode(92)).join("/");
    const dir = file.slice(0, file.lastIndexOf("/") + 1);
    saveAsDir = file && dir.toLowerCase().startsWith(root + "/") ? dir.slice(root.length + 1) : "";
    saveAsOpen = true;
    await loadFolders(saveAsDir);
  }
  async function loadFolders(dir: string) {
    const r = await app.run(() => api.listBuilds(dir, { filter: "" }));
    if (!r) return;
    saveAsDir = r.subPath;
    saveAsFolders = r.entries.filter((e) => e.isFolder).map((e) => e.folderName ?? "");
  }
  async function makeFolder() {
    const name = (saveAsNewFolder ?? "").trim();
    saveAsNewFolder = null;
    if (!name) return;
    const r = await app.run(() => api.newFolder(saveAsDir, name));
    if (r) await loadFolders(saveAsDir + name + "/");
  }

  async function save() {
    if (!h) return;
    await app.saveOrAsk();
  }
  // app.saveOrAsk (Ctrl+S, a "save first?" answer) asks for this dialog
  let seenSaveAsRequest = app.saveAsRequest;
  $effect(() => {
    const n = app.saveAsRequest;
    if (n !== seenSaveAsRequest) {
      seenSaveAsRequest = n;
      void openSaveAs();
    }
  });

  async function saveAs() {
    const name = saveAsName.trim();
    if (!name) return;
    saveAsOpen = false;
    const path = saveAsDir + name;
    // another build already has this name: POB greys Save out, here we ask
    let r = await app.saveAs(path);
    if (r === "exists" && confirm(t("bar.overwriteConfirm", { name }))) r = await app.saveAs(path, true);
    // not saved after all: a pending "then" (close, import a link) is dropped
    if (r !== "saved") app.cancelSaveAs();
  }
</script>

<header class="top" class:narrow={layout.narrow}>
  {#if layout.narrow}
    <button class="sidetoggle" class:on={!layout.collapsed} title={t(layout.collapsed ? "title.sideShow" : "title.sideHide")} aria-label={t(layout.collapsed ? "title.sideShow" : "title.sideHide")} aria-pressed={!layout.collapsed} onclick={() => layout.toggleSide()}>
      <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 6h16M4 12h16M4 18h16" /></svg>
    </button>
  {/if}
  <span class="brand" aria-hidden="true"><i class="mark"></i></span>
  {#if app.info}
    <div class="build">
      <div class="line">
        <span class="bname">{app.info.buildName}</span>
        {#if app.info.unsaved}<span class="pill warn"><i class="dot"></i>{t("title.unsaved")}</span>{/if}
      </div>
      {#if !layout.narrow}
        <div class="bpath num" title={app.info.dbFileName ?? ""}>{app.info.dbFileName || t("title.notSaved")}</div>
      {/if}
    </div>
  {/if}
  <span class="grow"></span>

  {#if app.loaded && loadouts && loadouts.entries.length > 1}
    <span class="loadout">
      {#if !layout.narrow}<span class="k">{t("bar.loadout")}</span>{/if}
      <Select class="select sm" ariaLabel={t("bar.loadout")} value={String(loadouts.selIndex)} title={t("bar.loadout")} options={loadouts.entries.map((e) => ({ value: String(e.index), label: e.kind === "loadout" ? e.label : e.labelZh || e.label, disabled: e.kind === "header" }))} onchange={(v) => pickLoadout(String(v))} />
    </span>
    <span class="vsep"></span>
  {/if}

  {#if app.loaded}
    <span class="icons">
      <button class="btn ghost sm icon" disabled={!app.canUndo || app.busy > 0} title="{t('tree.undo')} (Ctrl+Z)" aria-label={t("tree.undo")} onclick={() => void app.undo()}>
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M9 14L4 9l5-5M4 9h10a6 6 0 0 1 0 12h-3" /></svg>
      </button>
      <button class="btn ghost sm icon" disabled={!app.canUndo || app.busy > 0} title="{t('tree.redo')} (Ctrl+Y)" aria-label={t("tree.redo")} onclick={() => void app.undo(true)}>
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M15 14l5-5-5-5M20 9H10a6 6 0 0 0 0 12h3" /></svg>
      </button>
    </span>
  {/if}

  {#if app.has("translateToggle")}
    <span class="seg">
      <button aria-pressed={!app.english} onclick={() => app.english && void app.toggleEnglish()}>{t("title.langZh")}</button>
      <button aria-pressed={app.english} title="F2" onclick={() => !app.english && void app.toggleEnglish()}>{t("title.langEn")} <span class="key">F2</span></button>
    </span>
  {/if}

  {#if app.loaded && h}
    <span class="actions">
      {#if app.savedAt && !layout.narrow}<span class="dim savedat">{t("bar.savedAt", { time: app.savedAt })}</span>{/if}
      <button class="btn sm" disabled={app.busy > 0} onclick={openSaveAs}>{t("bar.saveAs")}</button>
      <button class="btn sm" class:primary={h.unsaved} disabled={app.busy > 0} title="Ctrl+S" onclick={save}>{t("bar.save")}</button>
    </span>
  {/if}

  {#if hostInfo.browser}
    <button class="end" title={t("title.endHint")} onclick={endProgram}>{t("title.end")}</button>
  {/if}
</header>

{#if newLoadout !== null}
  <div class="modal">
    <div class="dialog">
      <div class="label">{t("bar.newLoadout")}</div>
      <p class="dim">{t("bar.newLoadoutHint")}</p>
      <!-- svelte-ignore a11y_autofocus -->
      <input class="input" autofocus bind:value={newLoadout} onkeydown={(e) => e.key === "Enter" && makeLoadout()} />
      <div class="dlg-actions">
        <button class="btn ghost" onclick={() => (newLoadout = null)}>{t("tree.cancel")}</button>
        <button class="btn primary" disabled={!newLoadout.trim()} onclick={makeLoadout}>OK</button>
      </div>
    </div>
  </div>
{/if}

{#if saveAsOpen}
  <div class="modal">
    <div class="dialog">
      <div class="label">{t("bar.saveAsTitle")}</div>
      <p class="dim">{t("bar.saveAsBody")}</p>
      <div class="folders">
        <div class="fpath dim">{app.buildPath}{saveAsDir}</div>
        <div class="flist">
          {#if saveAsDir}
            <button class="frow" onclick={() => loadFolders(saveAsDir.replace(/[^/]+[/]$/, ""))}>↑ ..</button>
          {/if}
          {#each saveAsFolders as f}
            <button class="frow" onclick={() => loadFolders(saveAsDir + f + "/")}>📁 {f}</button>
          {/each}
        </div>
        {#if saveAsNewFolder !== null}
          <span class="fnew">
            <input class="input" placeholder={t("builds.newFolder")} bind:value={saveAsNewFolder} onkeydown={(e) => e.key === "Enter" && makeFolder()} />
            <button class="btn sm" onclick={makeFolder} disabled={!saveAsNewFolder.trim()}>OK</button>
          </span>
        {:else}
          <button class="btn ghost sm" onclick={() => (saveAsNewFolder = "")}>{t("builds.newFolder")}</button>
        {/if}
      </div>
      <input class="input" bind:value={saveAsName} onkeydown={(e) => e.key === "Enter" && saveAs()} />
      <div class="dlg-actions">
        <button class="btn ghost" onclick={() => { saveAsOpen = false; app.cancelSaveAs(); }}>{t("tree.cancel")}</button>
        <button class="btn primary" onclick={saveAs} disabled={!saveAsName.trim()}>{t("bar.save")}</button>
      </div>
    </div>
  </div>
{/if}

<style>
  .top {
    flex: none;
    height: var(--titlebar-h);
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 0 16px 0 0;
    min-width: 0;
    background: var(--surface-1);
    border-bottom: 1px solid var(--edge-0);
  }
  .top.narrow {
    gap: 8px;
    padding: 0 8px;
  }
  .top > * {
    flex: none;
  }
  svg {
    width: 16px;
    height: 16px;
    fill: none;
    stroke: currentColor;
    stroke-width: 1.75;
    stroke-linecap: round;
    stroke-linejoin: round;
  }
  /* the brand cell sits over the nav rail, the same width */
  .brand {
    width: var(--rail-w);
    align-self: stretch;
    display: grid;
    place-items: center;
  }
  .narrow .brand {
    width: auto;
  }
  .mark {
    width: 14px;
    height: 14px;
    background: var(--hl);
    transform: rotate(45deg);
    border-radius: 2px;
  }
  .sidetoggle {
    appearance: none;
    display: grid;
    place-items: center;
    width: 28px;
    height: 26px;
    padding: 0;
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
    background: none;
    color: var(--ink-2);
  }
  .sidetoggle:hover,
  .sidetoggle.on {
    color: var(--gold-text);
    border-color: var(--gold);
  }
  .top > .build {
    flex: 0 1 auto;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 1px;
  }
  .line {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }
  .bname {
    min-width: 0;
    font-size: var(--fs-lg);
    font-weight: 500;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .line .pill {
    flex: none;
  }
  .dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: currentColor;
  }
  .bpath {
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .top > .grow {
    flex: 1 1 0;
    min-width: 0;
  }
  .loadout,
  .icons,
  .actions {
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .top > .loadout {
    flex: 0 1 auto;
    min-width: 0;
  }
  .loadout :global(.select.sm) {
    height: 26px;
    max-width: 220px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
    white-space: nowrap;
  }
  .icons {
    gap: 2px;
  }
  .icon {
    width: 28px;
    height: 28px;
    padding: 0;
    justify-content: center;
  }
  .key {
    font-size: var(--fs-2xs);
    opacity: 0.7;
  }
  .savedat {
    font-size: var(--fs-2xs);
  }
  .end {
    appearance: none;
    border: 1px solid var(--edge-1);
    background: none;
    color: var(--ink-2);
    font-size: var(--fs-sm);
    padding: 2px 10px;
    border-radius: 4px;
  }
  .end:hover {
    color: var(--bad);
    border-color: var(--bad);
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
  .folders {
    display: flex;
    flex-direction: column;
    gap: 6px;
    margin-bottom: 8px;
  }
  .fpath {
    font-size: var(--fs-xs);
    word-break: break-all;
  }
  .flist {
    max-height: 180px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .frow {
    display: block;
    width: 100%;
    text-align: left;
    appearance: none;
    border: 0;
    background: none;
    color: var(--ink-1);
    padding: 3px 8px;
  }
  .frow:hover {
    background: var(--surface-2);
  }
  .fnew {
    display: flex;
    gap: 6px;
  }
</style>
