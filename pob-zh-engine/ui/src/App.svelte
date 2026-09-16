<script lang="ts">
  // Phase 1a shell: proves the round trip page -> host -> headless POB ->
  // page. Shows what the engine announced (hello / gate_result) and the
  // answer to `version`. The real layout replaces this in 1b.
  import { onMount } from "svelte";
  import { api, bridge, hostInfo, isHosted, type GateResult, type VersionInfo } from "$lib/bridge";

  let hello = $state<Record<string, unknown> | null>(null);
  let gate = $state<GateResult | null>(null);
  let version = $state<VersionInfo | null>(null);
  let error = $state<string | null>(null);
  let childExit = $state<number | null>(null);
  let restarted = $state(0);

  async function refresh() {
    error = null;
    try {
      version = await api.version();
      void api.setTitle(`${version.pobVersion} (${version.pobBranch})`);
    } catch (e: any) {
      error = `${e.code ?? "error"}: ${e.message ?? e}`;
    }
  }

  onMount(() => {
    const offs = [
      bridge.on("hello", (d) => {
        hello = d as Record<string, unknown>;
        void refresh();
      }),
      bridge.on("gate_result", (d) => (gate = d as GateResult)),
      bridge.on("restarted", () => (restarted += 1)),
      bridge.on("host.child_exited", (d: any) => (childExit = d?.exitCode ?? -1)),
      bridge.on("error", (d: any) => (error = String(d?.message ?? d))),
    ];
    return () => offs.forEach((f) => f());
  });
</script>

<main>
  <h1>PobTools 新介面 <span class="dim">Phase 1a</span></h1>
  <p class="muted">
    {isHosted ? "由 pob-zh.exe --modern-ui 承載" : "瀏覽器開發模式(mock transport)"} ·
    {hostInfo.game} / {hostInfo.locale} · PobTools {hostInfo.version}
  </p>

  <section class="panel">
    <div class="panel-head"><span class="label">引擎</span></div>
    <div class="body">
      {#if childExit !== null}
        <p class="bad">POB 引擎子程序已結束(exit {childExit})。</p>
        <button class="btn" onclick={() => bridge.call("host.restart_engine").then(() => (childExit = null))}>重新啟動引擎</button>
      {:else if !hello}
        <p class="dim pulse">等待引擎啟動(載入 POB 與翻譯字典)…</p>
      {:else}
        <p>hello:<code class="mono">{JSON.stringify(hello)}</code></p>
        {#if gate}
          <p>
            相容閘門:
            {#if gate.ok}<span class="ok">通過</span>{:else}<span class="bad">失敗</span>{/if}
            <span class="dim">({gate.checked} 項)</span>
            {#if gate.failed.length}<span class="bad"> — {gate.failed.join("; ")}</span>{/if}
          </p>
        {/if}
        {#if version}
          <p>
            POB <b>{version.pobVersion}</b> ({version.pobBranch}, {version.pobPlatform}) · bridge {version.bridge}
          </p>
          <p class="dim mono">buildPath: {version.buildPath}</p>
        {/if}
        {#if restarted}<p class="warn">引擎已重啟 {restarted} 次(POB 自我更新後)。</p>{/if}
      {/if}
      {#if error}<p class="bad">{error}</p>{/if}
    </div>
  </section>

  <section class="panel">
    <div class="panel-head"><span class="label">最近訊息</span></div>
    <pre class="log selectable">{bridge.log.slice(-12).join("\n")}</pre>
  </section>
</main>

<style>
  main {
    height: 100%;
    padding: 24px;
    overflow: auto;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  h1 {
    margin: 0;
    font-size: var(--fs-xl);
    font-weight: 500;
  }
  .body {
    padding: 10px;
  }
  .body p {
    margin: 4px 0;
  }
  .ok {
    color: var(--ok);
  }
  .bad {
    color: var(--bad);
  }
  .warn {
    color: var(--warn);
  }
  .pulse {
    animation: pulse 1.6s ease-in-out infinite;
  }
  .log {
    margin: 0;
    padding: 10px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    color: var(--fg-2);
    white-space: pre-wrap;
    word-break: break-all;
    max-height: 40vh;
    overflow: auto;
  }
</style>
