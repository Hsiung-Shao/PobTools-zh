<!-- 物品編輯面板:POB 的 displayItem 工作階段。上方是即時 tooltip,下方每個控制項
     都對應 ItemsTab 面板上同名的控制項(bridge 直接呼叫它們的回呼);附魔/塗油/腐化/
     新增詞綴/Crucible/編輯文字這些 POB 用 popup 做的,在這裡以內嵌小面板呈現,
     選項與按鈕都是 popup 自己的控制項。任何修改都不重算,按「儲存/新增」才進建置。 -->
<script lang="ts">
  import { api, type ItemEditPopup, type ItemEditPopupControl, type ItemEditPopupKind, type ItemEditSetParams, type ItemEditState, type TooltipLine } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { rarityColor } from "$lib/items";
  import { app } from "$lib/state.svelte";
  import PobText from "./PobText.svelte";

  let {
    item,
    report = null,
    onchange,
    onclose,
    ondone,
  }: {
    item: ItemEditState;
    report?: { untranslated: string[]; unsupported: string[] } | null;
    onchange: (s: ItemEditState) => void;
    onclose: () => void;
    ondone: (r: { id: number; added: boolean }) => void;
  } = $props();

  let popup = $state<ItemEditPopup | null>(null);
  let popupSearch = $state("");
  // the hovered notable's effect in the Anoint dialog (one calculation per node,
  // so it is fetched on hover the way POB builds its list tooltip)
  let nodeTip = $state<{ id: number; lines: TooltipLine[] } | null>(null);
  let nodeTipTimer = 0;
  let rawOpen = $state(false);
  let rawText = $state("");
  let err = $state<string | null>(null);

  // Narrow layout: the preview sits above the controls at a FIXED height and
  // scrolls inside itself, so changing an affix (which changes the tooltip's
  // length) never moves the controls under the pointer. The divider between
  // them drags; the height is kept per viewer.
  const PV_KEY = "pobtools.editorPreviewH";
  let previewH = $state(260);
  try {
    const v = Number(localStorage.getItem(PV_KEY));
    if (v >= 120 && v <= 900) previewH = v;
  } catch {
    /* private mode */
  }
  let workEl = $state<HTMLDivElement | null>(null);
  function dragSplit(e: PointerEvent) {
    const handle = e.currentTarget as HTMLElement;
    handle.setPointerCapture(e.pointerId);
    const y0 = e.clientY;
    const h0 = previewH;
    const max = Math.max(160, (workEl?.clientHeight ?? 600) - 140);
    const move = (ev: PointerEvent) => (previewH = Math.round(Math.min(max, Math.max(120, h0 + ev.clientY - y0))));
    const up = () => {
      handle.removeEventListener("pointermove", move);
      handle.removeEventListener("pointerup", up);
      try {
        localStorage.setItem(PV_KEY, String(previewH));
      } catch {
        /* private mode */
      }
    };
    handle.addEventListener("pointermove", move);
    handle.addEventListener("pointerup", up);
  }
  function nudgeSplit(e: KeyboardEvent) {
    if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
    e.preventDefault();
    previewH = Math.min(900, Math.max(120, previewH + (e.key === "ArrowDown" ? 24 : -24)));
    try {
      localStorage.setItem(PV_KEY, String(previewH));
    } catch {
      /* private mode */
    }
  }

  async function run<T>(fn: () => Promise<T>): Promise<T | undefined> {
    err = null;
    try {
      return await app.run(fn);
    } catch (e: any) {
      err = String(e?.message ?? e);
      return undefined;
    }
  }
  async function set(p: ItemEditSetParams) {
    const r = await run(() => api.itemEditSet(p));
    if (r) onchange(r);
  }
  async function affix(index: number, p: { sel?: number; roll?: number }) {
    const r = await run(() => api.itemEditAffix({ index, ...p }));
    if (r) onchange(r);
  }
  async function commit(equip: boolean) {
    const r = await run(() => api.itemEditCommit(equip));
    if (r) ondone({ id: r.id, added: r.added });
  }
  async function cancel() {
    await run(() => api.itemEditCancel());
    onclose();
  }

  // --- POB's dialogs, captured -------------------------------------------------
  async function openPopup(kind: ItemEditPopupKind, slot?: number) {
    popupSearch = "";
    nodeTip = null;
    const r = await run(() => api.itemEditPopup({ kind, action: "open", slot }));
    if (r) popup = r as ItemEditPopup;
  }
  async function pick(name: string, patch: { sel?: number; text?: string; state?: boolean; value?: number }) {
    const r = await run(() => api.itemEditPopup({ action: "pick", name, ...patch }));
    if (r) popup = r as ItemEditPopup;
  }
  async function applyPopup(button: string) {
    const r = await run(() => api.itemEditPopup({ action: "apply", button }));
    if (r && "tooltip" in r) {
      popup = null;
      onchange(r as ItemEditState);
    }
  }
  async function cancelPopup() {
    await run(() => api.itemEditPopup({ action: "cancel" }));
    popup = null;
    nodeTip = null;
  }
  function hoverNode(name: string, id: number | undefined) {
    clearTimeout(nodeTipTimer);
    if (id == null) return;
    nodeTipTimer = window.setTimeout(async () => {
      try {
        const r = await api.itemEditPopupTip(name, id);
        nodeTip = { id: r.id, lines: r.tooltip };
      } catch {
        /* the dialog moved on */
      }
    }, 180);
  }
  const isCloseButton = (name: string) => name === "close" || name === "cancel";
  const popupButtons = $derived(popup ? popup.controls.filter((c: ItemEditPopupControl) => c.kind === "button") : []);
  const popupFields = $derived(popup ? popup.controls.filter((c: ItemEditPopupControl) => c.kind !== "button") : []);
  const filterOpts = <T extends { label: string; labelZh: string }>(opts: T[], q: string) => {
    const f = q.trim().toLowerCase();
    return opts.map((o, i) => ({ o, i })).filter(({ o }) => !f || o.label.toLowerCase().includes(f) || o.labelZh.toLowerCase().includes(f));
  };

  // the socket dropdowns' colours as POB names them
  const socketColorName: Record<string, string> = { R: "var(--c-str, #c03030)", G: "var(--c-dex, #30a030)", B: "var(--c-int, #3060e0)", W: "var(--ink-0)", A: "var(--ink-3)" };

  const name = $derived(item.summary.nameZh || item.summary.name);
  const bigLine = (l: TooltipLine) => "size" in l && l.size >= 18;
</script>

{#snippet tipBody(lines: TooltipLine[])}
  {#each lines as l}
    {#if "sep" in l}
      <div class="sep"></div>
    {:else}
      <div class="line" class:center={l.center} class:big={bigLine(l)} class:small={l.size <= 12}><PobText text={l.text} /></div>
    {/if}
  {/each}
{/snippet}

<div class="editor">
  <div class="ehead">
    <span class="ename" style:color={rarityColor(item.summary.rarity)}>{item.isNew ? t("edit.new") : ""} {name}</span>
    <span class="grow"></span>
    {#if item.isNew}
      <button class="btn primary sm" disabled={app.busy > 0} onclick={() => commit(true)}>{t("edit.addEquip")}</button>
      <button class="btn sm" disabled={app.busy > 0} onclick={() => commit(false)}>{t("edit.addBuild")}</button>
    {:else}
      <button class="btn primary sm" disabled={app.busy > 0} onclick={() => commit(true)}>{t("edit.save")}</button>
    {/if}
    <button class="btn ghost sm" disabled={app.busy > 0} onclick={cancel}>{t("edit.cancel")}</button>
  </div>
  {#if err}<div class="bad selectable">{err}</div>{/if}
  {#if report && (report.untranslated.length || report.unsupported.length)}
    <details class="report">
      <summary>
        {#if report.untranslated.length}<span class="warn">{t("edit.pasteUntranslated", { n: report.untranslated.length })}</span>{/if}
        {#if report.unsupported.length}<span class="dim">{t("edit.pasteUnsupported", { n: report.unsupported.length })}</span>{/if}
      </summary>
      {#each report.untranslated as l}<div class="rline warn selectable">{l}</div>{/each}
      {#each report.unsupported as l}<div class="rline dim selectable">{l}</div>{/each}
    </details>
  {/if}

  <div class="ework" bind:this={workEl} style:--pv-h={`${previewH}px`}>
    <!-- 即時 tooltip:固定大小、自己捲動,改詞綴時不會把下方控制項推開 -->
    <div class="preview">
      <div class="tip pob-dark" style:border-top-color={item.tooltip.color ? undefined : "var(--edge-1)"}>
        {@render tipBody(item.tooltip.lines)}
      </div>
    </div>
    <!-- a focusable window splitter (ARIA separator with a value); Svelte does not know that pattern -->
    <!-- svelte-ignore a11y_no_noninteractive_tabindex, a11y_no_noninteractive_element_interactions -->
    <div class="split" role="separator" aria-orientation="horizontal" aria-label={t("edit.resizePreview")} aria-valuenow={previewH} aria-valuemin={120} aria-valuemax={900} tabindex="0" onpointerdown={dragSplit} onkeydown={nudgeSplit}></div>
  <div class="ebody">

    {#if popup}
      <!-- POB 的 popup 之一,內嵌呈現 -->
      <section class="pop">
        <div class="ptitle"><span class="label">{popup.title ? t(`edit.popup.${popup.popup}`) : ""}</span></div>
        {#each popupFields as c (c.name)}
          <div class="prow">
            <span class="k">{t(`edit.ctl.${c.name}`) === `edit.ctl.${c.name}` ? c.name : t(`edit.ctl.${c.name}`)}</span>
            {#if c.kind === "dropdown" && c.options}
              {#if c.options.length > 25}
                <input class="input sm" placeholder={t("edit.search")} bind:value={popupSearch} />
              {/if}
              <select class="select sm wide" disabled={!c.enabled} value={c.sel ?? 1} onchange={(e) => pick(c.name, { sel: Number(e.currentTarget.value) })}>
                {#each filterOpts(c.options, c.options.length > 25 ? popupSearch : "") as { o, i }}
                  <option value={i + 1}>{o.labelZh || o.label}</option>
                {/each}
              </select>
            {:else if c.kind === "edit"}
              <input class="input sm wide" value={c.text ?? ""} onchange={(e) => pick(c.name, { text: e.currentTarget.value })} />
            {:else if c.kind === "check"}
              <input type="checkbox" checked={!!c.state} disabled={!c.enabled} onchange={(e) => pick(c.name, { state: e.currentTarget.checked })} />
            {:else if c.kind === "slider"}
              <input class="slider" type="range" min="0" max="1" step="0.01" value={c.value ?? 0} onchange={(e) => pick(c.name, { value: Number(e.currentTarget.value) })} />
            {:else if c.kind === "nodes" && c.options}
              <div class="nodes">
                <input class="input sm" placeholder={t("edit.search")} bind:value={popupSearch} />
                <div class="nlist" onmouseleave={() => clearTimeout(nodeTipTimer)} role="presentation">
                  {#each filterOpts(c.options, popupSearch).slice(0, 200) as { o, i }}
                    <label class="nrow" class:on={c.sel === i + 1} onmouseenter={() => hoverNode(c.name, o.id)}>
                      <input type="radio" name="node" checked={c.sel === i + 1} onchange={() => pick(c.name, { value: o.id })} />
                      <span>{o.labelZh || o.label}</span>
                    </label>
                  {/each}
                </div>
                {#if nodeTip || c.tooltip}
                  <div class="tip node">{@render tipBody(nodeTip?.lines ?? c.tooltip ?? [])}</div>
                {/if}
              </div>
            {/if}
          </div>
        {/each}
        <div class="btns">
          {#each popupButtons as b (b.name)}
            {#if isCloseButton(b.name)}
              <button class="btn ghost sm" onclick={cancelPopup}>{t("edit.popupCancel")}</button>
            {:else}
              <button class="btn sm" class:primary={b.name === "save"} disabled={!b.enabled || app.busy > 0} onclick={() => applyPopup(b.name)}>
                <PobText text={b.labelZh || b.label} />
              </button>
            {/if}
          {/each}
        </div>
      </section>
    {:else}
      <!-- 基本:品質、符文/珠寶插槽數、催化劑、影響 —— 固定在控制項最上面 -->
      {#if item.quality.shown || item.runeSockets || item.jewelSockets || item.catalyst.shown || item.influence.shown}
        <section class="sec base">
          {#if item.quality.shown}
            <label class="fld"><span class="k">{t("edit.quality")}</span>
              <input class="input sm num" type="number" min="0" max="30" value={item.quality.value ?? 0} onchange={(e) => set({ quality: Number(e.currentTarget.value) })} />
            </label>
          {/if}
          {#if item.runeSockets}
            <label class="fld"><span class="k">{t("edit.runeSockets")}</span>
              <input class="input sm num" type="number" min="0" max="6" value={item.runeSockets.count} onchange={(e) => set({ runeSockets: Number(e.currentTarget.value) })} />
            </label>
          {/if}
          {#if item.jewelSockets}
            <label class="fld"><span class="k">{t("edit.jewelSockets")}</span>
              <input class="input sm num" type="number" min="0" max="6" value={item.jewelSockets.count} onchange={(e) => set({ jewelSockets: Number(e.currentTarget.value) })} />
            </label>
          {/if}
          {#if item.catalyst.shown}
            <label class="fld"><span class="k">{t("edit.catalyst")}</span>
              <select class="select sm" value={item.catalyst.sel} onchange={(e) => set({ catalyst: Number(e.currentTarget.value) })}>
                {#each item.catalyst.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
              </select>
              {#if item.catalyst.qualityShown}
                <input class="input sm num" type="number" min="0" max="30" aria-label={t("edit.catalyst")} value={item.catalyst.quality ?? 0} onchange={(e) => set({ catalystQuality: Number(e.currentTarget.value) })} />
              {/if}
            </label>
          {/if}
          {#if item.influence.shown}
            <span class="fld"><span class="k">{t("edit.influence")}</span>
              <select class="select sm" aria-label={t("edit.influence")} value={item.influence.sel[0]} onchange={(e) => set({ influence: [Number(e.currentTarget.value), item.influence.sel[1]] })}>
                {#each item.influence.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
              </select>
              <select class="select sm" aria-label={t("edit.influence")} value={item.influence.sel[1]} onchange={(e) => set({ influence: [item.influence.sel[0], Number(e.currentTarget.value)] })}>
                {#each item.influence.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
              </select>
            </span>
          {/if}
        </section>
      {/if}

      <!-- 插槽與連結 -->
      {#if item.socketShown.some((s) => s) || item.canAddSocket}
        <section class="sec">
          <span class="k">{t("edit.sockets")}</span>
          <div class="sockets">
            {#each item.sockets as s, i}
              {#if item.socketShown[i]}
                <select class="select sm sock" style:color={socketColorName[s.color] ?? undefined} value={s.color} onchange={(e) => set({ socket: { index: i + 1, color: e.currentTarget.value } })}>
                  {#each item.socketColors as col}<option value={col}>{col}</option>{/each}
                </select>
                {#if item.links[i]?.shown}
                  <label class="link" title={t("edit.link")}><input type="checkbox" checked={item.links[i].on} onchange={(e) => set({ link: { index: i + 1, on: e.currentTarget.checked } })} /></label>
                {/if}
              {/if}
            {/each}
            {#if item.canAddSocket}<button class="btn ghost sm" onclick={() => set({ addSocket: true })}>+</button>{/if}
          </div>
        </section>
      {/if}

      <!-- 符文:每個插槽一列,寬的時候兩欄 -->
      {#if item.runeSlots}
        <section class="sec">
          <span class="k">{t("edit.runes")}</span>
          <div class="runes">
            {#each item.runeSlots as r (r.index)}
              <label class="rune">
                <span class="rn">{r.index}</span>
                <select class="select sm wide" value={r.sel} onchange={(e) => set({ rune: { index: r.index, sel: Number(e.currentTarget.value) } })}>
                  {#each r.options as o, i}<option value={i + 1}>{i === 0 ? t("edit.runeNone") : `${o.labelZh || o.label}（${o.nameZh || o.name}）`}</option>{/each}
                </select>
              </label>
            {/each}
          </div>
        </section>
      {/if}

      <!-- 變體 -->
      {#if item.variants.length || item.versions}
        <section class="sec grid2">
          {#if item.versions}
            <span class="k">{t("edit.version")}</span>
            <select class="select sm wide" value={item.versions.sel} onchange={(e) => set({ version: Number(e.currentTarget.value) })}>
              {#each item.versions.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
            </select>
          {/if}
          {#each item.variants as v (v.control)}
            <span class="k">{t("edit.variant")}</span>
            <select class="select sm wide" disabled={!v.enabled} value={v.sel} onchange={(e) => set({ variant: { control: v.control, sel: Number(e.currentTarget.value) } })}>
              {#each v.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
            </select>
          {/each}
        </section>
      {/if}

      <!-- 星團珠寶打造 -->
      {#if item.cluster}
        <section class="sec grid2">
          <span class="k">{t("edit.cluster")}</span>
          <select class="select sm wide" value={item.cluster.sel} onchange={(e) => set({ cluster: { sel: Number(e.currentTarget.value) } })}>
            {#each item.cluster.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
          </select>
          <span class="k">{t("edit.nodes")}</span>
          <input class="input sm num" type="number" min={item.cluster.minNodes ?? 0} max={item.cluster.maxNodes ?? 12} value={item.cluster.nodeCount ?? 0} onchange={(e) => set({ cluster: { nodeCount: Number(e.currentTarget.value) } })} />
        </section>
      {/if}

      <!-- 工藝物品的詞綴槽 -->
      {#if item.crafted && item.affixes.length}
        <section class="sec">
          <div class="shead">
            <span class="k">{t("edit.affixes")}</span>
            {#if item.affixSort}
              <label class="fld sort"><span class="k">{t("edit.affixSort")}</span>
                <select class="select sm" value={item.affixSort.sel} onchange={(e) => set({ affixSort: Number(e.currentTarget.value) })}>
                  {#each item.affixSort.options as o, i}<option value={i + 1}>{o.labelZh || o.label}</option>{/each}
                </select>
              </label>
            {/if}
          </div>
          {#each item.affixes as a (a.index)}
            <div class="arow">
              <span class="ak"><PobText text={a.kindZh || a.kind} muted="var(--ink-2)" /></span>
              <select class="select sm wide" value={a.sel} onchange={(e) => affix(a.index, { sel: Number(e.currentTarget.value) })}>
                {#each a.options as o, i}<option value={i + 1}>{(o.labelZh || o.label).replace(/\^x[0-9a-fA-F]{6}|\^\d/g, "")}{o.tiers && o.tiers > 1 ? ` (${o.tiers})` : ""}</option>{/each}
              </select>
              <span class="roll">
                {#if a.rollShown}
                  <input class="slider" type="range" min="0" max="1" step="0.01" value={a.roll ?? 0.5} title={t("edit.roll")} aria-label={t("edit.roll")} onchange={(e) => affix(a.index, { roll: Number(e.currentTarget.value) })} />
                  <span class="rv">{Math.round((a.roll ?? 0.5) * 100)}%</span>
                {/if}
              </span>
            </div>
          {/each}
        </section>
      {/if}

      <!-- 數值範圍 -->
      {#if item.ranges.length}
        <section class="sec">
          <span class="k">{t("edit.ranges")}</span>
          {#each item.ranges as r (r.index)}
            <div class="arow range">
              <span class="rl"><PobText text={r.labelZh || r.label} muted="var(--ink-1)" /></span>
              <span class="roll">
                {#if r.showSlider}
                  <input class="slider" type="range" min="0" max="1" step="0.01" value={r.range ?? 0.5} aria-label={t("edit.roll")} onchange={(e) => set({ range: { index: r.index, value: Number(e.currentTarget.value) } })} />
                  <span class="rv">{Math.round((r.range ?? 0.5) * 100)}%</span>
                {/if}
              </span>
              {#if r.mutable}
                <label class="link" title={t("edit.mutate")}><input type="checkbox" checked={r.mutated} onchange={(e) => set({ range: { index: r.index, mutate: e.currentTarget.checked } })} /> {t("edit.mutate")}</label>
              {/if}
            </div>
          {/each}
        </section>
      {/if}

      <!-- 顯性 / Crucible 詞綴 -->
      {#if item.modLines.length}
        <section class="sec">
          <span class="k">{t("edit.mods")}</span>
          {#each item.modLines as m (m.index)}
            <div class="mrow" class:off={m.disabled}>
              {#if app.has("itemModLineToggle")}
                <input type="checkbox" checked={!m.disabled} title={t("edit.on")} onchange={(e) => set({ modLine: { index: m.index, enabled: e.currentTarget.checked } })} />
              {/if}
              {#if m.section && m.section !== "explicit"}<span class="tag">{t(`edit.section.${m.section}`)}</span>{/if}
              <span class="mt"><PobText text={m.textZh || m.text} /></span>
              {#if m.kind}<span class="dim small">{t(`edit.kind.${m.kind}`)}</span>{/if}
              {#if m.remove}<button class="btn ghost sm" onclick={() => set({ removeModLine: m.remove! })}>{t("edit.remove")}</button>{/if}
            </div>
          {/each}
        </section>
      {/if}

      <!-- 開 POB 的 popup -->
      <section class="sec btns">
        {#if item.actions.enchant}<button class="btn sm" onclick={() => openPopup("enchant", 1)}>{t("edit.enchant")}</button>{/if}
        {#if item.actions.enchant2}<button class="btn sm" onclick={() => openPopup("enchant", 2)}>{t("edit.enchant2")}</button>{/if}
        {#if item.actions.anoint}<button class="btn sm" onclick={() => openPopup("anoint", 1)}>{t("edit.anoint")}</button>{/if}
        {#if item.actions.anoint2}<button class="btn sm" onclick={() => openPopup("anoint", 2)}>{t("edit.anointN", { n: 2 })}</button>{/if}
        {#if item.actions.anoint3}<button class="btn sm" onclick={() => openPopup("anoint", 3)}>{t("edit.anointN", { n: 3 })}</button>{/if}
        {#if item.actions.anoint4}<button class="btn sm" onclick={() => openPopup("anoint", 4)}>{t("edit.anointN", { n: 4 })}</button>{/if}
        {#if item.actions.corrupt}<button class="btn sm" onclick={() => openPopup("corrupt")}>{t("edit.corrupt")}</button>{/if}
        {#if item.actions.addImplicit}<button class="btn sm" onclick={() => openPopup("implicit")}>{t("edit.implicit")}</button>{/if}
        {#if item.actions.custom}<button class="btn sm" onclick={() => openPopup("custom")}>{t("edit.custom")}</button>{/if}
        {#if item.actions.crucible}<button class="btn sm" onclick={() => openPopup("crucible")}>{t("edit.crucible")}</button>{/if}
        <button class="btn ghost sm" class:on={rawOpen} onclick={() => { rawOpen = !rawOpen; rawText = item.raw; }}>{t("edit.text")}</button>
      </section>
      {#if rawOpen}
        <section class="sec">
          <textarea class="input area" rows="12" bind:value={rawText}></textarea>
          <div class="btns">
            <button class="btn sm primary" disabled={!rawText.trim() || app.busy > 0} onclick={() => { void set({ raw: rawText }); rawOpen = false; }}>{t("edit.textApply")}</button>
            <button class="btn ghost sm" onclick={() => (rawOpen = false)}>{t("edit.popupCancel")}</button>
          </div>
        </section>
      {/if}
    {/if}
  </div>
  </div>
</div>

<style>
  .editor {
    display: flex;
    flex-direction: column;
    min-height: 0;
    height: 100%;
  }
  .ehead {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 10px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
  }
  .ename {
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
  }
  .grow {
    flex: 1;
  }
  /* preview + controls. Stacked by default: the preview has a fixed height
     (the divider drags it) and scrolls on its own, the controls scroll below.
     Wide enough for both: side by side, each column scrolling on its own. */
  .ework {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-rows: var(--pv-h, 260px) 6px minmax(0, 1fr);
    container-type: inline-size;
  }
  .preview {
    min-height: 0;
    overflow-y: auto;
    padding: 8px 10px 0;
  }
  .split {
    cursor: row-resize;
    position: relative;
  }
  .split::after {
    content: "";
    position: absolute;
    left: 50%;
    top: 2px;
    width: 36px;
    height: 2px;
    margin-left: -18px;
    border-radius: 1px;
    background: var(--edge-2);
  }
  .split:hover::after,
  .split:focus-visible::after {
    background: var(--gold);
  }
  .split:focus-visible {
    outline: none;
  }
  .ebody {
    min-height: 0;
    overflow-y: auto;
    padding: 4px 10px 16px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  @container (min-width: 860px) {
    .ework {
      grid-template-rows: minmax(0, 1fr);
      grid-template-columns: minmax(300px, 380px) minmax(0, 1fr);
    }
    .split {
      display: none;
    }
    .preview {
      padding: 8px 0 12px 10px;
    }
    .ebody {
      padding-top: 8px;
    }
  }
  .sec.base {
    flex-direction: row;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px 18px;
  }
  .fld {
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .shead {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }
  .runes {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
    gap: 4px 12px;
  }
  .rune {
    display: grid;
    grid-template-columns: 18px minmax(0, 1fr);
    align-items: center;
    gap: 6px;
  }
  .rn {
    font-size: var(--fs-2xs);
    color: var(--ink-3);
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
  .tip {
    padding: 8px 12px 10px;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-top: 3px solid var(--edge-1);
    border-radius: var(--radius-m);
    font-size: var(--fs-xs);
    line-height: 1.45;
  }
  .line {
    white-space: pre-wrap;
  }
  .line.center {
    text-align: center;
  }
  .line.big {
    font-size: var(--fs-sm);
    font-weight: 600;
  }
  .line.small {
    font-size: var(--fs-2xs);
  }
  .sep {
    height: 1px;
    margin: 5px 0;
    background: var(--edge-1);
  }
  .sec {
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 6px 8px 8px;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    background: var(--surface-1);
  }
  .sec.grid2 {
    display: grid;
    grid-template-columns: 72px 1fr;
    align-items: center;
    gap: 6px 8px;
  }
  .sec.btns {
    flex-direction: row;
    flex-wrap: wrap;
    gap: 6px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .select.sm,
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .select.wide,
  .input.wide {
    width: 100%;
    min-width: 0;
  }
  .num {
    width: 64px;
  }
  .sockets {
    display: flex;
    align-items: center;
    gap: 4px;
    flex-wrap: wrap;
  }
  .sock {
    width: 46px;
    font-weight: 600;
  }
  .link {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-size: var(--fs-2xs);
    color: var(--ink-2);
  }
  /* One line per affix: kind | modifier | roll. The slider column is always
     there (empty for a modifier with nothing to roll), so the drop-downs line
     up; a stacked label / drop-down / slider cost three lines per affix. */
  .arow {
    display: grid;
    grid-template-columns: max-content minmax(0, 1fr) 168px;
    align-items: center;
    gap: 8px;
    min-height: 28px;
  }
  .arow.range {
    grid-template-columns: minmax(0, 1fr) 168px auto;
  }
  /* a narrow editor column (small window, big zoom): the drop-down keeps the room */
  .sec:has(> .arow) {
    container-type: inline-size;
  }
  @container (max-width: 520px) {
    .arow {
      grid-template-columns: max-content minmax(0, 1fr) 104px;
    }
    .arow.range {
      grid-template-columns: minmax(0, 1fr) 104px auto;
    }
    .rv {
      display: none;
    }
  }
  .roll {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .roll .slider {
    flex: 1;
    min-width: 0;
  }
  .rv {
    width: 34px;
    text-align: right;
    font-size: var(--fs-2xs);
    color: var(--ink-2);
    font-variant-numeric: tabular-nums;
  }
  .tag {
    flex: 0 0 auto;
    padding: 0 5px;
    font-size: var(--fs-2xs);
    color: var(--ink-1);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s, 4px);
  }
  .mrow {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: var(--fs-xs);
  }
  .ak {
    min-width: 48px;
    font-size: var(--fs-2xs);
    white-space: nowrap;
  }
  .rl {
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .mt {
    flex: 1;
    min-width: 0;
  }
  .mrow.off .mt {
    opacity: 0.5;
    text-decoration: line-through;
  }
  .slider {
    width: 120px;
    accent-color: var(--gold);
  }
  .area {
    height: auto;
    padding: 6px 8px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    resize: vertical;
    line-height: 1.4;
    width: 100%;
  }
  .btns {
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
  }
  .btn.on {
    color: var(--gold);
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .bad {
    color: var(--bad);
    font-size: var(--fs-xs);
    padding: 4px 10px;
  }
  /* dialog panel */
  .pop {
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 8px 10px 10px;
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    background: var(--surface-1);
  }
  .ptitle {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
  }
  .prow {
    display: grid;
    grid-template-columns: 84px 1fr;
    align-items: center;
    gap: 4px 8px;
  }
  .prow .input.sm {
    grid-column: 2;
  }
  .nodes {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .tip.node {
    max-height: 30vh;
    margin-top: 6px;
    border-top-color: var(--gold);
  }
  .nlist {
    max-height: 220px;
    overflow-y: auto;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    padding: 2px;
  }
  .nrow {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 2px 6px;
    font-size: var(--fs-xs);
    border-radius: var(--radius-s);
  }
  .nrow:hover {
    background: var(--surface-hover);
  }
  .nrow.on {
    background: var(--gold-soft);
  }
  .report {
    font-size: var(--fs-xs);
    padding: 4px 8px;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
  }
  .report summary {
    cursor: pointer;
    display: flex;
    gap: 10px;
  }
  .report .rline {
    padding-left: 12px;
    font-family: var(--font-mono);
  }
  .report .warn {
    color: var(--warn);
  }
</style>
