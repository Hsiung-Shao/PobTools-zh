// English release history, shown in the launcher's version-history tab to every
// locale that is not Chinese (see DrawChangelogBody in launcher_ui.cpp).
//
// Same formatting contract as changelog.h -- "v" + digit starts a release, a
// blank line separates releases, U+3000 + U+00B7 starts a bullet -- with one
// extra rule that matters only here:
//
//   WRITE ONE BULLET PER LINE, NEVER A CONTINUATION LINE.
//
// The renderer folds a line that starts with U+3000 and is not a bullet into the
// previous one, and it only inserts a space when the characters either side of
// the join are ASCII alphanumerics. Chinese wraps mid-word so that rule is
// invisible there; in English a continuation would silently produce
// "the launcher.Use the button".
//
// Coverage: only releases from v1.0.0 on are translated (user ruling,
// 2026-09-12). The older entries stay Chinese-only and the last line says so.
// UPDATE THIS TOGETHER WITH changelog.h ON EVERY RELEASE.
#pragma once

inline constexpr const char* kChangelogTextEn =
	u8"v1.7.0 (2026-09-18, early build)\n"
	u8"Added\n"
	u8"　·New interface (Beta): one more button in the launcher opens a build in a new window. The sidebar, passive tree, items, skills, config, calcs, notes, party, compare and import/export are all there, POB keeps updating itself as usual, and the classic window is always one click away.\n"
	u8"　·The new interface supports PoE2, and Chinese item text can be pasted into it directly.\n"
	u8"　·Where there is no WebView2 (CrossOver or Wine on macOS and Linux), the new interface opens in the system browser instead.\n"
	u8"　·Trade panel in the new interface: POB's own Query Options dialog is shown and editable, and hovering a search result shows the item and what equipping it would change.\n"
	u8"　·Note: this is an early build, published to everyone who ticked \"Join the beta\". The new interface is still being tested; please report what you run into.\n"
	u8"\n"
	u8"v1.6.0 (2026-09-18)\n"
	u8"Fixed\n"
	u8"　·Appearance settings needed POB to be restarted before they showed: they now apply to a running POB as soon as you change them.\n"
	u8"Added\n"
	u8"　·POB performance: the Settings page can cap how many frames POB draws per second, separately for focused and background windows (60 and 15 by default). Changes apply to a running POB right away and cut GPU and CPU load.\n"
	u8"　·Performance diagnostics log (off by default): turn it on when tracking down POB stutter or high GPU/CPU use. The log goes to the problem log folder and contains no build or account data.\n"
	u8"　·Join the beta (off by default): with this on, program updates come from builds that have not been made official yet, so you can try new features early.\n"
	u8"　·Note: early builds are for testing and may have problems that are not fixed yet. Turning it back off does not downgrade -- you keep what you have until the stable line catches up. The translation data is the same either way.\n"
	u8"\n"
	u8"v1.5.0 (2026-09-12)\n"
	u8"Fixed\n"
	u8"　·Coming back to the launcher after closing POB: it used to take several seconds to reappear (since v1.4.0); it now comes back immediately.\n"
	u8"　·The launcher used its share of CPU and GPU while idle: a minimised window kept redrawing at full speed. Idle and minimised now cost almost nothing, tool windows included.\n"
	u8"Added\n"
	u8"　·Korean interface: 「한국어」 in the language dropdown. Items, skill gems, passives, modifiers, uniques and monster names show the official Korean, and about 80% of POB's own interface is translated; the rest stays English.\n"
	u8"　·Note: the launcher's Korean text is a first draft and has not been checked by a native speaker. Corrections are welcome; see the translator guide.\n"
	u8"　·Noto Sans KR is now shipped: it fills in Korean glyphs the main font does not have, in the launcher and in the POB window.\n"
	u8"　·Translator guide: a new document covering how a language folder is laid out, the dictionary format, testing locally and submitting changes.\n"
	u8"　·Stash revenue tracker: a new tool that snapshots the stash tabs you pick and compares two snapshots to show what the stash gained or lost over that time. (Test feature; you paste your own POESESSID.)\n"
	u8"　·Note: a POESESSID is a login token for your account. It is not remembered by default and can be cleared with one click. GGG advises against giving it to third-party programs; the tool's Help tab explains what this one does with it.\n"
	u8"　·A Revenue tab in the atlas planner: the per-map cost of a strategy next to what the stash actually earned, giving profit per hour, and a stretch of that history can be bound to a build and travels with its share code.\n"
	u8"　·Links for PoE2 -- official trade, database, wiki and community -- plus the Chrome and Firefox store pages for our trade-site localisation.\n"
	u8"　·This version history is now available in English: it is shown whenever the interface language is not Chinese (covering v1.0.0 onwards).\n"
	u8"Changed\n"
	u8"　·When the font atlas does not fit the GPU's texture limit, Korean glyphs are kept first for players using the Korean interface.\n"
	u8"　·The link board is grouped into PoE1, PoE2 and Chinese localisation columns, with Discord and the sponsor link moved below it.\n"
	u8"\n"
	u8"v1.4.0 (2026-09-10)\n"
	u8"Added\n"
	u8"　·Appearance tab: a new launcher tab that collects the POB window's appearance settings, with a separate set for PoE1 and PoE2. (Changes apply immediately to that game's open POB windows; Windows only.)\n"
	u8"　·POB panel opacity: the sidebar, the top toolbar and the passive tree's bottom toolbar can fade out completely. (Only the text and controls are left, over a passive tree that now fills the window; the desktop never shows through.)\n"
	u8"　·Background image: drop an image into Backgrounds inside the PobTools folder to use it as the bottom layer of the POB window, with an adjustable brightness. (PNG, JPG, WebP)\n"
	u8"　·Frosted glass: the sidebar and toolbars can blur whatever is behind them before the panel's own tint is laid over it.\n"
	u8"　·Passive tree backdrop: the dark texture behind the nodes can be dimmed so the background image shows through on the tree page too. (Both the PoE1 and the PoE2 tree.)\n"
	u8"　·Hangs and crashes now leave a reason behind: if POB or the launcher stops responding for more than 20 seconds, or exits unexpectedly, what it was doing at the time is written down. (Open the problem log folder from the Settings page and attach the files to a report.)\n"
	u8"　·Note: dragging the window or opening a dialog does not count as a hang and writes no file; when POB stops responding the launcher only says so, it never closes it for you.\n"
	u8"　·Install new versions at startup: a new option under Program updates on the Settings page, so the button in the top right is no longer needed. (Off by default.)\n"
	u8"　·Note: it only acts right after startup, before POB or any tool has been opened, and never while a POB is running.\n"
	u8"\n"
	u8"v1.3.0 (2026-09-03)\n"
	u8"Fixed\n"
	u8"　·Strings from Poe Regex could match maps they were not meant to: the game's search box also matches modifier names, tiers, tags, reminder text and item names that are not printed on the modifier line, and the generated fragments now avoid those as well.\n"
	u8"Added\n"
	u8"　·Font size on the Settings page, under Interface: a 14 to 26 px slider that scales the whole launcher and every tool window together. POB itself is not affected.\n"
	u8"　·The launcher window can be resized by dragging its edges and remembers the size when you let go; width and height can also be typed in under Interface. The separate-window and tabbed modes each keep their own size.\n"
	u8"\n"
	u8"v1.2.0 (2026-09-02)\n"
	u8"Fixed\n"
	u8"　·Opening POB from the launcher failed on a Mac and stopped on a screen full of errors: a component POB needs for rendering was not shipped with it, and now it is. (Affects CrossOver 26 and later.)\n"
	u8"　·Note: on an Intel Mac the interface looks somewhat transparent once it opens. Nothing else is affected.\n"
	u8"\n"
	u8"v1.1.0 (2026-08-30)\n"
	u8"Fixed\n"
	u8"　·Text in the POB window looked blurry, as if the whole thing had been scaled up: text is now drawn at its real size and aligned to the pixel grid, so the strokes are sharp. (Just as sharp when the interface scale is not 100%.)\n"
	u8"　·Note: with Noto Sans the text sits slightly lower than before. That is the corrected alignment.\n"
	u8"Added\n"
	u8"　·HTTP proxy on the Settings page: update checks, translation data and icon downloads can go through a proxy; leave it empty to follow the system proxy. (For networks that cannot reach GitHub.)\n"
	u8"\n"
	u8"v1.0.0 (2026-08-28)\n"
	u8"Fixed\n"
	u8"　·Everything turned back into English after POB updated to a beta: the new POB code used newer runtime syntax and translation stopped entirely. (The runtime has been upgraded to match.)\n"
	u8"　·Translated text took the colour of whatever came before it: a colour change in the middle of a line was swallowed during translation. (Colours are back in the skill group list, the sidebar's bracketed notes and everywhere else.)\n"
	u8"　·A custom modifier box showed English until you clicked into it: each line is now translated on its own, matched modifiers show in Chinese and your own notes are left as they are.\n"
	u8"　·\"(Unused)\" in the item list was mistranslated: it now reads correctly. (Fixed for Simplified Chinese too.)\n"
	u8"　·\"(Guard)\" in the sidebar was mistranslated: it now reads correctly in both Chinese variants.\n"
	u8"　·Some text showed as question marks in the Simplified Chinese interface: the launcher and the tool windows now fill missing glyphs from the other shipped fonts.\n"
	u8"　·The launcher did not show POB's version number when a POB beta was in use.\n"
	u8"Added\n"
	u8"　·Whole English paragraphs, such as the usage notes on the skills page and the drag-and-drop help in the item list, are now translated.\n"
	u8"　·The \"while at maximum...\" boss modifier families in the configuration page's modifier browser are now translated.\n"
	u8"Changed\n"
	u8"　·Translations: 15 more modifier and interface strings in both Chinese variants.\n"
	u8"\n"
	u8"Earlier releases (v0.1.0 - v0.29.0) are documented in Chinese only.\n";
