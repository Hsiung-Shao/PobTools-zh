<!-- 傳奇 / 稀有範本資料庫:POB ItemDBControl 的七組篩選(部位、類型、聯盟、需求、
     可取得、搜尋模式、排序)由 bridge 讀它的下拉清單給我們,篩選判定與排序也是它的;
     數值排序超過上限時 bridge 明講「太多」。列:hover 看 tooltip(含差異)、
     新增 / 新增並裝備、雙擊編輯副本、Ctrl+點擊新增並裝備。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type ItemDbOptions, type ItemDbPage, type ItemSummary } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { rarityColor } from "$lib/items";
  import { app } from "$lib/state.svelte";

  let {
    onadd,
    onedit,
    ontip,
    onleave,
  }: {
    onadd: (it: ItemSummary, equip: boolean) => void;
    onedit: (it: ItemSummary) => void;
    ontip: (raw: string, rarity: string, e: MouseEvent) => void;
    onleave: () => void;
  } = $props();

  let kind = $state<"unique" | "rare">("unique");
  const opts = $state<Partial<Record<"unique" | "rare", ItemDbOptions>>>({});
  let sel = $state({ slot: 1, type: 1, league: 1, requirement: 1, obtainable: 1, searchMode: 1 });
  let query = $state("");
  let sortMode = $state("");
  let page = $state(1);
  let db = $state<ItemDbPage | null>(null);
  let timer = 0;
  const SIZE = 40;

  const o = $derived(opts[kind]);

  async function loadOptions(k: "unique" | "rare") {
    if (opts[k]) return;
    const r = await app.run(() => api.itemDbOptions(k));
    if (r) opts[k] = r;
  }
  function search(resetPage = true) {
    clearTimeout(timer);
    if (resetPage) page = 1;
    timer = window.setTimeout(async () => {
      const r = await app.run(() => api.itemDb({ kind, ...sel, query: query.trim(), sortMode: kind === "unique" ? sortMode : undefined, page, size: SIZE }));
      if (r) db = r;
    }, 150);
  }
  async function switchKind(k: "unique" | "rare") {
    kind = k;
    sel = { slot: 1, type: 1, league: 1, requirement: 1, obtainable: 1, searchMode: 1 };
    db = null;
    await loadOptions(k);
    search();
  }
  $effect(() => {
    untrack(() => void loadOptions(kind).then(() => search()));
  });

  const power = (v: number | undefined) => (v == null || !Number.isFinite(v) ? "" : Math.abs(v) >= 1000 ? Math.round(v).toLocaleString() : v.toFixed(2));
  function rowClick(e: MouseEvent, it: ItemSummary) {
    if (e.ctrlKey) onadd(it, true);
  }
</script>

<div class="pane">
  <div class="bar">
    <span class="seg">
      <button class:on={kind === "unique"} onclick={() => switchKind("unique")}>{t("items.dbUnique")}</button>
      <button class:on={kind === "rare"} onclick={() => switchKind("rare")}>{t("items.dbRare")}</button>
    </span>
    <input class="input sm grow" placeholder={t("items.dbSearch")} bind:value={query} oninput={() => search()} />
    {#if o}
      <select class="select sm" bind:value={sel.searchMode} onchange={() => search()}>
        {#each o.searchMode as x, i}<option value={i + 1}>{x.labelZh || x.label}</option>{/each}
      </select>
    {/if}
  </div>
  {#if o}
    <div class="bar wrap">
      <select class="select sm" bind:value={sel.slot} onchange={() => search()}>
        {#each o.slot as x, i}<option value={i + 1}>{x.labelZh || x.label}</option>{/each}
      </select>
      <select class="select sm" bind:value={sel.type} onchange={() => search()}>
        {#each o.type as x, i}<option value={i + 1}>{x.labelZh || x.label}</option>{/each}
      </select>
      {#if o.league}
        <select class="select sm" bind:value={sel.league} onchange={() => search()}>
          {#each o.league as x, i}<option value={i + 1}>{x.labelZh || x.label}</option>{/each}
        </select>
      {/if}
      {#if o.requirement}
        <select class="select sm" bind:value={sel.requirement} onchange={() => search()}>
          {#each o.requirement as x, i}<option value={i + 1}>{x.labelZh || x.label}</option>{/each}
        </select>
      {/if}
      {#if o.obtainable}
        <select class="select sm" bind:value={sel.obtainable} onchange={() => search()}>
          {#each o.obtainable as x, i}<option value={i + 1}>{x.labelZh || x.label}</option>{/each}
        </select>
      {/if}
      {#if o.sort}
        <select class="select sm" bind:value={sortMode} onchange={() => search()}>
          <option value="">{o.sort[0]?.labelZh || o.sort[0]?.label}</option>
          {#each o.sort.slice(1) as x}<option value={x.sortMode}>{x.labelZh || x.label}</option>{/each}
        </select>
      {/if}
    </div>
  {/if}
  <p class="dim small hint">{t("items.dbHint")}</p>
  <div class="scroll">
    {#if db?.tooMany}
      <p class="warn">{t("items.dbTooMany", { n: db.total, max: db.max ?? 300 })}</p>
    {:else if db}
      {#each db.items as it (it.name)}
        <!-- svelte-ignore a11y_click_events_have_key_events a11y_no_static_element_interactions -->
        <div class="row" role="option" aria-selected="false" tabindex="0" onmouseenter={(e) => ontip(it.raw!, it.rarity, e)} onmouseleave={onleave} onclick={(e) => rowClick(e, it)} ondblclick={() => onedit(it)}>
          <span class="nm" style:color={rarityColor(it.rarity)}>{it.nameZh || it.name}</span>
          {#if db.statSort}<span class="num dim small">{power(it.measuredPower)}</span>{/if}
          <span class="dim small">{it.typeZh || it.type || ""}</span>
          <button class="btn ghost sm" title={t("items.dbEdit")} onclick={(e) => { e.stopPropagation(); onedit(it); }}>✎</button>
          <button class="btn ghost sm" onclick={(e) => { e.stopPropagation(); onadd(it, false); }}>{t("items.dbAdd")}</button>
        </div>
      {/each}
    {/if}
  </div>
  {#if db && !db.tooMany}
    <div class="pager">
      <span class="dim">{t("items.dbTotal", { n: db.total })}</span>
      <span class="grow"></span>
      <button class="btn ghost sm" disabled={page <= 1} onclick={() => { page--; search(false); }}>‹</button>
      <span class="num dim">{page} / {Math.max(1, Math.ceil(db.total / db.size))}</span>
      <button class="btn ghost sm" disabled={page * db.size >= db.total} onclick={() => { page++; search(false); }}>›</button>
    </div>
  {/if}
</div>

<style>
  .pane {
    display: flex;
    flex-direction: column;
    min-height: 0;
    height: 100%;
  }
  .bar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 10px 0;
  }
  .bar.wrap {
    flex-wrap: wrap;
  }
  .bar.wrap .select {
    flex: 1 1 120px;
    min-width: 0;
  }
  .seg {
    display: inline-flex;
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
    overflow: hidden;
    flex: none;
  }
  .seg button {
    appearance: none;
    border: 0;
    background: transparent;
    color: var(--ink-2);
    padding: 3px 8px;
    font-size: var(--fs-2xs);
  }
  .seg button.on {
    background: var(--gold-soft);
    color: var(--gold);
  }
  .select.sm,
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .grow {
    flex: 1;
    min-width: 0;
  }
  .hint {
    margin: 4px 10px 0;
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 6px 10px 12px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 3px 8px;
    border-radius: var(--radius-s);
    font-size: var(--fs-sm);
    cursor: default;
  }
  .row:hover {
    background: var(--surface-hover);
  }
  .nm {
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .row .btn {
    opacity: 0;
  }
  .row:hover .btn {
    opacity: 1;
  }
  .warn {
    color: var(--warn);
    font-size: var(--fs-xs);
    padding: 8px;
  }
  .pager {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 10px;
    border-top: 1px solid var(--edge-0);
    font-size: var(--fs-xs);
  }
</style>
