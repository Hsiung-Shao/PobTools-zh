<!-- 比較分頁(POB 的 Compare tab):載入另一個建置(分享碼或建置檔),
     摘要用 POB 兩邊的輸出逐項比,天賦樹/物品/技能/配置列出差異並可複製回來。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type CompareState, type CompareItemsRows, type CompareSkills, type CompareConfigRows, type CompareTree, type BuildEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

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
  let loadedRev = -1;

  async function reload() {
    try {
      st = await api.compareState();
      loadedRev = app.rev;
      if (st.activeIndex) await loadView(view);
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  }
  $effect(() => {
    const rev = app.rev;
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
</script>

<div class="page">
  <div class="bar">
    <span class="label">{t("compare.title")}</span>
    {#if st?.builds?.length}
      <select class="select sm" value={String(st.activeIndex)} disabled={busy} onchange={(e) => run(() => api.compareSelect(Number(e.currentTarget.value)))}>
        {#each st.builds as b (b.index)}<option value={String(b.index)}>{b.label || b.buildName}</option>{/each}
      </select>
      <button class="btn ghost sm" disabled={busy} onclick={() => run(() => api.compareRemove(st!.activeIndex))}>{t("compare.remove")}</button>
    {/if}
    <span class="grow"></span>
    <input class="input sm code" placeholder={t("compare.codeHint")} bind:value={code} />
    <button class="btn sm" disabled={!code.trim() || busy} onclick={() => run(() => api.compareLoad({ code: code.trim(), label: t("compare.pasted") }))}>{t("compare.importCode")}</button>
    <button class="btn sm" disabled={busy} onclick={openPicker}>{t("compare.importFile")}</button>
  </div>

  {#if err}<p class="bad pad">{err}</p>{/if}

  {#if !st?.activeIndex}
    <p class="dim pad">{t("compare.none")}</p>
  {:else}
    <div class="tabs">
      {#each [["summary", "compare.summary"], ["tree", "compare.tree"], ["items", "compare.items"], ["skills", "compare.skills"], ["config", "compare.config"]] as [id, key]}
        <button class="btn sm" class:primary={view === id} disabled={busy} onclick={() => loadView(id as SubView)}>{t(key)}</button>
      {/each}
      {#if busy}<span class="dim pulse">{t("tree.powerWorking")}</span>{/if}
    </div>

    <div class="scroll">
      {#if view === "summary"}
        <table class="stats">
          <thead><tr><th>{t("compare.stat")}</th><th class="n">{t("compare.mine")}</th><th class="n">{t("compare.theirs")}</th><th class="n">{t("compare.diff")}</th></tr></thead>
          <tbody>
            {#each st.stats ?? [] as r, i (i)}
              {#if r.gap}
                <tr class="gap"><td colspan="4"></td></tr>
              {:else}
                <tr>
                  <td>{r.labelZh || r.label}</td>
                  <td class="n">{r.primary}</td>
                  <td class="n cmp">{r.compare}</td>
                  <td class="n" class:good={r.better === true} class:bad={r.better === false}>{r.diff ?? ""}{r.diffPercent ? ` (${r.diffPercent})` : ""}</td>
                </tr>
              {/if}
            {/each}
          </tbody>
        </table>
      {:else if view === "tree" && tree}
        <div class="pad">
          <p>{t("compare.treePoints", { n: tree.points ?? 0 })}</p>
          <p class="good">{t("compare.treeOnlyCompare", { n: tree.onlyInCompare.length })}</p>
          <p class="bad">{t("compare.treeOnlyPrimary", { n: tree.onlyInPrimary.length })}</p>
          <div class="acts">
            <button class="btn" disabled={busy} onclick={() => use("tree")}>{t("compare.useTree")}</button>
          </div>
        </div>
      {:else if view === "items" && items}
        <table class="items">
          <thead><tr><th>{t("compare.slot")}</th><th>{t("compare.mine")}</th><th>{t("compare.theirs")}</th><th></th></tr></thead>
          <tbody>
            {#each items.rows as r (r.slot)}
              <tr class:same={r.same}>
                <td>{r.slotZh || r.slot}</td>
                <td>{r.primary ? r.primary.nameZh || r.primary.name : "—"}</td>
                <td class="cmp">{r.compare ? r.compare.nameZh || r.compare.name : "—"}</td>
                <td>{#if r.compare && !r.same}<button class="btn ghost sm" disabled={busy} onclick={() => use("item", r.slot)}>{t("compare.useItem")}</button>{/if}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {:else if view === "skills" && skills}
        <div class="cols">
          {#each [{ who: "mine", list: skills.primary }, { who: "theirs", list: skills.compare }] as side (side.who)}
            <div class="col">
              <h3>{t(side.who === "mine" ? "compare.mine" : "compare.theirs")}</h3>
              {#each side.list as g (g.index)}
                <div class="group" class:off={!g.enabled}>
                  <div class="gname">{g.labelZh || g.label}{g.isMain ? " ★" : ""}</div>
                  <div class="gems dim small">{g.gems.map((x: { name: string; nameZh?: string; level?: number; quality?: number }) => `${x.nameZh || x.name} ${x.level}/${x.quality}`).join(" · ")}</div>
                </div>
              {/each}
            </div>
          {/each}
        </div>
      {:else if view === "config" && config}
        {#if config.rows.length === 0}
          <p class="dim pad">{t("compare.configSame")}</p>
        {:else}
          <table class="stats">
            <thead><tr><th>{t("compare.option")}</th><th>{t("compare.mine")}</th><th>{t("compare.theirs")}</th></tr></thead>
            <tbody>
              {#each config.rows as r (r.var)}
                <tr><td>{r.labelZh || r.label}</td><td>{r.primary}</td><td class="cmp">{r.compare}</td></tr>
              {/each}
            </tbody>
          </table>
          <div class="acts pad"><button class="btn" disabled={busy} onclick={() => use("config")}>{t("compare.useConfig")}</button></div>
        {/if}
      {/if}
    </div>
  {/if}

  {#if picker}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("compare.importFile")}</div>
        <div class="list">
          {#each builds as b (b.fullFileName)}
            <button class="frow" onclick={() => loadFile(b)}>{b.buildName ?? b.fileName}</button>
          {/each}
        </div>
        <div class="dlg-actions"><button class="btn ghost" onclick={() => (picker = false)}>{t("tree.cancel")}</button></div>
      </div>
    </div>
  {/if}
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .bar,
  .tabs {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 12px;
    border-bottom: 1px solid var(--edge-0);
    flex-wrap: wrap;
  }
  .code {
    width: 260px;
  }
  .scroll {
    flex: 1;
    overflow: auto;
    min-height: 0;
  }
  .pad {
    padding: 12px;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--fs-sm);
  }
  th,
  td {
    padding: 2px 10px;
    text-align: left;
  }
  th.n,
  td.n {
    text-align: right;
  }
  tbody tr:hover {
    background: var(--surface-2);
  }
  tr.gap td {
    height: 8px;
  }
  tr.same td {
    opacity: 0.55;
  }
  .cmp {
    color: var(--accent, #7aa2ff);
  }
  .good {
    color: var(--good, #5adc8c);
  }
  .bad {
    color: var(--bad);
  }
  .cols {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
    padding: 12px;
  }
  .group {
    padding: 4px 0;
    border-bottom: 1px solid var(--edge-0);
  }
  .group.off {
    opacity: 0.5;
  }
  .gname {
    color: var(--ink-0);
  }
  .acts {
    display: flex;
    gap: 8px;
    margin-top: 10px;
  }
  .modal {
    position: fixed;
    inset: 0;
    z-index: 200;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 70%, transparent);
  }
  .dialog {
    width: min(520px, calc(100vw - 32px));
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: 8px;
  }
  .list {
    max-height: 360px;
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
    padding: 3px 10px;
  }
  .frow:hover {
    background: var(--surface-2);
  }
  .grow {
    flex: 1;
  }
</style>
