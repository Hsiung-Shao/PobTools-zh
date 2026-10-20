#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Offline harness for poecharm_inject.lua (CJK search & input).

Why this exists
---------------
Every bug this file guards against had the same shape: the patch reported
success and did nothing.

  * The gem box widened its input filter through `class._constructor`, which
    only the PoE2 fork has -- PoE1 community stores the constructor as a method
    named after the class. The branch was nil, was skipped in silence, and the
    trace still printed "patched GemSelectControl" while Chinese could not be
    typed into the box at all.
  * The item database matched a translated NAME but never a translated MODIFIER
    line, so "some items cannot be found" with no pattern a user could see.

So the rules here are:

  1. Run the REAL `Modules/Common.lua` from each installed fork. The two forks'
     `newClass`/`new` differ in exactly the way that caused the bug; a
     hand-written stub would have agreed with whichever one it was modelled on.
  2. Every behavioural test also runs with the inject OFF and requires the bug
     to reproduce. A test that passes both ways proves nothing.
  3. Separately assert the SHAPE of the installed POB sources (method names,
     the gem box's ASCII filter, the C++ source-patch anchors). That is what
     turns a future upstream rename into a failing test instead of a feature
     silently reverting to English.

Run:  python pob-zh-engine/tests/inject_cjk_test.py
Needs: pip install lupa      (Lua 5.1, no PoB or engine build required)
"""

import os
import re
import sys

try:
    import lupa.lua51 as lua51
except ImportError:  # pragma: no cover
    sys.exit("需要 lupa (Lua 5.1):  python -m pip install lupa")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # pob-zh-engine/
# POB_INJECT_TEST_DIST: a worktree has no deployed POB, so point at one that does
# (e.g. D:\codeproject\PobTest). Default stays this checkout's dist/.
DIST = os.environ.get("POB_INJECT_TEST_DIST") or os.path.join(ROOT, "dist")
INJECT = os.path.join(ROOT, "poecharm_inject.lua")
UI_API = os.path.join(ROOT, "ui_api.cpp")

FORKS = {
    "PoE1": os.path.join(DIST, "PathOfBuildingCommunity"),
    "PoE2": os.path.join(DIST, "PathOfBuildingCommunity-PoE2-Portable"),
}

# Fixture dictionary. Values are the real ones the shipped dictionaries return
# (verified with `pob-zh.exe --tr`), because a made-up value would still pass a
# test that only checks "something came back".
#
# "Volatility" is the whole point of the source dimension: the merged map
# answers with the PASSIVE (易爆) while the gem dropdown shows what gems.json
# says (易變輔助). A gem search that matches the unmarked lookup finds nothing
# when the user types what is on screen.
DICT = {
    None: {
        "Volatility": "易爆",
        "Fireball": "火球",
        "Kaom's Heart": "岡姆的壯志",
        "+500 to maximum Mana": "+500 最大魔力",
        "(30-40)% increased Lightning Damage": "增加 (30-40)%閃電傷害",
        "Summoned Skeleton": "召喚骷髏",
        "Spectral Throw": "幻影投擲",
        "Effective Hit Pool": "有效生命池",
        "Enemy Chilled": "敵人冰緩",
        # real shape of a coloured dictionary value -- the search has to see the
        # rendered text, not the "^xRRGGBB" runs that paint it
        "Is the enemy Shocked?": "^xFFFFFF敵人是否被^xADAA47【感電】^7?",
        "10% increased Attack Speed": "增加 10% 攻擊速度",
        "+30 to Dexterity": "+30 敏捷",
    },
    "gems": {
        "Volatility": "易變輔助",
        "Fireball": "火球",
    },
    # Source-only entries for the tooltip tests: none of these exist under the
    # unmarked source, so a lookup under the wrong source comes back untranslated.
    "passives": {"Elemental Overload": "元素超載",
                 # ST-P0:軍團替換後的新名稱,只在 passives 來源
                 "Lord of the Dead": "亡者之主"},
    "stats": {"+500 to maximum Mana": "+500 最大魔力",
              # L2a:未標記與 stats 都有、譯文刻意不同,才分得出走了哪一邊
              "+30 to Dexterity": "+30 點敏捷(stats)",
              # L2b:只有 stats 有,未標記查不到
              "Adds 1 to 2 Fire Damage to Attacks": "攻擊附加 1 - 2 火焰傷害"},
    "uniques": {"Kaom's Heart": "岡姆的壯志"},
    "items": {"Glorious Plate": "榮耀鎧甲"},
    "affixes": {"Heavy": "厚重的", "of the Whale": "鯨之"},
}

# Id-keyed dictionaries (passives_by_id.json / affixes_by_id.json) behind
# PobToolsTranslateId. Same English, different id, different wording -- so a
# lookup that ignores the id cannot produce both.
ID_DICT = {
    "passives": {"90001": "戰神", "90002": "勇士"},
    "affixes": {"TestModHeavyA": "厚重的甲", "TestModHeavyB": "厚重的乙",
                "TestModWhaleA": "鯨之甲"},
}

# Stubs every harness gets. Only what the loaded POB code touches at load time
# or inside the code paths under test -- deliberately not a POB emulator.
PRELUDE = rb"""
bit = setmetatable({}, {__index = function() return function() return 0 end end})
require = function() return setmetatable({}, {__index = function() return function() end end}) end
launch = { devMode = false }
main = { unicode = true, screenW = 1920, screenH = 1080, popups = {} }
colorCodes = setmetatable({}, {__index = function() return "^7" end})
function ConPrintf() end
function GetTime() return 0 end
function DrawString() end
function DrawStringWidth() return 10 end
function DrawStringCursorIndex() return 1 end
function SetDrawColor() end
function SetViewport() end
function DrawImage() end
function IsKeyDown() return false end
function Paste() return nil end
function Copy() end
function StripEscapes(s) return (s:gsub("%^%d", ""):gsub("%^x%x%x%x%x%x%x", "")) end
function PCall(f, ...)
	local r = { pcall(f, ...) }
	if r[1] then return nil, r[2] end
	return r[2]
end
function wipeTable(t) for k in pairs(t) do t[k] = nil end end
function copyTable(t) local n = {} for k, v in pairs(t) do n[k] = v end return n end
function isValueInTable(t, v) for _, x in pairs(t) do if x == v then return true end end end
function NewImageHandle()
	local h = {}
	function h:Load() end
	function h:Unload() end
	function h:IsValid() return false end
	function h:IsLoading() return false end
	function h:SetLoadingPriority() end
	function h:ImageSize() return 0, 0 end
	return h
end
function DrawImageQuad() end
function SetDrawLayer() end
function GetScreenSize() return 1920, 1080 end
function GetCursorPos() return 0, 0 end
function GetAsyncCount() return 0 end
function RenderInit() end
function ConExecute() end
function SetWindowTitle() end
function LoadModule_stub() end
"""


class Fail(Exception):
    pass


class Harness(object):
    """One Lua 5.1 state running a real fork's Common.lua."""

    def __init__(self, fork, with_inject=True, lua_mod=None):
        self.fork = fork
        self.dir = FORKS[fork]
        self.rt = (lua_mod or lua51).LuaRuntime(unpack_returned_tuples=True)
        self.source = None          # current PobToolsSetSource() name
        self.source_seen = set()    # every source active during a lookup
        self.lookups = []           # (text, source) of every lookup, in order
        self.id_lookups = []        # (source, id, en) of every PobToolsTranslateId
        self.title_lookups = []     # (text, source) of every PobToolsItemTitle
        self.enabled = True         # F2: off = every engine lookup answers nil
        # Tooltip lines arrive as "^xRRGGBBText"; the engine answers those by
        # colour segment. Off by default so the older tests keep exact-key lookup.
        self.loose_keys = False
        self._load(PRELUDE)
        self._install_engine_globals()
        g = self.rt.globals()
        g["LoadModule"] = self._load_module
        g["PLoadModule"] = self._load_module
        self._load(self._read(os.path.join(self.dir, "Modules", "Common.lua")))
        self.injected = False
        if with_inject:
            self.apply_inject()

    # ---- plumbing ----
    @staticmethod
    def _read(path):
        with open(path, "rb") as f:
            return f.read()

    def _chunk(self, src, name):
        return self.rt.eval("function(s, n) return assert(loadstring(s, n)) end")(src, name)

    def _load(self, src, name="=harness"):
        return self._chunk(src, name)()

    def _load_module(self, name, *args):
        name = name if name.endswith(".lua") else name + ".lua"
        path = os.path.join(self.dir, name.replace("/", os.sep))
        return self._chunk(self._read(path), "@" + path)(*args)

    def _install_engine_globals(self):
        g = self.rt.globals()

        def translate(text):
            if text is None or not self.enabled:
                return None
            table = DICT.get(self.source) or {}
            self.source_seen.add(self.source)
            self.lookups.append((text, self.source))
            out = table.get(text) or DICT[None].get(text)
            if out is None and self.loose_keys:
                m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(\s*)(.*?)(\s*)$",
                             text, re.S)
                core = m.group(3)
                if core and core != text:
                    hit = table.get(core) or DICT[None].get(core)
                    if hit:
                        out = m.group(1) + m.group(2) + hit + m.group(4)
            return out

        def translate_display(text):
            out = translate(text)
            return None if (out is None or out == text) else out

        def set_source(name=None):
            prev = self.source
            self.source = name
            return prev

        def translate_id(source, id_, en=None):
            # Lua integers may arrive as float; keys are id strings.
            if isinstance(id_, float) and id_.is_integer():
                id_ = int(id_)
            key = None if id_ is None else str(id_)
            self.id_lookups.append((source, key, en))
            if not self.enabled:
                return None         # engine: by-id and fallback both obey F2
            hit = (ID_DICT.get(source) or {}).get(key)
            if hit is not None:
                return hit
            # miss: ordinary lookup of `en` under that source
            prev = self.source
            self.source = source
            try:
                return translate(en)
            finally:
                self.source = prev

        g["__py_translate"] = translate
        g["__py_translate_id"] = translate_id
        g["__py_translate_display"] = translate_display
        g["__py_set_source"] = set_source
        def item_title(text):
            self.title_lookups.append((text, self.source))
            return translate(text)

        g["__py_item_title"] = item_title
        g["__py_enabled"] = lambda: self.enabled
        # Every failure the inject reports, in order. This is the engine-side
        # failure log seen from Lua; the test below is the only thing that proves
        # a broken patch actually reaches it.
        self.logged_errors = []
        g["__py_log_error"] = lambda feature, msg: self.logged_errors.append(
            (feature, msg))
        # Wrapped in real Lua functions on purpose: a Python callable handed
        # straight to Lua is userdata, and the inject's first line is a
        # type(PobToolsTranslate) ~= "function" bail-out. A harness that trips
        # that guard silently tests nothing at all.
        self._load(rb"""
function PobToolsTranslate(s) return __py_translate(s) end
function PobToolsTranslateDisplay(s) return __py_translate_display(s) end
function PobToolsSetSource(n) return __py_set_source(n) end
function PobToolsTranslateId(s, id, en) return __py_translate_id(s, id, en) end
function PobToolsGetTranslate() return __py_enabled() end
function PobToolsItemTitle(s) return __py_item_title(s) end
function PobToolsLogError(f, m) return __py_log_error(f, m) end
""")

    def apply_inject(self):
        self._load(self._read(INJECT), "@" + INJECT)
        self.injected = True

    # ---- helpers ----
    def run(self, src):
        return self._load(src if isinstance(src, bytes) else src.encode("utf-8"))

    def eval(self, expr):
        return self.rt.eval(expr)

    def klass(self, name):
        return self.rt.eval('common.classes["%s"]' % name)

    def define(self, name, poe1_src, poe2_src):
        """Register a synthetic class the way this fork's real files do."""
        self.run(poe1_src if self.fork == "PoE1" else poe2_src)


# ---------------------------------------------------------------------------
# Synthetic classes.
#
# Bodies are the upstream logic trimmed to what the patch interacts with, but
# they are REGISTERED through the fork's own newClass idiom -- that registration
# is the part the constructor patch depends on and the part that differs.
# ---------------------------------------------------------------------------

GEMSELECT_P1 = """
local C = newClass("GemSelectControl", "EditControl")
function C:GemSelectControl(anchor, rect)
	self:EditControl(anchor, rect, nil, nil, "^ %a':-")
	return self
end
function C:Draw() end
"""

GEMSELECT_P2 = """
local C = newClass("GemSelectControl", "EditControl", function(self, anchor, rect)
	self.EditControl(anchor, rect, nil, nil, "^ %a':-")
end)
function C:Draw() end
"""

# BuildList + the bits it leans on. English matching only, like upstream.
GEMLIST_BODY = """
local C = common.classes["GemSelectControl"]
C.gems = nil
function C:PopulateGemList()
	self.gems = {
		VolatilitySupport = { name = "Volatility" },
		Fireball = { name = "Fireball" },
	}
end
function C:FilterSupport() return true end
function C:SortGemList(list) table.sort(list) end
function C:BuildList(buf)
	self.list = {}
	self.searchStr = buf
	if #buf > 0 then
		for id, gem in pairs(self.gems) do
			if gem.name:lower():find(buf:lower(), 1, true) then table.insert(self.list, id) end
		end
		self:SortGemList(self.list)
	else
		for id in pairs(self.gems) do table.insert(self.list, id) end
	end
	if not self.list[1] then
		self.list[1] = ""
		self.noMatches = true
	else
		self.noMatches = false
	end
end
"""

ITEMDB_P1 = """
local C = newClass("ItemDBControl")
function C:ItemDBControl()
	self.controls = { search = { buf = "" }, searchMode = { selIndex = 1 } }
	return self
end
""" + """
function C:DoesItemMatchFilters(item)
	if not item.eligible then return false end
	local searchStr = self.controls.search.buf:lower()
	if searchStr:match("%S") then
		local found = false
		local mode = self.controls.searchMode.selIndex
		if mode == 1 or mode == 2 then
			if item.name:lower():find(searchStr, 1, true) then found = true end
		end
		if mode == 1 or mode == 3 then
			for _, line in ipairs(item.explicitModLines) do
				if line.line:lower():find(searchStr, 1, true) then found = true break end
			end
		end
		if not found then return false end
	end
	return true
end
"""

ITEMDB_P2 = ITEMDB_P1.replace(
    'local C = newClass("ItemDBControl")\nfunction C:ItemDBControl()\n'
    '\tself.controls = { search = { buf = "" }, searchMode = { selIndex = 1 } }\n'
    '\treturn self\nend\n',
    'local C = newClass("ItemDBControl", function(self)\n'
    '\tself.controls = { search = { buf = "" }, searchMode = { selIndex = 1 } }\n'
    'end)\n')

MINION_P1 = """
local C = newClass("MinionSearchListControl")
function C:MinionSearchListControl()
	self.controls = { searchText = { buf = "" } }
	self.data = {
		minions = { SkelWarrior = { name = "Summoned Skeleton", skillList = { "SpectralThrow" } } },
		skills = { SpectralThrow = { name = "Spectral Throw" } },
	}
	return self
end
function C:DoesEntryMatchFilters(searchStr, minionId, filterMode)
	local minion = self.data.minions[minionId]
	if (filterMode == 1 or filterMode == 3) and minion.name:lower():find(searchStr, 1, true) then
		return true
	end
	if filterMode == 2 or filterMode == 3 then
		for _, id in ipairs(minion.skillList) do
			if self.data.skills[id].name:lower():find(searchStr, 1, true) then return true end
		end
	end
	return false
end
"""

MINION_P2 = MINION_P1.replace(
    'local C = newClass("MinionSearchListControl")\nfunction C:MinionSearchListControl()\n',
    'local C = newClass("MinionSearchListControl", function(self)\n').replace(
    '\treturn self\nend\nfunction C:DoesEntryMatchFilters',
    'end)\nfunction C:DoesEntryMatchFilters')

CALCS_P1 = """
local C = newClass("CalcsTab")
function C:CalcsTab()
	self.controls = { search = { buf = "" } }
	return self
end
function C:SearchMatch(txt)
	local searchStr = self.controls.search.buf:lower()
	return string.len(searchStr) > 0 and txt:lower():find(searchStr, 1, true)
end
"""

CALCS_P2 = CALCS_P1.replace(
    'local C = newClass("CalcsTab")\nfunction C:CalcsTab()\n',
    'local C = newClass("CalcsTab", function(self)\n').replace(
    '\treturn self\nend\nfunction C:SearchMatch',
    'end)\nfunction C:SearchMatch')


def make(h, class_name):
    """new(class):Ctor() on PoE1, new(class) on PoE2 -- the forks differ here."""
    if h.fork == "PoE1":
        return h.eval('new("%s"):%s()' % (class_name, class_name))
    return h.eval('new("%s")' % class_name)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond), detail))
    if not cond:
        print("  FAIL  %s %s" % (name, detail))
    return bool(cond)


def gem_box(fork, with_inject):
    """Real EditControl chain + the gem box's one distinguishing ctor line."""
    h = Harness(fork, with_inject=False)
    h.run('LoadModule("Classes/EditControl")')            # loads the real file + parents
    h.define("GemSelectControl", GEMSELECT_P1, GEMSELECT_P2)
    if with_inject:
        h.apply_inject()
    obj = make(h, "GemSelectControl")
    h.eval('function(o) o:Insert("火球") end')(obj)
    return h, obj


def test_gem_input(fork):
    # inject OFF: the bug must reproduce, otherwise the ON case proves nothing.
    _, off = gem_box(fork, with_inject=False)
    check("%s gem box: bug reproduces without inject" % fork,
          off.buf == "", "buf=%r" % off.buf)

    h, on = gem_box(fork, with_inject=True)
    check("%s gem box: CJK survives typing" % fork,
          on.buf == "火球", "buf=%r filter=%r" % (on.buf, on.filter))
    check("%s gem box: filter widened" % fork,
          on.filter == "%c", "filter=%r" % on.filter)
    # the ASCII path upstream cares about must still work
    h.eval('function(o) o.buf = "" o.caret = 1 o:Insert("Fireball") end')(on)
    check("%s gem box: ASCII still accepted" % fork, on.buf == "Fireball",
          "buf=%r" % on.buf)


def test_gem_search(fork):
    for with_inject in (False, True):
        h = Harness(fork, with_inject=False)
        h.run('LoadModule("Classes/EditControl")')
        h.define("GemSelectControl", GEMSELECT_P1, GEMSELECT_P2)
        h.run(GEMLIST_BODY)
        if with_inject:
            h.apply_inject()
        obj = make(h, "GemSelectControl")
        h.eval('function(o) o:PopulateGemList() o:BuildList("易變") end')(obj)
        found = list(h.eval('function(o) return o.list end')(obj).values())
        if with_inject:
            check("%s gem search: 易變 finds the support gem" % fork,
                  "VolatilitySupport" in found, "list=%r" % found)
            # 易變輔助 only exists under source "gems"; matching it proves the
            # lookup ran source-marked and not against the passive 易爆.
            check("%s gem search: ran under source 'gems'" % fork,
                  "gems" in h.source_seen, "sources=%r" % (h.source_seen,))
        else:
            check("%s gem search: bug reproduces without inject" % fork,
                  "VolatilitySupport" not in found, "list=%r" % found)


ITEM = """
{
	eligible = true,
	name = "Kaom's Heart",
	explicitModLines = {
		{ line = "(30-40)% increased Lightning Damage" },
		{ line = "+500 to maximum Mana" },
	},
	implicitModLines = {},
	enchantModLines = {},
}
"""

INELIGIBLE = ITEM.replace("eligible = true", "eligible = false")


def test_itemdb(fork):
    def probe(with_inject, query, mode, item_src=ITEM):
        h = Harness(fork, with_inject=False)
        h.define("ItemDBControl", ITEMDB_P1, ITEMDB_P2)
        if with_inject:
            h.apply_inject()
        obj = make(h, "ItemDBControl")
        fn = h.eval('function(o, q, mode, item) o.controls.search.buf = q '
                    'o.controls.searchMode.selIndex = mode '
                    'return o:DoesItemMatchFilters(item) end')
        return fn(obj, query, mode, h.eval("function() return %s end" % item_src)())

    check("%s item DB: CJK name found (mode Anywhere)" % fork, probe(True, "岡姆", 1))
    check("%s item DB: CJK name found (mode Names)" % fork, probe(True, "岡姆", 2))
    check("%s item DB: CJK mod found (mode Anywhere)" % fork, probe(True, "閃電傷害", 1))
    check("%s item DB: CJK mod found (mode Modifiers)" % fork, probe(True, "閃電傷害", 3))
    check("%s item DB: CJK mod NOT found in Names mode" % fork,
          not probe(True, "閃電傷害", 2))
    check("%s item DB: ineligible item still filtered out" % fork,
          not probe(True, "岡姆", 1, INELIGIBLE))
    check("%s item DB: English search unchanged" % fork, probe(True, "kaom", 1))
    check("%s item DB: non-matching CJK rejected" % fork, not probe(True, "火球", 1))
    # without the inject the mod half is exactly what was missing
    check("%s item DB: CJK mod bug reproduces without inject" % fork,
          not probe(False, "閃電傷害", 1))


def test_minion(fork):
    def probe(with_inject, query, mode):
        h = Harness(fork, with_inject=False)
        h.define("MinionSearchListControl", MINION_P1, MINION_P2)
        if with_inject:
            h.apply_inject()
        obj = make(h, "MinionSearchListControl")
        fn = h.eval('function(o, q, mode) o.controls.searchText.buf = q '
                    'return o:DoesEntryMatchFilters(q:lower(), "SkelWarrior", mode) end')
        return fn(obj, query, mode)

    check("%s minion: CJK name found" % fork, probe(True, "骷髏", 1))
    check("%s minion: CJK skill found" % fork, probe(True, "幻影", 2))
    check("%s minion: CJK skill not matched in Names mode" % fork,
          not probe(True, "幻影", 1))
    check("%s minion: English search unchanged" % fork, probe(True, "skeleton", 1))
    check("%s minion: bug reproduces without inject" % fork, not probe(False, "骷髏", 1))


def test_calcs(fork):
    def probe(with_inject, query, label):
        h = Harness(fork, with_inject=False)
        h.define("CalcsTab", CALCS_P1, CALCS_P2)
        if with_inject:
            h.apply_inject()
        obj = make(h, "CalcsTab")
        fn = h.eval('function(o, q, label) o.controls.search.buf = q '
                    'return o:SearchMatch(label) and true or false end')
        return fn(obj, query, label)

    check("%s calcs: CJK label found" % fork, probe(True, "生命池", "Effective Hit Pool"))
    check("%s calcs: English still found" % fork, probe(True, "hit", "Effective Hit Pool"))
    check("%s calcs: unrelated CJK rejected" % fork,
          not probe(True, "火球", "Effective Hit Pool"))
    # spans two coloured runs in the dictionary value; only works if the
    # translation is stripped of its escapes before matching
    check("%s calcs: query spanning a colour escape" % fork,
          probe(True, "敵人是否被【感電】", "Is the enemy Shocked?"))
    check("%s calcs: bug reproduces without inject" % fork,
          not probe(False, "生命池", "Effective Hit Pool"))


def test_patch_failure_is_logged(fork):
    """A patch that cannot apply has to end up in the failure log.

    This is the whole reason the log exists. When POB changes upstream, the
    symptom a user sees is "Chinese input stopped working" -- there is no crash,
    no message, and the feature simply goes quiet. Before this, the only record
    was a trace file that exists solely when POB_ZH_INJECT_TRACE is set, i.e.
    never on a user's machine.

    The failure is manufactured the way upstream would cause it: the class is
    present but the method the patch wraps is gone.
    """
    h = Harness(fork, with_inject=False)
    # CalcsTab without SearchMatch -- exactly what an upstream rename looks like
    # from in here.
    # A bare table, not newClass(): the two forks disagree about that signature,
    # and what matters here is only that the class EXISTS while the method the
    # patch wraps does not -- which is exactly what an upstream rename leaves
    # behind.
    h.run(b'common.classes.CalcsTab = {}')
    h.apply_inject()

    hits = [(f, m) for (f, m) in h.logged_errors if f == "inject"]
    check("%s inject: a failed patch is written to the failure log" % fork,
          len(hits) >= 1, repr(h.logged_errors))
    if hits:
        msg = hits[0][1]
        # The name alone ("patch FAILED CalcsTab") is not actionable by whoever
        # reads the report, so the symptom has to travel with it.
        check("%s inject: the log line names the symptom, not just the class" % fork,
              "計算頁" in msg, msg)
        check("%s inject: the log line names the class too" % fork,
              "CalcsTab" in msg, msg)


def test_no_patch_failed(fork):
    """Every PATCHES entry that had a class present must have applied cleanly."""
    h = Harness(fork, with_inject=False)
    logged = []
    h.rt.globals()["ConPrintf"] = lambda fmt, msg=None: logged.append(msg)
    for name in ("EditControl", "Tooltip", "SearchHost"):
        h.run('LoadModule("Classes/%s")' % name)
    h.define("GemSelectControl", GEMSELECT_P1, GEMSELECT_P2)
    h.define("ItemDBControl", ITEMDB_P1, ITEMDB_P2)
    h.define("MinionSearchListControl", MINION_P1, MINION_P2)
    h.define("CalcsTab", CALCS_P1, CALCS_P2)
    h.apply_inject()
    failed = [m for m in logged if m and "patch FAILED" in m]
    check("%s inject: no patch reported FAILED" % fork, not failed, repr(failed))
    patched = sorted(m.split(" ", 1)[1] for m in logged if m and m.startswith("patched "))
    for want in ("GemSelectControl", "ItemDBControl", "MinionSearchListControl",
                 "CalcsTab", "EditControl", "Tooltip", "SearchHost"):
        check("%s inject: patched %s" % (fork, want), want in patched, repr(patched))


def test_paste_regression(fork):
    """The v0.24.0 paste fix must keep working -- it shares EditControl."""
    for with_inject in (False, True):
        h = Harness(fork, with_inject=False)
        h.run('LoadModule("Classes/EditControl")')
        h.rt.globals()["Paste"] = lambda: "我愛POE"
        h.rt.globals()["IsKeyDown"] = lambda key: key == "CTRL"
        if with_inject:
            h.apply_inject()
        if h.fork == "PoE1":
            obj = h.eval('new("EditControl")')
            h.eval('function(o) o:EditControl(nil, {0,0,100,20}) end')(obj)
        else:
            obj = h.eval('new("EditControl", nil, {0, 0, 100, 20})')
        h.eval('function(o) o:OnKeyDown("v") end')(obj)
        if with_inject:
            check("%s paste: CJK survives Ctrl+V" % fork, obj.buf == "我愛POE",
                  "buf=%r" % obj.buf)
        else:
            check("%s paste: bug reproduces without inject" % fork,
                  obj.buf != "我愛POE", "buf=%r" % obj.buf)


# ---- source dimension for tooltips (L1-L6) ---------------------------------
#
# The engine now answers per source (passives / affixes / stats / uniques /
# items). A tooltip line looked up under the wrong source still "translates" --
# just to the wrong dictionary's answer -- so every check below asserts the
# source that was ACTIVE at lookup time, recorded by the harness, not merely
# that some Chinese came back.

COLOUR_SETUP = b"""
colorCodes.UNIQUE = "^xAF6025"
colorCodes.RARE = "^xFFFF77"
colorCodes.MAGIC = "^x8888FF"
colorCodes.NORMAL = "^xC8C8C8"
"""

# Upstream AddNodeName/AddNodeTooltip trimmed to the lines that reach the
# tooltip. `node.explode` manufactures the failure L6 needs.
PASSIVE_METHODS = """
function C:AddNodeName(tooltip, node, build)
	local nodeName = node.dn
	tooltip.center = true
	tooltip:AddLine(24, nodeName..(launch.devModeAlt and " ["..node.id.."]" or ""), "FONTIN")
	tooltip.center = false
	if node.explode then error("synthetic AddNodeName failure") end
end
function C:AddNodeTooltip(tooltip, node, build)
	self:AddNodeName(tooltip, node, build)
	for _, line in ipairs(node.sd or {}) do
		if node.unsupported then
			-- 上游 addModInfoToTooltip:不支援的詞綴在 AddLine 時才接上註記
			local l = colorCodes.UNSUPPORTED..line
			l = main.notSupportedModTooltips and (l .. main.notSupportedTooltipText) or l
			tooltip:AddLine(16, l, "FONTIN")
		else
			tooltip:AddLine(16, "^7"..line)
		end
	end
	-- ST-L4:珠寶插槽,上游在 AddNodeTooltip 內呼叫 itemsTab:AddItemTooltip
	if node.jewel and build and build.itemsTab then
		build.itemsTab:AddItemTooltip(tooltip, node.jewel, { nodeId = node.id })
	end
end
"""

PASSIVETREE_P1 = """
local C = newClass("PassiveTreeView")
function C:PassiveTreeView() return self end
""" + PASSIVE_METHODS

PASSIVETREE_P2 = """
local C = newClass("PassiveTreeView", function(self) end)
""" + PASSIVE_METHODS

# Upstream AddItemTooltip's name block verbatim, then `item.otherLines` (L3c:
# requirement / quality style lines, neither name nor base), plus the PoE1 mod-line call
# (4th arg = modLine table). PoE2's 4th AddLine arg is a background, so the
# mod line goes in with nil there.
ITEMTIP_METHODS = """
function C:AddItemTooltip(tooltip, item, slot, dbMode, maxWidth)
	local rarityCode = colorCodes[item.rarity]
	tooltip.center = true
	tooltip.color = rarityCode
	if item.title then
		tooltip:AddLine(20, rarityCode..item.title, "FONTIN SC")
		tooltip:AddLine(20, rarityCode..item.baseName:gsub(" %(.+%)",""), "FONTIN SC")
	else
		tooltip:AddLine(20, rarityCode..item.namePrefix..item.baseName:gsub(" %(.+%)","")..item.nameSuffix, "FONTIN SC")
	end
	tooltip.center = false
	for _, l in ipairs(item.otherLines or {}) do
		tooltip:AddLine(16, l)
	end
	for _, modLine in ipairs(item.explicitModLines or {}) do
		tooltip:AddLine(16, colorCodes.MAGIC..modLine.line, "FONTIN SC", MODLINE_ARG)
	end
	if item.explode then error("synthetic AddItemTooltip failure") end
end
"""

ITEMSTAB_P1 = """
local C = newClass("ItemsTab")
function C:ItemsTab() return self end
""" + ITEMTIP_METHODS.replace("MODLINE_ARG", "modLine")

ITEMSTAB_P2 = """
local C = newClass("ItemsTab", function(self) end)
""" + ITEMTIP_METHODS.replace("MODLINE_ARG", "nil")

# ItemDBControl:GetRowValue verbatim: item.name is "Title, Base" for uniques.
ROWVALUE = """
function C:GetRowValue(column, index, item)
	if item and column == 1 then
		return colorCodes[item.rarity] .. item.name
	end
end
"""

UNIQUE_ITEM = """
{
	rarity = "UNIQUE",
	title = "Kaom's Heart",
	baseName = "Glorious Plate",
	base = {},
	name = "Kaom's Heart, Glorious Plate",
	explicitModLines = { { line = "+500 to maximum Mana" } },
}
"""

MAGIC_ITEM = """
{
	rarity = "MAGIC",
	namePrefix = "Heavy ",
	baseName = "Glorious Plate",
	nameSuffix = " of the Whale",
	base = {},
	name = "Heavy Glorious Plate of the Whale",
	explicitModLines = {},
}
"""

_ESC = re.compile(r"\^x[0-9A-Fa-f]{6}|\^\d")
NOT_LOOKED_UP = "<未查詢>"


def _norm(s):
    return _ESC.sub("", s or "").strip()


def src_of(h, text):
    """Source active at the FIRST lookup of `text` (colour/whitespace ignored)."""
    for t, s in h.lookups:
        if _norm(t) == text:
            return s
    return NOT_LOOKED_UP


def tooltip_harness(fork, with_inject, itemstab=None):
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.run(COLOUR_SETUP)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("PassiveTreeView", PASSIVETREE_P1, PASSIVETREE_P2)
    if itemstab is None:
        h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    else:
        h.run(itemstab)
    h.define("ItemDBControl", ITEMDB_P1 + ROWVALUE, ITEMDB_P2 + ROWVALUE)
    if with_inject:
        h.apply_inject()
    h.lookups = []     # only what the call under test looks up
    return h


def tooltip_lines(h, tip):
    got = h.eval('function(t) local o = {} for i, l in ipairs(t.lines) do '
                 'o[i] = l.text end return o end')(tip)
    return [got[i] for i in range(1, len(got) + 1)]


def run_node_name(fork, with_inject, explode=False):
    h = tooltip_harness(fork, with_inject)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    fn = h.eval('function(v, t, boom) local ok, e = pcall(function() '
                'v:AddNodeName(t, { dn = "Elemental Overload", id = 1, explode = boom }, {}) '
                'end) return ok, e end')
    ok, err = fn(view, tip, explode)
    return h, ok, err


def run_item_tooltip(fork, with_inject, item_src, explode=False, itemstab=None):
    h = tooltip_harness(fork, with_inject, itemstab)
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    item = h.eval("function() return %s end" % item_src)()
    item.explode = explode
    ok, err = h.eval('function(tab, t, item) local ok, e = pcall(function() '
                     'tab:AddItemTooltip(t, item) end) return ok, e end')(tab, tip, item)
    return h, tip, ok, err


def name_by_passives(h, dn, node_id):
    """名稱行在 passives 來源下查,或以 PobToolsTranslateId("passives", id, dn) 查。"""
    return (src_of(h, dn) == "passives"
            or ("passives", str(node_id), dn) in h.id_lookups)


def test_src_passive_name(fork):
    """L1 -- only the NAME line is passives (source or by id)."""
    h, ok, err = run_node_name(fork, with_inject=False)
    check("%s L1 未注入時重現 bug:天賦名稱行不在 passives 來源也沒依 id 查" % fork,
          not name_by_passives(h, "Elemental Overload", 1),
          "lookups=%r id_lookups=%r" % (h.lookups, h.id_lookups))
    h, ok, err = run_node_name(fork, with_inject=True)
    check("%s L1 AddNodeName 本身不出錯" % fork, ok, repr(err))
    check("%s L1 天賦名稱行在 passives 來源或依 id 查詢" % fork,
          name_by_passives(h, "Elemental Overload", 1),
          "lookups=%r id_lookups=%r" % (h.lookups, h.id_lookups))
    check("%s L1 AddNodeName 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)


def test_src_passive_stat_line(fork):
    """L1b -- AddNodeTooltip's stat lines: the shipping unmarked lookup."""
    h = tooltip_harness(fork, with_inject=True)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    h.eval('function(v, t) v:AddNodeTooltip(t, { dn = "Elemental Overload", id = 1, '
           'sd = { "10% increased Attack Speed" } }, {}) end')(view, tip)
    check("%s L1b 天賦屬性行走未標記查找(來源 nil)" % fork,
          src_of(h, "10% increased Attack Speed") is None, repr(h.lookups))


def mod_line_harness(fork, text):
    """PoE1 AddLine with a modLine 4th arg, outside any item tooltip."""
    h = tooltip_harness(fork, True)
    tip = make(h, "Tooltip")
    h.eval('function(t, s) t:AddLine(16, "^x8888FF" .. s, "FONTIN SC", { line = s }) end')(
        tip, text)
    lines = tooltip_lines(h, tip)
    return h, (_norm(lines[0]) if lines else "")


def srcs_of(h, text):
    """Every source active at a lookup of `text`, in order."""
    return [s for t, s in h.lookups if _norm(t) == text]


def test_src_mod_line(fork):
    """L2a/L2b -- only PoE1's AddLine has a modLine 4th parameter."""
    if fork != "PoE1":
        print("  N/A   %s L2:此分支 Tooltip:AddLine 第 4 參數是 background,沒有 modLine" % fork)
        return
    # L2a: unmarked has it -> unmarked answer, never stats
    h, out = mod_line_harness(fork, "+30 to Dexterity")
    check("%s L2a 詞綴行先走未標記查找:來源 nil、結果是未標記的值" % fork,
          src_of(h, "+30 to Dexterity") is None and out == "+30 敏捷",
          "out=%r lookups=%r" % (out, h.lookups))
    check("%s L2a AddLine 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # L2b: unmarked misses -> retried under stats
    h, out = mod_line_harness(fork, "Adds 1 to 2 Fire Damage to Attacks")
    srcs = srcs_of(h, "Adds 1 to 2 Fire Damage to Attacks")
    check("%s L2b 詞綴行查不到才用 stats:先試過未標記查找" % fork,
          None in srcs and srcs.index(None) < len(srcs) - 1, repr(h.lookups))
    check("%s L2b 詞綴行查不到才用 stats:最後一次在 stats 下、結果是 stats 的值" % fork,
          srcs and srcs[-1] == "stats" and out == "攻擊附加 1 - 2 火焰傷害",
          "out=%r lookups=%r" % (out, h.lookups))
    check("%s L2b AddLine 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # inside AddItemTooltip the mod line is unmarked too (no outer items scope)
    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM)
    check("%s L2a AddItemTooltip 內的詞綴行也先走未標記查找" % fork,
          src_of(h, "+500 to maximum Mana") is None, repr(h.lookups))


def test_src_unique_title(fork):
    """L3"""
    h, tip, ok, err = run_item_tooltip(fork, False, UNIQUE_ITEM)
    check("%s L3 未注入時重現 bug:傳奇名不在 uniques 來源" % fork,
          src_of(h, "Kaom's Heart") != "uniques", repr(h.lookups))
    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM)
    check("%s L3 AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    check("%s L3 傳奇物品標題行在 uniques 來源下查詢" % fork,
          src_of(h, "Kaom's Heart") == "uniques", repr(h.lookups))
    check("%s L3 傳奇物品基底行在 items 來源下查詢" % fork,
          src_of(h, "Glorious Plate") == "items", repr(h.lookups))
    lines = [_norm(t) for t in tooltip_lines(h, tip)]
    check("%s L3 tooltip 前兩行分別是傳奇名與基底的譯文" % fork,
          lines[:2] == ["岡姆的壯志", "榮耀鎧甲"], repr(lines))
    check("%s L3 AddItemTooltip 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)


def patched_itemstab(fork):
    """The synthetic ItemsTab after the engine's real source patches for
    itemstab.lua (ui_api.cpp) -- the title line then reads
    item.title:gsub("^.+$", PobToolsItemTitle or "%0")."""
    src = ITEMSTAB_P1 if fork == "PoE1" else ITEMSTAB_P2
    out, applied = apply_source_patches(
        src, "itemstab.lua", parse_source_patches(read_text(UI_API)))
    return out, applied


def test_src_unique_title_patched(fork):
    """L3b -- the shipping path: the title is translated by PobToolsItemTitle
    inside the source-patched expression, before AddLine sees it."""
    src, applied = patched_itemstab(fork)
    check("%s L3b 引擎 source patch 改寫了合成 ItemsTab 的標題式" % fork,
          applied >= 1 and 'gsub("^.+$", PobToolsItemTitle' in src, "applied=%d" % applied)
    h, tip, ok, err = run_item_tooltip(fork, False, UNIQUE_ITEM, itemstab=src)
    srcs = [s for t, s in h.title_lookups if t == "Kaom's Heart"]
    check("%s L3b 未注入時重現 bug:PobToolsItemTitle 不在 uniques 來源" % fork,
          srcs and srcs[0] != "uniques", repr(h.title_lookups))
    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM, itemstab=src)
    check("%s L3b AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    srcs = [s for t, s in h.title_lookups if t == "Kaom's Heart"]
    check("%s L3b 傳奇標題經 PobToolsItemTitle 在 uniques 來源下查詢" % fork,
          srcs == ["uniques"], repr(h.title_lookups))
    lines = [_norm(t) for t in tooltip_lines(h, tip)]
    check("%s L3b tooltip 第一行是傳奇名譯文" % fork,
          lines[:1] == ["岡姆的壯志"], repr(lines))
    check("%s L3b 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


OTHER_LINES_ITEM = UNIQUE_ITEM.replace(
    "\tbase = {},",
    '\tbase = {},\n\totherLines = { "^x7F7F7FRequires Level 68", "^x7F7F7FQuality: +20%" },')


def test_src_item_other_lines(fork):
    """L3c -- AddItemTooltip lines other than name / base: unmarked lookup."""
    h, tip, ok, err = run_item_tooltip(fork, True, OTHER_LINES_ITEM)
    check("%s L3c AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    srcs = [src_of(h, t) for t in ("Requires Level 68", "Quality: +20%")]
    check("%s L3c 物品需求行、品質行走未標記查找(來源 nil)" % fork,
          srcs == [None, None], "srcs=%r lookups=%r" % (srcs, h.lookups))


def test_src_affix_title(fork):
    """L4 -- no item.title: namePrefix .. baseName .. nameSuffix in one line."""
    h, tip, ok, err = run_item_tooltip(fork, False, MAGIC_ITEM)
    check("%s L4 未注入時重現 bug:前綴不在 affixes 來源" % fork,
          src_of(h, "Heavy") != "affixes", repr(h.lookups))
    h, tip, ok, err = run_item_tooltip(fork, True, MAGIC_ITEM)
    check("%s L4 AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    srcs = [src_of(h, t) for t in ("Heavy", "Glorious Plate", "of the Whale")]
    check("%s L4 無 title 的物品名拆三段,來源依序為 affixes/items/affixes" % fork,
          srcs == ["affixes", "items", "affixes"],
          "srcs=%r lookups=%r" % (srcs, h.lookups))
    lines = tooltip_lines(h, tip)
    first = _norm(lines[0]) if lines else ""
    pos = [first.find(w) for w in ("厚重的", "榮耀鎧甲", "鯨之")]
    check("%s L4 組合後標題依序為前綴、基底、後綴的譯文" % fork,
          -1 not in pos and pos == sorted(pos), repr(first))
    check("%s L4 組合後標題不殘留英文" % fork,
          first and not re.search(r"[A-Za-z]", first), repr(first))


def test_src_list_row(fork):
    """L5 -- list rows read "Title, Base" from item.name."""
    for with_inject in (False, True):
        h = tooltip_harness(fork, with_inject)
        db = make(h, "ItemDBControl")
        item = h.eval("function() return %s end" % UNIQUE_ITEM)()
        h.lookups = []
        val = h.eval('function(o, item) return o:GetRowValue(1, 1, item) end')(db, item)
        srcs = [src_of(h, "Kaom's Heart"), src_of(h, "Glorious Plate")]
        if not with_inject:
            check("%s L5 未注入時重現 bug:清單列沒拆段查詢" % fork,
                  srcs != ["uniques", "items"], repr(h.lookups))
            continue
        check("%s L5 清單列 title, base 拆兩段,來源依序為 uniques/items" % fork,
              srcs == ["uniques", "items"], "srcs=%r lookups=%r" % (srcs, h.lookups))
        out = _norm(val)
        pos = [out.find(w) for w in ("岡姆的壯志", "榮耀鎧甲")]
        check("%s L5 清單列輸出依序是傳奇名與基底的譯文" % fork,
              -1 not in pos and pos == sorted(pos), repr(val))
        check("%s L5 清單列查詢後來源還原為 nil" % fork,
              h.source is None, "source=%r" % h.source)


def test_src_restored_on_error(fork):
    """L6 -- a throwing wrapped function must not leave a source behind."""
    h, ok, err = run_node_name(fork, with_inject=True, explode=True)
    check("%s L6 AddNodeName 拋錯時錯誤照樣往外拋" % fork,
          not ok and "synthetic AddNodeName failure" in str(err), repr((ok, err)))
    # without this the nil check below passes even when nothing was wrapped
    check("%s L6 AddNodeName 拋錯前名稱行確實在 passives 來源或依 id 查" % fork,
          name_by_passives(h, "Elemental Overload", 1),
          "lookups=%r id_lookups=%r" % (h.lookups, h.id_lookups))
    check("%s L6 AddNodeName 拋錯後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)

    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM, explode=True)
    check("%s L6 AddItemTooltip 拋錯時錯誤照樣往外拋" % fork,
          not ok and "synthetic AddItemTooltip failure" in str(err), repr((ok, err)))
    check("%s L6 AddItemTooltip 拋錯前確實在 items 來源下" % fork,
          src_of(h, "Glorious Plate") == "items", repr(h.lookups))
    check("%s L6 AddItemTooltip 拋錯後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # The base line is items because of itemCtx (the wrap point), not an outer
    # scope: after the throw a bare base-name line must not still read as one.
    h.lookups = []
    tip2 = make(h, "Tooltip")
    h.eval('function(t) t:AddLine(20, "Glorious Plate") end')(tip2)
    check("%s L6 AddItemTooltip 拋錯後物品 context 不殘留(基底名不再走 items)" % fork,
          src_of(h, "Glorious Plate") is None, repr(h.lookups))


# ---- id-keyed lookups (L7-L8) -----------------------------------------------
#
# Same English, different id (GraphId / mod Id), different wording: the
# translation is decided by the id. A miss falls back to the English lookup.

def node_name_line(h, node_id, dn):
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    h.eval('function(v, t, id, dn) v:AddNodeName(t, { dn = dn, id = id }, {}) end')(
        view, tip, node_id, dn)
    lines = tooltip_lines(h, tip)
    return _norm(lines[0]) if lines else ""


def test_id_passive_name(fork):
    """L7"""
    h = tooltip_harness(fork, with_inject=True)
    got = [node_name_line(h, i, "Elemental Overload") for i in (90001, 90002, 99999)]
    check("%s L7 天賦名稱行依節點 id 取譯名:同名不同 id 各取己譯,id 未命中退回英文查找" % fork,
          got == ["戰神", "勇士", "元素超載"], "got=%r id_lookups=%r" % (got, h.id_lookups))
    called = [(s, i, en) for s, i, en in h.id_lookups if s == "passives"]
    check("%s L7 名稱行以 PobToolsTranslateId(\"passives\", node.id, node.dn) 查詢" % fork,
          ("passives", "90001", "Elemental Overload") in called, repr(h.id_lookups))
    check("%s L7 依 id 查完後來源還原為 nil" % fork,
          h.id_lookups and h.source is None, "source=%r" % h.source)


def test_id_obeys_f2(fork):
    """L9 -- F2 off: the by-id lookup answers nil like every other lookup."""
    h = tooltip_harness(fork, with_inject=True)
    h.enabled = False
    got = node_name_line(h, 90001, "Elemental Overload")
    check("%s L9 翻譯關閉時天賦名稱行維持英文(依 id 查詢也不翻)" % fork,
          got == "Elemental Overload", "got=%r id_lookups=%r" % (got, h.id_lookups))


# ST-P0:新引擎契約 -- 依 id 查時 (id, en) 都相符才回譯文,英文不符回 nil。
STRICT_ID_DICT = {("passives", "90001"): ("Elemental Overload", "戰神")}


def strict_id_harness(fork):
    """天賦提示 harness,PobToolsTranslateId 換成 (id, en) 皆符才命中的假引擎。"""
    h = tooltip_harness(fork, with_inject=True)

    def translate_id(source, id_, en=None):
        if isinstance(id_, float) and id_.is_integer():
            id_ = int(id_)
        key = None if id_ is None else str(id_)
        h.id_lookups.append((source, key, en))
        hit = STRICT_ID_DICT.get((source, key))
        return hit[1] if hit and hit[0] == en else None

    h.rt.globals()["__py_translate_id"] = translate_id
    return h


def test_id_replaced_node(fork):
    """ST-P0 -- 軍團/刺青替換節點保留原 id、只換 dn:名稱行不得沿用原節點譯名。"""
    h = tooltip_harness(fork, with_inject=True)   # 舊引擎:依 id 命中不比英文
    got = node_name_line(h, 90001, "Lord of the Dead")
    check("%s ST-P0 舊引擎(不比英文)重現 bug:替換節點顯示原節點譯名" % fork,
          got == "戰神", "got=%r id_lookups=%r" % (got, h.id_lookups))
    h = strict_id_harness(fork)
    got = node_name_line(h, 90001, "Lord of the Dead")
    check("%s ST-P0 替換節點(原 id、新 dn)名稱行不顯示原節點譯名" % fork,
          got != "戰神", "got=%r id_lookups=%r" % (got, h.id_lookups))
    check("%s ST-P0 替換節點名稱行改由 passives 來源查到新名稱譯文" % fork,
          got == "亡者之主" and src_of(h, "Lord of the Dead") == "passives",
          "got=%r lookups=%r" % (got, h.lookups))
    check("%s ST-P0 呼叫 PobToolsTranslateId 第 3 參數是替換後的 dn" % fork,
          ("passives", "90001", "Lord of the Dead") in h.id_lookups, repr(h.id_lookups))
    h = strict_id_harness(fork)
    got = node_name_line(h, 90001, "Elemental Overload")
    check("%s ST-P0 一般節點(id 與 dn 相符)仍用 id 譯文" % fork,
          got == "戰神", "got=%r id_lookups=%r" % (got, h.id_lookups))
    check("%s ST-P0 一般節點呼叫 PobToolsTranslateId 第 3 參數是 dn" % fork,
          ("passives", "90001", "Elemental Overload") in h.id_lookups, repr(h.id_lookups))
    check("%s ST-P0 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


def magic_item(prefix_mod, suffix_mod):
    def mods(m):
        return "{ { modId = %r } }" % m if m else "nil"
    return """
{
	rarity = "MAGIC",
	namePrefix = "Heavy ",
	baseName = "Glorious Plate",
	nameSuffix = " of the Whale",
	base = {},
	name = "Heavy Glorious Plate of the Whale",
	prefixes = %s,
	suffixes = %s,
	explicitModLines = {},
}
""" % (mods(prefix_mod), mods(suffix_mod))


def magic_title(fork, prefix_mod, suffix_mod):
    h, tip, ok, err = run_item_tooltip(fork, True, magic_item(prefix_mod, suffix_mod))
    lines = tooltip_lines(h, tip)
    return h, ok, err, (_norm(lines[0]) if lines else "")


def test_id_affix_title(fork):
    """L8"""
    ha, oka, erra, a = magic_title(fork, "TestModHeavyA", None)
    hb, okb, errb, b = magic_title(fork, "TestModHeavyB", None)
    hc, okc, errc, c = magic_title(fork, None, None)
    check("%s L8 前綴英文相同、modId 不同的兩件物品各取自己 id 的譯名" % fork,
          a.startswith("厚重的甲") and b.startswith("厚重的乙"),
          "a=%r b=%r id_lookups=%r" % (a, b, ha.id_lookups))
    check("%s L8 前綴以 PobToolsTranslateId(\"affixes\", modId, ...) 查詢" % fork,
          any(s == "affixes" and i == "TestModHeavyA" for s, i, _ in ha.id_lookups),
          repr(ha.id_lookups))
    check("%s L8 沒有 modId 時前綴退回英文查找" % fork,
          okc and c.startswith("厚重的") and not c.startswith("厚重的甲")
          and not c.startswith("厚重的乙") and "榮耀鎧甲" in c
          and not any(s == "affixes" and i == "TestModHeavyA" for s, i, _ in hc.id_lookups)
          and (a.startswith("厚重的甲")),
          "c=%r a=%r" % (c, a))
    hs, oks, errs, sfx = magic_title(fork, None, "TestModWhaleA")
    check("%s L8 後綴帶 modId 時同樣依 id 取譯名" % fork,
          sfx.endswith("鯨之甲") and sfx.startswith("厚重的"),
          "title=%r id_lookups=%r" % (sfx, hs.id_lookups))


# ---- stat 查詢(ST-L1 ~ ST-L3b) ---------------------------------------------
#
# 引擎提供 PobToolsTranslateStat(line, ctx),ctx = "item" / "passive",命中回中文、
# 否則 nil。詞綴行與天賦說明行先問它,未命中才走現行路徑;名稱行不問。
# 假函式以去色碼後的文字為鍵;收到帶色碼的行時連同色碼回傳(模擬引擎保留色碼),
# 所以注入層「去色碼再補回」或「整行交給引擎」兩種做法都會通過 ST-L3b。

STAT_DICT = {
    "+30 to Dexterity": "STAT中文",
    "10% increased Attack Speed": "STAT天賦",
}


def stat_harness(fork, stat_dict, define=True):
    """tooltip_harness 加上假 PobToolsTranslateStat(注入前定義);h.stat_calls 記 (line, ctx)。"""
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.stat_calls = []

    def translate_stat(line, ctx=None):
        h.stat_calls.append((line, ctx))
        if line is None:
            return None
        m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(.*)$", line, re.S)
        hit = (stat_dict or {}).get(m.group(2).strip())
        if hit is None:
            return None
        return m.group(1) + hit

    if define:
        h.rt.globals()["__py_translate_stat"] = translate_stat
        h.run(b"function PobToolsTranslateStat(l, c) return __py_translate_stat(l, c) end")
    h.run(COLOUR_SETUP)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("PassiveTreeView", PASSIVETREE_P1, PASSIVETREE_P2)
    h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    h.define("ItemDBControl", ITEMDB_P1 + ROWVALUE, ITEMDB_P2 + ROWVALUE)
    h.apply_inject()
    h.lookups = []
    return h


def stat_mod_line(fork, text, stat_dict, define=True):
    """PoE1 AddLine(16, "^x8888FF"..text, font, { line = text });回 (h, ok, err, 原始行)。"""
    h = stat_harness(fork, stat_dict, define)
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(t, s) local ok, e = pcall(function() '
                     't:AddLine(16, "^x8888FF" .. s, "FONTIN SC", { line = s }) end) '
                     'return ok, e end')(
        tip, text)
    lines = tooltip_lines(h, tip)
    return h, ok, err, (lines[0] if lines else "")


def stat_node_tooltip(fork, stat_dict, define=True):
    """AddNodeTooltip:名稱行 Elemental Overload + 說明行 10% increased Attack Speed。"""
    h = stat_harness(fork, stat_dict, define)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(v, t) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Elemental Overload", id = 1, sd = { "10% increased Attack Speed" } }, '
                     '{}) end) return ok, e end')(view, tip)
    return h, ok, err, [_norm(l) for l in tooltip_lines(h, tip)]


def test_stat_mod_line(fork):
    """ST-L1 / ST-L2 / ST-L2c / ST-L3b -- 只有 PoE1 的 AddLine 有 modLine 第 4 參數。"""
    if fork != "PoE1":
        print("  N/A   %s ST-L1/L2/L2c/L3b:此分支 AddLine 第 4 參數不是 modLine" % fork)
        return
    # ST-L1:命中就直接用,ctx 為 item
    h, ok, err, raw = stat_mod_line(fork, "+30 to Dexterity", STAT_DICT)
    check("%s ST-L1 詞綴行先走 stat 查詢:AddLine 寫入 stat 的譯文" % fork,
          ok and _norm(raw) == "STAT中文",
          "raw=%r calls=%r err=%r" % (raw, h.stat_calls, err))
    check("%s ST-L1 詞綴行呼叫 PobToolsTranslateStat 時 ctx 為 item" % fork,
          any(_norm(l) == "+30 to Dexterity" and c == "item" for l, c in h.stat_calls),
          repr(h.stat_calls))
    check("%s ST-L1 stat 命中後不再走字典查找" % fork,
          not srcs_of(h, "+30 to Dexterity"), "lookups=%r" % h.lookups)
    # ST-L3b:色碼前綴保留,且不重複
    check("%s ST-L3b 帶色碼的詞綴行命中 stat 後保留色碼前綴" % fork,
          raw == "^x8888FFSTAT中文", "raw=%r calls=%r" % (raw, h.stat_calls))

    # ST-L2:假函式回 nil -> 與 L2a/L2b 期望值相同,且與未定義時一致
    for text, want in (("+30 to Dexterity", "+30 敏捷"),
                       ("Adds 1 to 2 Fire Damage to Attacks", "攻擊附加 1 - 2 火焰傷害")):
        h, ok, err, raw = stat_mod_line(fork, text, {})
        h0, ok0, err0, raw0 = stat_mod_line(fork, text, None, define=False)
        check("%s ST-L2 stat 未命中退回現行順序:%s -> %s" % (fork, text, want),
              ok and _norm(raw) == want and raw == raw0,
              "raw=%r 未定義時=%r err=%r" % (raw, raw0, err))
        check("%s ST-L2 stat 未命中時仍有以 item 呼叫過 PobToolsTranslateStat:%s" % (fork, text),
              any(_norm(l) == text and c == "item" for l, c in h.stat_calls),
              repr(h.stat_calls))
        check("%s ST-L2c 舊引擎無 PobToolsTranslateStat 不報錯:%s" % (fork, text),
              ok0 and _norm(raw0) == want, "raw=%r err=%r" % (raw0, err0))


def test_stat_passive(fork):
    """ST-L3 / ST-L2c(天賦)-- 說明行帶 passive 情境,名稱行不呼叫。"""
    h, ok, err, lines = stat_node_tooltip(fork, STAT_DICT)
    check("%s ST-L3 AddNodeTooltip 本身不出錯" % fork, ok, repr(err))
    check("%s ST-L3 天賦說明行呼叫 PobToolsTranslateStat 時 ctx 為 passive" % fork,
          any(_norm(l) == "10% increased Attack Speed" and c == "passive"
              for l, c in h.stat_calls), repr(h.stat_calls))
    check("%s ST-L3 天賦名稱行不呼叫 PobToolsTranslateStat" % fork,
          not any(_norm(l) == "Elemental Overload" for l, c in h.stat_calls),
          repr(h.stat_calls))
    check("%s ST-L3 天賦說明行命中 stat 後顯示 stat 的譯文" % fork,
          lines[1:2] == ["STAT天賦"], "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s ST-L3 天賦提示中不以 item 情境呼叫" % fork,
          not any(c == "item" for l, c in h.stat_calls), repr(h.stat_calls))
    # 未命中 / 未定義:名稱行與說明行照現行路徑
    want = ["元素超載", "增加 10% 攻擊速度"]
    h, ok, err, lines = stat_node_tooltip(fork, {})
    check("%s ST-L2 天賦說明行 stat 未命中時照現行路徑" % fork,
          ok and lines == want, "lines=%r err=%r" % (lines, err))
    h, ok, err, lines = stat_node_tooltip(fork, None, define=False)
    check("%s ST-L2c 舊引擎無 PobToolsTranslateStat 時天賦提示不報錯且照現行路徑" % fork,
          ok and lines == want, "lines=%r err=%r" % (lines, err))


JEWEL_ITEM = """
{
	rarity = "UNIQUE",
	title = "Kaom's Heart",
	baseName = "Glorious Plate",
	base = {},
	name = "Kaom's Heart, Glorious Plate",
	otherLines = { "^x7F7F7FLimited to: 1", "^x7F7F7FRadius: Large", "^x7F7F7F+12 Life (+1.2%)" },
	explicitModLines = { { line = "+30 to Dexterity" } },
}
"""


def test_stat_jewel_in_node(fork):
    """ST-L4 -- 天賦提示內嵌珠寶物品提示:非詞綴行不以 passive 查;PoE1 詞綴行仍以 item 查。"""
    h = stat_harness(fork, STAT_DICT)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    tab = make(h, "ItemsTab")
    jewel = h.eval("function() return %s end" % JEWEL_ITEM)()
    ok, err = h.eval('function(v, t, tab, j) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Elemental Overload", id = 1, sd = { "10% increased Attack Speed" }, '
                     'jewel = j }, { itemsTab = tab }) end) return ok, e end')(view, tip, tab, jewel)
    check("%s ST-L4 天賦提示內嵌物品提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-L4 前提:天賦說明行仍以 passive 呼叫" % fork,
          any(_norm(l) == "10% increased Attack Speed" and c == "passive"
              for l, c in h.stat_calls), repr(h.stat_calls))
    item_lines = ("Kaom's Heart", "Glorious Plate", "Limited to: 1", "Radius: Large",
                  "+12 Life (+1.2%)", "+30 to Dexterity")
    bad = [(l, c) for l, c in h.stat_calls if _norm(l) in item_lines and c == "passive"]
    check("%s ST-L4 內嵌物品提示的行不以 passive 呼叫 PobToolsTranslateStat" % fork,
          not bad, "bad=%r calls=%r" % (bad, h.stat_calls))
    if fork == "PoE1":
        check("%s ST-L4 內嵌物品提示的詞綴行仍以 item 呼叫" % fork,
              any(_norm(l) == "+30 to Dexterity" and c == "item" for l, c in h.stat_calls),
              repr(h.stat_calls))
    check("%s ST-L4 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


# ---- 塗油 / 軍團 / 刺青的來源(ST-P3) -----------------------------------------
#
# 上游(PoE1 / PoE2 Classes/):
#   ItemsTab:anointItem -> enchantModLines { line = "Allocates " .. node.dn },之後經
#     AddItemTooltip 的 modline AddLine 顯示(PoE1 第 4 參數是 modLine;PoE2 是 background)。
#   NotableDBControl:GetRowValue(column, index, node) -> 色碼 .. node.dn(PoE1 CRAFTED、
#     PoE2 ENCHANTED;排序依 power 時為 ^xRR8080 / ^x808080)。
#   TreeTab:ModifyNodePopup(selectedNode)(只有 PoE1;刺青):區域 buildMods 建 modGroups,
#     label = node.dn .. 48 空白 .. table.concat(node.sd, ","),交給
#     controls.modSelect = DropDownControl,再 main:OpenPopup(...)。
#   TreeTab:FindTimelessJewel()(兩分支;軍團):區域 buildMods 建 modData,
#     label = node.dn .. 48 空白 .. node.sd[1];controls.nodeSelect = DropDownControl(modData);
#     換珠寶類型時 controls.jewelSelect 的 selFunc 會 wipeTable(modData) 後重建。
# 兩個下拉清單都是函式區域變數、在 popup 開著時才繪製(而且可能重建),所以合成版保留
# 這些形狀;測試只看「popup 開出來、畫過之後」清單項的 label 與查找來源。

LABEL_PAD = " " * 48

DICT["anoint"] = {"Whispers of Doom": "末日低語(塗油)"}
DICT["tattoo"] = {"Tattoo of the Ramako Scout": "拉瑪克斥候刺青(刺青)"}
DICT["legion"] = {"Ritual of Immolation": "獻祭儀式(軍團)",
                  "Hit and Run": "打帶跑(軍團)"}
DICT[None].update({
    "Allocates": "配置",
    # 現行整行查找的值(來源表沒有該名稱時要維持這個結果)
    "Allocates Heart of Oak": "配置 橡樹之心",
})

DROPDOWN_BODY = """
function C:Draw(viewPort)
	for _, v in ipairs(self.list or { }) do
		DrawString(0, 0, "LEFT", 16, "VAR", type(v) == "table" and v.label or v)
	end
end
"""
DROPDOWN_P1 = """
local C = newClass("DropDownControl")
function C:DropDownControl(anchor, rect, list, selFunc)
	self.list = list self.selFunc = selFunc self.selIndex = 1
	return self
end
""" + DROPDOWN_BODY
DROPDOWN_P2 = """
local C = newClass("DropDownControl", function(self, anchor, rect, list, selFunc)
	self.list = list self.selFunc = selFunc self.selIndex = 1
end)
""" + DROPDOWN_BODY

POPUP_SETUP = b"""
function main:OpenPopup(width, height, title, controls)
	local popup = { width = width, height = height, title = title, controls = controls }
	table.insert(self.popups, 1, popup)
	return popup
end
function main:ClosePopup() table.remove(self.popups, 1) end
"""

TREETAB_METHODS = """
function C:ModifyNodePopup(selectedNode)
	local controls = { }
	local modGroups = { }
	local treeNodes = self.build.spec.tree.nodes
	local nodeName = treeNodes[selectedNode.id].dn
	local function buildMods(selectedNode)
		wipeTable(modGroups)
		for id, node in pairs(self.build.spec.tree.tattoo.nodes) do
			if nodeName:match(node.targetType:gsub("^Small ", "")) then
				table.insert(modGroups, {
				label = node.dn .. "%(pad)s" .. table.concat(node.sd, ","),
				descriptions = copyTable(node.sd),
				id = id,
				})
			end
		end
		table.sort(modGroups, function(a, b) return a.label < b.label end)
	end
	buildMods(selectedNode)
	controls.modSelect = NEW_DROPDOWN(nil, {175, 25, 250, 18}, modGroups, function(idx) end)
	main:OpenPopup(600, 100, "Modify Node", controls, "save")
end
function C:FindTimelessJewel()
	local treeData = self.build.spec.tree
	local legionNodes = treeData.legion.nodes
	local timelessData = self.build.timelessData
	local controls = { }
	local modData = { }
	local jewelTypes = self.jewelTypes
	local function buildMods()
		wipeTable(modData)
		for _, node in pairs(legionNodes) do
			if node.id:match("^" .. timelessData.jewelType.name .. "_.+") and node["not"] then
				table.insert(modData, {
					label = node.dn .. "%(pad)s" .. node.sd[1],
					descriptions = copyTable(node.sd),
					type = timelessData.jewelType.name,
					id = node.id
				})
			end
		end
		table.sort(modData, function(a, b) return a.label < b.label end)
	end
	controls.jewelSelect = NEW_DROPDOWN(nil, {520, 25, 200, 18}, jewelTypes, function(index, value)
		timelessData.jewelType = value
		buildMods()
	end)
	buildMods()
	controls.nodeSelect = NEW_DROPDOWN(nil, {0, 7, 200, 18}, modData, function(index, value) end)
	main:OpenPopup(910, 517, "Find a Timeless Jewel", controls, nil, nil, "close")
end
""" % {"pad": LABEL_PAD}

TREETAB_P1 = """
local C = newClass("TreeTab")
function C:TreeTab(build) self.build = build return self end
""" + TREETAB_METHODS.replace(
    "NEW_DROPDOWN(", 'new("DropDownControl"):DropDownControl(')
TREETAB_P2 = """
local C = newClass("TreeTab", function(self, build) self.build = build end)
""" + TREETAB_METHODS.replace("NEW_DROPDOWN(", 'new("DropDownControl", ')

# NotableDBControl:GetRowValue 照上游(PoE1 CRAFTED、PoE2 ENCHANTED)
NOTABLE_ROW = """
function C:GetRowValue(column, index, node)
	if column == 1 then
		if self.sortDetail and self.sortDetail.stat then
			return "^x808080"..node.dn
		else
			return colorCodes.ROWCODE..node.dn
		end
	end
end
"""
NOTABLE_P1 = """
local C = newClass("NotableDBControl")
function C:NotableDBControl() self.controls = { search = { buf = "" } } return self end
""" + NOTABLE_ROW.replace("ROWCODE", "CRAFTED")
NOTABLE_P2 = """
local C = newClass("NotableDBControl", function(self) self.controls = { search = { buf = "" } } end)
""" + NOTABLE_ROW.replace("ROWCODE", "ENCHANTED")

P3_BUILD = """
{
	spec = { tree = {
		nodes = { [100] = { dn = "Strength", sd = { "+10 to Strength" } } },
		tattoo = { nodes = {
			ramako = { dn = "Tattoo of the Ramako Scout", targetType = "Small Strength",
				targetValue = "", sd = { "+2 to Strength" } },
			unknown = { dn = "Tattoo of the Unknown", targetType = "Small Strength",
				targetValue = "", sd = { "+1 to Strength" } },
		} },
		legion = { nodes = {
			{ id = "glorious_vanity_notable_1", dn = "Ritual of Immolation", ["not"] = true,
				sd = { "Burn things" } },
			{ id = "glorious_vanity_notable_2", dn = "Unknown Vaal Notable", ["not"] = true,
				sd = { "Something" } },
			{ id = "lethal_pride_notable_1", dn = "Hit and Run", ["not"] = true,
				sd = { "Run things" } },
		} },
	} },
	timelessData = { },
}
"""


def p3_harness(fork, with_inject=True):
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.run(COLOUR_SETUP)
    h.run(b'colorCodes.CRAFTED = "^xB8DAF1" colorCodes.ENCHANTED = "^xB8DAF1"')
    h.run(POPUP_SETUP)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    h.define("DropDownControl", DROPDOWN_P1, DROPDOWN_P2)
    h.define("TreeTab", TREETAB_P1, TREETAB_P2)
    h.define("NotableDBControl", NOTABLE_P1, NOTABLE_P2)
    if with_inject:
        h.apply_inject()
    h.lookups = []
    return h


def p3_tree_tab(h):
    build = h.eval("function() return %s end" % P3_BUILD)()
    if h.fork == "PoE1":
        tab = h.eval('function(b) return new("TreeTab"):TreeTab(b) end')(build)
    else:
        tab = h.eval('function(b) return new("TreeTab", b) end')(build)
    tab.jewelTypes = h.eval('function() return { { id = 1, name = "glorious_vanity" }, '
                            '{ id = 2, name = "lethal_pride" } } end')()
    build.timelessData.jewelType = tab.jewelTypes[1]
    return tab


def p3_popup_labels(h, control):
    """畫一次 popup 的指定下拉後回傳各項 label(去色碼)。"""
    return [_norm(x) for x in h.eval(
        'function(name) local c = main.popups[1].controls[name] c:Draw({}) '
        'local o = {} for i, v in ipairs(c.list) do o[i] = v.label end return o end')(
        control).values()]


def allocates_line(fork, name, with_inject=True):
    h = p3_harness(fork, with_inject)
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    item = h.eval('function(n) return { rarity = "RARE", title = "Doom Loop", '
                  'baseName = "Amber Amulet", base = {}, name = "Doom Loop, Amber Amulet", '
                  'explicitModLines = { { crafted = true, line = "Allocates " .. n } } } end')(name)
    h.lookups = []
    ok, err = h.eval('function(tab, t, item) local ok, e = pcall(function() '
                     'tab:AddItemTooltip(t, item) end) return ok, e end')(tab, tip, item)
    lines = [l for l in tooltip_lines(h, tip) if l is not None]
    return h, ok, err, (_norm(lines[-1]) if lines else "")


def test_src_anoint_legion_tattoo(fork):
    """ST-P3 -- 塗油名稱走 anoint、軍團下拉走 legion、刺青下拉走 tattoo。"""
    # (1) Allocates 行
    h, ok, err, line = allocates_line(fork, "Whispers of Doom")
    check("%s ST-P3 Allocates 行本身不出錯" % fork, ok, repr(err))
    check("%s ST-P3 Allocates 行的名稱在 anoint 來源下查詢" % fork,
          "anoint" in srcs_of(h, "Whispers of Doom"), "lookups=%r" % h.lookups)
    check("%s ST-P3 Allocates 行顯示前綴譯文與 anoint 名稱譯文" % fork,
          "配置" in line and "末日低語(塗油)" in line and not re.search(r"[A-Za-z]", line),
          "line=%r" % line)
    check("%s ST-P3 Allocates 行結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # (5) 名稱不在 anoint 表:整行照現行
    h, ok, err, line = allocates_line(fork, "Heart of Oak")
    h0, ok0, err0, line0 = allocates_line(fork, "Heart of Oak", with_inject=False)
    check("%s ST-P3 anoint 表沒有該名稱時 Allocates 行照現行" % fork,
          ok and line == "配置 橡樹之心", "line=%r 未注入時=%r" % (line, line0))

    # (2) NotableDBControl 列表
    for with_inject in (True, False):
        h = p3_harness(fork, with_inject)
        db = make(h, "NotableDBControl")
        row = h.eval('function(o, dn) return o:GetRowValue(1, 1, { dn = dn }) end')
        if with_inject:
            v = row(db, "Whispers of Doom")
            check("%s ST-P3 塗油列表項在 anoint 來源下查詢" % fork,
                  "anoint" in srcs_of(h, "Whispers of Doom"), "lookups=%r" % h.lookups)
            check("%s ST-P3 塗油列表項顯示 anoint 譯文且保留色碼" % fork,
                  v == "^xB8DAF1末日低語(塗油)", "v=%r" % v)
            check("%s ST-P3 塗油列表查詢後來源還原為 nil" % fork,
                  h.source is None, "source=%r" % h.source)
            miss = row(db, "Heart of Oak")
        else:
            miss0 = row(db, "Heart of Oak")
    check("%s ST-P3 anoint 表沒有該名稱時列表項同現行" % fork,
          miss == miss0, "v=%r 未注入時=%r" % (miss, miss0))

    # (3) 軍團下拉(兩分支)
    h = p3_harness(fork)
    tab = p3_tree_tab(h)
    h.eval('function(t) t:FindTimelessJewel() end')(tab)
    labels = p3_popup_labels(h, "nodeSelect")
    check("%s ST-P3 軍團下拉的 dn 在 legion 來源下查詢" % fork,
          "legion" in srcs_of(h, "Ritual of Immolation"), "lookups=%r" % h.lookups)
    check("%s ST-P3 軍團下拉 label 的 dn 段換成 legion 譯文、後段 sd 不變" % fork,
          ("獻祭儀式(軍團)" + LABEL_PAD + "Burn things").strip() in labels,
          "labels=%r" % labels)
    check("%s ST-P3 legion 表沒有的 dn 維持英文 label" % fork,
          ("Unknown Vaal Notable" + LABEL_PAD + "Something").strip() in labels,
          "labels=%r" % labels)
    check("%s ST-P3 軍團下拉繪製後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # 換珠寶類型 -> buildMods 重建 modData,新項目同樣要翻
    h.eval('function() local c = main.popups[1].controls.jewelSelect '
           'c.selFunc(2, c.list[2]) end')()
    labels = p3_popup_labels(h, "nodeSelect")
    check("%s ST-P3 換珠寶類型重建清單後新項目也以 legion 譯文顯示" % fork,
          ("打帶跑(軍團)" + LABEL_PAD + "Run things").strip() in labels,
          "labels=%r" % labels)

    # (3) 刺青下拉(只有 PoE1 有 ModifyNodePopup)
    if fork != "PoE1":
        print("  N/A   %s ST-P3 刺青:此分支 TreeTab 沒有 ModifyNodePopup" % fork)
        return
    h = p3_harness(fork)
    tab = p3_tree_tab(h)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    labels = p3_popup_labels(h, "modSelect")
    check("%s ST-P3 刺青下拉的 dn 在 tattoo 來源下查詢" % fork,
          "tattoo" in srcs_of(h, "Tattoo of the Ramako Scout"), "lookups=%r" % h.lookups)
    check("%s ST-P3 刺青下拉 label 的 dn 段換成 tattoo 譯文、後段 sd 不變" % fork,
          ("拉瑪克斥候刺青(刺青)" + LABEL_PAD + "+2 to Strength") in labels,
          "labels=%r" % labels)
    check("%s ST-P3 tattoo 表沒有的 dn 維持英文 label" % fork,
          ("Tattoo of the Unknown" + LABEL_PAD + "+1 to Strength") in labels,
          "labels=%r" % labels)
    check("%s ST-P3 刺青下拉繪製後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)


# ---- 與引擎 source patch 並存(ST-FB) -----------------------------------------
#
# ui_api.cpp「刺青與軍團珠寶下拉的名稱維持英文」把 treetab.lua 的
#   label = node.dn .. "
# 換成
#   label = ((PobToolsTranslateDisplay and PobToolsTranslateDisplay(node.dn)) or node.dn) .. "
# 建清單時(來源 nil)就寫入全域譯名,注入層之後拿到的 dn 段已是中文,
# legion / tattoo 來源查不到。這組測試在合成 TreeTab 上套真的 source patch。

DICT[None].update({
    "Bleed Avoidance": "流血迴避(全域)",
    "Tattoo of the Ramako Scout": "拉瑪克斥候紋身(全域)",
})
DICT["legion"]["Bleed Avoidance"] = "流血避免"


def fb_harness(fork):
    """p3_harness,但 TreeTab 用套過引擎 treetab.lua source patch 的版本。"""
    src = TREETAB_P1 if fork == "PoE1" else TREETAB_P2
    patched, applied = apply_source_patches(
        src, "treetab.lua", parse_source_patches(read_text(UI_API)))
    h = p3_harness(fork, with_inject=False)
    h.run(patched)
    h.apply_inject()
    h.lookups = []
    return h, patched, applied


def fb_build(h):
    tab = p3_tree_tab(h)
    tab.build.spec.tree.legion.nodes[4] = h.eval(
        'function() return { id = "glorious_vanity_notable_3", dn = "Bleed Avoidance", '
        '["not"] = true, sd = { "Avoid bleed" } } end')()
    return tab


def test_src_legion_tattoo_with_source_patch(fork):
    """ST-FB -- 套用引擎 source patch 後,下拉 label 的 dn 段仍是 legion / tattoo 來源譯名。"""
    h, patched, applied = fb_harness(fork)
    check("%s ST-FB 前提:引擎 source patch 確實改寫了合成 TreeTab 的 label 式" % fork,
          applied >= 1 and "PobToolsTranslateDisplay(node.dn)" in patched,
          "applied=%d" % applied)
    tab = fb_build(h)
    h.eval('function(t) t:FindTimelessJewel() end')(tab)
    labels = p3_popup_labels(h, "nodeSelect")
    want = ("流血避免" + LABEL_PAD + "Avoid bleed").strip()
    check("%s ST-FB 軍團下拉 label 的 dn 段是 legion 來源譯名而非全域譯名" % fork,
          want in labels, "labels=%r lookups=%r" % (labels, h.lookups))
    check("%s ST-FB 軍團下拉 label 不殘留全域譯名" % fork,
          not any(l.startswith("流血迴避(全域)") for l in labels), "labels=%r" % labels)
    check("%s ST-FB 軍團下拉繪製後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    if fork != "PoE1":
        print("  N/A   %s ST-FB 刺青:此分支 TreeTab 沒有 ModifyNodePopup" % fork)
        return
    h, patched, applied = fb_harness(fork)
    tab = fb_build(h)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    labels = p3_popup_labels(h, "modSelect")
    check("%s ST-FB 刺青下拉 label 的 dn 段是 tattoo 來源譯名而非全域譯名" % fork,
          any(l.startswith("拉瑪克斥候刺青(刺青)" + LABEL_PAD) for l in labels),
          "labels=%r lookups=%r" % (labels, h.lookups))
    check("%s ST-FB 刺青下拉 label 不殘留全域譯名" % fork,
          not any(l.startswith("拉瑪克斥候紋身(全域)") for l in labels), "labels=%r" % labels)


# ---- 多行 stat 模板被折成兩個 sd 元素(ST-P2b) --------------------------------
#
# node 51108:GGPK 一個兩行模板,POB 拆成 sd 兩個元素逐行 AddLine。
# 假引擎只認 "\n" 合併句、回兩行中文;單行一律 nil。

SD_A = "10% increased Effect of Arcane Surge on you per"
SD_B = "200 Mana spent Recently, up to 50%"
P2B_STAT_DICT = {SD_A + "\n" + SD_B: "近期每消耗 200 魔力,\n秘術增強對你的效果增加 10%,最多 50%"}
P2B_ZH = ["近期每消耗 200 魔力,", "秘術增強對你的效果增加 10%,最多 50%"]


def p2b_tooltips(fork, stat_dict, nodes, define=True):
    """依序對多個節點各建一個天賦提示;回 (h, ok, err, [[原始行...] 每節點])。"""
    h = stat_harness(fork, stat_dict, define)
    view = make(h, "PassiveTreeView")
    out = []
    ok, err = True, None
    for i, sd in enumerate(nodes):
        tip = make(h, "Tooltip")
        sd_lua = h.eval("function(...) return {...} end")(*sd)
        o, e = h.eval('function(v, t, id, sd) local ok, e = pcall(function() '
                      'v:AddNodeTooltip(t, { dn = "Elemental Overload", id = id, sd = sd }, {}) '
                      'end) return ok, e end')(view, tip, 51108 + i, sd_lua)
        ok, err = ok and o, err or e
        out.append([l for l in tooltip_lines(h, tip) if l is not None])
    return h, ok, err, out


def test_stat_multiline_sd(fork):
    """ST-P2b -- 相鄰兩個 sd 行以 "\\n" 合併查 stat,命中兩行中文則各取一行。"""
    # (1)(2)(5)
    h, ok, err, tips = p2b_tooltips(fork, P2B_STAT_DICT, [[SD_A, SD_B]])
    lines = tips[0]
    check("%s ST-P2b 天賦提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-P2b 合併句命中時兩行各顯示對應的中文行" % fork,
          [_norm(l) for l in lines[1:3]] == P2B_ZH,
          "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s ST-P2b 以 \"\\n\" 合併句且 ctx passive 呼叫 PobToolsTranslateStat" % fork,
          (SD_A + "\n" + SD_B, "passive") in [(_norm_ml(l), c) for l, c in h.stat_calls],
          repr(h.stat_calls))
    check("%s ST-P2b 帶色碼的說明行對上後補回原色碼前綴" % fork,
          lines[1:3] == ["^7" + z for z in P2B_ZH], "lines=%r" % (lines,))
    # (1b) 前一組配對失敗會往下滑:X+A 不中、A+B 中;X 照現行
    h, ok, err, tips = p2b_tooltips(
        fork, P2B_STAT_DICT, [["10% increased Attack Speed", SD_A, SD_B]])
    got = [_norm(l) for l in tips[0][1:4]]
    check("%s ST-P2b 三行中後兩行配對成功、第一行照現行" % fork,
          got == ["增加 10% 攻擊速度"] + P2B_ZH, "got=%r calls=%r" % (got, h.stat_calls))
    # (3) 合併查不到:兩行照現行逐行路徑(與未定義函式時相同)
    h, ok, err, tips = p2b_tooltips(fork, {}, [[SD_A, SD_B]])
    h0, ok0, err0, tips0 = p2b_tooltips(fork, None, [[SD_A, SD_B]], define=False)
    check("%s ST-P2b 合併句未命中時兩行照現行逐行路徑" % fork,
          ok and tips[0] == tips0[0] and [_norm(l) for l in tips[0][1:3]] == [SD_A, SD_B],
          "lines=%r 未定義時=%r" % (tips[0], tips0[0]))
    # (4) 對照只屬該節點:下一個節點的同一行不沿用
    h, ok, err, tips = p2b_tooltips(fork, P2B_STAT_DICT,
                                    [[SD_A, SD_B], [SD_A, "10% increased Attack Speed"]])
    got1 = [_norm(l) for l in tips[0][1:3]]
    got2 = [_norm(l) for l in tips[1][1:3]]
    check("%s ST-P2b 第一個節點配對成功(前提)" % fork, got1 == P2B_ZH, "got=%r" % got1)
    check("%s ST-P2b 天賦提示結束後對照清空,下一個節點同一行不沿用" % fork,
          got2 == [SD_A, "增加 10% 攻擊速度"], "got2=%r calls=%r" % (got2, h.stat_calls))


# ---- ST-FR3:不支援詞綴的註記 --------------------------------------------------
#
# 註記不在 node.sd 裡:PassiveTreeView addModInfoToTooltip(PoE1 L1558-1561、PoE2 L1719-1722)
# 在 AddLine 當下才組 colorCodes.UNSUPPORTED .. sd .. main.notSupportedTooltipText,
# 後者 = " ^8(Not supported in PoB yet)"(Main.lua),受 main.notSupportedModTooltips 開關。
# 例:node 2275 Nature's Concoction 兩行 sd,單一多行模板。

FR3_A = "Flasks adjacent to Active Tinctures gain 1 Charge"
FR3_B = "when you Hit an Enemy with a Weapon"
FR3_ZH = ["與作用中酊劑相鄰的藥劑", "在你以武器擊中敵人時獲得 1 充能"]
FR3_STAT_DICT = {FR3_A + "\n" + FR3_B: "\n".join(FR3_ZH)}
FR3_UNSUP = "^xF05050"
FR3_NOTE = " ^8(Not supported in PoB yet)"


def fr3_tooltip(fork, stat_dict, note_on=True, define=True):
    h = stat_harness(fork, stat_dict, define)
    h.run(('colorCodes.UNSUPPORTED = "%s" main.notSupportedModTooltips = %s '
           'main.notSupportedTooltipText = "%s"') % (FR3_UNSUP, "true" if note_on else "false",
                                                   FR3_NOTE))
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(v, t, a, b) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Nature\'s Concoction", id = 2275, unsupported = true, sd = { a, b } }, {}) '
                     'end) return ok, e end')(view, tip, FR3_A, FR3_B)
    return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None]


def test_stat_multiline_sd_unsupported(fork):
    """ST-FR3 -- 不支援註記的兩行 sd:合併查詢不含註記,命中後註記補回各行尾。"""
    h, ok, err, lines = fr3_tooltip(fork, FR3_STAT_DICT)
    check("%s ST-FR3 天賦提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-FR3 合併查詢用不含註記的兩行 sd" % fork,
          (FR3_A + "\n" + FR3_B, "passive") in [(_norm_ml(l), c) for l, c in h.stat_calls]
          and not any("Not supported" in (l or "") and "\n" in (l or "")
                      for l, c in h.stat_calls), repr(h.stat_calls))
    check("%s ST-FR3 命中後兩行顯示中文且行尾補回註記與原色碼" % fork,
          lines[1:3] == [FR3_UNSUP + z + FR3_NOTE for z in FR3_ZH],
          "lines=%r calls=%r" % (lines, h.stat_calls))
    # 註記開關關閉:同一配對,行尾沒有註記
    h, ok, err, lines = fr3_tooltip(fork, FR3_STAT_DICT, note_on=False)
    check("%s ST-FR3 註記關閉時兩行顯示中文且無註記" % fork,
          lines[1:3] == [FR3_UNSUP + z for z in FR3_ZH], "lines=%r" % (lines,))
    # 合併未命中:與未定義函式時相同(英文 + 註記)
    h, ok, err, lines = fr3_tooltip(fork, {})
    h0, ok0, err0, lines0 = fr3_tooltip(fork, None, define=False)
    check("%s ST-FR3 合併未命中時兩行照現行(保留英文與註記)" % fork,
          ok and lines == lines0 and lines[1:3] == [FR3_UNSUP + s + FR3_NOTE for s in (FR3_A, FR3_B)],
          "lines=%r 未定義時=%r" % (lines, lines0))


def _norm_ml(s):
    """去每行色碼後以 \\n 接回(合併句比對用)。"""
    return "\n".join(_norm(p) for p in (s or "").split("\n"))


# ---- 寶石提示(ST-P1) ---------------------------------------------------------
#
# 上游 Classes/GemTooltip.lua 不是 newClass 類別,是模組表:
#   GemTooltip.AddGemTooltip(tooltip, build, gemInstance, options)(點呼叫、4 參數),
#   PoE1 以 require("Classes.GemTooltip") 取得(package.loaded 快取同一張表),
#   PoE2 以 LoadModule("Classes/GemTooltip") 取得(每個呼叫端各跑一次、各一張表)。
#   GemSelectControl:AddGemTooltip(gemInstance) 再轉呼叫它;一個提示可含多個
#   grantedEffect(PoE1 secondaryGrantedEffect、PoE2 additionalGrantedEffects),各有說明行。
# 合成模組照各分支的取得方式註冊,且在注入「之後」才延遲載入(同 POB)。

GEM_COLOUR_SETUP = b"""
colorCodes.GEM = "^x1AA29B"
colorCodes.GEMDESCRIPTION = "^x7F7F7F"
colorCodes.GEMINFO = "^x7F7F7F"
"""

GEMTOOLTIP_SYN_P1 = """
local GemTooltip = { }
-- 上游 addQualityRangeInfo:describeStats 的行再 gsub 成 (0-N),改過的行不在 lineMap 裡
local function addQualityRange(tooltip, build, grantedEffect)
	if not grantedEffect.qualityStats then return end
	local descriptions = build.data.describeStats(grantedEffect.qualityStats,
		grantedEffect.statDescriptionScope, true)
	for _, line in ipairs(descriptions) do
		line = line:gsub("([%+%-]?%d+%.?%d*)", function(v) return "(0-" .. v .. ")" end, 1)
		tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC")
	end
end
local function addCommonGemInfo(tooltip, build, gemInstance, grantedEffect)
	if grantedEffect.description then
		tooltip:AddLine(16, colorCodes.GEM .. grantedEffect.description, "FONTIN SC")
	end
	if build.data.describeStats then
		local descriptions, lineMap = build.data.describeStats(grantedEffect.stats or { },
			grantedEffect.statDescriptionScope)
		for _, line in ipairs(descriptions) do
			tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC")
		end
		addQualityRange(tooltip, build, grantedEffect)
	end
end
function GemTooltip.AddGemTooltip(tooltip, build, gemInstance, options)
	options = options or { }
	tooltip.center = true
	tooltip.color = colorCodes.GEM
	local primary = gemInstance.gemData.grantedEffect
	local secondary = gemInstance.gemData.secondaryGrantedEffect
	tooltip:AddLine(20, colorCodes.GEM .. primary.name, "FONTIN SC")
	addCommonGemInfo(tooltip, build, gemInstance, primary)
	if secondary and (not secondary.support or gemInstance.gemData.secondaryEffectName) then
		tooltip:AddLine(20, colorCodes.GEM .. secondary.name, "FONTIN SC")
		addCommonGemInfo(tooltip, build, gemInstance, secondary)
	end
end
return GemTooltip
"""

GEMTOOLTIP_SYN_P2 = """
local GemTooltip = { }
-- 上游 addQualityRangeInfo:describeStats 的行再 gsub 成 (0-N),改過的行不在 lineMap 裡
local function addQualityRange(tooltip, build, grantedEffect)
	if not grantedEffect.qualityStats then return end
	local descriptions = build.data.describeStats(grantedEffect.qualityStats,
		grantedEffect.statDescriptionScope, true)
	for _, line in ipairs(descriptions) do
		line = line:gsub("([%+%-]?%d+%.?%d*)", function(v) return "(0-" .. v .. ")" end, 1)
		tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC")
	end
end
local function addDescriptionLine(tooltip, line, index)
	local bg = (index % 2 == 0) and "GemHoverModBg" or nil
	tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC", bg)
end
local function addGrantedEffectInfo(tooltip, build, gemInstance, grantedEffect)
	tooltip.center = true
	if grantedEffect.description then
		tooltip:AddLine(16, colorCodes.GEMDESCRIPTION .. grantedEffect.description, "FONTIN ITALIC")
	end
end
local function addEffectStats(tooltip, build, gemInstance, grantedEffect)
	local descriptions, lineMap = build.data.describeStats(grantedEffect.stats or { },
		grantedEffect.statDescriptionScope)
	for i, line in ipairs(descriptions) do
		addDescriptionLine(tooltip, line, i)
	end
	addQualityRange(tooltip, build, grantedEffect)
end
function GemTooltip.AddGemTooltip(tooltip, build, gemInstance, options)
	options = options or { }
	tooltip.center = false
	tooltip.color = colorCodes.GEM
	local grantedEffect = gemInstance.gemData.grantedEffect
	tooltip:AddLine(20, colorCodes.GEM .. gemInstance.gemData.name, "FONTIN SC")
	addGrantedEffectInfo(tooltip, build, gemInstance, grantedEffect)
	addEffectStats(tooltip, build, gemInstance, grantedEffect)
	for _, additional in ipairs(gemInstance.gemData.additionalGrantedEffects or { }) do
		tooltip:AddLine(20, colorCodes.GEM .. additional.name, "FONTIN SC")
		addGrantedEffectInfo(tooltip, build, gemInstance, additional)
		addEffectStats(tooltip, build, gemInstance, additional)
	end
end
return GemTooltip
"""

# 上游 GemSelectControl 檔頂取得 gemTooltip、AddGemTooltip 轉呼叫;Draw 只留建提示那段。
GEMSELECT_TIP_BODY = """
function C:AddGemTooltip(gemInstance)
	gemTooltip.AddGemTooltip(self.tooltip, self.skillsTab.build, gemInstance)
end
function C:Draw(viewPort, noTooltip)
	if self.tipGem then self:AddGemTooltip(self.tipGem) end
end
"""

GEMSELECT_TIP_P1 = ('local gemTooltip = require("Classes.GemTooltip")\n'
                    + GEMSELECT_P1.replace("function C:Draw() end\n", "")
                    + GEMSELECT_TIP_BODY)
GEMSELECT_TIP_P2 = ('local gemTooltip = LoadModule("Classes/GemTooltip")\n'
                    + GEMSELECT_P2.replace("function C:Draw() end\n", "")
                    + GEMSELECT_TIP_BODY)

# 照真 Lua require:先 package.loaded,再依序問 package.loaders(含 preload);
# 只有找不到的名稱才退回 PRELUDE 的假模組。
FAITHFUL_REQUIRE = b"""
do
	local dummy = require
	require = function(name)
		if package.loaded[name] ~= nil then return package.loaded[name] end
		for _, loader in ipairs(package.loaders) do
			local f = loader(name)
			if type(f) == "function" then
				local m = f(name)
				if m == nil then m = true end
				package.loaded[name] = m
				return m
			end
		end
		return dummy(name)
	end
end
"""

GEM_STAT_DICT = {"Deals 10 to 20 Fire Damage": "STAT寶石"}
GEM_DESC_ID = {
    ("gems_desc", "Fireball"): ("Unleashes a ball of fire.", "釋放一顆火球。"),
    ("gems_desc", "FireballSecondary"): ("Secondary blast.", "次要爆炸。"),
}
# 現行路徑(Draw 包在 gems 來源)會查到的值:說明行與 stat 行在 gems 來源的譯文
DICT["gems"].update({
    "Unleashes a ball of fire.": "火球說明(gems)",
    "Deals 10 to 20 Fire Damage": "造成 10-20 火焰傷害(gems)",
})
GEM_EXPECT_EXISTING = ["火球", "火球說明(gems)", "造成 10-20 火焰傷害(gems)",
                       "(0-20)% increased Area of Effect"]

# StatDescriber 的替身,回傳形狀照上游 Modules/StatDescriber.lua:
#   return out, lineMap  -- out = 行陣列;lineMap[行] = 單一 stat id 字串
#   (該描述 stats 清單裡「最後一個有值」的 stat;不是全部 id)
SYN_DESCRIBER = b"""
local SYN_DESC = {
	{ stats = { "spell_minimum_base_fire_damage", "spell_maximum_base_fire_damage" },
	  fmt = function(v) return string.format("Deals %d to %d Fire Damage", v[1], v[2]) end },
	{ stats = { "base_skill_area_of_effect_+%" },
	  fmt = function(v) return string.format("%d%% increased Area of Effect", v[1]) end },
}
SynDescribeStats = function(stats, scopeName, quality)
	local out, lineMap = { }, { }
	for _, d in ipairs(SYN_DESC) do
		local v, stat = { }, nil
		for i, s in ipairs(d.stats) do
			v[i] = stats[s] or 0
			if stats[s] then stat = s end
		end
		if stat then
			local line = d.fmt(v)
			table.insert(out, line)
			lineMap[line] = stat
		end
	end
	return out, lineMap
end
data = data or { }
data.describeStats = SynDescribeStats
"""


# 現在的 data.describeStats 與原替身同輸入比對:回傳個數、out 每行、lineMap 每對
DESCRIBE_SAME = """function()
	local stats = { spell_minimum_base_fire_damage = 3, spell_maximum_base_fire_damage = 7,
		["base_skill_area_of_effect_+%"] = 5 }
	local function pack(...)
		local t = { n = select("#", ...) }
		for i = 1, t.n do t[i] = select(i, ...) end
		return t
	end
	local a = pack(data.describeStats(stats, "gem_stat_descriptions", true))
	local b = pack(SynDescribeStats(stats, "gem_stat_descriptions", true))
	if a.n ~= b.n then return "n " .. a.n .. "~=" .. b.n end
	if #a[1] ~= #b[1] then return "out len" end
	for i = 1, #b[1] do if a[1][i] ~= b[1][i] then return "out " .. i end end
	for k, v in pairs(b[2]) do if a[2][k] ~= v then return "lineMap " .. k end end
	for k, v in pairs(a[2]) do if b[2][k] ~= v then return "lineMap extra " .. k end end
	return "same"
end"""


def gem_instance_src(fork, with_secondary=False):
    sec = ('{ id = "FireballSecondary", name = "Fire Burst", '
           'description = "Secondary blast.", stats = { } }')
    extra = ""
    if with_secondary:
        extra = ("secondaryGrantedEffect = %s," % sec if fork == "PoE1"
                 else "additionalGrantedEffects = { %s }," % sec)
    return """
{
	level = 1,
	gemData = {
		name = "Fireball",
		grantedEffect = { id = "Fireball", name = "Fireball",
			description = "Unleashes a ball of fire.",
			stats = { spell_minimum_base_fire_damage = 10, spell_maximum_base_fire_damage = 20 },
			qualityStats = { ["base_skill_area_of_effect_+%%"] = 20 },
			statDescriptionScope = "gem_stat_descriptions" },
		%s
	},
}
""" % extra


def gem_harness(fork, stat_dict, define_stat=True, id_dict=None):
    """寶石提示 harness:假 PobToolsTranslateStat + 嚴格依 (id, en) 的 PobToolsTranslateId。"""
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.stat_calls = []
    h.stat_ids = []          # (line, ctx, 第 3 參數 stat ids)

    def translate_stat(line, ctx=None, ids=None):
        h.stat_calls.append((line, ctx))
        h.stat_ids.append((line, ctx, ids))
        if line is None:
            return None
        m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(.*)$", line, re.S)
        hit = (stat_dict or {}).get(m.group(2).strip())
        return None if hit is None else m.group(1) + hit

    def translate_id(source, id_, en=None):
        if isinstance(id_, float) and id_.is_integer():
            id_ = int(id_)
        key = None if id_ is None else str(id_)
        h.id_lookups.append((source, key, en))
        if not h.enabled:
            return None
        hit = (GEM_DESC_ID if id_dict is None else id_dict).get((source, key))
        return hit[1] if hit and hit[0] == en else None

    g = h.rt.globals()
    g["__py_translate_id"] = translate_id
    if define_stat:
        g["__py_translate_stat"] = translate_stat
        h.run(b"function PobToolsTranslateStat(l, c, i) return __py_translate_stat(l, c, i) end")
    h.run(COLOUR_SETUP)
    h.run(GEM_COLOUR_SETUP)
    h.run(SYN_DESCRIBER)
    h.run('LoadModule("Classes/Tooltip")')
    h.run('LoadModule("Classes/EditControl")')
    h.define("PassiveTreeView", PASSIVETREE_P1, PASSIVETREE_P2)
    h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    # 合成檔:GemTooltip 與 GemSelectControl 經 LoadModule / require 供應
    syn = {"Classes/GemTooltip": GEMTOOLTIP_SYN_P1 if fork == "PoE1" else GEMTOOLTIP_SYN_P2,
           "Classes/GemSelectControl": GEMSELECT_TIP_P1 if fork == "PoE1" else GEMSELECT_TIP_P2}
    real_lm = h._load_module

    def load_module(name, *args):
        key = name[:-4] if name.endswith(".lua") else name
        if key in syn:
            return h._chunk(syn[key].encode("utf-8"), "@" + key)(*args)
        return real_lm(name, *args)

    # 包成真 Lua 函式:注入層只在 type(LoadModule) == "function" 時才掛延遲載入的 patch
    g["__py_load_module"] = load_module
    h.run(b"function LoadModule(...) return __py_load_module(...) end "
          b"PLoadModule = LoadModule")
    h.run(FAITHFUL_REQUIRE)
    g["__py_gemtooltip_chunk"] = lambda: h._chunk(GEMTOOLTIP_SYN_P1.encode("utf-8"),
                                                  "@Classes/GemTooltip")
    h.run(b'package.preload["Classes.GemTooltip"] = function(...) '
          b'return __py_gemtooltip_chunk()(...) end')
    h.apply_inject()
    h.run('LoadModule("Classes/GemSelectControl")')   # 注入後才延遲載入
    h.lookups = []
    return h


def gem_tooltip_lines(fork, stat_dict, define_stat=True, id_dict=None,
                      with_secondary=False, h=None):
    """GemSelectControl:Draw 建寶石提示;回 (h, ok, err, 去色碼行)。"""
    h = h or gem_harness(fork, stat_dict, define_stat, id_dict)
    obj = make(h, "GemSelectControl")
    tip = make(h, "Tooltip")
    gem = h.eval("function() return %s end" % gem_instance_src(fork, with_secondary))()
    ok, err = h.eval('function(o, t, g) o.tooltip = t o.skillsTab = { build = { data = data } } '
                     'o.tipGem = g local ok, e = pcall(function() o:Draw({}, false) end) '
                     'return ok, e end')(obj, tip, gem)
    lines = [_norm(l) for l in tooltip_lines(h, tip) if l is not None]
    return h, ok, err, lines


def test_stat_gem_tooltip(fork):
    """ST-P1 -- 寶石提示:說明行依 gems_desc + 效果 id,stat 行以 gem 情境查 stat。"""
    # (1)(2) 命中
    h, ok, err, lines = gem_tooltip_lines(fork, GEM_STAT_DICT)
    check("%s ST-P1 寶石提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-P1 說明行以 PobToolsTranslateId(\"gems_desc\", 效果 id, 英文) 查詢" % fork,
          ("gems_desc", "Fireball", "Unleashes a ball of fire.") in h.id_lookups,
          repr(h.id_lookups))
    check("%s ST-P1 說明行採用依 id 查到的譯文" % fork,
          "釋放一顆火球。" in lines, "lines=%r id_lookups=%r" % (lines, h.id_lookups))
    check("%s ST-P1 stat 行以 ctx gem 呼叫 PobToolsTranslateStat" % fork,
          any(_norm(l) == "Deals 10 to 20 Fire Damage" and c == "gem"
              for l, c in h.stat_calls), repr(h.stat_calls))
    check("%s ST-P1 stat 行採用 stat 查到的譯文" % fork,
          "STAT寶石" in lines, "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s ST-P1 寶石提示結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # (6) stat 行第 3 參數帶該行 lineMap 的 stat id(空白分隔清單須含它)
    fire = [ids for l, c, ids in h.stat_ids
            if _norm(l) == "Deals 10 to 20 Fire Damage" and c == "gem"]
    check("%s ST-P1 寶石 stat 行第 3 參數含該行 lineMap 的 stat id" % fork,
          fire and all(isinstance(i, str) and "spell_maximum_base_fire_damage" in i.split()
                       for i in fire), "ids=%r stat_ids=%r" % (fire, h.stat_ids))
    # (7) 改寫過、不在 lineMap 的行:以 gem 呼叫、第 3 參數 nil
    q = [(c, ids) for l, c, ids in h.stat_ids
         if _norm(l) == "(0-20)% increased Area of Effect"]
    check("%s ST-P1 無 lineMap 對照的寶石行以 gem 呼叫且第 3 參數為 nil" % fork,
          q and all(c == "gem" and ids is None for c, ids in q), "q=%r" % q)
    # (8) describeStats 包裝後回傳值完全不變(個數、行、lineMap)
    same = h.eval(DESCRIBE_SAME)()
    check("%s ST-P1 data.describeStats 包裝後回傳值完全不變" % fork,
          same == "same", repr(same))
    # (1e) 次要/附加效果的說明行用它自己的 id
    h, ok, err, lines = gem_tooltip_lines(fork, GEM_STAT_DICT, with_secondary=True)
    check("%s ST-P1 次要效果說明行以自己的效果 id 查詢並採用" % fork,
          ok and ("gems_desc", "FireballSecondary", "Secondary blast.") in h.id_lookups
          and "次要爆炸。" in lines,
          "lines=%r id_lookups=%r err=%r" % (lines, h.id_lookups, err))
    # (3) 未命中:與現行完全相同
    h, ok, err, lines = gem_tooltip_lines(fork, {}, id_dict={})
    h0, ok0, err0, lines0 = gem_tooltip_lines(fork, None, define_stat=False, id_dict={})
    check("%s ST-P1 stat 與 id 都未命中時結果同現行(gems 來源)" % fork,
          ok and lines == GEM_EXPECT_EXISTING and lines == lines0,
          "lines=%r 未定義 stat 時=%r err=%r" % (lines, lines0, err))
    check("%s ST-P1 舊引擎無 PobToolsTranslateStat 寶石提示不報錯且同現行" % fork,
          ok0 and lines0 == GEM_EXPECT_EXISTING, "lines=%r err=%r" % (lines0, err0))
    # (5) 寶石情境不外漏:之後的天賦/物品提示不以 gem 呼叫
    h, ok, err, lines = gem_tooltip_lines(fork, GEM_STAT_DICT)
    n = len(h.stat_calls)
    view = make(h, "PassiveTreeView")
    tab = make(h, "ItemsTab")
    t1 = make(h, "Tooltip")
    t2 = make(h, "Tooltip")
    h.eval('function(v, t) v:AddNodeTooltip(t, { dn = "Elemental Overload", id = 1, '
           'sd = { "10% increased Attack Speed" } }, {}) end')(view, t1)
    item = h.eval("function() return %s end" % JEWEL_ITEM)()
    h.eval('function(tab, t, item) tab:AddItemTooltip(t, item) end')(tab, t2, item)
    later = h.stat_calls[n:]
    check("%s ST-P1 寶石提示後天賦/物品提示的行不以 gem 情境呼叫" % fork,
          later and not any(c == "gem" for l, c in later), repr(later))


def test_wrapped_arity_gem(fork):
    """ST-P1(4) -- GemTooltip.AddGemTooltip 包裝後 nparams 與上游相同(4、非 vararg)。"""
    import lupa.luajit21 as luajit21
    h = Harness(fork, with_inject=False, lua_mod=luajit21)
    h.run(ARITY_STUBS)
    h.run(FAITHFUL_REQUIRE)
    path = os.path.join(h.dir, "Classes", "GemTooltip.lua")
    h.rt.globals()["__py_gemtooltip_chunk"] = lambda: h._chunk(h._read(path), "@" + path)
    h.run(b'package.preload["Classes.GemTooltip"] = function(...) '
          b'return __py_gemtooltip_chunk()(...) end')
    h.run('data.describeStats = LoadModule("Modules/StatDescriber")')   # 同 Data.lua
    dshape = h.run('return function() local i = debug.getinfo(data.describeStats, "u") '
                   'return i.nparams, i.isvararg and true or false end')
    dbefore = dshape()
    get = ('return function() return require("Classes.GemTooltip") end' if fork == "PoE1"
           else 'return function() return LoadModule("Classes/GemTooltip") end')
    shape = h.run('return function(m) local i = debug.getinfo(m.AddGemTooltip, "u") '
                  'return i.nparams, i.isvararg and true or false end')
    before = shape(h.run(get)())
    h.apply_inject()
    h.run('LoadModule("Classes/GemSelectControl")')
    after = shape(h.run(get)())
    dafter = dshape()
    check("%s ST-P1 注入後 data.describeStats 參數數不變(上游 3 參數非 vararg)" % fork,
          tuple(dbefore) == (3, False) and tuple(dafter) == tuple(dbefore),
          "before=%r after=%r" % (dbefore, dafter))
    check("%s ST-P1 上游 GemTooltip.AddGemTooltip 形狀為 4 參數非 vararg" % fork,
          tuple(before) == (4, False), "before=%r" % (before,))
    check("%s ST-P1 注入後 GemTooltip.AddGemTooltip 參數數不變" % fork,
          tuple(after) == tuple(before), "before=%r after=%r" % (before, after))


# ---- upstream shape checks (catch a rename before a user does) -------------

# ---- 包裝後參數數不變(新介面相容性閘門) -------------------------------------
#
# bridge.lua 的 probe 以 debug.getinfo(fn,"u").nparams 驗 POB 上游形狀(例:
# AddNodeTooltip 要 4 或 5、BUILD.Init 要 6)。inject 的包裝若寫成
# function(self, ...),nparams 變 1,閘門 ok=false,新介面退回經典視窗。
# Lua 5.1 沒有 nparams,所以這組測試跑在 LuaJIT(與 POB 相同)。

ARITY_CLASSES = ("EditControl", "Tooltip", "SearchHost", "SkillListControl",
                 "GemSelectControl", "PassiveTreeView", "NotableDBControl",
                 "ItemDBControl", "MinionSearchListControl", "CalcsTab",
                 "ItemsTab", "Item", "TreeTab")

# 上游頂層會碰到 itemLib / data;這裡只要檔案載得完、方法定義得出來。
ARITY_STUBS = rb"""
local function deep()
	return setmetatable({}, { __index = function(t, k) local v = deep() rawset(t, k, v) return v end,
	                          __call = function() return deep() end })
end
itemLib = itemLib or deep()
data = data or deep()
main.DrawBackground = function(self, viewPort) end
main.modes = {
	BUILD = { Init = function(self, a, b, c, d, e) end, OnFrame = function(self, inputEvents) end },
	LIST = { OnFrame = function(self, inputEvents) end },
}
function PobToolsBackground() return "", 50, 0, 100 end
function PobToolsPerfEnabled() return true end
function PobToolsPerfNow() return 0 end
function PobToolsPerfMark() end
function PobToolsPerfEvent() end
"""

# 記下每個函式(類別方法、main 與 modes 的方法、PobToolsItemTitle)的原物件與形狀。
ARITY_SNAPSHOT = """
return function()
	local snap = {}
	local function rec(key, fn)
		if type(fn) ~= "function" then return end
		local i = debug.getinfo(fn, "u")
		snap[key] = { fn = fn, np = i.nparams, va = i.isvararg and true or false }
	end
	for cname, cls in pairs(common.classes) do
		for k, v in pairs(cls) do rec(cname .. "." .. tostring(k), v) end
	end
	rec("main.DrawBackground", main.DrawBackground)
	for mode, m in pairs(main.modes) do
		for k, v in pairs(m) do rec("main.modes." .. mode .. "." .. tostring(k), v) end
	end
	rec("PobToolsItemTitle", PobToolsItemTitle)
	return snap
end
"""

# 依快照比對:被換掉的函式 -> {key: (原 np, 原 va, 新 np, 新 va)}
ARITY_DIFF = """
return function(snap)
	local out = {}
	local function cur(key)
		local cname, k = key:match("^([^.]+)%.(.+)$")
		if key == "PobToolsItemTitle" then return PobToolsItemTitle end
		if key == "main.DrawBackground" then return main.DrawBackground end
		local mode, mk = key:match("^main%.modes%.([^.]+)%.(.+)$")
		if mode then return main.modes[mode][mk] end
		return common.classes[cname][k]
	end
	for key, s in pairs(snap) do
		local fn = cur(key)
		if fn ~= s.fn and type(fn) == "function" then
			local i = debug.getinfo(fn, "u")
			out[key] = { s.np, s.va, i.nparams, i.isvararg and true or false }
		end
	end
	return out
end
"""

# 至少這些必須被包裝且形狀不變(bridge probe 實際檢查的 + 任務點名的)。
# AddNodeTooltip 不列:外層不再套 passives 來源,可以不包;有包的話下方迴圈照樣驗形狀。
ARITY_MUST = ("PassiveTreeView.AddNodeName",
              "ItemsTab.AddItemTooltip", "ItemDBControl.GetRowValue",
              "Tooltip.AddLine", "main.modes.BUILD.Init")


def test_wrapped_arity(fork):
    import lupa.luajit21 as luajit21
    h = Harness(fork, with_inject=False, lua_mod=luajit21)
    h.run(ARITY_STUBS)
    for c in ARITY_CLASSES:
        h.run('LoadModule("Classes/%s")' % c)
    snap = h.run(ARITY_SNAPSHOT)()
    h.apply_inject()
    diff = h.run(ARITY_DIFF)(snap)
    got = {k: tuple(v[i] for i in range(1, 5)) for k, v in diff.items()}
    for must in ARITY_MUST:
        check("%s 參數數:%s 有被包裝" % (fork, must), must in got, repr(sorted(got)))
    for key in sorted(got):
        np0, va0, np1, va1 = got[key]
        check("%s 參數數:%s 包裝後 nparams/isvararg 不變" % (fork, key),
              (np0, va0) == (np1, va1),
              "原 %s%s → 包裝後 %s%s" % (np0, "+..." if va0 else "", np1, "+..." if va1 else ""))


# 建構子裡才包的 tooltipFunc / selFunc(實例欄位):用合成 ItemsTab。
ITEMSTAB_CTOR_BODY = """
	self.controls = {
		displayItemAffix1 = { tooltipFunc = function(tooltip, mode, index, value) end },
		displayItemAffixRange1 = { tooltipFunc = function(tooltip, val) end },
		displayItemRune1 = { selFunc = function(index, value) end },
	}
	self._orig = {
		affix = self.controls.displayItemAffix1.tooltipFunc,
		range = self.controls.displayItemAffixRange1.tooltipFunc,
		rune = self.controls.displayItemRune1.selFunc,
	}
"""
ITEMSTAB_CTOR_P1 = ('local C = newClass("ItemsTab")\nfunction C:ItemsTab()\n'
                    + ITEMSTAB_CTOR_BODY + '\treturn self\nend\n')
ITEMSTAB_CTOR_P2 = ('local C = newClass("ItemsTab", function(self)\n'
                    + ITEMSTAB_CTOR_BODY + 'end)\n')


def test_wrapped_arity_instance(fork):
    import lupa.luajit21 as luajit21
    h = Harness(fork, with_inject=False, lua_mod=luajit21)
    h.define("ItemsTab", ITEMSTAB_CTOR_P1, ITEMSTAB_CTOR_P2)
    h.apply_inject()
    tab = make(h, "ItemsTab")
    rows = h.eval("""function(t)
		local function sh(fn) local i = debug.getinfo(fn, "u") return i.nparams .. (i.isvararg and "+" or "") end
		local c = t.controls
		return {
			{ "displayItemAffix1.tooltipFunc", c.displayItemAffix1.tooltipFunc ~= t._orig.affix, sh(t._orig.affix), sh(c.displayItemAffix1.tooltipFunc) },
			{ "displayItemAffixRange1.tooltipFunc", c.displayItemAffixRange1.tooltipFunc ~= t._orig.range, sh(t._orig.range), sh(c.displayItemAffixRange1.tooltipFunc) },
			{ "displayItemRune1.selFunc", c.displayItemRune1.selFunc ~= t._orig.rune, sh(t._orig.rune), sh(c.displayItemRune1.selFunc) },
		}
	end""")(tab)
    for i in range(1, 4):
        name, wrapped, before, after = (rows[i][j] for j in range(1, 5))
        check("%s 參數數:ItemsTab %s 有被包裝" % (fork, name), wrapped)
        check("%s 參數數:ItemsTab %s 包裝後 nparams/isvararg 不變" % (fork, name),
              before == after, "原 %s → 包裝後 %s" % (before, after))


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


UPSTREAM_SHAPE = {
    "Classes/GemSelectControl.lua": [
        (r'"\^ %a\':-"', "gem box still ships an ASCII-only input filter"),
        (r"function GemSelectClass:BuildList", "BuildList"),
        (r"function GemSelectClass:FilterSupport", "FilterSupport"),
        (r"function GemSelectClass:SortGemList", "SortGemList"),
    ],
    "Classes/ItemDBControl.lua": [
        (r"function ItemDBClass:DoesItemMatchFilters", "DoesItemMatchFilters"),
        (r"self\.controls\.searchMode", "searchMode control"),
        (r"return colorCodes\[item\.rarity\] \.\. item\.name", "GetRowValue row text"),
    ],
    "Classes/MinionSearchListControl.lua": [
        (r"function MinionSearchListClass:DoesEntryMatchFilters", "DoesEntryMatchFilters"),
        (r"self\.controls\.searchText", "searchText control"),
    ],
    "Classes/PassiveTreeView.lua": [
        (r"function PassiveTreeViewClass:AddNodeName\(tooltip, node, build", "AddNodeName"),
        (r"function PassiveTreeViewClass:AddNodeTooltip\(tooltip, node, build", "AddNodeTooltip"),
    ],
    "Classes/ItemsTab.lua": [
        (r"function ItemsTabClass:AddItemTooltip\(tooltip, item", "AddItemTooltip"),
        (r"rarityCode\.\.item\.namePrefix\.\.item\.baseName", "prefix..base..suffix title line"),
    ],
    "Classes/CalcsTab.lua": [
        (r"function CalcsTabClass:SearchMatch", "SearchMatch"),
    ],
    "Classes/NotableDBControl.lua": [
        (r"function NotableDBClass:DoesNotableMatchFilters", "DoesNotableMatchFilters"),
    ],
    "Classes/EditControl.lua": [
        (r"function EditClass:OnKeyDown", "OnKeyDown"),
        (r'text:gsub\("\[\\128-\\255\]",\s*"\?"\)', "the '?' line the paste patch drops"),
    ],
}


def test_upstream_shape(fork):
    base = FORKS[fork]
    for rel, checks in UPSTREAM_SHAPE.items():
        path = os.path.join(base, rel.replace("/", os.sep))
        if not os.path.exists(path):
            check("%s shape: %s exists" % (fork, rel), False, "missing")
            continue
        src = read_text(path)
        for pattern, what in checks:
            check("%s shape: %s -- %s" % (fork, rel, what),
                  re.search(pattern, src) is not None)


# ---- C++ source-patch anchors ---------------------------------------------

def parse_source_patches(cpp):
    """Pull (what, file, anchor, insert_after, replace_with, one_fork_only) out of
    kPobToolsSourcePatches[].

    replace_with is None for insert-mode patches. Adjacent C string literals are
    concatenated by the compiler, so the payload is whatever literals follow the
    first three.

    Replace mode is decided by WHERE nullptr sits, not by whether the entry
    contains the word: since one-fork-only entries end in `nullptr, true`, a
    substring test would call every one of them a replace patch and then compare
    the wrong field against the shipped POB source.
    """
    body = cpp.split("kPobToolsSourcePatches[] = {", 1)[1]
    body = body.split("\n};", 1)[0]
    body = re.sub(r"//[^\n]*", "", body)               # comments hold example code
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)  # /*oneForkOnly=*/ markers
    entries = []
    depth = 0
    cur = ""
    for ch in body:
        if ch == "{":
            depth += 1
            if depth == 1:
                cur = ""
                continue
        elif ch == "}":
            depth -= 1
            if depth == 0:
                entries.append(cur)
                continue
        if depth >= 1:
            cur += ch
    out = []
    for e in entries:
        lits = list(re.finditer(r'"((?:[^"\\]|\\.)*)"', e))
        if len(lits) < 3:
            continue
        fields = [unescape_c(m.group(1)) for m in lits]
        what, leaf, anchor = fields[0], fields[1], fields[2]
        payload = "".join(fields[3:])
        # Everything between the anchor literal and the next one. `nullptr` there
        # means the insertAfter slot was skipped, i.e. this is a replace patch.
        gap = e[lits[2].end():lits[3].start()] if len(lits) > 3 else e[lits[2].end():]
        is_replace = "nullptr" in gap
        one_fork = re.search(r"\btrue\b", e[lits[-1].end():]) is not None
        if is_replace:
            out.append((what, leaf, anchor, None, payload, one_fork))
        else:
            out.append((what, leaf, anchor, payload, None, one_fork))
    return out


def apply_source_patches(src, leaf, patches):
    """Mirror of pobtools_loadfile_patched() in ui_api.cpp."""
    applied = 0
    for _what, p_leaf, anchor, insert_after, replace_with, _one_fork in patches:
        if p_leaf != leaf:
            continue
        at = src.find(anchor)
        if at < 0:
            continue                                   # anchor drifted: skipped
        if replace_with is not None:
            applied += src.count(anchor)
            src = src.replace(anchor, replace_with)
        else:
            if src.find(anchor, at + 1) >= 0:
                continue                               # ambiguous: insert mode refuses
            end = at + len(anchor)
            src = src[:end] + insert_after + src[end:]
            applied += 1
    return src, applied


def unescape_c(s):
    return (s.replace("\\\\", "\x00").replace('\\"', '"').replace("\\n", "\n")
             .replace("\\t", "\t").replace("\x00", "\\"))


def test_source_patch_anchors():
    patches = parse_source_patches(read_text(UI_API))
    check("source patches: table parsed", len(patches) >= 3, "n=%d" % len(patches))
    for what, leaf, anchor, insert_after, replace_with, one_fork in patches:
        is_replace = replace_with is not None
        total = 0
        for fork, base in FORKS.items():
            path = None
            for root, _dirs, files in os.walk(base):
                for f in files:
                    if f.lower() == leaf:
                        path = os.path.join(root, f)
                        break
                if path:
                    break
            if not path:
                continue
            n = read_text(path).count(anchor)
            total += n
            if not is_replace:
                # insert mode refuses to apply when the anchor is ambiguous
                check("anchor unique in %s/%s: %r" % (fork, leaf, anchor[:48]), n <= 1,
                      "found %d" % n)
        check("anchor matches somewhere: %s %r" % (leaf, anchor[:48]), total >= 1,
              "found 0 in both forks")
        # oneForkOnly decides whether a miss is reported to the user at runtime
        # (ui_api.cpp logs "[inject] <what> ... anchor drifted"). Flag it wrongly
        # and the log either cries wolf on every launch of one fork, or stays
        # silent about the exact regression it exists to catch -- so the flag is
        # checked against the shipped sources rather than trusted.
        forks_hit = 0
        for fork, base in FORKS.items():
            for root, _dirs, files in os.walk(base):
                if any(f.lower() == leaf for f in files):
                    fp = os.path.join(root, next(f for f in files if f.lower() == leaf))
                    if anchor in read_text(fp):
                        forks_hit += 1
                    break
        if one_fork:
            check("oneForkOnly is true because it really is one fork: %s" % what,
                  forks_hit == 1, "matched in %d fork(s)" % forks_hit)
        else:
            check("a patch not marked oneForkOnly matches both forks: %s" % what,
                  forks_hit == len(FORKS), "matched in %d of %d" % (forks_hit, len(FORKS)))


def test_source_patch_compiles():
    """The patched text must still be valid Lua.

    Anchors are matched blind, so a payload with an unbalanced quote or a stray
    `end` would only surface as a POB that refuses to open a tab. Compiling the
    real file, patched exactly the way the engine patches it, is the cheap way
    to find that here instead.
    """
    patches = parse_source_patches(read_text(UI_API))
    leaves = sorted(set(p[1] for p in patches))   # p[1] is the file leaf
    for fork, base in FORKS.items():
        rt = lua51.LuaRuntime(unpack_returned_tuples=True)
        # always two values: one return value would not arrive as a tuple
        loadstring = rt.eval("function(s, n) local f, e = loadstring(s, n) "
                             "if f then return true, '' end return false, tostring(e) end")
        for leaf in leaves:
            path = None
            for root, _dirs, files in os.walk(base):
                for f in files:
                    if f.lower() == leaf:
                        path = os.path.join(root, f)
                        break
                if path:
                    break
            if not path:
                continue
            with open(path, "rb") as fh:
                raw = fh.read()
            if raw[:3] == b"\xef\xbb\xbf":          # BOM: loadbuffer chokes
                raw = raw[3:]
            src = raw.decode("latin-1")
            patched, applied = apply_source_patches(src, leaf, patches)
            check("%s %s: at least one patch applied" % (fork, leaf), applied >= 1,
                  "applied=%d" % applied)
            # Control first. POB runs on its own Lua build, and the PoE2 fork's
            # ItemsTab.lua uses a `continue` keyword stock Lua 5.1 rejects. That
            # is not something a patch broke, so the comparison is patched-vs-
            # unpatched, never patched-vs-"must compile here".
            base_ok, _ = loadstring(src.encode("latin-1"), "@" + leaf)
            ok, err = loadstring(patched.encode("latin-1"), "@" + leaf)
            if not base_ok:
                check("%s %s: patch does not change compilability" % (fork, leaf),
                      not ok, "unpatched rejected by stock Lua 5.1, patched accepted?")
            else:
                check("%s %s: patched source compiles" % (fork, leaf), ok, str(err))


def test_source_patch_reaches_required_modules():
    """require() 載入的被 patch 檔也要經過 patch 載入器。

    LoadModule 走 pobtools_loadfile_patched,但 Lua 原生 require 不走:上游把
    fuzzyScore 搬進 require("Modules.ConfigModBrowser") 後,錨點就算對得上也
    永遠不會套用。被 require 的檔必須有 ui_api.cpp 註冊的 searcher 接手。
    """
    cpp = read_text(UI_API)
    has_searcher = re.search(r"pobtools_require_searcher", cpp) is not None \
        and re.search(r'"loaders"', cpp) is not None
    leaves = sorted(set(p[1] for p in parse_source_patches(cpp)))
    for leaf in leaves:
        stem = os.path.splitext(leaf)[0]
        req = re.compile(r"require\s*\(?\s*[\"'](?:[\w.]+\.)?%s[\"']" % re.escape(stem),
                         re.IGNORECASE)
        users = []
        for fork, base in FORKS.items():
            for root, _dirs, files in os.walk(base):
                for f in files:
                    if f.lower().endswith(".lua") and req.search(read_text(os.path.join(root, f))):
                        users.append("%s/%s" % (fork, f))
        if users:
            check("require 載入的 %s 也套得到 source patch" % leaf, has_searcher,
                  "被 %s require,但 ui_api.cpp 沒有 require searcher" % ", ".join(users[:3]))


def main():
    missing = [f for f, d in FORKS.items() if not os.path.isdir(d)]
    if missing:
        sys.exit("找不到 POB 安裝(需要已部署的 dist/):%s" % ", ".join(missing))

    for fork in FORKS:
        print("== %s ==" % fork)
        test_gem_input(fork)
        test_gem_search(fork)
        test_itemdb(fork)
        test_minion(fork)
        test_calcs(fork)
        test_paste_regression(fork)
        test_no_patch_failed(fork)
        test_patch_failure_is_logged(fork)
        test_src_passive_name(fork)
        test_src_passive_stat_line(fork)
        test_src_mod_line(fork)
        test_src_unique_title(fork)
        test_src_unique_title_patched(fork)
        test_src_item_other_lines(fork)
        test_src_affix_title(fork)
        test_src_list_row(fork)
        test_src_restored_on_error(fork)
        test_id_passive_name(fork)
        test_id_affix_title(fork)
        test_id_obeys_f2(fork)
        test_id_replaced_node(fork)
        test_stat_mod_line(fork)
        test_stat_passive(fork)
        test_stat_jewel_in_node(fork)
        test_stat_multiline_sd(fork)
        test_stat_multiline_sd_unsupported(fork)
        test_src_anoint_legion_tattoo(fork)
        test_src_legion_tattoo_with_source_patch(fork)
        test_stat_gem_tooltip(fork)
        test_wrapped_arity_gem(fork)
        test_wrapped_arity(fork)
        test_wrapped_arity_instance(fork)
        test_upstream_shape(fork)
    print("== C++ source patches ==")
    test_source_patch_anchors()
    test_source_patch_compiles()
    test_source_patch_reaches_required_modules()

    bad = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed" % (len(RESULTS), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
