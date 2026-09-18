<!-- POB 的交易面板(TradeQuery:PriceItem):每個裝備欄一列,可產生加權搜尋、
     貼上交易網址查價、挑結果、匯入物品或取得密語。面板本體在引擎那邊開著。 -->
<script lang="ts">
  import { onDestroy, onMount } from "svelte";
  import { api, type TradeState } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  let { onclose }: { onclose: () => void } = $props();

  let st = $state<TradeState | null>(null);
  let busy = $state<number | null>(null);
  let err = $state<string | null>(null);
  let urls = $state<Record<number, string>>({});

  async function run(fn: () => Promise<TradeState>, row?: number) {
    busy = row ?? 0;
    err = null;
    try {
      st = await fn();
      for (const r of st.rows ?? []) if (urls[r.index] == null) urls[r.index] = r.url;
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
    busy = null;
  }

  // the realm and league lists arrive from the site; keep asking while empty
  let poll: ReturnType<typeof setInterval> | undefined;
  onMount(async () => {
    await run(() => api.tradeOpen());
    poll = setInterval(async () => {
      if (!st || busy !== null) return;
      if ((st.league?.options.length ?? 0) > 0) {
        clearInterval(poll);
        poll = undefined;
        return;
      }
      try {
        st = await api.tradeRefresh(1);
      } catch {
        /* keep the pane usable */
      }
    }, 2000);
  });
  onDestroy(() => {
    if (poll) clearInterval(poll);
    void api.tradeClose().catch(() => {});
  });

  const set = (p: Record<string, unknown>, row?: number) => run(() => api.tradeSet(p), row);

  async function whisper(row: number) {
    const r = await app.run(() => api.tradeWhisper(row));
    if (r?.text && (await copyText(r.text))) app.notice = t("trade.whisperCopied");
  }
  async function importItem(row: number) {
    busy = row;
    try {
      const r = await api.tradeImport(row);
      st = r.state;
      await app.refresh();
      app.notice = t("trade.imported");
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
    busy = null;
  }
  // POB's "Adjust search weights" (the stat multipliers its Find best uses)
  let weights = $state<{ stats: { stat: string; label: string; labelZh?: string; weight: number }[]; selected: { stat: string; label: string; labelZh?: string; weight: number }[] } | null>(null);
  let weightsOpen = $state(false);
  let draft = $state<Record<string, number>>({});
  async function openWeights() {
    weightsOpen = true;
    try {
      weights = await api.tradeWeights();
      draft = {};
      for (const s of weights.selected) draft[s.stat] = s.weight;
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  }
  async function saveWeights() {
    const list = Object.entries(draft)
      .filter(([, w]) => w > 0)
      .map(([stat, weight]) => ({ stat, weight }));
    try {
      weights = await api.tradeWeights({ weights: list });
      weightsOpen = false;
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  }

  function openUrl(url: string) {
    if (url) window.open(url, "_blank", "noopener");
  }
</script>

<div class="modal">
  <div class="dialog">
    <div class="top">
      <span class="label">{t("trade.title")}</span>
      {#if st}
        <span class="dim">{st.authLabel ?? ""}</span>
        <button class="btn sm" disabled={busy !== null} onclick={() => run(() => api.tradeAuth())}>{st.authenticated ? t("trade.logout") : t("trade.login")}</button>
      {/if}
      <span class="grow"></span>
      {#if busy !== null}<span class="dim pulse">{t("tree.powerWorking")}</span>{/if}
      <button class="btn ghost sm" onclick={onclose}>{t("tree.done")}</button>
    </div>
    {#if err}<p class="bad">{err}</p>{/if}
    {#if st}
      <p class="dim small">{t("trade.hint")}</p>
      <div class="bar">
        <span class="k">{t("trade.realm")}</span>
        <select class="select sm" value={st.realm?.sel} disabled={busy !== null} onchange={(e) => set({ realm: Number(e.currentTarget.value) })}>
          {#each st.realm?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        <span class="k">{t("trade.league")}</span>
        <select class="select sm" value={st.league?.sel} disabled={busy !== null} onchange={(e) => set({ league: Number(e.currentTarget.value) })}>
          {#each st.league?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        <span class="k">{t("trade.tradeType")}</span>
        <select class="select sm" value={st.tradeType?.sel} disabled={busy !== null} onchange={(e) => set({ tradeType: Number(e.currentTarget.value) })}>
          {#each st.tradeType?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        <span class="k">{t("trade.sort")}</span>
        <select class="select sm" value={st.sort?.sel} disabled={busy !== null} onchange={(e) => set({ sort: Number(e.currentTarget.value) })}>
          {#each st.sort?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        <button class="btn sm" disabled={busy !== null} onclick={openWeights}>{t("trade.weights")}</button>
        <span class="k">{t("trade.pages")}</span>
        <input class="input sm num pages" value={st.fetchPages ?? ""} disabled={busy !== null} onchange={(e) => set({ fetchPages: Number(e.currentTarget.value) })} />
      </div>
      {#if st.notice}<p class="notice">{st.notice}</p>{/if}
      <div class="rows">
        {#each st.rows ?? [] as r (r.index)}
          <div class="row">
            <span class="slot" class:unique={r.unique}>{r.nameZh || r.name}</span>
            <button class="btn sm" disabled={busy !== null || !r.canFindBest} title={t("trade.findBestHint")} onclick={() => run(() => api.tradeFindBest(r.index), r.index)}>{t("trade.findBest")}</button>
            <input class="input sm url" placeholder={t("trade.urlHint")} bind:value={urls[r.index]} onchange={() => set({ url: { row: r.index, text: urls[r.index] } }, r.index)} />
            <button class="btn ghost sm" disabled={!urls[r.index]} title={t("trade.openUrl")} onclick={() => openUrl(urls[r.index])}>↗</button>
            <button class="btn sm" disabled={busy !== null || !r.canPrice} onclick={() => run(() => api.tradePrice(r.index), r.index)}>{t("trade.price")}</button>
            {#if r.hasResults}
              <select class="select sm results" value={r.selected} disabled={busy !== null} onchange={(e) => run(() => api.tradePick(r.index, Number(e.currentTarget.value)), r.index)}>
                {#each r.results as res}<option value={res.index}>{res.labelZh || res.label}</option>{/each}
              </select>
              <button class="btn sm" disabled={busy !== null} onclick={() => importItem(r.index)}>{t("trade.import")}</button>
              <button class="btn ghost sm" disabled={busy !== null} onclick={() => whisper(r.index)}>{t("trade.whisper")}</button>
              <button class="btn ghost sm" disabled={busy !== null} onclick={() => run(() => api.tradeReset(r.index), r.index)}>{t("trade.again")}</button>
            {/if}
          </div>
        {/each}
      </div>
      {#if st.totalPrice}<p class="total">{st.totalPrice}</p>{/if}
      {#if weightsOpen && weights}
        <div class="weights">
          <div class="top">
            <span class="label">{t("trade.weightsTitle")}</span>
            <span class="grow"></span>
            <button class="btn ghost sm" onclick={() => (weightsOpen = false)}>{t("tree.cancel")}</button>
            <button class="btn primary sm" onclick={saveWeights}>{t("bar.save")}</button>
          </div>
          <div class="wlist">
            {#each weights.stats as s (s.stat)}
              <label class="wrow">
                <span class="wname">{s.labelZh || s.label}</span>
                <input type="range" min="0" max="1" step="0.05" value={draft[s.stat] ?? 0} onchange={(e) => (draft[s.stat] = Number(e.currentTarget.value))} />
                <span class="num">{(draft[s.stat] ?? 0).toFixed(2)}</span>
              </label>
            {/each}
          </div>
        </div>
      {/if}
    {/if}
  </div>
</div>

<style>
  .modal {
    position: fixed;
    inset: 0;
    z-index: 240;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 74%, transparent);
  }
  .dialog {
    width: min(1180px, calc(100vw - 24px));
    max-height: calc(100vh - 24px);
    overflow: auto;
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: 8px;
  }
  .top,
  .bar {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }
  .rows {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .slot {
    width: 150px;
    flex: 0 0 150px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .slot.unique {
    color: var(--c-unique, var(--gold));
  }
  .url {
    flex: 1;
    min-width: 120px;
  }
  .results {
    flex: 1;
    min-width: 160px;
  }
  .pages {
    width: 70px;
  }
  .notice {
    color: var(--warn);
  }
  .total {
    text-align: right;
  }
  .grow {
    flex: 1;
  }
  .weights {
    border: 1px solid var(--edge-1);
    border-radius: 6px;
    padding: 10px;
  }
  .wlist {
    max-height: 260px;
    overflow: auto;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 4px 12px;
  }
  .wrow {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: var(--fs-sm);
  }
  .wname {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
