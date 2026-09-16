<!-- 計算頁:緊湊表格。POB CalcsTab 的欄位規則(group 1 佔前三欄、寬三欄的區段橫跨、
     group 2 第四欄、group 3 第五欄)照排;每列 16px、標籤/數值兩欄對齊;cell 文字由 POB 的
     formatCalcStr 產出,有細項的 cell hover 開細項浮層(與側欄同一個 BreakdownPanel)。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type BreakdownSection, type CalcCell, type CalcsData } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import BreakdownPanel from "../components/BreakdownPanel.svelte";
  import PobText from "../components/PobText.svelte";

  let data = $state<CalcsData | null>(null);
  let loadedRev = -1;
  let search = $state("");
  let bd = $state<{ sections: BreakdownSection[]; x: number; y: number; pinned: boolean; key: string } | null>(null);
  let bdTimer = 0;
  const bdCache = new Map<string, BreakdownSection[]>();

  async function reload() {
    const r = await app.run(() => api.getCalcs());
    if (r) {
      data = r;
      loadedRev = r.rev;
      bdCache.clear();
      if (bd && !bd.pinned) bd = null;
    }
  }
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });
  async function setInput(v: string, value: unknown) {
    const r = await app.run(() => api.setCalcsInput(v, value));
    if (r) {
      await reload();
      await app.afterTreeChange();
    }
  }

  function pobColor(code: string | undefined): string | null {
    const m = code ? /^\^x([0-9a-fA-F]{6})/.exec(code) : null;
    return m ? `#${m[1]}` : null;
  }
  function rowMatch(label: string | undefined, labelZh: string | undefined): boolean {
    const f = search.trim().toLowerCase();
    if (!f) return true;
    return (label ?? "").toLowerCase().includes(f) || (labelZh ?? "").toLowerCase().includes(f);
  }

  function showBd(key: string, si: number, ui: number, ri: number, ci: number, e: MouseEvent, pin: boolean) {
    clearTimeout(bdTimer);
    if (pin && bd?.pinned && bd.key === key) {
      bd = null;
      return;
    }
    if (bd?.pinned && !pin) return;
    const x = Math.round(Math.min(e.clientX + 16, window.innerWidth - 600));
    const y = Math.round(Math.min(e.clientY + 14, window.innerHeight - 380));
    const hit = bdCache.get(key);
    if (hit) {
      bd = { sections: hit, x, y, pinned: pin, key };
      return;
    }
    bdTimer = window.setTimeout(async () => {
      try {
        const r = await api.calcsBreakdown(si, ui, ri, ci);
        bdCache.set(key, r.sections);
        bd = { sections: r.sections, x, y, pinned: pin, key };
      } catch {
        bd = null;
      }
    }, pin ? 0 : 140);
  }
  function leaveBd() {
    clearTimeout(bdTimer);
    if (bd && !bd.pinned) bd = null;
  }
  const isSelector = (c: CalcCell) => !!c.control;
  // POB places every cell at a column index (`ci`); a row that has no cell in a
  // column leaves it empty, so rows are laid out on one grid per subsection
  // with as many value columns as the widest row uses.
  const colCount = (sub: CalcsData["sections"][number]["subsections"][number]) =>
    Math.max(1, ...sub.rows.flatMap((r) => r.cells.filter((c) => !isSelector(c)).map((c) => c.ci)));

  // POB's column groups (CalcsTab:235-300): 1 = the first three columns, 2 = the fourth, 3 = the fifth
  const groups = $derived.by(() => {
    const g: Record<1 | 2 | 3, CalcsData["sections"]> = { 1: [], 2: [], 3: [] };
    for (const s of data?.sections ?? []) {
      if (s.id === "SkillSelect") continue;
      const k = (s.group === 2 || s.group === 3 ? s.group : 1) as 1 | 2 | 3;
      g[k].push(s);
    }
    return g;
  });
  // subsection collapse follows POB's own `collapsed`, toggled by clicking the head
  let collapsed = $state<Record<string, boolean>>({});
  const isCollapsed = (si: number, ui: number, def: boolean) => collapsed[`${si}:${ui}`] ?? def;
  const toggle = (si: number, ui: number, def: boolean) => (collapsed[`${si}:${ui}`] = !isCollapsed(si, ui, def));
</script>

{#snippet section(sec: CalcsData["sections"][number])}
  <section class="card" class:off={!sec.enabled} class:wide={sec.widthCols >= 3} style:--sec={pobColor(sec.colour) ?? "var(--gold)"}>
    {#each sec.subsections as sub (sub.ui)}
      {@const rows = sec.enabled ? sub.rows.filter((r) => rowMatch(r.label, r.labelZh)) : []}
      {@const closed = isCollapsed(sec.si, sub.ui, sub.collapsed)}
      {@const ncol = colCount(sub)}
      <div class="sub" class:multi={ncol > 1} style:--ncol={ncol}>
        <button class="subhead" onclick={() => toggle(sec.si, sub.ui, sub.collapsed)} title={closed ? "+" : "−"}>
          <span class="caret" class:closed>▾</span>
          <span class="st">{sub.labelZh || sub.label || sec.id}</span>
          {#if sub.extra}<span class="extra"><PobText text={sub.extra} muted="var(--ink-2)" /></span>{/if}
          {#if !sec.enabled}<span class="extra dim">{t("calcs.hidden")}</span>{/if}
        </button>
        {#if sec.enabled && !closed}
          {#each rows as row (row.ri)}
            {@const cells = row.cells.filter((c) => !isSelector(c))}
            <div class="row" style:font-size={row.textSize && row.textSize > 16 ? `${row.textSize - 4}px` : undefined}>
              {#if row.label}<span class="rl" style:color={pobColor(row.color) ?? "var(--ink-2)"}>{row.labelZh || row.label}</span>{:else}<span class="rl"></span>{/if}
              {#each cells as c (c.ci)}
                {@const key = `${sec.si}:${sub.ui}:${row.ri}:${c.ci}`}
                <!-- svelte-ignore a11y_click_events_have_key_events a11y_no_static_element_interactions -->
                <span
                  class="cell"
                  style:grid-column={cells.length === 1 && ncol > 1 ? `${c.ci + 1} / -1` : c.ci + 1}
                  role={c.hasBreakdown ? "button" : undefined}
                  class:live={c.hasBreakdown}
                  class:pinned={bd?.pinned && bd.key === key}
                  onmouseenter={(e) => c.hasBreakdown && showBd(key, sec.si, sub.ui, row.ri, c.ci, e, false)}
                  onmouseleave={leaveBd}
                  onclick={(e) => c.hasBreakdown && showBd(key, sec.si, sub.ui, row.ri, c.ci, e, true)}
                >
                  <PobText text={c.text ?? ""} />
                </span>
              {/each}
            </div>
          {/each}
        {/if}
      </div>
    {/each}
  </section>
{/snippet}

<div class="page">
  <div class="bar">
    {#if data}
      <span class="k">{t("calcs.skill")}</span>
      <select class="select sm wide" value={data.input.skill_number} onchange={(e) => setInput("skill_number", Number(e.currentTarget.value))}>
        {#each data.selectors.mainSocketGroup?.list ?? [] as o}
          <option value={o.val}>{o.labelZh || o.label}</option>
        {/each}
      </select>
      {#if data.selectors.mainSkill}
        <select class="select sm" value={data.selectors.mainSkill.index} onchange={(e) => setInput("mainActiveSkill", Number(e.currentTarget.value))}>
          {#each data.selectors.mainSkill.list as o}<option value={o.val}>{o.labelZh || o.label}</option>{/each}
        </select>
      {/if}
      {#if data.selectors.mainSkillPart}
        <select class="select sm" value={data.selectors.mainSkillPart.index} onchange={(e) => setInput("skillPart", Number(e.currentTarget.value))}>
          {#each data.selectors.mainSkillPart.list as o}<option value={o.val}>{o.labelZh || o.label}</option>{/each}
        </select>
      {/if}
      <span class="k">{t("calcs.mode")}</span>
      <select class="select sm" value={data.input.misc_buffMode} onchange={(e) => setInput("misc_buffMode", e.currentTarget.value)}>
        {#each ["EFFECTIVE", "COMBAT", "BUFFED", "UNBUFFED"] as m}<option value={m}>{t(`calcs.mode.${m}`)}</option>{/each}
      </select>
      {#if data.hasMinion}
        <label class="chk"><input type="checkbox" checked={data.input.showMinion} onchange={(e) => setInput("showMinion", e.currentTarget.checked)} /> {t("calcs.showMinion")}</label>
      {/if}
    {/if}
    <span class="grow"></span>
    <input class="input sm" placeholder={t("calcs.search")} bind:value={search} />
  </div>

  <div class="scroll">
    {#if data}
      <div class="wall">
        <div class="g1">
          {#each groups[1] as sec (sec.si)}{@render section(sec)}{/each}
        </div>
        <div class="gcol">
          {#each groups[2] as sec (sec.si)}{@render section(sec)}{/each}
        </div>
        <div class="gcol">
          {#each groups[3] as sec (sec.si)}{@render section(sec)}{/each}
        </div>
      </div>
    {/if}
  </div>

  {#if bd}
    <div class="float" style:left={`${bd.x}px`} style:top={`${bd.y}px`}>
      <div class="fhead"><span class="label">{t("calcs.breakdown")}</span>{#if bd.pinned}<span class="pin">{t("sidebar.pinned")}</span>{/if}</div>
      <div class="fbody"><BreakdownPanel sections={bd.sections} /></div>
    </div>
  {/if}
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .bar {
    display: flex;
    align-items: center;
    gap: 8px;
    height: 36px;
    padding: 0 12px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
    flex-wrap: nowrap;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
    white-space: nowrap;
  }
  .select.sm,
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
    max-width: 220px;
  }
  .select.wide {
    max-width: 320px;
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: var(--fs-xs);
    white-space: nowrap;
  }
  .grow {
    flex: 1;
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 8px 10px 20px;
  }
  /* POB's five columns: group 1 spans the first three, groups 2 and 3 take one each */
  .wall {
    display: grid;
    grid-template-columns: minmax(0, 3fr) minmax(230px, 1fr) minmax(230px, 1fr);
    gap: 8px;
    align-items: start;
  }
  .g1 {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    grid-auto-flow: dense;
    gap: 8px;
    align-items: start;
    min-width: 0;
  }
  .gcol {
    display: flex;
    flex-direction: column;
    gap: 8px;
    min-width: 0;
  }
  @media (max-width: 1360px) {
    .wall {
      grid-template-columns: 1fr;
    }
    .gcol {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(260px, 1fr));
    }
  }
  .card {
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-top: 2px solid var(--sec);
    border-radius: var(--radius-s);
    padding: 0 6px 3px;
    min-width: 0;
  }
  .card.wide {
    grid-column: 1 / -1;
  }
  .card.off {
    opacity: 0.45;
  }
  .sub + .sub {
    border-top: 1px solid var(--edge-0);
  }
  .subhead {
    appearance: none;
    width: 100%;
    display: flex;
    align-items: center;
    gap: 6px;
    height: 18px;
    padding: 0;
    margin: 0;
    background: none;
    border: 0;
    color: var(--ink-2);
    font: inherit;
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    font-weight: 600;
    text-align: left;
    cursor: pointer;
  }
  .subhead:hover .st {
    color: var(--ink-0);
  }
  .caret {
    display: inline-block;
    width: 8px;
    font-size: 9px;
    color: var(--ink-3);
    transition: transform 0.1s;
  }
  .caret.closed {
    transform: rotate(-90deg);
  }
  .st {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .extra {
    margin-left: auto;
    letter-spacing: 0;
    font-weight: 400;
    white-space: nowrap;
  }
  /* one row = 16px: label column right-aligned, then POB's value columns on a
     shared grid so column N lines up down the whole subsection */
  .sub {
    --label-w: minmax(0, 46%);
  }
  .sub.multi {
    --label-w: 150px;
  }
  .row {
    display: grid;
    grid-template-columns: var(--label-w) repeat(var(--ncol, 1), minmax(0, 1fr));
    align-items: baseline;
    height: 16px;
    line-height: 16px;
    font-size: var(--fs-xs);
  }
  .row:hover {
    background: var(--surface-hover);
  }
  .rl {
    grid-column: 1;
    max-width: 190px;
    padding-right: 6px;
    text-align: right;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .cell {
    min-width: 0;
    padding: 0 3px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
    border-left: 1px solid var(--edge-0);
  }
  .cell.live {
    cursor: default;
    border-left-color: var(--edge-2);
  }
  .cell.live:hover,
  .cell.pinned {
    background: var(--gold-soft);
    border-left-color: var(--gold);
  }
  .float {
    position: fixed;
    z-index: 40;
    width: 580px;
    max-width: calc(100vw - 40px);
    max-height: 60vh;
    display: flex;
    flex-direction: column;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    pointer-events: none;
  }
  .fhead {
    display: flex;
    justify-content: space-between;
    padding: 8px 14px 6px;
    border-bottom: 1px solid var(--edge-0);
  }
  .pin {
    font-size: var(--fs-2xs);
    color: var(--gold);
  }
  .fbody {
    padding: 10px 14px 12px;
    overflow-y: auto;
  }
</style>
