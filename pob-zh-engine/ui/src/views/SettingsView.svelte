<!-- 設定頁:新介面自己的偏好(介面大小,由 host 記在 pob-zh.ini)、POB 原生的「選項」
     (main:OpenOptionsPopup 的控制項,儲存走它自己的 Save → Settings.xml 與 manifest 分支)
     與快捷鍵一覽;不需要開建置。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import AboutDialog from "../components/AboutDialog.svelte";
  let aboutOpen = $state(false);
  import { api, bridge, isHosted, type PobOption, type PobOptions } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { prefs, ZOOM_MIN, ZOOM_MAX, ZOOM_STEP, FONT_MIN, FONT_MAX, DEFAULTS, THEMES, ACCENTS, type Theme } from "$lib/prefs.svelte";

  // Each theme button is a small picture of that theme: page, panel, text, accent.
  const SWATCH: Record<Theme, [string, string, string, string]> = {
    slate: ["#0e1116", "#192029", "#ece6d8", "#d8aa4b"],
    light: ["#f4f1ea", "#e4dfd4", "#1c1e23", "#946510"],
    contrast: ["#000000", "#1b1b1b", "#ffffff", "#ffc83d"],
    parchment: ["#1a140e", "#2a2117", "#efe2c4", "#e0a94a"],
  };
  const fontName = (f: string) => f.replace(/\.ttf$/i, "");
  // Background card: shows what is on screen (the launcher's set while following);
  // any change there makes it the page's own (prefs.setLook).
  const bg = $derived(prefs.effectiveLook);
  const BG_SLIDERS = [
    ["bgBright", "bg.bright"],
    ["panelOpacity", "bg.panelOpacity"],
    ["glassBlur", "bg.blur"],
    ["treeBg", "bg.treeBg"],
  ] as const;
  const openBgFolder = () => bridge.call("host.open_folder", { which: "backgrounds" }).catch(() => {});
  const lookIsDefault = $derived(prefs.theme === DEFAULTS.theme && prefs.accent === DEFAULTS.accent && prefs.font === DEFAULTS.font);
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
    ["F2", "settings.keyEnglish"],
    ["Ctrl + R", "settings.keyRename"],
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
      <button class="btn ghost sm" disabled={prefs.zoom === DEFAULTS.zoom && prefs.fontSize === DEFAULTS.fontSize} onclick={() => prefs.set({ zoom: DEFAULTS.zoom, fontSize: DEFAULTS.fontSize })}>{t("prefs.reset")}</button>
    </div>
  </section>

  <section class="card">
    <h2>{t("look.title")}</h2>
    <div class="lrow">
      <span class="k">{t("look.theme")}</span>
      <div class="themes" role="radiogroup" aria-label={t("look.theme")}>
        {#each THEMES as th (th)}
          <button class="theme" class:on={prefs.theme === th} role="radio" aria-checked={prefs.theme === th} aria-label={t(`look.theme.${th}`)} onclick={() => prefs.set({ theme: th })}>
            <span class="sw" style:background={SWATCH[th][0]}>
              <span class="sw-panel" style:background={SWATCH[th][1]}>
                <span class="sw-line" style:background={SWATCH[th][2]}></span>
                <span class="sw-line short" style:background={SWATCH[th][3]}></span>
              </span>
            </span>
            <span class="tname">{t(`look.theme.${th}`)}</span>
          </button>
        {/each}
      </div>
    </div>
    <div class="lrow">
      <span class="k">{t("look.accent")}</span>
      <div class="accents">
        <button class="dot auto" class:on={!prefs.accent} title={t("look.accentTheme")} aria-label={t("look.accentTheme")} onclick={() => prefs.set({ accent: "" })}>{t("look.accentAuto")}</button>
        {#each ACCENTS as c (c)}
          <button class="dot" class:on={prefs.accent === c} style:background={c} title={c} aria-label={c} onclick={() => prefs.set({ accent: c })}></button>
        {/each}
        <label class="dot custom" class:on={!!prefs.accent && !(ACCENTS as readonly string[]).includes(prefs.accent)} title={t("look.accentCustom")}>
          <input type="color" value={prefs.accent || "#d8aa4b"} onchange={(e) => prefs.set({ accent: e.currentTarget.value })} aria-label={t("look.accentCustom")} />
        </label>
      </div>
    </div>
    <div class="lrow">
      <span class="k">{t("look.font")}</span>
      <div class="fontctl">
        <select class="select sm" value={prefs.font} onchange={(e) => prefs.set({ font: e.currentTarget.value })}>
          <option value="">{t("look.fontLauncher", { name: fontName(prefs.launcherFont) || "Noto Sans TC" })}</option>
          {#each prefs.fonts as f (f)}<option value={f}>{fontName(f)}</option>{/each}
        </select>
        <span class="sample">天賦樹 詞綴 Passive Tree 패시브 트리 1,234.5%</span>
      </div>
    </div>
    <div class="foot">
      <span class="dim hint">{t("look.hint")}</span>
      <button class="btn ghost sm" disabled={lookIsDefault} onclick={() => prefs.set({ theme: DEFAULTS.theme, accent: DEFAULTS.accent, font: DEFAULTS.font })}>{t("prefs.reset")}</button>
    </div>
  </section>

  <section class="card">
    <h2>{t("bg.title")}</h2>
    <label class="follow">
      <input type="checkbox" checked={prefs.look.follow} onchange={(e) => (e.currentTarget.checked ? prefs.followLauncher() : prefs.setLook({}))} />
      <span>{t("bg.follow")}</span>
    </label>
    <div class="lrow">
      <span class="k">{t("bg.image")}</span>
      <div class="fontctl">
        <select class="select sm" value={bg.background} onchange={(e) => prefs.setLook({ background: e.currentTarget.value })}>
          <option value="">{t("bg.none")}</option>
          {#each prefs.backgrounds as f (f)}<option value={f}>{f}</option>{/each}
          {#if bg.background && !prefs.backgrounds.includes(bg.background)}<option value={bg.background}>{bg.background}</option>{/if}
        </select>
        {#if isHosted}
          <button class="btn ghost sm" onclick={openBgFolder}>{t("bg.openFolder")}</button>
          <button class="btn ghost sm" onclick={() => prefs.refreshBackgrounds()}>{t("bg.refresh")}</button>
        {/if}
      </div>
    </div>
    {#each BG_SLIDERS as [key, label] (key)}
      <div class="row">
        <span class="k">{t(label)}</span>
        <input class="slider" type="range" min="0" max="100" step="1" value={bg[key]} disabled={!bg.background} onchange={(e) => prefs.setLook({ [key]: Number(e.currentTarget.value) })} />
        <span class="num val">{bg[key]}%</span>
      </div>
    {/each}
    <div class="foot">
      <span class="dim hint">{t("bg.hint")}</span>
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
  .follow {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 4px;
    font-size: var(--fs-sm);
    color: var(--ink-1);
    cursor: pointer;
  }
  .lrow {
    display: grid;
    grid-template-columns: minmax(96px, max-content) minmax(0, 1fr);
    align-items: center;
    gap: 12px;
    padding: 5px 0;
  }
  .themes,
  .accents,
  .fontctl {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
  }
  .theme {
    display: inline-flex;
    flex-direction: column;
    align-items: center;
    gap: 4px;
    padding: 5px;
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--ink-1);
    cursor: pointer;
  }
  .theme:hover {
    background: var(--surface-hover);
  }
  .theme.on {
    border-color: var(--gold);
    box-shadow: 0 0 0 1px var(--gold);
    color: var(--ink-0);
  }
  .sw {
    display: flex;
    align-items: flex-end;
    width: 64px;
    height: 40px;
    padding: 6px 0 0 8px;
    border-radius: var(--radius-s);
    box-shadow: inset 0 0 0 1px rgba(128, 128, 128, 0.35);
  }
  .sw-panel {
    display: flex;
    flex-direction: column;
    gap: 4px;
    width: 100%;
    height: 100%;
    padding: 6px;
    border-radius: 3px 0 0 0;
  }
  .sw-line {
    display: block;
    height: 3px;
    width: 80%;
    border-radius: 2px;
  }
  .sw-line.short {
    width: 45%;
  }
  .tname {
    font-size: var(--fs-2xs);
    white-space: nowrap;
  }
  .dot {
    width: 24px;
    height: 24px;
    padding: 0;
    border-radius: 50%;
    border: 2px solid var(--surface-1);
    box-shadow: 0 0 0 1px var(--edge-2);
    cursor: pointer;
  }
  .dot.on {
    box-shadow: 0 0 0 2px var(--ink-0);
  }
  .dot.auto {
    width: auto;
    min-width: 24px;
    padding: 0 9px;
    border-radius: 12px;
    background: var(--surface-2);
    color: var(--ink-1);
    font-size: var(--fs-2xs);
    white-space: nowrap;
  }
  .dot.custom {
    position: relative;
    overflow: hidden;
    background: conic-gradient(#e0645a, #d8aa4b, #4fbf7a, #3fc1c9, #5b9dff, #a77bff, #e0645a);
  }
  .dot.custom input {
    position: absolute;
    inset: -8px;
    opacity: 0;
    cursor: pointer;
  }
  .fontctl .select.sm {
    max-width: 280px;
  }
  .sample {
    color: var(--ink-1);
    font-size: var(--fs-md);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
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
