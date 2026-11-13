<!-- POB 的計算細項:純文字段、表格段、半徑段,原樣呈現,只換字型與顏色。
     浮層寬度跟著內容走、上限是視窗寬;放不下時表格的長文字欄換行,數值與短欄不換行,
     任何一段都不出現水平捲軸。 -->
<script lang="ts">
  import { api, type BreakdownSection, type BreakdownTip, type BreakdownTipRef, type TooltipLine } from "$lib/bridge";
  import TooltipCard from "$lib/ui/TooltipCard.svelte";
  import NodeMiniMap from "./NodeMiniMap.svelte";
  import { t } from "$lib/i18n";
  import PobText from "$lib/ui/PobText.svelte";

  // tipFor: asks POB for a source cell's own tooltip (the item, or the passive
  // node) -- what the classic window shows when a breakdown row is pointed at.
  let { sections, tipFor }: { sections: BreakdownSection[]; tipFor?: (ref: BreakdownTipRef) => Promise<BreakdownTip> } = $props();

  type Box = { left: number; top: number; right: number; bottom: number };
  let tip = $state<{ lines: TooltipLine[]; node?: number; x: number; y: number; avoid: Box } | null>(null);
  let tipSeq = 0;
  const tipCache = new Map<string, { lines: TooltipLine[]; node?: number }>();
  $effect(() => {
    void sections;
    tipCache.clear();
    tip = null;
  });
  const refOf = (s: Table, ri: number, key: string) => (tipFor ? s.tips?.find((x) => x.r === ri + 1 && x.k === key) : undefined);
  async function enterCell(e: MouseEvent, ref: BreakdownTipRef) {
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    const at = { x: Math.round(r.right + 8), y: Math.round(r.top), avoid: { left: r.left, top: r.top, right: r.right, bottom: r.bottom } };
    const key = `${ref.s}:${ref.r}:${ref.k}`;
    const seq = ++tipSeq;
    let got = tipCache.get(key);
    if (!got) {
      try {
        const t = await tipFor!(ref);
        got = { lines: t.lines ?? (t.node != null ? (await api.nodeInfo(t.node)).lines : []), node: t.node };
        tipCache.set(key, got);
      } catch {
        return; // the build changed under the request
      }
    }
    if (seq === tipSeq && got.lines.length) tip = { ...got, ...at };
  }
  function leaveCell() {
    tipSeq++;
    tip = null;
  }

  // Free-text columns of POB's breakdown tables (CalcBreakdownControl: the
  // modifier list's Stat / Skill types / Notes / Source Name, the reservation
  // and conversion tables' skill / source names). When the panel is capped at
  // the window width these wrap; every other column (values, totals, short
  // source tags) keeps one line. A long left-aligned column that is not in
  // the list (a table added by a later POB) is treated as free text too.
  const TEXT_KEYS = new Set(["name", "flags", "tags", "sourceName", "sourceLabel", "skillName", "label"]);
  type Table = Extract<BreakdownSection, { type: "table" }>;
  function wrapCols(s: Table): boolean[] {
    return s.cols.map((c) => {
      if (c.right) return false;
      if (TEXT_KEYS.has(c.key)) return true;
      return s.rows.some((r) => (r[c.key] ?? "").length > 24);
    });
  }
  // a wrapping column may shrink to a few characters, never to nothing
  // (min() cannot take max-content: that makes the whole template invalid)
  const track = (wrap: boolean) => (wrap ? "minmax(5em, auto)" : "auto");
</script>

<div class="panel">
  {#each sections as s}
    {#if s.type === "text"}
      <div class="text" class:big={s.size >= 16}>
        {#each s.lines as line}
          <div class="line"><PobText text={line} muted="var(--ink-1)" /></div>
        {/each}
      </div>
    {:else if s.type === "table"}
      {@const wrap = wrapCols(s)}
      <div class="table">
        {#if s.label}<div class="caption"><PobText text={s.label} /></div>{/if}
        <div class="grid" style:grid-template-columns={wrap.map(track).join(" ")}>
          {#each s.cols as c, i}
            <div class="th" class:right={c.right} class:wrap={wrap[i]}>{c.label}</div>
          {/each}
          {#each s.rows as row, ri}
            {#each s.cols as c, i}
              {@const ref = refOf(s, ri, c.key)}
              {#if ref}
                <!-- svelte-ignore a11y_no_static_element_interactions -->
                <div class="td num src" class:right={c.right} class:wrap={wrap[i]} onmouseenter={(e) => enterCell(e, ref)} onmouseleave={leaveCell}><PobText text={row[c.key] ?? ""} muted="var(--ink-1)" /></div>
              {:else}
                <div class="td num" class:right={c.right} class:wrap={wrap[i]}><PobText text={row[c.key] ?? ""} muted="var(--ink-1)" /></div>
              {/if}
            {/each}
          {/each}
        </div>
        {#if s.footer}<div class="footer"><PobText text={s.footer} muted="var(--ink-2)" /></div>{/if}
      </div>
    {:else if s.type === "radius"}
      <div class="radius">{t("breakdown.radius", { radius: s.radius })}</div>
    {/if}
  {/each}
  {#if tip}
    <TooltipCard lines={tip.lines} x={tip.x} y={tip.y} width={360} avoid={tip.avoid}>
      {#if tip.node != null}<NodeMiniMap node={tip.node} />{/if}
    </TooltipCard>
  {/if}
  {#if sections.length === 0}
    <div class="radius">{t("breakdown.none")}</div>
  {/if}
</div>

<style>
  .panel {
    display: flex;
    flex-direction: column;
    gap: 12px;
    font-size: var(--fs-xs);
    line-height: 1.5;
  }
  .line {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    line-height: 21px;
    font-variant-numeric: tabular-nums;
  }
  .big .line {
    font-size: var(--fs-sm);
  }
  .caption {
    margin-bottom: 4px;
    font-weight: 600;
  }
  .grid {
    display: grid;
    gap: 0 16px;
    align-items: baseline;
  }
  .th {
    padding: 0 0 3px;
    font-size: var(--fs-2xs);
    letter-spacing: 0.08em;
    color: var(--ink-2);
    border-bottom: 1px solid var(--edge-1);
    white-space: nowrap;
  }
  .td {
    padding: 2px 0;
    font-size: var(--fs-2xs);
    white-space: nowrap;
    border-bottom: 1px solid var(--edge-0);
  }
  /* a source POB has a tooltip for: dotted underline, as a hint it can be pointed at */
  .td.src {
    cursor: help;
    text-decoration: underline dotted var(--ink-3);
    text-underline-offset: 3px;
  }
  .th.wrap,
  .td.wrap {
    white-space: normal;
    overflow-wrap: anywhere;
  }
  .right {
    text-align: right;
  }
  .footer {
    margin-top: 5px;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  .radius {
    color: var(--ink-3);
  }
</style>
