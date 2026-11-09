# Reading a Path of Exile 1 build (PobTools MCP guide)

Mechanics principles only, not a league meta. Every number you report must come
from the tools (Path of Building's own calculation); use this page to interpret them.

Your own Path of Exile knowledge may come from an older league: skills, uniques and
mechanics change every patch. When it disagrees with what the tools return (a gem's
effect, a unique's mods, a node's stats), trust the tools and say that you did.

## 1. Find the build's intent before judging it
Read `build_summary.keystones` (with their stat lines) and `immuneTo` first.
Many "bad looking" numbers are the plan, not a flaw:
- **Chaos Inoculation (CI)**: maximum life is 1 and the build is immune to chaos
  damage. Judge survivability on energy shield, not life; chaos resistance does
  not matter. `immuneTo` contains "Chaos".
- **Low life** (life reserved to below 50%, often with Pain Attunement or
  "while on low life" mods): low unreserved life is intended; energy shield is
  the real pool.
- **Mind over Matter / Eldritch Battery / Ghost Reaver / Zealot's Oath / Vaal
  Pact**: they move damage, costs or recovery between life, mana and energy
  shield. Read the pool the build actually uses.
- **Minion builds**: player DPS can be near zero; the damage is on the minions
  (`build_summary.minionStats`). Survivability of the minions matters as well as
  the player's.
- **Totem, trap and mine builds**: the player does not hit directly, but totems,
  traps and mines still use the player's speed and accuracy modifiers; for traps
  and mines `Speed` is the throw / lay rate. Attack totems can miss.
- **Damage over time builds (ignite, poison, bleed, degen spells)**: the hit DPS
  is not the damage number, but hit damage still scales ailments, and crit still
  matters for applying them (and for ailment damage with Perfect Agony). Look at
  TotalDot / TotalDotDPS and IgniteDPS / BleedDPS / PoisonDPS.
- **Keystones from items** (e.g. a Cloak of Defiance's Mind over Matter) are in
  `keystones` with `fromItem: true`.

## 2. Defence: layers, not one number
- **Pool**: life, or energy shield, or both (see intent). Ward is a separate layer.
- **Resistances**: elemental resistances are expected at their cap (normally 75%)
  for endgame. Chaos resistance matters unless the build is chaos immune.
- **Mitigation layers**: armour / physical damage reduction, evasion, block and
  spell block, spell suppression, damage taken as another type, endurance charges,
  fortification, guard skills. Two or more solid layers is the usual target.
- **Maximum hit taken** per damage type is the clearest single view of one-shot
  risk; the lowest one is usually the weak spot. A type listed in `immuneTo`
  (and missing from the max-hit stats) cannot hurt the build under the current
  configuration (enemy damage type, temporary buffs such as Vaal Arctic Armour).
- **Recovery**: regeneration, leech, recoup, flasks. A large pool with no recovery
  dies to sustained damage.
- **Ailments and stun**: freeze, shock, stun and curses cause deaths that no
  pool number shows. Check the `...AvoidChance` stats (100 = immune).

## 3. Offence: the number depends on the configuration
- DPS in Path of Building assumes the configuration (enemy type and resistances,
  charges, flasks active, buffs, "is the enemy shocked/ignited", boss vs map
  mobs). Read `get_config` before trusting or comparing DPS, and state the
  assumptions you rely on.
- CombinedDPS is the main skill's hit DPS plus its damage over time and
  ailments; TotalDPS is hits only. FullDPS adds the other skill groups, but only
  those marked "include in full DPS" (otherwise it can be 0). A build may have
  several damage skills; check `list_skills` and which group is main.
- 100% crit or hit chance below ~90% are visible in the summary; accuracy only
  matters for attacks that can miss.
- Mana or life cost the build cannot pay makes the skill unusable regardless of DPS.

## 4. Changing a build
- Use `what_if` to measure a change before recommending it, and report the
  difference it returned.
- Verify every change: read back the thing you changed (`list_skills` shows
  whether a group is `active`, `set_config` returns `before`/`after`/`applied`,
  `tree_summary`, `list_items`), then compare the same stats before and after.
  An unchanged number does not prove the change failed, and a successful call
  does not prove it took effect: a gem in an inactive group, a condition that is
  off or a buff that is not enabled all leave the numbers as they were.
- `rank_supports` and `rank_anoints` use Path of Building's own sorting
  (estimated DPS per support gem, recalculated build per anoint); confirm the
  pick you recommend with `what_if`. Read their `basis`: a support's gain is for
  adding it as an extra gem (in a full group, test replacing each current
  support), and an anoint's power is against no anoint at all (the current
  anoint appears in the ranking too).
- `suggest_nodes` ranks by power per node; `pathDist` is how many points it
  costs to reach. Compare against remaining points in `build_summary.points`.
- Rare items are calculated exactly as written; do not invent mods that cannot
  roll on that base or item level, and say when an item is hypothetical.
- Hand the result to the user with `export_code`; nothing is saved to their files.

## 5. When a tool fails
- An error about your arguments (unknown gem, node, config key): fix the call.
- An error that says it was written to `PobTools\logs`: that is a PobTools bug or
  a broken install. Do not keep retrying; tell the user, and call
  `report_problem` to give them a report they can review and paste into a
  GitHub issue.

## 6. Language
Names come in English (`name`) and Traditional Chinese (`nameZh`). When answering
in Chinese, use the `nameZh` the tools returned instead of translating names yourself.
