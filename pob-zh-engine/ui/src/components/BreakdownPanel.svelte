<!-- Adapted from pob-redux (MIT, (c) 2026 Judd): strings through i18n. See NOTICE.md. -->
<script lang="ts">
  import type { BreakdownSection } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import PobText from "./PobText.svelte";

  let { sections }: { sections: BreakdownSection[] } = $props();
</script>

<div class="bd">
  {#each sections as s}
    {#if s.type === "text"}
      <div class="txt" class:big={s.size >= 16}>
        {#each s.lines as line}
          <div class="tl"><PobText text={line} /></div>
        {/each}
      </div>
    {:else if s.type === "table"}
      <div class="tbl">
        {#if s.label}<div class="tlabel"><PobText text={s.label} /></div>{/if}
        <div class="grid" style:grid-template-columns={`repeat(${s.cols.length}, auto)`}>
          {#each s.cols as c}
            <div class="th" class:r={c.right}>{c.label}</div>
          {/each}
          {#each s.rows as row}
            {#each s.cols as c}
              <div class="td" class:r={c.right}><PobText text={row[c.key] ?? ""} /></div>
            {/each}
          {/each}
        </div>
        {#if s.footer}<div class="tfoot"><PobText text={s.footer} /></div>{/if}
      </div>
    {:else if s.type === "radius"}
      <div class="dim small">{t("breakdown.radius", { radius: s.radius })}</div>
    {/if}
  {/each}
  {#if sections.length === 0}
    <div class="dim small">{t("breakdown.none")}</div>
  {/if}
</div>

<style>
  .bd {
    display: flex;
    flex-direction: column;
    gap: 10px;
    font-size: var(--fs-xs);
    line-height: 1.45;
  }
  .txt .tl {
    color: var(--fg-1);
    white-space: pre-wrap;
  }
  .txt.big .tl {
    font-size: var(--fs-sm);
  }
  .tlabel {
    font-weight: 600;
    color: var(--fg-0);
    margin-bottom: 4px;
  }
  .grid {
    display: grid;
    gap: 1px 14px;
    overflow-x: auto;
    align-items: baseline;
  }
  .th {
    font-size: var(--fs-2xs);
    font-weight: 600;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    color: var(--fg-2);
    padding-bottom: 3px;
    border-bottom: 1px solid var(--line-1);
    white-space: nowrap;
  }
  .td {
    color: var(--fg-1);
    white-space: nowrap;
    font-family: var(--font-mono);
    font-size: var(--fs-2xs);
  }
  .r {
    text-align: right;
  }
  .tfoot {
    margin-top: 4px;
    color: var(--fg-2);
    white-space: pre-wrap;
  }
  .small {
    font-size: var(--fs-xs);
  }
</style>
