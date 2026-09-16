<!-- Stat list + breakdown popup after pob-redux's Sidebar (MIT, (c) 2026 Judd);
     the header (class/ascendancy/level/points) is read-only here and the
     loadout / main-skill controls are not part of Phase 1. See NOTICE.md. -->
<script lang="ts">
  import PobText from "./PobText.svelte";
  import BreakdownPanel from "./BreakdownPanel.svelte";
  import { api, type BreakdownSection, type SidebarRow } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { groupSidebar } from "$lib/sidebar-groups";
  import { app } from "$lib/state.svelte";

  // breakdown popup for hovered/pinned stat rows
  let bd = $state<{ sections: BreakdownSection[]; row: number; y: number; pinned: boolean } | null>(null);
  let bdTimer = 0;
  const bdCache = new Map<string, BreakdownSection[]>();

  function rowBreakdown(clientY: number, rowIndex: number, pin: boolean) {
    clearTimeout(bdTimer);
    if (pin && bd?.pinned && bd.row === rowIndex) {
      bd = null;
      return;
    }
    const y = Math.max(40, Math.min(clientY - 40, window.innerHeight - 420));
    const key = `${rowIndex}:${app.rev}`;
    const apply = (sections: BreakdownSection[]) => {
      bd = { sections, row: rowIndex, y, pinned: pin || (bd?.pinned && bd.row === rowIndex) || false };
    };
    const cached = bdCache.get(key);
    if (cached) {
      apply(cached);
      return;
    }
    bdTimer = window.setTimeout(async () => {
      try {
        const r = await api.sidebarBreakdown(rowIndex);
        bdCache.set(key, r.sections);
        apply(r.sections);
      } catch {
        /* row changed under us */
      }
    }, pin ? 0 : 140);
  }

  function rowLeave() {
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

  // PoB emits "label:" / "value" pairs plus header rows (only lhs) and spacer
  // rows (no text).
  function kind(r: SidebarRow) {
    if (!r.lhs && !r.rhs) return "space";
    if (r.align === "CENTER_X") return "center";
    if (r.lhs && !r.rhs) return "head";
    return "row";
  }
</script>

<aside class="sidebar">
  {#if info}
    <section class="head">
      <div class="buildname">
        <span class="label">{t("sidebar.build")}</span>
        <span class="bn" title={info.dbFileName ?? ""}>{info.buildName}</span>
      </div>
      <div class="row2">
        <div class="field">
          <span class="label">{t("sidebar.class")}</span>
          <span class="val">{info.classNameZh ?? info.className ?? ""}</span>
        </div>
        <div class="field">
          <span class="label">{t("sidebar.ascendancy")}</span>
          <span class="val">{info.ascendClassNameZh ?? info.ascendClassName ?? t("sidebar.none")}</span>
        </div>
      </div>
      <div class="row2">
        <div class="field">
          <span class="label">{t("sidebar.level")}</span>
          <span class="val num">{info.level}</span>
        </div>
        <div class="field" title={info.points.req ?? ""}>
          <span class="label">{t("sidebar.points")}</span>
          <div class="pts num">
            <span class:over={info.points.usedMax != null && info.points.used > info.points.usedMax}>
              {info.points.used}<span class="dim">/{info.points.usedMax ?? "?"}</span>
            </span>
            <span class="sep">·</span>
            <span class:over={info.points.ascMax != null && info.points.ascUsed > info.points.ascMax}>
              {info.points.ascUsed}<span class="dim">/{info.points.ascMax ?? "?"}</span>
            </span>
            <span class="dim label2">{t("sidebar.asc")}</span>
          </div>
        </div>
      </div>
    </section>

    <div class="stats" class:busy={app.busy > 0}>
      {#if side}
        {#each sections as sec (sec.key)}
          {#if sec.label}
            <div class="sgroup"><span>{sec.label}</span></div>
          {/if}
          {#each sec.items as { row: r, index: rowIndex } (rowIndex)}
            {@const k = kind(r)}
            {#if k === "space"}
              <div class="space"></div>
            {:else if k === "head"}
              <div class="shead"><PobText text={r.lhs} /></div>
            {:else if k === "center"}
              <div class="scenter"><PobText text={r.lhs} defaultColor="var(--fg-2)" /></div>
            {:else}
              <div
                class="srow"
                class:hasbd={r.hasBreakdown}
                class:pinnedrow={bd?.pinned && bd.row === rowIndex}
                role="button"
                tabindex={r.hasBreakdown ? 0 : -1}
                title={r.lhsRaw !== r.lhs ? r.lhsRaw : undefined}
                onmouseenter={(e) => r.hasBreakdown && rowBreakdown(e.clientY, rowIndex, false)}
                onmouseleave={rowLeave}
                onclick={(e) => r.hasBreakdown && rowBreakdown(e.clientY, rowIndex, true)}
                onkeydown={(e) => e.key === "Enter" && r.hasBreakdown && rowBreakdown(200, rowIndex, true)}
              >
                <span class="k"><PobText text={r.lhs?.replace(/[:：]\s*$/, "")} defaultColor="var(--fg-1)" /></span>
                <span class="v num"><PobText text={r.rhs} /></span>
              </div>
            {/if}
          {/each}
        {/each}
        {#if side.warnings.length}
          <div class="warnings">
            <div class="label" style:color="var(--warn)">{t("sidebar.warnings")}</div>
            {#each side.warnings as w}
              <div class="warn"><PobText text={w.text} /></div>
            {/each}
          </div>
        {/if}
      {/if}
    </div>
  {:else}
    <div class="empty">
      <span class="label">{t("sidebar.noBuild")}</span>
    </div>
  {/if}

  {#if bd}
    <div class="bdpop" style:top={`${bd.y}px`}>
      <div class="bdhead">
        <span class="label">{t("sidebar.breakdown")}</span>
        {#if bd.pinned}<span class="dim small">{t("sidebar.pinned")}</span>{/if}
      </div>
      <div class="bdscroll">
        <BreakdownPanel sections={bd.sections} />
      </div>
    </div>
  {/if}
</aside>

<style>
  .sidebar {
    width: var(--sidebar-w);
    display: flex;
    flex-direction: column;
    background: var(--bg-1);
    border-right: 1px solid var(--line-0);
    min-height: 0;
  }
  .head {
    padding: 10px 12px 12px;
    border-bottom: 1px solid var(--line-0);
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .row2 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
  }
  .val {
    font-size: var(--fs-sm);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .buildname {
    padding: 0 0 8px;
    margin-bottom: 2px;
    border-bottom: 1px solid var(--line-0);
    display: flex;
    flex-direction: column;
    gap: 4px;
    min-width: 0;
  }
  .bn {
    font-size: var(--fs-md);
    font-weight: 600;
    color: var(--fg-0);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .pts {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: var(--fs-sm);
    color: var(--fg-0);
  }
  .pts .sep {
    color: var(--fg-4);
  }
  .label2 {
    font-size: var(--fs-2xs);
    letter-spacing: 0.06em;
    text-transform: uppercase;
  }
  .over {
    color: var(--bad);
  }
  .stats {
    flex: 1;
    overflow-y: auto;
    padding: 8px 12px 16px;
    transition: opacity 120ms;
  }
  .stats.busy {
    opacity: 0.6;
  }
  .space {
    height: 7px;
  }
  .sgroup {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 14px 0 4px;
    font-size: var(--fs-2xs);
    font-weight: 600;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--fg-3);
  }
  .sgroup::after {
    content: "";
    flex: 1;
    height: 1px;
    background: var(--line-0);
  }
  .sgroup:first-child {
    padding-top: 4px;
  }
  .shead {
    padding: 8px 0 3px;
    font-size: var(--fs-xs);
    font-weight: 600;
    letter-spacing: 0.06em;
    color: var(--fg-2);
  }
  .scenter {
    text-align: center;
    font-size: var(--fs-xs);
    padding: 1px 0;
  }
  .srow {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    gap: 10px;
    padding: 1px 0;
    font-size: var(--fs-sm);
    line-height: 17px;
    border-radius: 2px;
  }
  .srow .k {
    color: var(--fg-1);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .srow .v {
    white-space: nowrap;
    color: var(--fg-0);
  }
  .srow.hasbd:hover,
  .srow.pinnedrow {
    background: var(--bg-2);
    margin: 0 -6px;
    padding: 1px 6px;
  }
  .srow.pinnedrow {
    box-shadow: inset 2px 0 0 var(--focus);
  }
  .bdpop {
    position: fixed;
    left: calc(var(--sidebar-w) + 8px);
    width: 560px;
    max-width: calc(100vw - var(--sidebar-w) - 24px);
    max-height: 60vh;
    display: flex;
    flex-direction: column;
    background: color-mix(in srgb, var(--bg-1) 96%, transparent);
    border: 1px solid var(--line-1);
    border-radius: var(--r-2);
    box-shadow: var(--shadow-pop);
    backdrop-filter: blur(8px);
    z-index: 20;
    pointer-events: none;
  }
  .bdhead {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    padding: 8px 12px 6px;
    border-bottom: 1px solid var(--line-0);
  }
  .bdscroll {
    padding: 10px 12px;
    overflow-y: auto;
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .warnings {
    margin-top: 12px;
    padding-top: 10px;
    border-top: 1px solid var(--line-0);
    display: flex;
    flex-direction: column;
    gap: 5px;
  }
  .warn {
    font-size: var(--fs-xs);
    color: var(--warn);
    line-height: 1.35;
  }
  .empty {
    flex: 1;
    display: grid;
    place-items: center;
  }
</style>
