<!-- POB tooltip lines (item / node) as a floating card: colour codes through
     PobText, separators as hairlines, the rarity colour on the border. -->
<script lang="ts">
  import type { TooltipLine } from "$lib/bridge";
  import PobText from "./PobText.svelte";

  let {
    lines,
    accent = null,
    x = 0,
    y = 0,
    width = 340,
    centerHead = true,
    avoid = null,
  }: {
    lines: TooltipLine[];
    accent?: string | null;
    x?: number;
    y?: number;
    width?: number;
    /** item/gem tooltips: the first two lines are the centred name block; config tooltips have none */
    centerHead?: boolean;
    /** the hovered control's box: a card placed beside it (x past its right edge) flips to its left,
     *  one placed below it (y past its bottom) flips above, instead of being clamped over it (POB's Tooltip:Draw) */
    avoid?: { left: number; top: number; right: number; bottom: number } | null;
  } = $props();

  function pobColor(code: string | null | undefined): string | null {
    if (!code) return null;
    const m = /^\^x([0-9a-fA-F]{6})/.exec(code);
    return m ? `#${m[1]}` : null;
  }
  const border = $derived(pobColor(accent) ?? "var(--ink-3)");
  // Keep the whole card on screen: measured height, then shifted up / left
  // when the anchor sits near the bottom or right edge.
  let h = $state(0);
  let w = $state(0);
  const top = $derived.by(() => {
    const vh = typeof window !== "undefined" ? window.innerHeight : 0;
    let t = y;
    if (vh && h && avoid && y >= avoid.bottom && y + h > vh - 8 && avoid.top - h - 4 >= 8) t = avoid.top - h - 4;
    return Math.round(vh && h ? Math.max(8, Math.min(t, vh - h - 8)) : t);
  });
  const left = $derived.by(() => {
    const vw = typeof window !== "undefined" ? window.innerWidth : 0;
    let l = x;
    if (vw && w && avoid && x >= avoid.right && x + w > vw - 8 && avoid.left - w - 8 >= 8) l = avoid.left - w - 8;
    return Math.round(vw && w ? Math.max(8, Math.min(l, vw - w - 8)) : l);
  });
</script>

<div class="card pob-dark" bind:clientHeight={h} bind:clientWidth={w} style:left={`${left}px`} style:top={`${top}px`} style:width={`${width}px`} style:border-color={border}>
  {#each lines as l, i}
    {#if "sep" in l}
      <div class="sep"></div>
    {:else}
      <div class="line" class:center={l.center || (centerHead && i < 2)} class:big={l.size >= 18} class:small={l.size <= 14} style:padding-left={l.indent ? `${l.indent * 1.5}em` : undefined}>
        <PobText text={l.text} muted="var(--ink-1)" />
      </div>
    {/if}
  {/each}
</div>

<style>
  .card {
    position: fixed;
    z-index: 40;
    padding: 10px 14px;
    background: var(--surface-0-c);
    border: 1px solid var(--ink-3);
    border-radius: 3px;
    box-shadow: var(--shadow-float);
    pointer-events: none;
    font-size: var(--fs-xs);
    line-height: 1.45;
    max-height: calc(100vh - 16px);
    overflow: hidden;
  }
  /* POB 的多行說明用 \n\t 縮排(配置頁),tab 顯示成縮排而不是 8 格空白 */
  .line {
    white-space: pre-wrap;
    tab-size: 3;
    font-variant-numeric: tabular-nums;
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
    background: var(--edge-2);
  }
</style>
