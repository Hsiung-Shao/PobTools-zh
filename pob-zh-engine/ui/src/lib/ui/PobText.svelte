<script lang="ts">
  // A POB-coloured string as spans. `muted` is the colour a run without a
  // code takes (POB's default is white; a sidebar label wants softer).
  import { pobRuns } from "$lib/pobtext";

  let { text, muted = null }: { text: string | null | undefined; muted?: string | null } = $props();
  const runs = $derived(pobRuns(text));
</script>

<!-- A literal ^xRRGGBB (not one of pobtext's tokens) was picked for a dark
     background: .pob-lit lets the light theme darken it (app.css). -->
{#each runs as r}{#if r.color?.startsWith("#")}<span class="pob-lit" style:--pc={r.color}>{r.text}</span>{:else}<span style:color={r.color ?? muted ?? undefined}>{r.text}</span>{/if}{/each}
