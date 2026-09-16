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
</script>

<div class="card" style:left={`${Math.round(x)}px`} style:top={`${Math.round(y)}px`} style:width={`${width}px`} style:border-top-color={border}>
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
    max-height: 80vh;
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
