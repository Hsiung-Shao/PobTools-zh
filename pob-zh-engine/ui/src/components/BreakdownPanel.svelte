<!-- POB 的計算細項:純文字段、表格段、半徑段,原樣呈現,只換字型與顏色。 -->
<script lang="ts">
  import type { BreakdownSection } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import PobText from "./PobText.svelte";

  let { sections }: { sections: BreakdownSection[] } = $props();
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
      <div class="table">
        {#if s.label}<div class="caption"><PobText text={s.label} /></div>{/if}
        <div class="grid" style:grid-template-columns={`repeat(${s.cols.length}, auto)`}>
          {#each s.cols as c}
            <div class="th" class:right={c.right}>{c.label}</div>
          {/each}
          {#each s.rows as row}
            {#each s.cols as c}
              <div class="td num" class:right={c.right}><PobText text={row[c.key] ?? ""} muted="var(--ink-1)" /></div>
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
    overflow-x: auto;
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
  .right {
    text-align: right;
  }
  .footer {
    margin-top: 5px;
    white-space: pre-wrap;
  }
  .radius {
    color: var(--ink-3);
  }
</style>
