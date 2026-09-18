<!-- POB 的交易面板(TradeQuery:PriceItem):每個裝備欄一列,可產生加權搜尋、
     貼上交易網址查價、挑結果、匯入物品或取得密語。面板本體在引擎那邊開著。
     「找最佳」會開 POB 自己的「查詢選項」對話框;結果清單 hover 時顯示 POB
     的物品 tooltip(含「裝上去會給你」的差異)。 -->
<script lang="ts">
  import { onDestroy, onMount } from "svelte";
  import { api, type TradeMods, type TradeOptionControl, type TradeOptions, type TradeResultTooltip, type TradeState } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import TooltipCard from "./TooltipCard.svelte";

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

  // --- POB's "Query Options" dialog ("Find best" opens it) --------------------
  let opts = $state<TradeOptions | null>(null);
  // the two buttons at its bottom are ours; every other control is POB's
  const skipControl = (name: string) => name === "generateQuery" || name === "cancel" || /^mod\w*SelectorClear\d+$/.test(name);
  const optShown = $derived((opts?.controls ?? []).filter((c) => !(c.kind === "button" && skipControl(c.name))));
  // controls POB anchors to the right of another one belong on its row
  const optRows = $derived(optShown.filter((c) => !c.after || !optShown.some((o) => o.name === c.after)));
  const optExtra = (name: string) => optShown.filter((c) => c.after === name);
  // POB's own caption for the row, then the control's own label, then its name
  const optName = (c: { captionZh?: string; caption?: string; labelZh?: string; label?: string; name: string }) =>
    c.captionZh || c.caption || c.labelZh || c.label || c.name;

  async function findBest(row: number) {
    busy = row;
    err = null;
    try {
      opts = await api.tradeOptionsOpen(row);
      modPicker = null;
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
    busy = null;
  }
  async function optSet(p: Parameters<typeof api.tradeOptionsSet>[0]) {
    try {
      opts = await api.tradeOptionsSet(p);
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  }
  async function optExecute() {
    const row = opts?.row ?? 0;
    opts = null;
    modPicker = null;
    await run(() => api.tradeOptionsExecute(), row);
  }
  async function optCancel() {
    opts = null;
    modPicker = null;
    await run(() => api.tradeOptionsCancel());
  }

  // one mod selector's list: thousands of entries, so the engine searches
  let modPicker = $state<{ prefix: string; row: number; query: string } | null>(null);
  let modList = $state<TradeMods | null>(null);
  let modTimer = 0;
  function openModPicker(prefix: string, row: number) {
    modPicker = { prefix, row, query: "" };
    modList = null;
    void loadMods();
  }
  async function loadMods() {
    if (!modPicker) return;
    const want = modPicker;
    try {
      const r = await api.tradeOptionsMods({ prefix: want.prefix, query: want.query, limit: 60 });
      if (modPicker === want || (modPicker && modPicker.prefix === want.prefix && modPicker.row === want.row)) modList = r;
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  }
  function modQuery(q: string) {
    if (!modPicker) return;
    modPicker = { ...modPicker, query: q };
    clearTimeout(modTimer);
    modTimer = window.setTimeout(loadMods, 160);
  }
  async function pickMod(index: number) {
    const p = modPicker;
    if (!p) return;
    modPicker = null;
    await optSet({ mod: { prefix: p.prefix, row: p.row, sel: index } });
  }

  // --- the result tooltip (POB's own: the item plus the stat difference) -----
  let tip = $state<{ lines: TradeResultTooltip["lines"]; color?: string; x: number; y: number } | null>(null);
  let tipTimer = 0;
  const tipCache = new Map<string, TradeResultTooltip>();
  function showTip(row: number, index: number, e: MouseEvent) {
    clearTimeout(tipTimer);
    const x = Math.round(Math.min(e.clientX + 18, window.innerWidth - 380));
    const y = Math.round(Math.min(e.clientY + 12, window.innerHeight - 320));
    const key = `${row}:${index}`;
    const hit = tipCache.get(key);
    if (hit) {
      tip = { lines: hit.lines, color: hit.color, x, y };
      return;
    }
    tipTimer = window.setTimeout(async () => {
      try {
        const r = await api.tradeResultTooltip(row, index);
        tipCache.set(key, r);
        tip = { lines: r.lines, color: r.color, x, y };
      } catch {
        tip = null;
      }
    }, 120);
  }
  function hideTip() {
    clearTimeout(tipTimer);
    tip = null;
  }

  // the results of a row as a list of our own, so hovering one can show POB's
  // tooltip for it (a native <select> gives no hover of its options)
  let openRow = $state<number | null>(null);
  function toggleRow(row: number) {
    openRow = openRow === row ? null : row;
    hideTip();
  }
  async function pickResult(row: number, index: number) {
    openRow = null;
    hideTip();
    tipCache.clear();
    await run(() => api.tradePick(row, index), row);
  }
  function resultLabel(r: NonNullable<TradeState["rows"]>[number]): string {
    const hit = r.results.find((x) => x.index === r.selected) ?? r.results[0];
    return hit ? hit.labelZh || hit.label : "";
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
      {#if st.timedOut}<p class="notice">{t("trade.stillRunning")}</p>{/if}
      <div class="rows">
        {#each st.rows ?? [] as r (r.index)}
          <div class="row">
            <span class="slot" class:unique={r.unique}>{r.nameZh || r.name}</span>
            <button class="btn sm" disabled={busy !== null || !r.canFindBest} title={t("trade.findBestHint")} onclick={() => findBest(r.index)}>{t("trade.findBest")}</button>
            <input class="input sm url" placeholder={t("trade.urlHint")} bind:value={urls[r.index]} onchange={() => set({ url: { row: r.index, text: urls[r.index] } }, r.index)} />
            <button class="btn ghost sm" disabled={!urls[r.index]} title={t("trade.openUrl")} onclick={() => openUrl(urls[r.index])}>↗</button>
            <button class="btn sm" disabled={busy !== null || !r.canPrice} onclick={() => run(() => api.tradePrice(r.index), r.index)}>{t("trade.price")}</button>
            {#if r.hasResults}
              <div class="results">
                <button
                  class="combo"
                  disabled={busy !== null}
                  title={t("trade.resultHint")}
                  onclick={() => toggleRow(r.index)}
                  onmouseenter={(e) => showTip(r.index, r.selected, e)}
                  onmouseleave={hideTip}
                >
                  <span class="txt">{resultLabel(r)}</span>
                  <span class="caret">▾</span>
                </button>
                {#if openRow === r.index}
                  <ul class="list">
                    {#each r.results as res (res.index)}
                      <li>
                        <button
                          class="opt"
                          class:on={res.index === r.selected}
                          onclick={() => pickResult(r.index, res.index)}
                          onmouseenter={(e) => showTip(r.index, res.index, e)}
                          onmouseleave={hideTip}
                        >
                          {res.labelZh || res.label}
                        </button>
                      </li>
                    {/each}
                  </ul>
                {/if}
              </div>
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

{#if opts}
  <div class="modal opts">
    <div class="dialog narrow">
      <div class="top">
        <span class="label">{t("trade.options")}</span>
        <span class="grow"></span>
        <button class="btn ghost sm" onclick={optCancel}>{t("tree.cancel")}</button>
        <button class="btn primary sm" onclick={optExecute}>{t("trade.execute")}</button>
      </div>
      <p class="dim small">{t("trade.optionsHint")}</p>
      <div class="olist">
        {#snippet widget(c: TradeOptionControl)}
          {#if c.kind === "check"}
            <input type="checkbox" checked={c.state} disabled={!c.enabled} onchange={(e) => optSet({ values: { [c.name]: e.currentTarget.checked } })} />
          {:else if c.kind === "dropdown"}
            <select class="select sm" value={c.sel} disabled={!c.enabled} onchange={(e) => optSet({ values: { [c.name]: Number(e.currentTarget.value) } })}>
              {#each c.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
            </select>
          {:else if c.kind === "edit"}
            <input class="input sm" value={c.text ?? ""} disabled={!c.enabled} onchange={(e) => optSet({ values: { [c.name]: e.currentTarget.value } })} />
          {:else if c.kind === "label"}
            <span class="oval">{c.labelZh || c.label}</span>
          {/if}
        {/snippet}
        {#each optRows as c (c.name)}
          {#if c.kind === "mod"}
            <div class="orow mod">
              <button class="combo grow" onclick={() => openModPicker(c.prefix!, c.row!)}>
                <span class="txt">{c.labelZh || c.label || t("trade.modSearch")}</span>
                <span class="caret">▾</span>
              </button>
              {#if c.sel && c.sel > 1}
                <input
                  class="input sm num min"
                  placeholder={t("trade.modMin")}
                  value={c.min ?? ""}
                  onchange={(e) => optSet({ mod: { prefix: c.prefix!, row: c.row!, min: e.currentTarget.value } })}
                />
                <button class="btn ghost sm" title={t("trade.modClear")} onclick={() => optSet({ mod: { prefix: c.prefix!, row: c.row!, sel: 1 } })}>✕</button>
              {/if}
            </div>
          {:else}
            <div class="orow" class:label-only={c.kind === "label"}>
              <span class="oname" title={c.tooltipZh || c.tooltip}>{c.kind === "label" ? c.captionZh || c.caption || "" : optName(c)}</span>
              {@render widget(c)}
              {#each optExtra(c.name) as x (x.name)}{@render widget(x)}{/each}
            </div>
          {/if}
        {/each}
      </div>
      {#if modPicker}
        <div class="picker">
          <input
            class="input sm"
            placeholder={t("trade.modSearch")}
            value={modPicker.query}
            oninput={(e) => modQuery(e.currentTarget.value)}
          />
          <ul class="list">
            {#each modList?.mods ?? [] as m (m.index)}
              <li><button class="opt" onclick={() => pickMod(m.index)}>{m.labelZh || m.label}</button></li>
            {/each}
          </ul>
          {#if modList && modList.total > modList.mods.length}
            <p class="dim small">{t("trade.modMore", { n: modList.total - modList.mods.length })}</p>
          {/if}
          <div class="top">
            <span class="grow"></span>
            <button class="btn ghost sm" onclick={() => (modPicker = null)}>{t("tree.cancel")}</button>
          </div>
        </div>
      {/if}
    </div>
  </div>
{/if}

{#if tip}<TooltipCard lines={tip.lines} accent={tip.color} x={tip.x} y={tip.y} width={360} />{/if}

<style>
  .modal {
    position: fixed;
    inset: 0;
    z-index: 240;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 74%, transparent);
  }
  .modal.opts {
    z-index: 250;
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
  .dialog.narrow {
    width: min(560px, calc(100vw - 24px));
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
    position: relative;
    flex: 1;
    min-width: 160px;
  }
  .combo {
    display: flex;
    align-items: center;
    gap: 6px;
    width: 100%;
    padding: 3px 8px;
    font: inherit;
    font-size: var(--fs-sm);
    color: inherit;
    text-align: left;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s, 4px);
    cursor: pointer;
  }
  .combo:disabled {
    opacity: 0.6;
    cursor: default;
  }
  .combo .txt {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .list {
    position: absolute;
    z-index: 30;
    left: 0;
    right: 0;
    top: calc(100% + 2px);
    max-height: 320px;
    overflow: auto;
    margin: 0;
    padding: 0;
    list-style: none;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s, 4px);
    box-shadow: var(--shadow-float);
  }
  .opt {
    display: block;
    width: 100%;
    padding: 3px 8px;
    font: inherit;
    font-size: var(--fs-sm);
    color: inherit;
    text-align: left;
    background: none;
    border: 0;
    cursor: pointer;
  }
  .opt:hover {
    background: var(--surface-3, var(--surface-1));
  }
  .opt.on {
    color: var(--gold);
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
  .olist {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .orow {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .orow .oname {
    flex: 1;
    min-width: 0;
    font-size: var(--fs-sm);
  }
  .orow.label-only .oname {
    color: var(--ink-1);
  }
  .orow .oval {
    font-size: var(--fs-sm);
  }
  .orow.mod {
    position: relative;
  }
  .min {
    width: 72px;
  }
  .picker {
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 8px;
    border: 1px solid var(--edge-1);
    border-radius: 6px;
  }
  .picker .list {
    position: static;
    max-height: 240px;
    box-shadow: none;
  }
</style>
