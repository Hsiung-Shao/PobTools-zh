<!-- 左側導覽列:各分頁的圖示 + 名稱;設定推到最下方。窄視窗只留圖示。 -->
<script lang="ts">
  import { t } from "$lib/i18n";
  import { navItems, type NavId } from "$lib/nav";
  import { app } from "$lib/state.svelte";
  import { layout } from "$lib/layout.svelte";

  const ICON: Record<NavId, string> = {
    builds: "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z",
    tree: "M12 4v5m0 0-6 5m6-5 6 5M6 14v5m12-5v5M12 9v10",
    items: "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z",
    skills: "M6 3h12l4 6-10 12L2 9zM2 9h20",
    config: "M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1M15 4v4M9 10v4M17 16v4",
    calcs: "M6 3h12a1 1 0 0 1 1 1v16a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1zM8 7h8M8 12h2M14 12h2M8 16h2M14 16h2",
    notes: "M7 3h7l5 5v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1zM14 3v5h5M9 13h6M9 17h6",
    party: "M9 11a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM3 20a6 6 0 0 1 12 0M16 5a3 3 0 0 1 0 6M17 14a6 6 0 0 1 4 6",
    import: "M7 4v14m0 0-3-3m3 3 3-3M17 20V6m0 0-3 3m3-3 3 3",
    compare: "M4 4h6v16H4zM14 4h6v16h-6z",
    settings: "M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M5 19l2-2M17 7l2-2",
  };

  const items = $derived(navItems(app.loaded));
</script>

<nav class="rail" class:narrow={layout.narrow}>
  {#each items as it (it.id)}
    {@const label = t(it.key)}
    <button
      class="item"
      class:end={it.id === "settings"}
      class:on={app.view === it.id}
      aria-current={app.view === it.id ? "page" : undefined}
      disabled={it.disabled}
      title={layout.narrow ? label : undefined}
      aria-label={layout.narrow ? label : undefined}
      onclick={() => (app.view = it.id)}
    >
      <svg viewBox="0 0 24 24" aria-hidden="true"><path d={ICON[it.id]} /></svg>
      {#if !layout.narrow}<span class="txt">{label}</span>{/if}
    </button>
  {/each}
</nav>

<style>
  .rail {
    flex: none;
    width: var(--rail-w);
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding: 8px 6px;
    min-height: 0;
    overflow-y: auto;
    scrollbar-width: none;
    background: var(--surface-1);
    border-right: 1px solid var(--edge-0);
  }
  .rail::-webkit-scrollbar {
    display: none;
  }
  .rail.narrow {
    width: 56px;
  }
  .item {
    flex: none;
    appearance: none;
    height: 54px;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 4px;
    padding: 0 2px;
    border: 0;
    border-radius: 8px;
    background: none;
    color: var(--ink-2);
    cursor: pointer;
  }
  .item.end {
    margin-top: auto;
  }
  .item:hover:not(:disabled) {
    color: var(--ink-0);
    background: var(--surface-hover);
  }
  .item.on,
  .item.on:hover:not(:disabled) {
    background: var(--gold-soft);
    color: var(--gold-text);
  }
  .item:disabled {
    opacity: var(--dim-opacity);
    cursor: default;
  }
  svg {
    width: 20px;
    height: 20px;
    fill: none;
    stroke: currentColor;
    stroke-width: 1.75;
    stroke-linecap: round;
    stroke-linejoin: round;
  }
  /* a long name ("Import / Export") takes a second line rather than being cut */
  .txt {
    max-width: 100%;
    font-size: var(--fs-xs);
    line-height: 1.15;
    text-align: center;
    overflow: hidden;
    display: -webkit-box;
    -webkit-box-orient: vertical;
    -webkit-line-clamp: 2;
    line-clamp: 2;
  }
</style>
