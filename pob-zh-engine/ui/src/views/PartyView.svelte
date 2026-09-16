<!-- 隊伍頁:七個文字區(隊友屬性/光環/詛咒/戰吼/連結/敵人狀態/敵人詞綴)各自套用,
     加上「匯出本建置增益」開關與匯出內容(唯讀)。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type PartyData } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  type FieldKey = keyof PartyData["fields"];
  const keys: FieldKey[] = ["partyMemberStats", "aura", "curse", "warcry", "link", "enemyCond", "enemyMods"];

  let data = $state<PartyData | null>(null);
  let drafts = $state<Record<string, string>>({});
  let loadedRev = -1;

  async function reload() {
    const r = await app.run(() => api.getParty());
    if (r) {
      data = r;
      loadedRev = r.rev;
      const d: Record<string, string> = {};
      for (const k of keys) d[k] = r.fields[k] ?? "";
      drafts = d;
    }
  }
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  async function apply(k: FieldKey) {
    if (!data || drafts[k] === data.fields[k]) return;
    const r = await app.run(() => api.setParty(k, drafts[k]));
    if (r) {
      await reload();
      await app.afterTreeChange();
    }
  }
  async function toggleExport(v: boolean) {
    const r = await app.run(() => api.setPartyExport(v));
    if (r) {
      await reload();
      await app.afterTreeChange();
    }
  }
</script>

<div class="page">
  <div class="bar">
    <span class="dim small">{t("party.hint")}</span>
    <span class="grow"></span>
    {#if data}
      <label class="chk"><input type="checkbox" checked={data.enableExportBuffs} onchange={(e) => toggleExport(e.currentTarget.checked)} /> {t("party.export")}</label>
    {/if}
  </div>
  <div class="scroll">
    {#if data}
      <div class="grid">
        {#each keys as k (k)}
          <section class="card">
            <div class="head">
              <h3>{t(`party.${k}`)}</h3>
              <button class="btn sm" class:primary={drafts[k] !== data.fields[k]} disabled={drafts[k] === data.fields[k] || app.busy > 0} onclick={() => apply(k)}>{t("party.apply")}</button>
            </div>
            <textarea class="input area" rows="7" bind:value={drafts[k]} spellcheck="false"></textarea>
          </section>
        {/each}
        {#if data.enableExportBuffs}
          <section class="card wide">
            <div class="head"><h3>{t("party.exports")}</h3></div>
            {#each Object.entries(data.exports) as [name, txt]}
              {#if txt}
                <div class="k">{name}</div>
                <pre class="exp selectable">{txt}</pre>
              {/if}
            {/each}
          </section>
        {/if}
      </div>
    {/if}
  </div>
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
  }
  .grow {
    flex: 1;
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: var(--fs-xs);
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 12px 14px 24px;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(360px, 1fr));
    gap: 10px;
  }
  .card {
    padding: 10px 12px 12px;
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .card.wide {
    grid-column: 1 / -1;
  }
  .head {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  h3 {
    margin: 0;
    padding-left: 10px;
    position: relative;
    font-size: var(--fs-2xs);
    letter-spacing: 0.14em;
    font-weight: 600;
    color: var(--ink-2);
  }
  h3::before {
    content: "";
    position: absolute;
    left: 0;
    top: 2px;
    bottom: 2px;
    width: 2px;
    background: var(--gold);
  }
  .area {
    height: auto;
    padding: 8px 9px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    line-height: 1.4;
    resize: vertical;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
    margin-top: 4px;
  }
  .exp {
    margin: 0;
    padding: 6px 8px;
    background: var(--surface-0);
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    font-family: var(--font-mono);
    font-size: var(--fs-2xs);
    white-space: pre-wrap;
    max-height: 200px;
    overflow-y: auto;
  }
</style>
