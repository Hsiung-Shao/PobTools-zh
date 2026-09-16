<script lang="ts">
  // A POB-coloured string as spans. `muted` is the colour a run without a
  // code takes (POB's default is white; a sidebar label wants softer).
  import { pobRuns } from "$lib/pobtext";

  let { text, muted = null }: { text: string | null | undefined; muted?: string | null } = $props();
  const runs = $derived(pobRuns(text));
</script>

{#each runs as r}<span style:color={r.color ?? muted ?? undefined}>{r.text}</span>{/each}
