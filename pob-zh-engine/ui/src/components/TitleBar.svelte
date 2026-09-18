<!-- 頂列:品牌、分頁、右側目前建置。分頁用底線標示,不用膠囊。 -->
<script lang="ts">
  import { api, bridge, hostInfo } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app, type ViewId } from "$lib/state.svelte";

  const tabs: { id: ViewId; key: string }[] = [
    { id: "builds", key: "title.builds" },
    { id: "tree", key: "title.tree" },
    { id: "items", key: "title.items" },
    { id: "skills", key: "title.skills" },
    { id: "config", key: "title.config" },
    { id: "calcs", key: "title.calcs" },
    { id: "notes", key: "title.notes" },
    { id: "party", key: "title.party" },
    { id: "compare", key: "title.compare" },
    { id: "import", key: "title.import" },
    { id: "settings", key: "settings.title" },
  ];

  // The OS window title carries the build name for the taskbar.
  $effect(() => {
    const name = app.info?.buildName;
    const title = name ? `${name}${app.info?.unsaved ? " •" : ""}` : "";
    void api.setTitle(title);
    // in the system browser the tab title is the page's own
    if (hostInfo.browser) document.title = title ? `${title} - PobTools` : "PobTools";
  });

  // In the system browser closing the tab does not end the program right away
  // (it waits to see whether the page comes back), so the page has its own End.
  function endProgram() {
    if (app.info?.unsaved && !confirm(t("title.endUnsaved"))) return;
    app.link = "closed";
    void bridge.call("host.close").catch(() => {});
  }
</script>

<header class="top">
  <span class="brand"><i class="mark"></i>PobTools</span>
  <nav>
    {#each tabs as tb}
      <button class="tab" class:on={app.view === tb.id} disabled={tb.id !== "builds" && tb.id !== "settings" && !app.loaded} onclick={() => (app.view = tb.id)}>
        {t(tb.key)}
      </button>
    {/each}
  </nav>
  <span class="grow"></span>
  {#if app.info}
    <span class="build">
      <span class="bname">{app.info.buildName}</span>
      <span class="bcls">{app.info.ascendClassNameZh ?? app.info.classNameZh ?? ""}</span>
      {#if app.info.unsaved}<span class="unsaved">{t("title.unsaved")}</span>{/if}
    </span>
  {/if}
  {#if hostInfo.browser}
    <button class="end" title={t("title.endHint")} onclick={endProgram}>{t("title.end")}</button>
  {/if}
</header>

<style>
  .top {
    height: var(--titlebar-h);
    display: flex;
    align-items: center;
    gap: 18px;
    padding: 0 16px;
    background: var(--surface-1);
    border-bottom: 1px solid var(--edge-0);
  }
  .brand {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-weight: 600;
    letter-spacing: 0.05em;
  }
  .mark {
    width: 10px;
    height: 10px;
    background: var(--gold);
    transform: rotate(45deg);
    border-radius: 2px;
  }
  nav {
    display: flex;
    gap: 4px;
  }
  .tab {
    appearance: none;
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    padding: 0 10px;
    color: var(--ink-2);
    font-size: var(--fs-sm);
    letter-spacing: 0.02em;
  }
  .tab:hover:not(:disabled) {
    color: var(--ink-0);
  }
  .tab.on {
    color: var(--ink-0);
    border-bottom-color: var(--gold);
  }
  .tab:disabled {
    opacity: var(--dim-opacity);
  }
  .grow {
    flex: 1;
  }
  .build {
    display: inline-flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
  }
  .bname {
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    max-width: 40vw;
  }
  .bcls {
    color: var(--gold);
    font-size: var(--fs-sm);
  }
  .end {
    appearance: none;
    border: 1px solid var(--edge-1);
    background: none;
    color: var(--ink-2);
    font-size: var(--fs-sm);
    padding: 2px 10px;
    border-radius: 4px;
  }
  .end:hover {
    color: var(--bad);
    border-color: var(--bad);
  }
  .unsaved {
    color: var(--warn);
    font-size: var(--fs-xs);
    padding: 1px 6px;
    border: 1px solid var(--warn);
    border-radius: 9px;
  }
</style>
