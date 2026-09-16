<!-- The gear in the title bar: window zoom and base font size, applied live
     and remembered by the host (pob-zh.ini). Ctrl+= / Ctrl+- / Ctrl+0 and
     Ctrl+wheel do the zoom without opening this. -->
<script lang="ts">
  import { t } from "$lib/i18n";
  import { prefs, ZOOM_MIN, ZOOM_MAX, ZOOM_STEP, FONT_MIN, FONT_MAX, DEFAULTS } from "$lib/prefs.svelte";

  let root = $state<HTMLDivElement | null>(null);
  function onDocClick(e: MouseEvent) {
    if (prefs.open && root && !root.contains(e.target as Node)) prefs.open = false;
  }
</script>

<svelte:document onmousedown={onDocClick} />

<div class="wrap" bind:this={root}>
  <button class="gear" class:on={prefs.open} title={t("prefs.title")} aria-label={t("prefs.title")} onclick={() => (prefs.open = !prefs.open)}>
    <svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true">
      <path
        fill="currentColor"
        d="M8.6 1.5h2.8l.4 2.1c.5.2 1 .4 1.4.7l2-.8 1.4 2.4-1.6 1.4c.1.5.1 1 0 1.5l1.6 1.4-1.4 2.4-2-.8c-.4.3-.9.6-1.4.7l-.4 2.1H8.6l-.4-2.1a6 6 0 0 1-1.4-.7l-2 .8-1.4-2.4 1.6-1.4a6 6 0 0 1 0-1.5L3.4 5.9l1.4-2.4 2 .8c.4-.3.9-.6 1.4-.7l.4-2.1ZM10 7a3 3 0 1 0 0 6 3 3 0 0 0 0-6Z"
      />
    </svg>
  </button>
  {#if prefs.open}
    <div class="pop" role="dialog" aria-label={t("prefs.title")}>
      <div class="row">
        <span class="k">{t("prefs.zoom")}</span>
        <span class="ctl">
          <button class="btn ghost sm" disabled={prefs.zoom <= ZOOM_MIN} onclick={() => prefs.zoomBy(-1)}>−</button>
          <span class="num val">{prefs.zoom}%</span>
          <button class="btn ghost sm" disabled={prefs.zoom >= ZOOM_MAX} onclick={() => prefs.zoomBy(1)}>+</button>
        </span>
      </div>
      <input class="slider" type="range" min={ZOOM_MIN} max={ZOOM_MAX} step={ZOOM_STEP} value={prefs.zoom} oninput={(e) => prefs.set({ zoom: Number(e.currentTarget.value) })} />
      <div class="row">
        <span class="k">{t("prefs.fontSize")}</span>
        <span class="ctl">
          <button class="btn ghost sm" disabled={prefs.fontSize <= FONT_MIN} onclick={() => prefs.set({ fontSize: prefs.fontSize - 1 })}>−</button>
          <span class="num val">{prefs.fontSize}px</span>
          <button class="btn ghost sm" disabled={prefs.fontSize >= FONT_MAX} onclick={() => prefs.set({ fontSize: prefs.fontSize + 1 })}>+</button>
        </span>
      </div>
      <input class="slider" type="range" min={FONT_MIN} max={FONT_MAX} step="1" value={prefs.fontSize} oninput={(e) => prefs.set({ fontSize: Number(e.currentTarget.value) })} />
      <div class="foot">
        <span class="dim hint">{t("prefs.hint")}</span>
        <button class="btn ghost sm" disabled={prefs.zoom === DEFAULTS.zoom && prefs.fontSize === DEFAULTS.fontSize} onclick={() => prefs.reset()}>{t("prefs.reset")}</button>
      </div>
    </div>
  {/if}
</div>

<style>
  .wrap {
    position: relative;
    display: inline-flex;
    align-items: center;
  }
  .gear {
    appearance: none;
    border: 0;
    background: none;
    color: var(--ink-2);
    width: 28px;
    height: 28px;
    border-radius: var(--radius-s);
    display: inline-grid;
    place-items: center;
    cursor: pointer;
  }
  .gear:hover,
  .gear.on {
    color: var(--ink-0);
    background: var(--surface-hover);
  }
  .pop {
    position: absolute;
    top: calc(100% + 6px);
    right: 0;
    width: 260px;
    padding: 12px 14px 10px;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    z-index: 35;
    display: flex;
    flex-direction: column;
    gap: 6px;
    font-size: var(--fs-sm);
  }
  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .ctl {
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }
  .val {
    min-width: 44px;
    text-align: center;
  }
  .slider {
    width: 100%;
    margin: 0 0 6px;
    accent-color: var(--gold);
  }
  .foot {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    margin-top: 2px;
  }
  .hint {
    font-size: var(--fs-2xs);
  }
</style>
