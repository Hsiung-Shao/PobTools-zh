<!-- 設定頁:新介面自己的偏好(不是 POB 建置的「配方」)。目前只有介面大小
     (視窗縮放、字體大小,由 host 記在 pob-zh.ini)與快捷鍵一覽;不需要開建置。 -->
<script lang="ts">
  import { t } from "$lib/i18n";
  import { prefs, ZOOM_MIN, ZOOM_MAX, ZOOM_STEP, FONT_MIN, FONT_MAX, DEFAULTS } from "$lib/prefs.svelte";

  const keys = [
    ["Ctrl + 1 … 9", "settings.keyTabs"],
    ["Ctrl + S", "settings.keySave"],
    [t("settings.comboZoom"), "settings.keyZoom"],
  ] as const;
</script>

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

<style>
  .page {
    height: 100%;
    overflow-y: auto;
    padding: 16px 20px 24px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    max-width: 720px;
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
