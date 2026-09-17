<!-- POB 的「Find a Timeless Jewel」:對話框由引擎那邊開著(不繪製),這裡只是
     操作它的控制項並顯示 build.timelessData 的搜尋結果。 -->
<script lang="ts">
  import { onDestroy, onMount } from "svelte";
  import { api, type TimelessState } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  let { onclose }: { onclose: () => void } = $props();

  let st = $state<TimelessState | null>(null);
  let busy = $state(false);
  let err = $state<string | null>(null);
  let searchList = $state("");
  let fallbackList = $state("");
  let minWeight = $state("");

  async function run<T>(fn: () => Promise<TimelessState>) {
    busy = true;
    err = null;
    try {
      st = await fn();
      searchList = st.searchList ?? "";
      fallbackList = st.searchListFallback ?? "";
      minWeight = st.totalMinimumWeight ?? "";
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
    busy = false;
  }

  onMount(() => run(() => api.tjOpen()));
  onDestroy(() => {
    void api.tjClose().catch(() => {});
  });

  const set = (p: Parameters<typeof api.tjSet>[0]) => run(() => api.tjSet(p));
  const pct = (v?: number) => Math.round((v ?? 0) * 100);

  async function copyJewel(index: number) {
    const r = await app.run(() => api.tjResult(index));
    if (r && (await copyText(r.raw))) app.notice = t("tj.copied");
  }
  async function socketJewel(index: number) {
    const r = await app.run(() => api.tjResult(index, "socket"));
    if (r) {
      await app.afterTreeChange();
      onclose();
    }
  }
</script>

<div class="modal">
  <div class="dialog">
    <div class="top">
      <span class="label">{t("tj.title")}</span>
      {#if busy}<span class="dim pulse">{t("tree.powerWorking")}</span>{/if}
      <span class="grow"></span>
      <button class="btn ghost sm" onclick={onclose}>{t("tree.done")}</button>
    </div>
    {#if err}<p class="bad">{err}</p>{/if}
    {#if st}
      <div class="grid">
        <span class="k">{t("tj.jewel")}</span>
        <select class="select sm" value={st.jewel?.sel} disabled={busy} onchange={(e) => set({ jewel: Number(e.currentTarget.value) })}>
          {#each st.jewel?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        <span class="k">{t("tj.conqueror")}</span>
        <select class="select sm" value={st.conqueror?.sel} disabled={busy} onchange={(e) => set({ conqueror: Number(e.currentTarget.value) })}>
          {#each st.conqueror?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        <span class="k">{t("tj.socket")}</span>
        <select class="select sm" value={st.socket?.sel} disabled={busy} onchange={(e) => set({ socket: Number(e.currentTarget.value) })}>
          {#each st.socket?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        {#if st.devotion1?.shown}
          <span class="k">{t("tj.devotion")}</span>
          <span class="two">
            <select class="select sm" value={st.devotion1.sel} disabled={busy} onchange={(e) => set({ devotion1: Number(e.currentTarget.value) })}>
              {#each st.devotion1.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
            </select>
            <select class="select sm" value={st.devotion2?.sel} disabled={busy} onchange={(e) => set({ devotion2: Number(e.currentTarget.value) })}>
              {#each st.devotion2?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
            </select>
          </span>
        {/if}
        {#if st.abyssAscendancy?.shown}
          <span class="k">{t("tj.abyssAsc")}</span>
          <select class="select sm" value={st.abyssAscendancy.sel} disabled={busy} onchange={(e) => set({ abyssAscendancy: Number(e.currentTarget.value) })}>
            {#each st.abyssAscendancy.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
          </select>
        {/if}
        <span class="k">{t("tj.node")}</span>
        <select class="select sm" value={st.node?.sel} disabled={busy} onchange={(e) => set({ node: Number(e.currentTarget.value) })}>
          {#each st.node?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
        </select>
        <span class="k">{t("tj.weights")}</span>
        <span class="sliders">
          <label class="sl">{t("tj.primary")}<input type="range" min="0" max="1" step="0.01" value={st.weights?.primary ?? 1} disabled={busy} onchange={(e) => set({ primary: Number(e.currentTarget.value) })} /><span class="num">{st.weights?.primaryLabel ?? `${pct(st.weights?.primary)}%`}</span></label>
          <label class="sl">{t("tj.secondary")}<input type="range" min="0" max="1" step="0.01" value={st.weights?.secondary ?? 1} disabled={busy} onchange={(e) => set({ secondary: Number(e.currentTarget.value) })} /><span class="num">{st.weights?.secondaryLabel ?? `${pct(st.weights?.secondary)}%`}</span></label>
          <label class="sl">{t("tj.minimum")}<input type="range" min="0" max="1" step="0.01" value={st.weights?.minimum ?? 0} disabled={busy} onchange={(e) => set({ minimum: Number(e.currentTarget.value) })} /><span class="num">{st.weights?.minimumLabel ?? `${pct(st.weights?.minimum)}%`}</span></label>
        </span>
        <span class="k">{t("tj.options")}</span>
        <span class="opts">
          {#if st.filterNodes?.shown}<label class="chk"><input type="checkbox" checked={st.filterNodes.state} disabled={busy} onchange={(e) => set({ filterNodes: e.currentTarget.checked })} /> {t("tj.filterNodes")}</label>{/if}
          {#if st.socketJewel?.shown}<label class="chk"><input type="checkbox" checked={st.socketJewel.state} disabled={busy} onchange={(e) => set({ socketJewel: e.currentTarget.checked })} /> {t("tj.socketJewel")}</label>{/if}
          {#if st.protectAllocated?.shown}<label class="chk"><input type="checkbox" checked={st.protectAllocated.state} disabled={busy} onchange={(e) => set({ protectAllocated: e.currentTarget.checked })} /> {t("tj.protect")}</label>{/if}
          {#if st.nodeDistance?.shown}
            <label class="sl">{t("tj.distance")}<input type="range" min="0" max="1" step="0.05" value={st.nodeDistance.value ?? 0} disabled={busy} onchange={(e) => set({ nodeDistance: Number(e.currentTarget.value) })} /></label>
          {/if}
        </span>
        <span class="k">{t("tj.minTotal")}</span>
        <span class="two">
          <input class="input sm num" bind:value={minWeight} onchange={() => set({ totalMinimumWeight: minWeight })} />
          <select class="select sm" value={st.fallbackWeights?.sel} disabled={busy} onchange={(e) => set({ fallbackWeights: Number(e.currentTarget.value) })}>
            {#each st.fallbackWeights?.options ?? [] as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
          </select>
          <button class="btn sm" disabled={busy} onclick={() => set({ generateFallback: true })}>{t("tj.generate")}</button>
        </span>
        <span class="k">{t("tj.searchList")}</span>
        <textarea class="input area" rows="4" bind:value={searchList} onchange={() => set({ searchList })}></textarea>
        <span class="k">{t("tj.fallbackList")}</span>
        <textarea class="input area" rows="3" bind:value={fallbackList} onchange={() => set({ searchListFallback: fallbackList })}></textarea>
      </div>
      <div class="acts">
        <button class="btn" disabled={busy} onclick={() => set({ reset: true })}>{t("tj.reset")}</button>
        <span class="grow"></span>
        <span class="dim">{t("tj.results", { n: st.resultCount ?? 0 })}</span>
        <button class="btn primary" disabled={busy} onclick={() => run(() => api.tjSearch())}>{t("tj.search")}</button>
      </div>
      {#if st.results?.length}
        <div class="results">
          {#each st.results as r (r.index)}
            <div class="row">
              <span class="seed">{r.label}</span>
              <span class="dim">{r.socketLabel ?? ""}</span>
              <span class="grow"></span>
              <span class="num">{Math.round(r.total)}</span>
              <button class="btn ghost sm" onclick={() => copyJewel(r.index)}>{t("tj.copy")}</button>
              <button class="btn ghost sm" onclick={() => socketJewel(r.index)}>{t("tj.socketIt")}</button>
            </div>
          {/each}
        </div>
      {/if}
    {/if}
  </div>
</div>

<style>
  .modal {
    position: fixed;
    inset: 0;
    z-index: 230;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 72%, transparent);
  }
  .dialog {
    width: min(960px, calc(100vw - 24px));
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
  .top {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .grid {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 6px 10px;
    align-items: center;
  }
  .two,
  .opts,
  .sliders {
    display: flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
  }
  .sl {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: var(--fs-sm);
    color: var(--ink-2);
  }
  .acts {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .results {
    max-height: 260px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 2px 8px;
  }
  .row:hover {
    background: var(--surface-2);
  }
  .seed {
    font-family: var(--font-mono, monospace);
  }
  .grow {
    flex: 1;
  }
</style>
