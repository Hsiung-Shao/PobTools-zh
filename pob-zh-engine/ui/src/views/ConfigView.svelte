<!-- 配置頁:照 POB 原版 —— ConfigOptions 依區段順序排列(POB 的 col 1 全部在前、
     col 2 在後),卡片以 CSS 多欄排版填滿寬度(視窗越寬欄越多,每欄平衡高度);
     每項一列「標籤 …… 值」,數字欄不截斷、下拉依內容寬;POB 判定不相關的選項預設隱藏;tooltip 走我們的浮層;自訂詞綴區塊。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type ConfigItem, type ConfigList, type ConfigSection, type CustomModBlock, type TooltipLine } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import PobText from "../components/PobText.svelte";
  import TooltipCard from "../components/TooltipCard.svelte";

  let data = $state<ConfigList | null>(null);
  // tooltip (POB's per-control tooltipText), on hover after a short delay
  let tip = $state<{ lines: TooltipLine[]; x: number; y: number } | null>(null);
  let tipTimer = 0;
  function tipEnter(it: ConfigItem, e: MouseEvent) {
    clearTimeout(tipTimer);
    const text = it.tooltipZh || it.tooltip;
    if (!text) return;
    const el = e.currentTarget as HTMLElement;
    tipTimer = window.setTimeout(() => {
      const r = el.getBoundingClientRect();
      const lines: TooltipLine[] = text.split("\n").map((s) => ({ size: 14, text: s, raw: s, center: false }));
      const x = Math.round(Math.min(r.left + 24, window.innerWidth - 380));
      const y = Math.round(r.bottom + 4 + 260 > window.innerHeight ? Math.max(8, r.top - 8 - Math.min(260, 20 * lines.length + 24)) : r.bottom + 4);
      tip = { lines, x, y };
    }, 220);
  }
  function tipLeave() {
    clearTimeout(tipTimer);
    tip = null;
  }
  const selectedLabel = (it: ConfigItem) => (it.list ?? []).find((o) => String(o.val ?? "") === String(it.value ?? ""));
  let loadedRev = -1;
  let filter = $state("");
  let showAll = $state(false);
  let custom = $state<CustomModBlock[]>([]);
  let customDirty = $state(false);
  let setDialog = $state<{ mode: "new" | "rename"; title: string; copy: boolean } | null>(null);

  async function reload() {
    const r = await app.run(() => api.listConfig());
    if (r) {
      data = r;
      loadedRev = r.rev;
      if (!customDirty) custom = r.customMods.map((b) => ({ ...b }));
    }
  }
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });
  async function changed() {
    await reload();
    await app.afterTreeChange();
  }

  function matches(it: ConfigItem): boolean {
    if (!showAll && !it.visible) return false;
    const f = filter.trim().toLowerCase();
    if (!f) return true;
    return (it.labelZh ?? "").toLowerCase().includes(f) || (it.label ?? "").toLowerCase().includes(f) || it.var.toLowerCase().includes(f);
  }
  const hiddenCount = $derived(data ? data.sections.reduce((n, s) => n + s.items.filter((i) => !i.visible).length, 0) : 0);
  // POB's order: every col-1 section, then every col-2 section
  const sections = $derived.by((): ConfigSection[] => {
    if (!data) return [];
    return [...data.sections.filter((s) => s.col !== 2), ...data.sections.filter((s) => s.col === 2)];
  });

  async function set(it: ConfigItem, value: unknown) {
    const r = await app.run(() => api.setConfig(it.var, value as any));
    if (r) await changed();
  }
  async function reset(it: ConfigItem) {
    const r = await app.run(() => api.resetConfig(it.var));
    if (r) await changed();
  }
  function numVal(v: unknown): string {
    return v == null || v === "" ? "" : String(v);
  }
  function isDefault(it: ConfigItem): boolean {
    return it.value == null || it.value === false || it.value === "";
  }

  async function applyCustom() {
    const r = await app.run(() => api.setCustomMods(custom));
    customDirty = false;
    if (r) await changed();
  }

  async function setChange(v: string) {
    const r = await app.run(() => api.setConfigSet(Number(v)));
    if (r) await changed();
  }
  async function setDialogOk() {
    if (!setDialog || !data) return;
    const d = setDialog;
    const active = data.activeConfigSetId;
    setDialog = null;
    const r = d.mode === "new" ? await app.run(() => api.newConfigSet(d.title.trim(), d.copy)) : await app.run(() => api.renameConfigSet(active, d.title.trim()));
    if (r) await changed();
  }
  async function deleteSet() {
    if (!data || data.configSets.length <= 1 || !confirm(t("items.deleteSet") + "?")) return;
    const id = data.activeConfigSetId;
    const r = await app.run(() => api.deleteConfigSet(id));
    if (r) await changed();
  }
  const setTitle = (s: { id: number; title?: string }) => s.title || t("items.defaultSet");
</script>

<div class="page">
  <div class="bar">
    <input class="input sm" placeholder={t("config.search")} bind:value={filter} />
    <label class="chk"><input type="checkbox" bind:checked={showAll} /> {t("config.showAll")}</label>
    {#if hiddenCount}<span class="dim small">{t("config.hidden", { n: hiddenCount })}</span>{/if}
    <span class="grow"></span>
    {#if data}
      <span class="k">{t("config.configSet")}</span>
      <select class="select sm" value={String(data.activeConfigSetId)} onchange={(e) => setChange(e.currentTarget.value)}>
        {#each data.configSets as s}<option value={String(s.id)}>{setTitle(s)}</option>{/each}
      </select>
      <button class="btn ghost sm" onclick={() => (setDialog = { mode: "new", title: "", copy: true })}>+</button>
      <button class="btn ghost sm" onclick={() => (setDialog = { mode: "rename", title: data!.configSets.find((s) => s.id === data!.activeConfigSetId)?.title ?? "", copy: false })}>✎</button>
      <button class="btn ghost sm" disabled={data.configSets.length <= 1} onclick={deleteSet}>×</button>
    {/if}
  </div>

  <div class="scroll">
    {#if data}
      <div class="cols">
        {#each sections as sec (sec.name)}
          {@const items = sec.items.filter(matches)}
          {#if items.length}
            <section class="card">
              <h3>{sec.nameZh || sec.name}</h3>
              {#each items as it (it.var)}
                {@const sel = it.type === "list" ? selectedLabel(it) : undefined}
                {@const selText = sel ? sel.labelZh || sel.label : ""}
                <!-- svelte-ignore a11y_no_static_element_interactions -->
                <div class="opt" class:muted={!it.visible} class:text={it.type === "text"} onmouseenter={(e) => tipEnter(it, e)} onmouseleave={tipLeave}>
                  {#if it.type === "check"}
                    <label class="lbl chkrow">
                      <input type="checkbox" checked={!!it.value} onchange={(e) => set(it, e.currentTarget.checked)} />
                      <span><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></span>
                    </label>
                  {:else if it.type === "list"}
                    <span class="lbl"><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></span>
                    <select class="select sm val list" value={it.value == null ? "" : String(it.value)} onchange={(e) => { const v = e.currentTarget.value; const o = (it.list ?? []).find((x) => String(x.val ?? "") === v); void set(it, o ? o.val : null); }}>
                      {#each it.list ?? [] as o}
                        <option value={String(o.val ?? "")}>{o.labelZh || o.label}</option>
                      {/each}
                    </select>
                    {#if selText.length > 22}<div class="cur dim"><PobText text={selText} muted="var(--ink-2)" /></div>{/if}
                  {:else if it.type === "text"}
                    <span class="lbl"><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></span>
                    <textarea class="input area" rows="3" value={numVal(it.value)} onchange={(e) => set(it, e.currentTarget.value)}></textarea>
                  {:else}
                    <span class="lbl"><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></span>
                    <input
                      class="input sm val num"
                      class:wide={it.type === "float"}
                      type="text"
                      inputmode="decimal"
                      value={numVal(it.value)}
                      placeholder={it.placeholder != null ? String(it.placeholder) : ""}
                      onchange={(e) => { const s = e.currentTarget.value.trim(); if (s === "") return void set(it, null); const n = Number(s); if (Number.isFinite(n)) void set(it, n); else e.currentTarget.value = numVal(it.value); }}
                      onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()}
                    />
                  {/if}
                  {#if !isDefault(it)}
                    <button class="btn ghost sm rst" title={t("config.reset")} onclick={() => reset(it)}>↺</button>
                  {/if}
                </div>
              {/each}
            </section>
          {/if}
        {/each}
        <section class="card">
          <h3>{t("config.custom")}</h3>
          <p class="dim small">{t("config.customHint")}</p>
          {#each custom as blk, i}
            <div class="block">
              <div class="bhead">
                <input type="checkbox" bind:checked={blk.enabled} onchange={() => (customDirty = true)} />
                <input class="input sm" placeholder={t("config.customTitle")} bind:value={blk.title} oninput={() => (customDirty = true)} />
                <button class="btn ghost sm" onclick={() => { custom.splice(i, 1); customDirty = true; }}>×</button>
              </div>
              <textarea class="input area" rows="4" bind:value={blk.text} oninput={() => (customDirty = true)}></textarea>
            </div>
          {/each}
          <div class="btns">
            <button class="btn sm" onclick={() => { custom.push({ title: `Group ${custom.length + 1}`, text: "", enabled: true }); customDirty = true; }}>{t("config.customAdd")}</button>
            <button class="btn sm primary" disabled={!customDirty || app.busy > 0} onclick={applyCustom}>{t("config.customApply")}</button>
          </div>
        </section>
      </div>
    {/if}
  </div>

  {#if tip}<TooltipCard lines={tip.lines} x={tip.x} y={tip.y} width={360} />{/if}

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("config.configSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}<label class="chk"><input type="checkbox" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>{/if}
        <div class="btns right"><button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" onclick={setDialogOk}>OK</button></div>
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
  .bar {
    display: flex;
    align-items: center;
    gap: 8px;
    height: 36px;
    padding: 0 12px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
  }
  .bar .input {
    width: 220px;
  }
  .grow {
    flex: 1;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: var(--fs-xs);
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .select.sm,
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .opt .val {
    height: 20px;
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 12px 14px 24px;
  }
  /* cards packed into as many columns as fit (POB's order runs down each column) */
  .cols {
    column-width: 300px;
    column-gap: 10px;
  }
  .card {
    break-inside: avoid;
    margin: 0 0 10px;
    padding: 6px 8px 8px;
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-m);
  }
  h3 {
    margin: 0 0 6px;
    padding-left: 10px;
    position: relative;
    font-size: var(--fs-2xs);
    letter-spacing: 0.14em;
    font-weight: 600;
    color: var(--ink-2);
  }
  h3::before {
    content: "";
    position: absolute;
    left: 0;
    top: 2px;
    bottom: 2px;
    width: 2px;
    background: var(--gold);
  }
  /* one option = one line: label on the left, its value on the right (POB's own row shape) */
  .opt {
    position: relative;
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    align-items: center;
    column-gap: 8px;
    min-height: 22px;
    padding: 0 24px 0 4px;
    border-radius: var(--radius-s);
    font-size: var(--fs-xs);
  }
  .opt:hover {
    background: var(--surface-hover);
  }
  .opt.muted {
    opacity: 0.55;
  }
  .opt.text {
    grid-template-columns: 1fr;
    row-gap: 4px;
    padding-top: 4px;
    padding-bottom: 4px;
  }
  .lbl {
    min-width: 0;
    line-height: 1.3;
  }
  .chkrow {
    grid-column: 1 / -1;
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
  }
  .cur {
    grid-column: 1 / -1;
    font-size: var(--fs-2xs);
    padding-left: 2px;
    margin-top: -2px;
  }
  /* values: numbers show whole (no spinner, wide enough for 7 digits), lists as wide as their text */
  .val.num {
    width: 9ch;
    text-align: right;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  .val.num.wide {
    width: 11ch;
  }
  .val.num::placeholder {
    text-align: right;
  }
  .val.list {
    width: auto;
    min-width: 100px;
    max-width: 200px;
  }
  .rst {
    position: absolute;
    right: 2px;
    top: 50%;
    transform: translateY(-50%);
    opacity: 0;
    height: 20px;
    padding: 0 4px;
  }
  .opt:hover .rst {
    opacity: 1;
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
  .block {
    margin: 6px 0;
    padding: 6px;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .bhead {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .bhead .input {
    flex: 1;
  }
  .btns {
    display: flex;
    gap: 6px;
    margin-top: 6px;
  }
  .btns.right {
    justify-content: flex-end;
  }
  p {
    margin: 0 0 6px;
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
    width: 380px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
</style>
