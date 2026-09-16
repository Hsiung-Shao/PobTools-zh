<!-- 左側欄:建置摘要 + POB 的計算側欄(分段)+ 細項浮層。
     摘要區唯讀;細項用 hover 看、點一下釘住。 -->
<script lang="ts">
  import PobText from "./PobText.svelte";
  import BreakdownPanel from "./BreakdownPanel.svelte";
  import { api, type BreakdownSection, type SidebarRow } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { groupSidebar } from "$lib/sidebar-groups";
  import { app } from "$lib/state.svelte";

  let bd = $state<{ sections: BreakdownSection[]; row: number; y: number; pinned: boolean } | null>(null);
  let bdTimer = 0;
  const bdCache = new Map<string, BreakdownSection[]>();

  function showBreakdown(clientY: number, rowIndex: number, pin: boolean) {
    clearTimeout(bdTimer);
    if (pin && bd?.pinned && bd.row === rowIndex) {
      bd = null;
      return;
    }
    const y = Math.round(Math.max(48, Math.min(clientY - 36, window.innerHeight - 440)));
    const key = `${rowIndex}:${app.rev}`;
    const show = (sections: BreakdownSection[]) => {
      bd = { sections, row: rowIndex, y, pinned: pin || (bd?.pinned && bd.row === rowIndex) || false };
    };
    const hit = bdCache.get(key);
    if (hit) {
      show(hit);
      return;
    }
    bdTimer = window.setTimeout(async () => {
      try {
        const r = await api.sidebarBreakdown(rowIndex);
        bdCache.set(key, r.sections);
        show(r.sections);
      } catch {
        /* the list changed under the request */
      }
    }, pin ? 0 : 120);
  }

  function leaveRow() {
    clearTimeout(bdTimer);
    if (bd && !bd.pinned) bd = null;
  }

  $effect(() => {
    app.rev;
    bdCache.clear();
    bd = null;
  });

  const info = $derived(app.info);
  const side = $derived(app.sidebar);
  const sections = $derived(side ? groupSidebar(side.rows) : []);

  // POB row shapes: "label:" + value, a heading (label only), a centred
  // message, or an empty spacer.
  function shape(r: SidebarRow) {
    if (!r.lhs && !r.rhs) return "gap";
    if (r.align === "CENTER_X") return "note";
    if (r.lhs && !r.rhs) return "heading";
    return "pair";
  }

  const overCap = (used: number, max?: number | null) => max != null && used > max;
</script>

<aside class="side">
  {#if info}
    <section class="summary">
      <div class="name" title={info.dbFileName ?? ""}>{info.buildName}</div>
      <div class="who">
        <span class="cls">{info.ascendClassNameZh ?? info.ascendClassName ?? info.classNameZh ?? info.className ?? ""}</span>
        {#if info.ascendClassName}<span class="base dim">{info.classNameZh ?? info.className}</span>{/if}
      </div>
      <dl class="facts">
        <div>
          <dt>{t("sidebar.level")}</dt>
          <dd class="num">{info.level}</dd>
        </div>
        <div title={info.points.req ?? ""}>
          <dt>{t("sidebar.points")}</dt>
          <dd class="num">
            <b class:over={overCap(info.points.used, info.points.usedMax)}>{info.points.used}</b><span class="dim">/{info.points.usedMax ?? "?"}</span>
          </dd>
        </div>
        <div>
          <dt>{t("sidebar.asc")}</dt>
          <dd class="num">
            <b class:over={overCap(info.points.ascUsed, info.points.ascMax)}>{info.points.ascUsed}</b><span class="dim">/{info.points.ascMax ?? "?"}</span>
          </dd>
        </div>
      </dl>
    </section>

    <div class="stats" class:busy={app.busy > 0}>
      {#if side}
        {#each sections as sec (sec.key)}
          {#if sec.label}
            <h3 class="sec">{sec.label}</h3>
          {/if}
          {#each sec.items as { row: r, index: rowIndex } (rowIndex)}
            {@const s = shape(r)}
            {#if s === "gap"}
              <div class="gap"></div>
            {:else if s === "heading"}
              <div class="heading"><PobText text={r.lhs} muted="var(--ink-2)" /></div>
            {:else if s === "note"}
              <div class="note"><PobText text={r.lhs} muted="var(--ink-2)" /></div>
            {:else}
              <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
              <div
                class="pair"
                class:live={r.hasBreakdown}
                class:pinned={bd?.pinned && bd.row === rowIndex}
                role={r.hasBreakdown ? "button" : undefined}
                tabindex={r.hasBreakdown ? 0 : undefined}
                title={r.lhsRaw !== r.lhs ? r.lhsRaw : undefined}
                onmouseenter={(e) => r.hasBreakdown && showBreakdown(e.clientY, rowIndex, false)}
                onmouseleave={leaveRow}
                onclick={(e) => r.hasBreakdown && showBreakdown(e.clientY, rowIndex, true)}
                onkeydown={(e) => e.key === "Enter" && r.hasBreakdown && showBreakdown(200, rowIndex, true)}
              >
                <span class="k"><PobText text={r.lhs?.replace(/[:：]\s*$/, "")} muted="var(--ink-1)" /></span>
                <span class="dots"></span>
                <span class="v num"><PobText text={r.rhs} /></span>
              </div>
            {/if}
          {/each}
        {/each}
        {#if side.warnings.length}
          <h3 class="sec warn">{t("sidebar.warnings")}</h3>
          {#each side.warnings as w}
            <div class="warning"><PobText text={w.text} /></div>
          {/each}
        {/if}
      {/if}
    </div>
  {:else}
    <div class="empty"><span class="label">{t("sidebar.noBuild")}</span></div>
  {/if}

  {#if bd}
    <div class="float" style:top={`${bd.y}px`}>
      <div class="float-head">
        <span class="label">{t("sidebar.breakdown")}</span>
        {#if bd.pinned}<span class="pin">{t("sidebar.pinned")}</span>{/if}
      </div>
      <div class="float-body">
        <BreakdownPanel sections={bd.sections} />
      </div>
    </div>
  {/if}
</aside>

<style>
  .side {
    width: var(--sidebar-w);
    display: flex;
    flex-direction: column;
    min-height: 0;
    background: var(--surface-1);
    border-right: 1px solid var(--edge-0);
  }

  /* 摘要:名稱大字、職業一行、三個數字並排 */
  .summary {
    padding: 14px 16px 12px;
    border-bottom: 1px solid var(--edge-0);
    background: linear-gradient(180deg, var(--surface-2), var(--surface-1));
  }
  .name {
    font-size: var(--fs-lg);
    font-weight: 600;
    line-height: 1.3;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .who {
    display: flex;
    align-items: baseline;
    gap: 8px;
    margin-top: 2px;
    font-size: var(--fs-sm);
  }
  .cls {
    color: var(--gold);
  }
  .base {
    font-size: var(--fs-xs);
  }
  .facts {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 8px;
    margin: 12px 0 0;
  }
  .facts div {
    padding: 6px 8px;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    background: var(--surface-1);
  }
  .facts dt {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .facts dd {
    margin: 2px 0 0;
    font-size: var(--fs-md);
    color: var(--ink-0);
  }
  .facts b {
    font-weight: 600;
  }
  .over {
    color: var(--bad);
  }

  /* 統計列表 */
  .stats {
    flex: 1;
    overflow-y: auto;
    padding: 6px 12px 18px 16px;
    transition: opacity 120ms;
  }
  .stats.busy {
    opacity: 0.55;
  }
  .sec {
    margin: 16px 0 4px;
    padding-left: 10px;
    position: relative;
    font-size: var(--fs-2xs);
    font-weight: 600;
    letter-spacing: 0.14em;
    color: var(--ink-2);
  }
  .sec::before {
    content: "";
    position: absolute;
    left: 0;
    top: 3px;
    bottom: 3px;
    width: 2px;
    background: var(--gold);
    border-radius: 1px;
  }
  .sec.warn {
    color: var(--warn);
  }
  .sec.warn::before {
    background: var(--warn);
  }
  .stats > .sec:first-child {
    margin-top: 6px;
  }
  .gap {
    height: 6px;
  }
  .heading {
    padding: 8px 0 2px;
    font-size: var(--fs-xs);
    letter-spacing: 0.04em;
  }
  .note {
    padding: 4px 0;
    text-align: center;
    font-size: var(--fs-xs);
    line-height: 1.4;
  }
  .pair {
    display: flex;
    align-items: baseline;
    gap: 6px;
    height: 18px;
    margin: 0 -6px;
    padding: 0 6px;
    font-size: var(--fs-sm);
    border-radius: var(--radius-s);
  }
  .pair .k {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .pair .dots {
    flex: 1;
    min-width: 8px;
    border-bottom: 1px dotted var(--edge-1);
    transform: translateY(-4px);
    opacity: 0.7;
  }
  .pair .v {
    white-space: nowrap;
  }
  .pair.live {
    cursor: default;
  }
  .pair.live:hover,
  .pair.pinned {
    background: var(--gold-soft);
  }
  .pair.pinned {
    box-shadow: inset 2px 0 0 var(--gold);
  }
  .warning {
    padding: 2px 0;
    font-size: var(--fs-xs);
    line-height: 1.4;
    color: var(--warn);
  }
  .empty {
    flex: 1;
    display: grid;
    place-items: center;
  }

  /* 細項浮層:貼在側欄右邊 */
  .float {
    position: fixed;
    left: calc(var(--sidebar-w) + 10px);
    width: 580px;
    max-width: calc(100vw - var(--sidebar-w) - 28px);
    max-height: 62vh;
    display: flex;
    flex-direction: column;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    z-index: 20;
    pointer-events: none;
  }
  .float-head {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    padding: 8px 14px 6px;
    border-bottom: 1px solid var(--edge-0);
  }
  .pin {
    font-size: var(--fs-2xs);
    color: var(--gold);
    letter-spacing: 0.08em;
  }
  .float-body {
    padding: 10px 14px 12px;
    overflow-y: auto;
  }
</style>
