<script lang="ts">
  // 版面殼:頂列 / 側欄 + 主視圖 / 狀態列。Phase 1 的主視圖:建置清單與天賦樹。
  import { onMount } from "svelte";
  import { bridge } from "$lib/bridge";
  import { loadLocale, t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import TitleBar from "./components/TitleBar.svelte";
  import StatusBar from "./components/StatusBar.svelte";
  import Sidebar from "./components/Sidebar.svelte";
  import BuildList from "./components/BuildList.svelte";
  import TreeView from "./views/TreeView.svelte";

  let i18nReady = $state(false);
  onMount(() => {
    void loadLocale().then(() => (i18nReady = true));
  });
</script>

{#if i18nReady}
  <div class="app">
    <TitleBar />
    <div class="body">
      <Sidebar />
      <main class="view">
        {#if app.engine === "gone"}
          <div class="center">
            <p class="bad">{t("app.childExited", { code: app.childExit ?? -1 })}</p>
            <button class="btn" onclick={() => bridge.call("host.restart_engine")}>{t("app.restartEngine")}</button>
          </div>
        {:else if app.engine === "booting"}
          <div class="center"><p class="dim pulse">{t("app.booting")}</p></div>
        {:else if app.gate && !app.gate.ok}
          <div class="center"><p class="bad">{t("app.gateFailed", { failed: app.gate.failed.join("; ") })}</p></div>
        {:else if app.view === "tree" && app.loaded}
          <TreeView />
        {:else}
          <BuildList />
        {/if}
      </main>
    </div>
    <StatusBar />
  </div>
{/if}

<style>
  .app {
    height: 100%;
    display: flex;
    flex-direction: column;
  }
  .body {
    flex: 1;
    display: flex;
    min-height: 0;
  }
  .view {
    flex: 1;
    min-width: 0;
    min-height: 0;
    position: relative;
  }
  .center {
    height: 100%;
    display: grid;
    place-items: center;
    text-align: center;
  }
  .bad {
    color: var(--bad);
  }
  .pulse {
    animation: pulse 1.6s ease-in-out infinite;
  }
</style>
