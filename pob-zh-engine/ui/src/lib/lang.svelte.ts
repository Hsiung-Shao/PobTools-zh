/**
 * The display language as something Svelte can track. t() reads `lang.v`, so
 * every template that calls t() re-renders when F2 flips the language --
 * without re-mounting the page (which threw away everything typed but not yet
 * applied). Bumped on every setEnglish call, not only on a real change.
 */
export const lang = $state({ v: 0 });

/**
 * The key everything fetched from POB is cached and re-fetched on: POB's
 * outputRevision plus the language counter. POB's revision does not move when
 * only the display language changed, but its strings did.
 */
export function dataKey(rev: number, langRev: number): string {
  return `${rev}:${langRev}`;
}
