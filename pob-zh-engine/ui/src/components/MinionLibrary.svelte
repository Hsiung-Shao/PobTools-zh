<!-- POB 的幽魂/野獸庫(Build.lua 的 Manage Spectres...):左邊是建置中的清單,
     右邊是可選擇的召喚物,存檔時整份寫回 build.spectreList / beastList。 -->
<script lang="ts">
  import { onMount } from "svelte";
  import { api, type MinionEntry } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";

  let { kind, onclose }: { kind: "spectre" | "beast"; onclose: () => void } = $props();

  let inBuild = $state<MinionEntry[]>([]);
  let available = $state<MinionEntry[]>([]);
  let query = $state("");
  let onlyRecommended = $state(false);
  let sel = $state<string | null>(null);
  let err = $state<string | null>(null);

  const chosen = $derived(new Set(inBuild.map((m) => m.id)));
  const hits = $derived(
    available.filter(
      (m) =>
        (!onlyRecommended || m.recommended) &&
        (!query.trim() || (m.nameZh ?? m.name).includes(query.trim()) || m.name.toLowerCase().includes(query.trim().toLowerCase())),
    ),
  );

  onMount(async () => {
    try {
      const r = await api.minionLibrary(kind);
      inBuild = r.inBuild;
      available = r.available;
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  });

  function add(m: MinionEntry) {
    if (!chosen.has(m.id)) inBuild = [...inBuild, m];
  }
  function remove(id: string) {
    inBuild = inBuild.filter((m) => m.id !== id);
  }
  async function save() {
    const ok = await app.run(() => api.setMinionLibrary(kind, inBuild.map((m) => m.id)));
    if (ok) {
      await app.refresh();
      onclose();
    }
  }
</script>

<div class="modal">
  <div class="dialog">
    <div class="label">{t("minion.title")}</div>
    {#if err}<p class="bad">{err}</p>{/if}
    <div class="cols">
      <div class="col">
        <div class="chead">{t("minion.inBuild")} <span class="dim num">{inBuild.length}</span></div>
        <div class="list">
          {#each inBuild as m (m.id)}
            <div class="row">
              <span class="nm">{m.nameZh || m.name}</span>
              <button class="btn ghost sm" onclick={() => remove(m.id)}>{t("minion.remove")}</button>
            </div>
          {/each}
        </div>
      </div>
      <div class="col">
        <div class="chead">
          {t("minion.available")}
          <input class="input sm" placeholder={t("tree.search")} bind:value={query} />
          <label class="chk small"><input type="checkbox" bind:checked={onlyRecommended} /> {t("minion.recommended")}</label>
        </div>
        <div class="list">
          {#each hits.slice(0, 400) as m (m.id)}
            <div class="row" class:on={chosen.has(m.id)}>
              <span class="nm">{m.nameZh || m.name}</span>
              {#if m.category}<span class="dim small">{m.category}</span>{/if}
              <button class="btn ghost sm" disabled={chosen.has(m.id)} onclick={() => add(m)}>{t("minion.add")}</button>
            </div>
          {/each}
        </div>
      </div>
    </div>
    <p class="dim small">{t("minion.note")}</p>
    <div class="dlg-actions">
      <button class="btn ghost" onclick={onclose}>{t("tree.cancel")}</button>
      <button class="btn primary" disabled={app.busy > 0} onclick={save}>{t("bar.save")}</button>
    </div>
  </div>
</div>

<style>
  .modal {
    position: fixed;
    inset: 0;
    z-index: 220;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 70%, transparent);
  }
  .dialog {
    width: min(860px, calc(100vw - 32px));
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: 8px;
  }
  .cols {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
  }
  .col {
    min-width: 0;
  }
  .chead {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 6px;
    flex-wrap: wrap;
  }
  .list {
    height: 320px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 2px 8px;
  }
  .row:hover {
    background: var(--surface-2);
  }
  .row.on .nm {
    color: var(--gold);
  }
  .nm {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
