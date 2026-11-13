<!-- 設定頁:新介面自己的偏好(介面大小,由 host 記在 pob-zh.ini)、POB 原生的「選項」
     (main:OpenOptionsPopup 的控制項,儲存走它自己的 Save → Settings.xml 與 manifest 分支)
     與快捷鍵一覽;不需要開建置。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import AboutDialog from "$lib/ui/AboutDialog.svelte";
  let aboutOpen = $state(false);
  import { api, bridge, isHosted, type PobOption, type PobOptions } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { prefs, ZOOM_MIN, ZOOM_MAX, ZOOM_STEP, FONT_MIN, FONT_MAX, DEFAULTS, THEMES, isVideoFile, type Theme } from "$lib/prefs.svelte";
  import { ACCENTS } from "../accents";

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
  import Select from "$lib/ui/Select.svelte";

  // POB's Options dialog: read from its controls, edited as a draft, saved
  // through the dialog's own Save (only the changed values are sent).
  let pob = $state<PobOptions | null>(null);
  let draft = $state<Record<string, string | number | boolean>>({});
  let pobErr = $state<string | null>(null);
  async function loadPob(keepDraft = false) {
    pobErr = null;
    try {
      const r = await app.run(() => api.pobOptions());
      if (r) {
        pob = r;
        if (!keepDraft) draft = {};
      }
    } catch (e: any) {
      pobErr = String(e?.message ?? e);
    }
  }
  $effect(() => {
    if (app.engine === "ready") untrack(() => void loadPob());
  });
  // F2: POB's option labels come translated -- read them again, keep the draft
  let seenLang = app.langRev;
  $effect(() => {
    const lr = app.langRev;
    if (lr === seenLang) return;
    seenLang = lr;
    if (app.engine === "ready") untrack(() => void loadPob(true));
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
    [t("settings.comboBack"), "settings.keyBack"],
  ] as const;

  // 左側分類(本地狀態,不存 prefs)
  const NAV = [
    ["look", "look.title"],
    ["font", "settings.navFont"],
    ["bg", "bg.title"],
    ["pob", "settings.pob"],
    ["keys", "settings.keys"],
  ] as const;
  let section = $state<(typeof NAV)[number][0]>("look");
</script>

{#snippet control(o: PobOption)}
  {#if o.kind === "dropdown"}
    <Select class="select sm" ariaLabel={labelOf(o)} value={String(value(o))} options={(o.options ?? []).map((opt, i) => ({ value: i + 1, label: clean(opt.labelZh || opt.label) }))} onchange={(v) => change(o, Number(v))} />
  {:else if o.kind === "check"}
    <input type="checkbox" class="sw" checked={!!value(o)} onchange={(e) => change(o, e.currentTarget.checked)} />
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
  <nav class="nav" aria-label={t("settings.title")}>
    {#each NAV as [id, key] (id)}
      <button class:on={section === id} aria-current={section === id ? "true" : undefined} onclick={() => (section = id)}>{t(key)}</button>
    {/each}
    {#if section !== "pob"}<span class="note dim">{t("settings.autosave")}</span>{/if}
  </nav>

  <div class="content">
    <h1>{t(NAV.find(([id]) => id === section)![1])}</h1>

    {#if section === "look"}
      <section class="card">
        <div class="srow">
          <div class="txt"><span class="name">{t("look.theme")}</span></div>
          <div class="seg" role="radiogroup" aria-label={t("look.theme")}>
            {#each THEMES as th (th)}
              <button role="radio" aria-checked={prefs.theme === th} aria-label={t(`look.theme.${th}`)} onclick={() => prefs.set({ theme: th })}>
                <span class="chip" style:background={`linear-gradient(135deg, ${SWATCH[th][0]} 50%, ${SWATCH[th][3]} 50%)`}></span>
                {t(`look.theme.${th}`)}
              </button>
            {/each}
          </div>
        </div>
        <div class="srow">
          <div class="txt"><span class="name">{t("look.accent")}</span></div>
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
        <div class="srow">
          <div class="txt"><span class="dim">{t("look.hint")}</span></div>
          <button class="btn ghost sm" disabled={lookIsDefault} onclick={() => prefs.set({ theme: DEFAULTS.theme, accent: DEFAULTS.accent, font: DEFAULTS.font })}>{t("prefs.reset")}</button>
        </div>
      </section>
    {:else if section === "font"}
      <section class="card">
        <div class="srow">
          <div class="txt">
            <span class="name">{t("look.font")}</span>
            <span class="sample">天賦樹 詞綴 Passive Tree 패시브 트리 1,234.5%</span>
          </div>
          <div class="fontctl">
            <Select
              class="select sm"
              ariaLabel={t("look.font")}
              value={prefs.font}
              options={[{ value: "", label: t("look.fontLauncher", { name: fontName(prefs.launcherFont) || "Noto Sans TC" }) }, ...prefs.fonts.map((f) => ({ value: f, label: fontName(f) }))]}
              onchange={(v) => prefs.set({ font: String(v) })}
            />
          </div>
        </div>
        <div class="srow">
          <div class="txt"><span class="name">{t("prefs.fontSize")}</span></div>
          <span class="ctl">
            <input class="slider" type="range" min={FONT_MIN} max={FONT_MAX} step="1" value={prefs.fontSize} oninput={(e) => prefs.set({ fontSize: Number(e.currentTarget.value) })} />
            <button class="btn ghost sm" disabled={prefs.fontSize <= FONT_MIN} onclick={() => prefs.set({ fontSize: prefs.fontSize - 1 })}>−</button>
            <span class="num val">{prefs.fontSize}px</span>
            <button class="btn ghost sm" disabled={prefs.fontSize >= FONT_MAX} onclick={() => prefs.set({ fontSize: prefs.fontSize + 1 })}>+</button>
          </span>
        </div>
        <div class="srow">
          <div class="txt"><span class="name">{t("prefs.zoom")}</span><span class="dim">{t("prefs.hint")}</span></div>
          <span class="ctl">
            <input class="slider" type="range" min={ZOOM_MIN} max={ZOOM_MAX} step={ZOOM_STEP} value={prefs.zoom} oninput={(e) => prefs.set({ zoom: Number(e.currentTarget.value) })} />
            <button class="btn ghost sm" disabled={prefs.zoom <= ZOOM_MIN} onclick={() => prefs.zoomBy(-1)}>−</button>
            <span class="num val">{prefs.zoom}%</span>
            <button class="btn ghost sm" disabled={prefs.zoom >= ZOOM_MAX} onclick={() => prefs.zoomBy(1)}>+</button>
          </span>
        </div>
        <div class="srow">
          <div class="txt"></div>
          <button class="btn ghost sm" disabled={prefs.zoom === DEFAULTS.zoom && prefs.fontSize === DEFAULTS.fontSize} onclick={() => prefs.set({ zoom: DEFAULTS.zoom, fontSize: DEFAULTS.fontSize })}>{t("prefs.reset")}</button>
        </div>
      </section>
    {:else if section === "bg"}
      <section class="card">
        <label class="srow">
          <span class="txt"><span class="name">{t("bg.follow")}</span></span>
          <input type="checkbox" class="sw" checked={prefs.look.follow} onchange={(e) => (e.currentTarget.checked ? prefs.followLauncher() : prefs.setLook({}))} />
        </label>
        <div class="srow">
          <div class="txt"><span class="name">{t("bg.image")}</span></div>
          <div class="fontctl">
            <Select
              class="select sm"
              ariaLabel={t("bg.image")}
              value={bg.background}
              options={[
                { value: "", label: t("bg.none") },
                ...prefs.backgrounds.map((f) => ({ value: f, label: f })),
                ...(bg.background && !prefs.backgrounds.includes(bg.background) ? [{ value: bg.background, label: bg.background }] : []),
              ]}
              onchange={(v) => prefs.setLook({ background: String(v) })}
            />
            {#if isHosted}
              <button class="btn ghost sm" onclick={openBgFolder}>{t("bg.openFolder")}</button>
              <button class="btn ghost sm" onclick={() => prefs.refreshBackgrounds()}>{t("bg.refresh")}</button>
            {/if}
          </div>
        </div>
        <div class="srow">
          <div class="txt"><span class="name">{t("bg.scope")}</span></div>
          <div class="seg" role="group" aria-label={t("bg.scope")}>
            {#each [[0, "bg.scopeWindow"], [1, "bg.scopeTree"]] as const as [n, key] (n)}
              <button aria-pressed={(bg.bgScope ?? 0) === n} onclick={() => (bg.bgScope ?? 0) !== n && prefs.setLook({ bgScope: n })}>{t(key)}</button>
            {/each}
          </div>
        </div>
        {#each BG_SLIDERS as [key, label] (key)}
          <!-- "only behind the tree" keeps the panels solid: their opacity does not apply;
               with no image there is nothing for the blur to blur -->
          {@const off = (key === "glassBlur" && !bg.background) || (key === "panelOpacity" && bg.bgScope === 1)}
          <div class="srow">
            <div class="txt"><span class="name" class:dim={off}>{t(label)}</span></div>
            <span class="ctl">
              <input class="slider" type="range" min="0" max="100" step="1" value={bg[key]} disabled={off} onchange={(e) => prefs.setLook({ [key]: Number(e.currentTarget.value) })} />
              <span class="num val" class:dim={off}>{bg[key]}%</span>
            </span>
          </div>
        {/each}
      </section>
      <p class="dim hint">{t("bg.hint")}</p>
      {#if isVideoFile(bg.background)}<p class="dim hint">{t("bg.videoHint")}</p>{/if}
    {:else if section === "pob"}
      <p class="dim hint">{t("settings.pobHint")}</p>
      {#if pobErr}<p class="bad">{pobErr}</p>{/if}
      {#if pob}
        <section class="card pobcard">
          <div class="pobcols">
            {#each pob.sections as sec (sec.id)}
              <div class="pobsec">
                <h3 class="card-title">{clean(sec.titleZh || sec.title)}</h3>
                {#each rowsOf(sec.id) as o (o.name)}
                  {@render optionRow(o)}
                {/each}
              </div>
            {/each}
          </div>
        </section>
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
    {:else}
      <section class="card pad">
        <dl class="keys">
          {#each keys as [combo, key]}
            <dt><kbd>{combo}</kbd></dt>
            <dd>{t(key)}</dd>
          {/each}
        </dl>
      </section>
    {/if}
  </div>
</div>

{#if aboutOpen}<AboutDialog onclose={() => (aboutOpen = false)} />{/if}

<style>
  .page {
    height: 100%;
    display: grid;
    grid-template-columns: 200px minmax(0, 1fr);
    min-height: 0;
  }
  .nav {
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding: 16px 10px;
    border-right: 1px solid var(--edge-0);
    min-height: 0;
    overflow-y: auto;
  }
  .nav button {
    appearance: none;
    flex: none;
    height: 36px;
    padding: 0 12px;
    border: 0;
    border-radius: var(--radius-s);
    background: none;
    color: var(--ink-1);
    text-align: left;
    white-space: nowrap;
  }
  .nav button:hover {
    background: var(--surface-hover);
    color: var(--ink-0);
  }
  .nav button.on {
    background: var(--gold-soft);
    color: var(--gold-text);
  }
  .note {
    margin-top: auto;
    padding: 8px 12px 0;
    font-size: var(--fs-2xs);
  }
  .content {
    min-height: 0;
    overflow-y: auto;
    padding: 20px 24px 24px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .content > :global(*) {
    max-width: 1080px;
  }
  /* 窄:左側導覽改成頂部一列,可橫向捲動 */
  @container view (max-width: 759px) {
    .page {
      grid-template-columns: minmax(0, 1fr);
      grid-template-rows: auto minmax(0, 1fr);
    }
    .nav {
      flex-direction: row;
      align-items: center;
      padding: 6px 10px;
      border-right: 0;
      border-bottom: 1px solid var(--edge-0);
      overflow-x: auto;
      overflow-y: hidden;
    }
    .note {
      margin: 0 0 0 auto;
      padding: 0 8px;
      white-space: nowrap;
    }
  }
  h1 {
    margin: 0;
    font-size: calc(var(--fs-base) + 9px);
    font-weight: 500;
  }
  .card {
    padding: 0 20px;
  }
  .card.pad,
  .card.pobcard {
    padding: 12px 20px;
  }
  .name {
    font-size: calc(var(--fs-base) + 1px);
  }
  .srow .dim {
    font-size: var(--fs-xs);
  }
  label.srow {
    cursor: pointer;
  }
  .seg button {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    white-space: nowrap;
  }
  .chip {
    width: 12px;
    height: 12px;
    border-radius: 3px;
    box-shadow: inset 0 0 0 1px rgba(128, 128, 128, 0.4);
    flex: none;
  }
  .pobcols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(420px, 100%), 1fr));
    gap: 8px 24px;
  }
  h3 {
    margin: 4px 0 6px;
  }
  .orow {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    align-items: center;
    gap: 10px;
    min-height: 32px;
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
  .octl :global(.select.sm),
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
    margin: 0;
  }
  /* 軌道 4px;Chromium(WebView2)的 range 要自畫軌道與把手才改得了粗細 */
  .slider {
    appearance: none;
    width: 200px;
    max-width: 100%;
    height: 16px;
    background: transparent;
    accent-color: var(--gold);
  }
  .slider::-webkit-slider-runnable-track {
    height: 4px;
    border-radius: 2px;
    background: var(--surface-3);
  }
  .slider::-webkit-slider-thumb {
    appearance: none;
    width: 14px;
    height: 14px;
    margin-top: -5px;
    border-radius: 50%;
    background: var(--gold);
  }
  .slider:disabled {
    opacity: var(--dim-opacity);
  }
  .octl .slider {
    width: 140px;
  }
  .ctl {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    flex: none;
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
  }
  .hint {
    font-size: var(--fs-2xs);
  }
  .accents,
  .fontctl {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: flex-end;
    gap: 8px;
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
  .fontctl :global(.select.sm) {
    max-width: 280px;
  }
  .sample {
    color: var(--ink-2);
    font-size: var(--fs-md);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
  }
  .keys {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 8px 16px;
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
