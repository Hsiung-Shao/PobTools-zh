<!-- POB 的計算細項:純文字段、表格段、半徑段,原樣呈現,只換字型與顏色。
     浮層寬度跟著內容走、上限是視窗寬;放不下時表格的長文字欄換行,數值與短欄不換行,
     任何一段都不出現水平捲軸。 -->
<script lang="ts">
  import type { BreakdownSection } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import PobText from "./PobText.svelte";

  let { sections }: { sections: BreakdownSection[] } = $props();

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
          {#each s.rows as row}
            {#each s.cols as c, i}
              <div class="td num" class:right={c.right} class:wrap={wrap[i]}><PobText text={row[c.key] ?? ""} muted="var(--ink-1)" /></div>
            {/each}
          {/each}
        </div>
        {#if s.footer}<div class="footer"><PobText text={s.footer} muted="var(--ink-2)" /></div>{/if}
      </div>
    {:else if s.type === "radius"}
      <div class="radius">{t("breakdown.radius", { radius: s.radius })}</div>
    {/if}
  {/each}
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
    color: var(--ink-3);
    border-bottom: 1px solid var(--edge-1);
    white-space: nowrap;
  }
  .td {
    padding: 2px 0;
    font-size: var(--fs-2xs);
    white-space: nowrap;
    border-bottom: 1px solid var(--edge-0);
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
