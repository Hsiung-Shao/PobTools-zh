<!-- 計算頁:POB 的 CalcSections 依 group 排成卡片牆,cell 文字由 POB 的 formatCalcStr 產出;
     有細項的 cell hover 開細項浮層(與側欄同一個 BreakdownPanel)。 -->
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
    const x = Math.min(e.clientX + 16, window.innerWidth - 600);
    const y = Math.min(e.clientY + 14, window.innerHeight - 380);
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
</script>

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
        {#each data.sections.filter((s) => s.id !== "SkillSelect") as sec (sec.si)}
          <section class="card" class:off={!sec.enabled} style:--sec={pobColor(sec.colour) ?? "var(--gold)"} style:grid-column={`span ${Math.min(3, Math.max(1, sec.widthCols))}`}>
            {#each sec.subsections as sub (sub.ui)}
              <div class="sub">
                <div class="subhead">
                  <span>{sub.labelZh || sub.label || sec.id}</span>
                  {#if sub.extra}<span class="extra"><PobText text={sub.extra} muted="var(--ink-2)" /></span>{/if}
                </div>
                {#if sec.enabled}
                  {#each sub.rows.filter((r) => rowMatch(r.label, r.labelZh)) as row (row.ri)}
                    <div class="row" style:font-size={row.textSize ? `${Math.max(10, row.textSize - 2)}px` : undefined}>
                      {#if row.label}<span class="rl" style:color={pobColor(row.color) ?? "var(--ink-2)"}>{row.labelZh || row.label}</span>{/if}
                      {#each row.cells.filter((c) => !isSelector(c)) as c (c.ci)}
                        {@const key = `${sec.si}:${sub.ui}:${row.ri}:${c.ci}`}
                        <!-- svelte-ignore a11y_click_events_have_key_events a11y_no_static_element_interactions -->
                        <span
                          class="cell"
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
                {:else}
                  <div class="dim small pad">{t("calcs.hidden")}</div>
                {/if}
              </div>
            {/each}
          </section>
        {/each}
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
    padding: 12px 14px 24px;
  }
  .wall {
    display: grid;
    grid-template-columns: repeat(3, minmax(280px, 1fr));
    gap: 10px;
    align-items: start;
  }
  .card {
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-top: 2px solid var(--sec);
    border-radius: var(--radius-m);
    padding: 6px 10px 8px;
    min-width: 0;
  }
  .card.off {
    opacity: 0.5;
  }
  .sub + .sub {
    margin-top: 8px;
    padding-top: 6px;
    border-top: 1px solid var(--edge-0);
  }
  .subhead {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    margin-bottom: 3px;
    font-size: var(--fs-2xs);
    letter-spacing: 0.12em;
    font-weight: 600;
    color: var(--ink-2);
  }
  .extra {
    letter-spacing: 0;
    font-weight: 400;
  }
  .row {
    display: flex;
    align-items: baseline;
    gap: 6px;
    min-height: 18px;
    font-size: var(--fs-xs);
    border-radius: 2px;
  }
  .rl {
    flex: 0 0 132px;
    text-align: right;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .cell {
    flex: 1;
    min-width: 0;
    padding: 0 4px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  .cell.live {
    cursor: default;
    box-shadow: inset 2px 0 0 var(--edge-2);
  }
  .cell.live:hover,
  .cell.pinned {
    background: var(--gold-soft);
    box-shadow: inset 2px 0 0 var(--gold);
  }
  .pad {
    padding: 4px 0;
  }
  .small {
    font-size: var(--fs-2xs);
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
