<!-- POB 的「關於」:版本歷史與說明兩份清單(main:OpenAboutPopup),照它的欄與色碼排。 -->
<script lang="ts">
  import { onMount } from "svelte";
  import { api, type AboutData } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import PobText from "./PobText.svelte";

  let { onclose }: { onclose: () => void } = $props();

  let data = $state<AboutData | null>(null);
  let err = $state<string | null>(null);
  let tab = $state<"changelog" | "help">("changelog");
  const rows = $derived(data ? (tab === "changelog" ? data.changelog : data.help) : []);

  onMount(async () => {
    try {
      data = await api.about();
    } catch (e: any) {
      err = String(e?.message ?? e);
    }
  });
</script>

<div class="modal" role="presentation" onclick={(e) => e.target === e.currentTarget && onclose()}>
  <div class="dialog">
    <div class="top">
      <span class="label">{t("about.title")}</span>
      {#if data?.version}<span class="dim"><PobText text={data.version} /></span>{/if}
      <span class="grow"></span>
      <button class="btn sm" class:primary={tab === "changelog"} onclick={() => (tab = "changelog")}>{t("about.changelog")}</button>
      <button class="btn sm" class:primary={tab === "help"} onclick={() => (tab = "help")}>{t("about.help")}</button>
      <button class="btn ghost sm" onclick={onclose}>{t("tree.done")}</button>
    </div>
    {#if err}
      <p class="bad">{err}</p>
    {:else if !data}
      <p class="dim">{t("builds.loading")}</p>
    {:else}
      <div class="list selectable">
        {#each rows as r, i (i)}
          {#if !r.cols.length}
            <div class="gap"></div>
          {:else}
            <div class="row" class:title={r.height >= 24} class:sub={r.height === 20}>
              {#if r.cols.length > 1}
                <span class="c1"><PobText text={r.cols[0]} /></span><span class="c2"><PobText text={r.cols[1]} /></span>
              {:else}
                <span><PobText text={r.cols[0]} /></span>
              {/if}
            </div>
          {/if}
        {/each}
      </div>
    {/if}
  </div>
</div>

<style>
  .modal {
    position: fixed;
    inset: 0;
    z-index: 300;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 70%, transparent);
  }
  .dialog {
    width: min(860px, calc(100vw - 32px));
    height: min(680px, calc(100vh - 32px));
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: 8px;
  }
  .top {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 8px;
  }
  .grow {
    flex: 1;
  }
  .list {
    flex: 1;
    min-height: 0;
    overflow: auto;
    font-size: var(--fs-sm);
    line-height: 1.45;
  }
  .row {
    display: flex;
    gap: 12px;
  }
  .row.title {
    font-size: 1.15em;
    font-weight: 600;
    margin-top: 6px;
  }
  .row.sub {
    font-weight: 600;
  }
  .c1 {
    flex: 0 0 120px;
  }
  .c2 {
    flex: 1;
  }
  .gap {
    height: 6px;
  }
</style>
