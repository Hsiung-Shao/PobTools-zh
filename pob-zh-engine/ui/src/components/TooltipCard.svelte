<!-- POB tooltip lines (item / node) as a floating card: colour codes through
     PobText, separators as hairlines, the rarity colour on the border. -->
<script lang="ts">
  import type { TooltipLine } from "$lib/bridge";
  import PobText from "./PobText.svelte";

  let { lines, accent = null, x = 0, y = 0, width = 340 }: { lines: TooltipLine[]; accent?: string | null; x?: number; y?: number; width?: number } = $props();

  function pobColor(code: string | null | undefined): string | null {
    if (!code) return null;
    const m = /^\^x([0-9a-fA-F]{6})/.exec(code);
    return m ? `#${m[1]}` : null;
  }
  const border = $derived(pobColor(accent) ?? "var(--edge-1)");
  // Keep the whole card on screen: measured height, then shifted up / left
  // when the anchor sits near the bottom or right edge.
  let h = $state(0);
  let w = $state(0);
  const top = $derived.by(() => {
    const vh = typeof window !== "undefined" ? window.innerHeight : 0;
    return Math.round(vh && h ? Math.max(8, Math.min(y, vh - h - 8)) : y);
  });
  const left = $derived.by(() => {
    const vw = typeof window !== "undefined" ? window.innerWidth : 0;
    return Math.round(vw && w ? Math.max(8, Math.min(x, vw - w - 8)) : x);
  });
</script>

<div class="card" bind:clientHeight={h} bind:clientWidth={w} style:left={`${left}px`} style:top={`${top}px`} style:width={`${width}px`} style:border-top-color={border}>
  {#each lines as l, i}
    {#if "sep" in l}
      <div class="sep"></div>
    {:else}
      <div class="line" class:center={l.center || i < 2} class:big={l.size >= 18} class:small={l.size <= 14}>
        <PobText text={l.text} muted="var(--ink-1)" />
      </div>
    {/if}
  {/each}
</div>

<style>
  .card {
    position: fixed;
    z-index: 40;
    padding: 8px 12px 10px;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-top: 3px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    pointer-events: none;
    font-size: var(--fs-xs);
    line-height: 1.45;
    max-height: calc(100vh - 16px);
    overflow: hidden;
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
</style>
