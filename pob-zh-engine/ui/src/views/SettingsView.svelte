<!-- 設定頁:新介面自己的偏好(介面大小,由 host 記在 pob-zh.ini)、POB 原生的「選項」
     (main:OpenOptionsPopup 的控制項,儲存走它自己的 Save → Settings.xml 與 manifest 分支)
     與快捷鍵一覽;不需要開建置。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import AboutDialog from "../components/AboutDialog.svelte";
  let aboutOpen = $state(false);
  import { api, type PobOption, type PobOptions } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { prefs, ZOOM_MIN, ZOOM_MAX, ZOOM_STEP, FONT_MIN, FONT_MAX, DEFAULTS } from "$lib/prefs.svelte";
  import { app } from "$lib/state.svelte";

  // POB's Options dialog: read from its controls, edited as a draft, saved
  // through the dialog's own Save (only the changed values are sent).
  let pob = $state<PobOptions | null>(null);
  let draft = $state<Record<string, string | number | boolean>>({});
  let pobErr = $state<string | null>(null);
  async function loadPob() {
    pobErr = null;
    try {
      const r = await app.run(() => api.pobOptions());
      if (r) {
        pob = r;
        draft = {};
      }
    } catch (e: any) {
      pobErr = String(e?.message ?? e);
    }
  }
  $effect(() => {
    if (app.engine === "ready") untrack(() => void loadPob());
  });
  const current = (o: PobOption): string | number | boolean =>
    o.kind === "dropdown" ? (o.sel ?? 1) : o.kind === "check" ? !!o.state : o.kind === "slider" ? (o.value ?? 0) : (o.text ?? "");
  const value = (o: PobOption) => (o.name in draft ? draft[o.name] : current(o));
  function change(o: PobOption, v: string | number | boolean) {
    if (v === current(o)) delete draft[o.name];
    else draft[o.name] = v;
  }
  const dirty = $derived(Object.keys(draft).length > 0);
  async function savePob() {
    pobErr = null;
    try {
      const r = await app.run(() => api.setPobOptions({ ...draft }));
      if (r) {
        pob = r;
        draft = {};
        app.notice = t("settings.pobSaved");
      }
    } catch (e: any) {
      pobErr = String(e?.message ?? e);
    }
  }
  const clean = (s: string | undefined) => (s ?? "").replace(/\^x[0-9a-fA-F]{6}|\^\d/g, "").replace(/\s*[:：]\s*$/, "");
  const labelOf = (o: PobOption) => clean(o.labelZh || o.label) || o.name;
  const tipOf = (o: PobOption) => clean(o.tooltipZh || o.tooltip) || undefined;
  const rowsOf = (section: string) => (pob ? pob.options.filter((o) => o.section === section && !o.anchoredTo) : []);
  const attachedTo = (name: string) => (pob ? pob.options.filter((o) => o.anchoredTo === name) : []);

  const keys = [
    ["Ctrl + 1 … 9", "settings.keyTabs"],
    ["Ctrl + S", "settings.keySave"],
    [t("settings.comboZoom"), "settings.keyZoom"],
  ] as const;
</script>

{#snippet control(o: PobOption)}
  {#if o.kind === "dropdown"}
    <select class="select sm" value={value(o)} onchange={(e) => change(o, Number(e.currentTarget.value))}>
      {#each o.options ?? [] as opt, i}<option value={i + 1}>{clean(opt.labelZh || opt.label)}</option>{/each}
    </select>
  {:else if o.kind === "check"}
    <input type="checkbox" checked={!!value(o)} onchange={(e) => change(o, e.currentTarget.checked)} />
  {:else if o.kind === "slider"}
    <span class="ctl">
      <input class="slider" type="range" min="0" max="1" step="0.01" value={value(o)} oninput={(e) => change(o, Number(e.currentTarget.value))} />
      <span class="num val">{Math.round(Number(value(o)) * 100)}%</span>
    </span>
  {:else}
    <input class="input sm" value={value(o)} onchange={(e) => change(o, e.currentTarget.value)} />
  {/if}
{/snippet}

{#snippet optionRow(o: PobOption)}
  <div class="orow" class:changed={o.name in draft || attachedTo(o.name).some((a) => a.name in draft)} title={tipOf(o)}>
    <span class="olabel">{labelOf(o)}</span>
    <span class="octl">
      {@render control(o)}
      {#each attachedTo(o.name) as a (a.name)}{@render control(a)}{/each}
    </span>
  </div>
{/snippet}

<div class="page">
  <section class="card">
    <h2>{t("prefs.title")}</h2>
    <div class="row">
      <span class="k">{t("prefs.zoom")}</span>
      <input class="slider" type="range" min={ZOOM_MIN} max={ZOOM_MAX} step={ZOOM_STEP} value={prefs.zoom} oninput={(e) => prefs.set({ zoom: Number(e.currentTarget.value) })} />
      <span class="ctl">
        <button class="btn ghost sm" disabled={prefs.zoom <= ZOOM_MIN} onclick={() => prefs.zoomBy(-1)}>−</button>
        <span class="num val">{prefs.zoom}%</span>
        <button class="btn ghost sm" disabled={prefs.zoom >= ZOOM_MAX} onclick={() => prefs.zoomBy(1)}>+</button>
      </span>
    </div>
    <div class="row">
      <span class="k">{t("prefs.fontSize")}</span>
      <input class="slider" type="range" min={FONT_MIN} max={FONT_MAX} step="1" value={prefs.fontSize} oninput={(e) => prefs.set({ fontSize: Number(e.currentTarget.value) })} />
      <span class="ctl">
        <button class="btn ghost sm" disabled={prefs.fontSize <= FONT_MIN} onclick={() => prefs.set({ fontSize: prefs.fontSize - 1 })}>−</button>
        <span class="num val">{prefs.fontSize}px</span>
        <button class="btn ghost sm" disabled={prefs.fontSize >= FONT_MAX} onclick={() => prefs.set({ fontSize: prefs.fontSize + 1 })}>+</button>
      </span>
    </div>
    <div class="foot">
      <span class="dim hint">{t("prefs.hint")}</span>
      <button class="btn ghost sm" disabled={prefs.zoom === DEFAULTS.zoom && prefs.fontSize === DEFAULTS.fontSize} onclick={() => prefs.reset()}>{t("prefs.reset")}</button>
    </div>
  </section>

  <section class="card wide">
    <h2>{t("settings.pob")}</h2>
    <p class="dim hint">{t("settings.pobHint")}</p>
    {#if pobErr}<p class="bad">{pobErr}</p>{/if}
    {#if pob}
      <div class="pobcols">
        {#each pob.sections as sec (sec.id)}
          <div class="pobsec">
            <h3>{clean(sec.titleZh || sec.title)}</h3>
            {#each rowsOf(sec.id) as o (o.name)}
              {@render optionRow(o)}
            {/each}
          </div>
        {/each}
      </div>
      <div class="foot">
        <span class="dim hint">{t("settings.pobVersion", { version: pob.version ?? "", branch: pob.branch ?? "" })}</span>
        <button class="btn ghost sm" onclick={() => (aboutOpen = true)}>{t("about.open")}</button>
        <span class="btns">
          <button class="btn ghost sm" disabled={!dirty || app.busy > 0} onclick={() => (draft = {})}>{t("settings.pobRevert")}</button>
          <button class="btn primary sm" disabled={!dirty || app.busy > 0} onclick={savePob}>{t("settings.pobSave")}</button>
        </span>
      </div>
    {:else if app.engine === "ready"}
      <p class="dim">{t("settings.pobLoading")}</p>
    {/if}
  </section>

  <section class="card">
    <h2>{t("settings.keys")}</h2>
    <dl class="keys">
      {#each keys as [combo, key]}
        <dt><kbd>{combo}</kbd></dt>
        <dd>{t(key)}</dd>
      {/each}
    </dl>
  </section>
</div>

{#if aboutOpen}<AboutDialog onclose={() => (aboutOpen = false)} />{/if}

<style>
  .page {
    height: 100%;
    overflow-y: auto;
    padding: 16px 20px 24px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    max-width: 1080px;
  }
  .card.wide {
    max-width: none;
  }
  .pobcols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(420px, 1fr));
    gap: 8px 24px;
  }
  h3 {
    margin: 4px 0 6px;
    font-size: var(--fs-2xs);
    letter-spacing: 0.12em;
    color: var(--ink-2);
    font-weight: 600;
  }
  .orow {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    align-items: center;
    gap: 10px;
    min-height: 28px;
    padding: 0 6px;
    border-radius: var(--radius-s);
    font-size: var(--fs-xs);
  }
  .orow:hover {
    background: var(--surface-hover);
  }
  .orow.changed {
    box-shadow: inset 2px 0 0 var(--gold);
  }
  .olabel {
    color: var(--ink-1);
  }
  .octl {
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .octl .input.sm {
    width: 150px;
  }
  .octl .select.sm,
  .octl .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .btns {
    display: inline-flex;
    gap: 6px;
  }
  .bad {
    color: var(--bad);
    font-size: var(--fs-xs);
  }
  p {
    margin: 0 0 8px;
  }
  .card {
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    padding: 12px 16px 14px;
  }
  h2 {
    margin: 0 0 10px;
    font-size: var(--fs-sm);
    font-weight: 600;
    letter-spacing: 0.04em;
  }
  .row {
    display: grid;
    grid-template-columns: 96px minmax(120px, 1fr) auto;
    align-items: center;
    gap: 12px;
    height: 32px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .slider {
    width: 100%;
    accent-color: var(--gold);
  }
  .ctl {
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }
  .val {
    min-width: 48px;
    text-align: center;
  }
  .foot {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    margin-top: 6px;
  }
  .hint {
    font-size: var(--fs-2xs);
  }
  .keys {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 6px 16px;
    margin: 0;
    font-size: var(--fs-xs);
  }
  .keys dt,
  .keys dd {
    margin: 0;
  }
  kbd {
    font-family: var(--font-mono);
    font-size: var(--fs-2xs);
    padding: 1px 6px;
    border: 1px solid var(--edge-1);
    border-bottom-width: 2px;
    border-radius: var(--radius-s);
    background: var(--surface-2);
    white-space: nowrap;
  }
</style>
