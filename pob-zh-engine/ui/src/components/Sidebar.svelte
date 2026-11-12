<!-- 左側欄:建置摘要 + POB 的計算側欄(分段)+ 細項浮層。
     摘要區唯讀;細項用 hover 看、點一下釘住。 -->
<script lang="ts">
  import PobText from "./PobText.svelte";
  import BreakdownPanel from "./BreakdownPanel.svelte";
  import { api, type BreakdownSection, type SidebarRow } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import SideHead from "./SideHead.svelte";
  import { groupSidebar, headlineStat } from "$lib/sidebar-groups";
  import { pobPlain } from "$lib/pobtext";
  import { loadCollapsed, saveCollapsed, sidebarSummary } from "$lib/sidebar-collapse";
  import { app } from "$lib/state.svelte";
  import { fitFloat, type FloatAnchor } from "$lib/floatFit";

  let bd = $state<{ sections: BreakdownSection[]; row: number; anchor: FloatAnchor; pinned: boolean } | null>(null);
  let sideEl: HTMLElement | undefined = $state();
  let bdTimer = 0;
  const bdCache = new Map<string, BreakdownSection[]>();

  function showBreakdown(clientY: number, rowIndex: number, pin: boolean) {
    clearTimeout(bdTimer);
    if (pin && bd?.pinned && bd.row === rowIndex) {
      bd = null;
      return;
    }
    // The panel prefers to sit right of the sidebar with its top a little above
    // the row; fitFloat moves it (above the row, over the sidebar, ...) when the
    // window has no room there, once its real size is known.
    const r = sideEl?.getBoundingClientRect();
    const anchor: FloatAnchor = {
      x: { after: (r?.right ?? 0) + 10, before: (r?.left ?? 0) - 10 },
      y: { after: clientY - 36, before: clientY + 36 },
    };
    const key = `${rowIndex}:${app.dataRev}`;
    const show = (sections: BreakdownSection[]) => {
      bd = { sections, row: rowIndex, anchor, pinned: pin || (bd?.pinned && bd.row === rowIndex) || false };
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
    app.dataRev;
    bdCache.clear();
    bd = null;
  });

  const info = $derived(app.info);
  const side = $derived(app.sidebar);
  const sections = $derived(side ? groupSidebar(side.rows) : []);
  const headline = $derived(side ? headlineStat(side.rows) : null);

  // POB row shapes: "label:" + value, a heading (label only), a centred
  // message, or an empty spacer.
  function shape(r: SidebarRow) {
    if (!r.lhs && !r.rhs) return "gap";
    if (r.align === "CENTER_X") return "note";
    if (r.lhs && !r.rhs) return "heading";
    return "pair";
  }

  // 使用者收合的分組;預設全部展開
  let folded = $state(loadCollapsed());
  function toggleGroup(key: string, rows: number[]) {
    const next = new Set(folded);
    if (!next.delete(key)) {
      next.add(key);
      // 釘住的細項若在要收合的組裡,收合後就沒地方點掉,先關掉
      if (bd && rows.includes(bd.row)) bd = null;
    }
    folded = next;
    saveCollapsed(next);
  }
</script>

<aside class="side" bind:this={sideEl}>
  {#if info}
    <section class="summary">
      <SideHead />
      {#if headline}
        {@const hl = headline}
        {@const live = side?.rows[hl.index - 1]?.hasBreakdown}
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div
          class="headline"
          class:live
          class:pinned={bd?.pinned && bd.row === hl.index}
          role={live ? "button" : undefined}
          tabindex={live ? 0 : undefined}
          onmouseenter={(e) => live && showBreakdown(e.clientY, hl.index, false)}
          onmouseleave={leaveRow}
          onclick={(e) => live && showBreakdown(e.clientY, hl.index, true)}
          onkeydown={(e) => e.key === "Enter" && live && showBreakdown(200, hl.index, true)}
        >
          <div class="hl-k"><PobText text={hl.lhs.replace(/[:：]\s*$/, "")} muted="var(--ink-2)" /></div>
          <div class="hl-v num">{pobPlain(hl.rhs)}</div>
        </div>
      {/if}
    </section>

    <div class="stats" class:busy={app.busy > 0}>
      {#if side}
        {#each sections as sec (sec.key)}
          {@const closed = !!sec.label && folded.has(sec.key)}
          {#if sec.label}
            <h3 class="sec">
              <button class="sec-btn" aria-expanded={!closed} onclick={() => toggleGroup(sec.key, sec.items.map((it) => it.index))}>
                <span>{sec.label}</span>
                {#if closed}<span class="sum num">{sidebarSummary(sec.items)}</span>{/if}
                <span class="chev" aria-hidden="true">{closed ? "▸" : "▾"}</span>
              </button>
            </h3>
          {/if}
          {#each closed ? [] : sec.items as { row: r, index: rowIndex } (rowIndex)}
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
          <h3 class="sec warn"><span class="sec-row">{t("sidebar.warnings")}<span class="sum num">{side.warnings.length}</span></span></h3>
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
    <div class="float" class:pinned={bd.pinned} use:fitFloat={bd.anchor}>
      {#if bd.pinned}<span class="pin">{t("sidebar.pinned")}</span>{/if}
      <div class="float-body">
        <BreakdownPanel sections={bd.sections} tipFor={(ref) => api.sidebarBreakdownTip(bd!.row, ref)} />
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

  /* 摘要:職業/等級/主要技能(SideHead)+ 主數字;很多技能控制項時自己捲動,不擠掉數值清單 */
  .summary {
    flex: none;
    max-height: 60%;
    overflow-y: auto;
    scrollbar-width: thin;
    background: var(--surface-1);
    border-bottom: 1px solid var(--edge-0);
  }
  .headline {
    margin: 6px 10px;
    padding: 6px 6px 8px;
    border-radius: var(--radius-s);
  }
  .hl-k {
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .hl-v {
    font-size: calc(var(--fs-base) + 13px);
    line-height: calc(var(--fs-base) + 19px);
    font-weight: 500;
    color: var(--hl);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .headline.live:hover,
  .headline.pinned {
    background: var(--gold-soft);
  }
  .headline.pinned {
    box-shadow: inset 2px 0 0 var(--gold);
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
    font-size: var(--fs-2xs);
    font-weight: 600;
    letter-spacing: 0.14em;
    color: var(--ink-2);
  }
  /* 標題文字右邊一條細線,延伸到摘要或箭頭前 */
  .sec-btn > span:first-child::after,
  .sec-row::after {
    content: "";
    flex: 1;
    min-width: 12px;
    align-self: center;
    border-top: 1px solid var(--edge-0);
  }
  .sec-row .sum {
    order: 1;
  }
  .sec-btn > span:first-child {
    flex: 1 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  /* 分組標題是收合鈕;收合時顯示該組前幾個數值 */
  .sec-btn,
  .sec-row {
    all: unset;
    box-sizing: border-box;
    width: 100%;
    display: flex;
    align-items: baseline;
    gap: 8px;
    cursor: pointer;
  }
  .sec-row {
    cursor: default;
  }
  .sec-btn:focus-visible {
    outline: 1px solid var(--focus);
    outline-offset: 2px;
  }
  .sum {
    margin-left: auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: var(--fs-xs);
    font-weight: 400;
    letter-spacing: 0;
    color: var(--ink-1);
  }
  .sec.warn .sum {
    color: var(--warn);
  }
  .chev {
    margin-left: auto;
    width: 10px;
    text-align: center;
    color: var(--ink-3);
    letter-spacing: 0;
    font-weight: 400;
  }
  .sum + .chev {
    margin-left: 0;
  }
  .sec-btn:hover .chev {
    color: var(--ink-0);
  }
  .sec.warn {
    color: var(--warn);
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
    min-width: 56px;
    text-align: right;
    font-variant-numeric: tabular-nums;
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

  /* 細項浮層:寬高跟著內容走(上限是視窗扣掉 fitFloat 的 10px 邊距),
     位置由 fitFloat 量過實際大小後決定,優先貼在側欄右邊;表格的長文字欄換行而不捲動。 */
  .float {
    position: fixed;
    left: 0;
    top: 0;
    width: max-content;
    max-width: calc(100vw - 20px);
    max-height: calc(100vh - 20px);
    display: flex;
    flex-direction: column;
    background: var(--surface-1);
    border: 1px solid var(--edge-2);
    border-radius: 4px;
    box-shadow: var(--shadow-float);
    z-index: 20;
    pointer-events: none;
  }
  /* 浮層內容照 POB 原生浮窗,不加標頭;釘住時只在右上角留一個小標記 */
  .pin {
    position: absolute;
    top: 3px;
    right: 6px;
    font-size: var(--fs-2xs);
    color: var(--gold-text);
    letter-spacing: 0.08em;
  }
  /* 最後手段:內容比整個視窗還高才在這裡捲動;釘住時讓浮層接滑鼠,滾輪才捲得到 */
  .float-body {
    min-height: 0;
    padding: 10px 14px;
    overflow-y: auto;
  }
  .float.pinned {
    pointer-events: auto;
  }
</style>
