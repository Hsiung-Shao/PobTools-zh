<script lang="ts">
  // 版面殼:頂列 / 側欄 + 主視圖 / 狀態列。Phase 1 的主視圖:建置清單與天賦樹。
  import { onMount } from "svelte";
  import { bridge } from "$lib/bridge";
  import { loadLocale, t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import { prefs } from "$lib/prefs.svelte";
  import TitleBar from "./components/TitleBar.svelte";
  import StatusBar from "./components/StatusBar.svelte";
  import Sidebar from "./components/Sidebar.svelte";
  import BuildList from "./components/BuildList.svelte";
  import BuildBar from "./components/BuildBar.svelte";
  import TreeView from "./views/TreeView.svelte";
  import ImportView from "./views/ImportView.svelte";
  import ItemsView from "./views/ItemsView.svelte";
  import SkillsView from "./views/SkillsView.svelte";
  import ConfigView from "./views/ConfigView.svelte";
  import CalcsView from "./views/CalcsView.svelte";
  import NotesView from "./views/NotesView.svelte";
  import PartyView from "./views/PartyView.svelte";
  import SettingsView from "./views/SettingsView.svelte";
  import type { ViewId } from "$lib/state.svelte";

  // Tabs by number, save by Ctrl+S: the shortcuts POB's own top bar has.
  const tabOrder: ViewId[] = ["builds", "tree", "items", "skills", "config", "calcs", "notes", "party", "import"];
  function onKey(e: KeyboardEvent) {
    if (!e.ctrlKey || e.altKey) return;
    const k = e.key.toLowerCase();
    if (k === "s" && !e.shiftKey && app.loaded) {
      e.preventDefault();
      void app.save();
      return;
    }
    // Build.lua: Ctrl+I opens the import tab, Ctrl+W leaves the build for the list
    if (k === "i" && !e.shiftKey && app.loaded) {
      e.preventDefault();
      app.view = "import";
      return;
    }
    if (k === "w" && !e.shiftKey && app.loaded) {
      e.preventDefault();
      app.view = "builds";
      return;
    }
    // window zoom, the browser's own shortcuts (WebView2's are switched off)
    if (k === "=" || k === "+") {
      e.preventDefault();
      void prefs.zoomBy(1);
      return;
    }
    if (k === "-") {
      e.preventDefault();
      void prefs.zoomBy(-1);
      return;
    }
    if (k === "0") {
      e.preventDefault();
      void prefs.set({ zoom: 100 });
      return;
    }
    const n = Number(e.key);
    if (n >= 1 && n <= tabOrder.length) {
      const id = tabOrder[n - 1];
      if (id === "builds" || app.loaded) {
        e.preventDefault();
        app.view = id;
      }
    }
  }

  function onWheel(e: WheelEvent) {
    if (!e.ctrlKey) return;
    e.preventDefault();
    void prefs.zoomBy(e.deltaY < 0 ? 1 : -1);
  }

  let i18nReady = $state(false);
  onMount(() => {
    prefs.init();
    void loadLocale().then(() => (i18nReady = true));
  });
</script>

<svelte:window onkeydown={onKey} onwheel={onWheel} />

{#if i18nReady}
  <div class="app">
    <TitleBar />
    {#if app.loaded && app.view !== "builds" && app.view !== "settings"}<BuildBar />{/if}
    <div class="body">
      <Sidebar />
      <main class="view">
        {#if app.engine === "gone"}
          <div class="center">
            <p class="bad">{t("app.childExited", { code: app.childExit ?? -1 })}</p>
            <button class="btn" onclick={() => bridge.call("host.restart_engine")}>{t("app.restartEngine")}</button>
          </div>
        {:else if app.view === "settings"}
          <SettingsView />
        {:else if app.engine === "booting"}
          <div class="center"><p class="dim pulse">{t("app.booting")}</p></div>
        {:else if app.gate && !app.gate.ok}
          <div class="center"><p class="bad">{t("app.gateFailed", { failed: app.gate.failed.join("; ") })}</p></div>
        {:else if app.view === "tree" && app.loaded}
          <TreeView />
        {:else if app.view === "import" && app.loaded}
          <ImportView />
        {:else if app.view === "items" && app.loaded}
          <ItemsView />
        {:else if app.view === "skills" && app.loaded}
          <SkillsView />
        {:else if app.view === "config" && app.loaded}
          <ConfigView />
        {:else if app.view === "calcs" && app.loaded}
          <CalcsView />
        {:else if app.view === "notes" && app.loaded}
          <NotesView />
        {:else if app.view === "party" && app.loaded}
          <PartyView />
        {:else if app.view !== "builds" && app.loaded}
          <div class="center"><p class="dim">{t("app.viewSoon")}</p></div>
        {:else}
          <BuildList />
        {/if}
      </main>
    </div>
    <StatusBar />
  </div>
  {#if app.link !== "ok"}
    <div class="linkveil" role="alertdialog" aria-live="assertive">
      <div class="linkbox">
        {#if app.link === "closed"}
          <p class="linkhead">{t("app.linkClosed")}</p>
          <p class="dim">{t("app.linkClosedHint")}</p>
        {:else}
          <p class="linkhead bad">{t("app.linkLost")}</p>
          <p class="dim pulse">{t("app.linkLostHint")}</p>
        {/if}
      </div>
    </div>
  {/if}
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
  .linkveil {
    position: fixed;
    inset: 0;
    z-index: 1000;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 78%, transparent);
  }
  .linkbox {
    max-width: 440px;
    padding: 24px 28px;
    text-align: center;
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-radius: 8px;
  }
  .linkhead {
    font-size: 1.1em;
    font-weight: 600;
    margin: 0 0 8px;
  }
  .pulse {
    animation: pulse 1.6s ease-in-out infinite;
  }
</style>
