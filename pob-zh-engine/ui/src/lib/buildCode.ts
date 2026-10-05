// What a piece of clipboard text is, for the import page's "bring the
// clipboard in" step: a POB share code (base64url of the deflated build XML),
// a link to one of the build sites POB downloads from, or neither. Only a
// guess to pre-fill a field -- decoding and downloading stay with POB.

/** Shortest text taken for a share code: even an empty build's code is longer. */
export const MIN_SHARE_CODE_LENGTH = 100;

/** POB share code shape: base64 / base64url characters only, no whitespace inside. */
export function looksLikeShareCode(text: string): boolean {
  const t = text.trim();
  return t.length >= MIN_SHARE_CODE_LENGTH && /^[A-Za-z0-9_\-+/=]+$/.test(t);
}

// The hosts POB's Modules/BuildSiteTools.lua websiteList imports from (PoE1
// and PoE2 lists together), each with the path its matchURL requires.
const SITE_PATTERNS: RegExp[] = [
  /^https:\/\/maxroll\.gg\/poe2?\/pob\/\S+$/i,
  /^https:\/\/pob\.codes\/b\/\S+$/i,
  /^https:\/\/pobb\.in\/\S+$/i,
  /^https:\/\/poe2?\.ninja\/(?:poe[12]\/)?pob\/\S+$/i,
  /^https:\/\/pastebinp?\.com\/\w+\/?$/i,
  /^https:\/\/rentry\.co\/\w+\/?$/i,
  /^https:\/\/poe2?db\.tw\/\S+$/i,
];

/** A link to a build on one of the sites POB imports from. */
export function looksLikeBuildUrl(text: string): boolean {
  const t = text.trim();
  if (!t || /\s/.test(t)) return false;
  return SITE_PATTERNS.some((re) => re.test(t));
}

export type ClipboardBuild = { kind: "code"; text: string } | { kind: "url"; text: string } | null;

/** Which import field the clipboard text belongs in (trimmed), or null. */
export function classifyClipboard(text: string | null | undefined): ClipboardBuild {
  if (!text) return null;
  const t = text.trim();
  if (looksLikeShareCode(t)) return { kind: "code", text: t };
  if (looksLikeBuildUrl(t)) return { kind: "url", text: t };
  return null;
}

/**
 * Whether the page should fill a field from the clipboard on its own: only into
 * an empty field, and only text it has not brought in before (so clearing the
 * field, or importing, does not bring the same thing straight back).
 */
export function shouldAutoFill(found: ClipboardBuild, fieldValue: string, lastAuto: string): boolean {
  return !!found && fieldValue.trim() === "" && found.text !== lastAuto;
}
