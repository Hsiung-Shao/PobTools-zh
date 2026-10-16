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
    "passives": {"Elemental Overload": "元素超載"},
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
		tooltip:AddLine(16, "^7"..line)
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
                 "ItemsTab", "Item")

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
        test_stat_mod_line(fork)
        test_stat_passive(fork)
        test_wrapped_arity(fork)
        test_wrapped_arity_instance(fork)
        test_upstream_shape(fork)
    print("== C++ source patches ==")
    test_source_patch_anchors()
    test_source_patch_compiles()

    bad = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed" % (len(RESULTS), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
