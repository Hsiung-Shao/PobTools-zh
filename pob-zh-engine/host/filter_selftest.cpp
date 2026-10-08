#include "filter_selftest.h"
#include "filter_parser.h"
#include "filter_doc_editor.h"
#include "filter_schema.h"
#include "filter_batch.h"
#include "custom_rules_io.h"
#include "sound_library_service.h"
#include "sound_manager.h"
#include "filter_data.h"
#include "filter_i18n.h"
#include "filter_preview.h"
#include "filter_item_import.h"
#include "item_library.h"
#include "paste_fixtures.h"
#include "clipboard_util.h"
#include "editor_shell.h"
#include "filter_file_watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <fstream>

#include <imgui.h>

namespace {

struct TestReport {
	std::string text;
	int failures = 0;

	void check(bool ok, const char* what, const std::string& detail = std::string())
	{
		text += ok ? "PASS  " : "FAIL  ";
		text += what;
		if (!detail.empty()) { text += "  ("; text += detail; text += ")"; }
		text += "\n";
		if (!ok) failures++;
	}
	void note(const std::string& s) { text += "      " + s + "\n"; }
};

// Synthetic NeverSink-style filter: BOM + CRLF + final newline, decorated
// comments, quoted multi-value lists, trailing comments, an unknown keyword and
// two blocks sharing the same header text (anchor tie-break material).
std::string synthetic_crlf()
{
	std::string s;
	s += "\xEF\xBB\xBF";
	s += "#===============================================\r\n";
	s += "# NeverSink style preamble\r\n";
	s += "\r\n";
	s += "Show # %D4 $type->currency $tier->t1\r\n";
	s += "    BaseType \"Divine Orb\" \"Exalted Orb\" # tail note\r\n";
	s += "    Rarity >= Rare\r\n";
	s += "    SomeFutureKeyword abc 123\r\n";
	s += "    SetFontSize 45\r\n";
	s += "    SetTextColor 255 0 0 255\r\n";
	s += "\r\n";
	s += "Show # dup\r\n";
	s += "    BaseType \"Chaos Orb\"\r\n";
	s += "    SetFontSize 40\r\n";
	s += "\r\n";
	s += "Show # dup\r\n";
	s += "    BaseType \"Vaal Orb\"\r\n";
	s += "    SetFontSize 32\r\n";
	s += "\r\n";
	s += "Hide\r\n";
	s += "    BaseType \"Scroll of Wisdom\"\r\n";
	return s;
}

// LF variant: no BOM, no final newline.
std::string synthetic_lf()
{
	std::string s;
	s += "# lf file\n";
	s += "Show\n";
	s += "    Class \"Divination Cards\"\n";
	s += "    SetFontSize 38";
	return s;
}

} // namespace

int RunFilterSelfTest(const std::wstring& exeDir)
{
	// WIN32-subsystem app: surface printf when started from a console.
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	TestReport rep;

	// ---- T1: parse/serialize round-trip is byte-exact --------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		rep.check(SerializeFilter(f) == src, "T1 round-trip CRLF+BOM byte-exact");
		rep.check(f.hadBom && f.crlf && f.finalNewline, "T1 BOM/CRLF/final-newline detected");
		rep.check(f.blocks.size() == 4, "T1 block count",
		          "got " + std::to_string(f.blocks.size()));
		rep.check(!f.blocks[0].hide && f.blocks[3].hide, "T1 Show/Hide verbs");
		rep.check(f.blocks[0].headerComment == "%D4 $type->currency $tier->t1",
		          "T1 header comment extracted", f.blocks[0].headerComment);
		rep.check(f.blocks[0].idxFontSize >= 0 && f.blocks[0].idxTextColor >= 0,
		          "T1 action cache populated");
		bool unknownKept = false;
		for (const FilterLine& ln : f.lines)
			if (ln.kind == FilterLineKind::Unknown && ln.keyword == "SomeFutureKeyword") unknownKept = true;
		rep.check(unknownKept, "T1 unknown keyword kept as Unknown");

		const std::string lfSrc = synthetic_lf();
		FilterFile lf = ParseFilter(lfSrc);
		rep.check(SerializeFilter(lf) == lfSrc, "T1 round-trip LF no-final-newline byte-exact");
		rep.check(!lf.hadBom && !lf.crlf && !lf.finalNewline, "T1 LF flags detected");
	}

	// ---- T2: negation operators ------------------------------------------
	{
		FilterLine a = ParseFilterLine("    Rarity != Unique");
		rep.check(a.op == "!=" && a.values.size() == 1 && a.values[0].text == "Unique",
		          "T2 '!=' operator split", a.op);
		FilterLine b = ParseFilterLine("    Rarity ! Unique");
		rep.check(b.op == "!" && b.values.size() == 1 && b.values[0].text == "Unique",
		          "T2 '!' operator split", b.op);
		rep.check(FilterSerializeLine(a) == "    Rarity != Unique", "T2 clean line emits raw");
		a.dirty = true;
		rep.check(FilterSerializeLine(a) == "    Rarity != Unique", "T2 dirty rebuild keeps '!='");
		FilterLine c = ParseFilterLine("    ItemLevel >= 84");
		rep.check(c.op == ">=" && c.values[0].text == "84", "T2 '>=' unchanged");
	}

	// ---- T3: document CRUD ------------------------------------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);

		unsigned v0 = doc.structureVersion();
		int qLine = doc.InsertLine(0, "Quality", ">=", { FilterToken{ "20", false } });
		rep.check(qLine >= 0 && f.lines[qLine].kind == FilterLineKind::Condition,
		          "T3 InsertLine condition kind");
		// The new condition must sit after "Rarity >= Rare" and before the
		// block's actions (conditions group together).
		rep.check(f.lines[qLine - 1].keyword == "Rarity", "T3 condition inserted after last condition",
		          "prev=" + f.lines[qLine - 1].keyword);
		rep.check(f.lines[qLine].indent == f.lines[qLine - 1].indent, "T3 indent copied from neighbour");
		rep.check(doc.structureVersion() > v0, "T3 structureVersion bumped");

		int bLine = doc.InsertLine(0, "SetBorderColor", "", {
			FilterToken{ "255", false }, FilterToken{ "0", false }, FilterToken{ "0", false } });
		rep.check(bLine >= 0 && f.lines[bLine - 1].keyword == "SetTextColor",
		          "T3 action inserted after last action", "prev=" + f.lines[bLine - 1].keyword);

		std::string out = SerializeFilter(f);
		FilterFile f2 = ParseFilter(out);
		rep.check(f2.blocks.size() == 4, "T3 reparse block count stable");
		rep.check(f2.blocks[0].idxBorderColor >= 0, "T3 reparse sees new SetBorderColor");
		rep.check(out.find("    BaseType \"Divine Orb\" \"Exalted Orb\" # tail note\r\n") != std::string::npos,
		          "T3 untouched line bytes preserved");
		rep.check(out.find("    Quality >= 20\r\n") != std::string::npos, "T3 new condition serialized");

		size_t nBlocks = f.blocks.size();
		int nb = doc.CreateBlock(-1, true, "my custom");
		rep.check(nb == (int)nBlocks && f.blocks.size() == nBlocks + 1, "T3 CreateBlock appends");
		rep.check(f.lines[f.blocks[nb].headerLineIdx].keyword == "Hide", "T3 CreateBlock verb");

		int dup = doc.DuplicateBlock(0);
		rep.check(dup == 1 && f.blocks.size() == nBlocks + 2, "T3 DuplicateBlock inserts after source");
		rep.check(FilterSerializeLine(f.lines[f.blocks[dup].headerLineIdx]) ==
		          FilterSerializeLine(f.lines[f.blocks[0].headerLineIdx]),
		          "T3 duplicate header identical");

		size_t nLines = f.lines.size();
		doc.RemoveBlock(dup);
		rep.check(f.blocks.size() == nBlocks + 1, "T3 RemoveBlock restores count");
		rep.check(f.lines.size() < nLines, "T3 RemoveBlock erased lines");

		int rmLine = doc.FindLine(0, "Quality");
		size_t before = f.lines.size();
		doc.RemoveLine(rmLine);
		rep.check(f.lines.size() == before - 1 && doc.FindLine(0, "Quality") == -1,
		          "T3 RemoveLine hard-deletes");
	}

	// ---- T4: disable (#!) / restore --------------------------------------
	{
		FilterFile f = ParseFilter(synthetic_crlf());
		FilterDocumentEditor doc;
		doc.Attach(&f);

		int li = doc.FindLine(0, "SetFontSize");
		const std::string origRaw = f.lines[li].raw;
		doc.CommentOutLine(li);
		rep.check(f.lines[li].kind == FilterLineKind::Comment, "T4 disabled line is a comment");
		rep.check(f.lines[li].raw == "    #! SetFontSize 45", "T4 '#!' marker format", f.lines[li].raw);
		rep.check(doc.IsDisabledLine(li), "T4 IsDisabledLine recognises it");
		rep.check(f.blocks[0].idxFontSize == -1, "T4 action cache cleared");
		rep.check(doc.FindLine(0, "SetFontSize") == -1, "T4 FindLine skips disabled line");

		// Survives a save/load cycle.
		FilterFile f2 = ParseFilter(SerializeFilter(f));
		FilterDocumentEditor doc2;
		doc2.Attach(&f2);
		rep.check(doc2.IsDisabledLine(li), "T4 disabled line survives reparse");

		// Ordinary comments never qualify.
		rep.check(!doc.IsDisabledLine(1), "T4 plain comment not a disabled line");

		bool restored = doc.RestoreLine(li);
		rep.check(restored && f.lines[li].raw == origRaw, "T4 restore is byte-idempotent",
		          f.lines[li].raw);
		rep.check(f.blocks[0].idxFontSize == li, "T4 action cache back after restore");
	}

	// ---- T5: selection anchor across structural mutations ----------------
	{
		FilterFile f = ParseFilter(synthetic_crlf());
		FilterDocumentEditor doc;
		doc.Attach(&f);

		// Anchor the SECOND "Show # dup" block (blocks[2], BaseType "Vaal Orb").
		BlockAnchor a = doc.CaptureAnchor(2);
		rep.check(a.valid() && !a.firstCondRaw.empty(), "T5 anchor captured");

		doc.CreateBlockAtLine(0, false, "front insert");   // shifts every line
		int resolved = doc.ResolveAnchor(a);
		bool ok = resolved >= 0 &&
		          doc.FindLine(resolved, "BaseType") >= 0 &&
		          FilterHasValue(f.lines[doc.FindLine(resolved, "BaseType")], "Vaal Orb");
		rep.check(ok, "T5 anchor resolves to same block after front insert",
		          "resolved=" + std::to_string(resolved));

		doc.CommentOutBlock(0);                            // the block we just added
		resolved = doc.ResolveAnchor(a);
		ok = resolved >= 0 && doc.FindLine(resolved, "BaseType") >= 0 &&
		     FilterHasValue(f.lines[doc.FindLine(resolved, "BaseType")], "Vaal Orb");
		rep.check(ok, "T5 anchor survives CommentOutBlock", "resolved=" + std::to_string(resolved));

		// CommentOutBlock must not leave orphan syntax lines behind.
		bool orphanFree = true;
		FilterFile fr = ParseFilter(SerializeFilter(f));
		if (!fr.blocks.empty())
			for (int idx = 0; idx < fr.blocks[0].headerLineIdx; idx++) {
				FilterLineKind k = fr.lines[idx].kind;
				if (k == FilterLineKind::Condition || k == FilterLineKind::Action ||
				    k == FilterLineKind::Unknown)
					orphanFree = false;
			}
		rep.check(orphanFree, "T5 CommentOutBlock leaves no orphan syntax lines");
	}

	// ---- T6: schema integrity --------------------------------------------
	{
		bool kwOk = true, defOk = true, exclOk = true, enumOk = true, grpOk = true;
		std::string bad;
		for (const CardSchema& c : FilterSchemaAll()) {
			// Every keyword (and alias) must be one the parser recognises, and
			// isAction must match the parser's classification.
			bool known = c.isAction ? FilterIsKnownAction(c.keyword) : FilterIsKnownCondition(c.keyword);
			if (!known) { kwOk = false; bad += std::string(c.keyword) + " "; }
			if (c.alias) {
				bool aknown = c.isAction ? FilterIsKnownAction(c.alias) : FilterIsKnownCondition(c.alias);
				if (!aknown) { kwOk = false; bad += std::string(c.alias) + " "; }
			}
			// The default line must parse back to the same keyword and to the
			// expected kind (a bad default would insert broken syntax).
			FilterLine dl = ParseFilterLine(c.defaultLine);
			bool kindOk = c.isAction ? (dl.kind == FilterLineKind::Action)
			                         : (dl.kind == FilterLineKind::Condition);
			if (dl.keyword != c.keyword || !kindOk) { defOk = false; bad += std::string(c.defaultLine) + " "; }
			if (c.exclusiveGroup > 0 && !c.isAction) exclOk = false;
			for (const SchemaEnumValue& e : c.enums)
				if (!e.token || !e.token[0] || !e.zh || !e.zh[0]) enumOk = false;
			bool grpFound = false;
			for (const char* g : FilterSchemaGroups()) if (g == c.group) grpFound = true;
			if (!grpFound) grpOk = false;
		}
		rep.check(kwOk, "T6 schema keywords known to parser", bad);
		rep.check(defOk, "T6 default lines parse to same keyword+kind", bad);
		rep.check(exclOk, "T6 exclusive groups are actions only");
		rep.check(enumOk, "T6 enum tokens/labels non-empty");
		rep.check(grpOk, "T6 every card belongs to a listed group");
		rep.check(FilterSchemaFind("PlayAlertSoundPositional") != nullptr &&
		          FilterSchemaFind("CustomAlertSoundOptional") != nullptr,
		          "T6 aliases resolve to cards");
		rep.note("schema cards=" + std::to_string(FilterSchemaAll().size()));
	}

	// ---- T7: batch apply --------------------------------------------------
	{
		EditorShell es;   // default-constructed shell: doc + model only, no services
		es.model = ParseFilter(synthetic_crlf());
		es.doc.Attach(&es.model);
		es.loaded = true;

		BatchStyleOp op;
		op.showHide = BatchStyleOp::Tri::Set;   op.hide = true;
		op.fontSize = BatchStyleOp::Tri::Set;   op.size = 38;
		op.borderColor = BatchStyleOp::Tri::Remove;
		op.textColor = BatchStyleOp::Tri::Set;
		op.text[0] = 10; op.text[1] = 20; op.text[2] = 30; op.text[3] = 255;

		int touched = ApplyBatchStyle(es, { 0, 1, 2 }, op);
		rep.check(touched > 0, "T7 batch touched lines", std::to_string(touched));

		FilterFile r = ParseFilter(SerializeFilter(es.model));
		bool allHidden = r.blocks.size() >= 3 && r.blocks[0].hide && r.blocks[1].hide && r.blocks[2].hide;
		rep.check(allHidden, "T7 batch Show->Hide applied");
		bool fontOk = true, textOk = true;
		for (int i = 0; i < 3; i++) {
			const FilterBlock& b = r.blocks[i];
			fontOk = fontOk && b.idxFontSize >= 0 && FilterValueInt(r.lines[b.idxFontSize], 0, -1) == 38;
			int rr, gg, bb, aa; bool ha;
			bool has = b.idxTextColor >= 0;
			if (has) FilterGetColor(r.lines[b.idxTextColor], rr, gg, bb, aa, ha);
			textOk = textOk && has && rr == 10 && gg == 20 && bb == 30;
		}
		rep.check(fontOk, "T7 batch SetFontSize insert-or-update");
		rep.check(textOk, "T7 batch SetTextColor set");
		bool borderGone = true;
		for (int i = 0; i < 3; i++) borderGone = borderGone && r.blocks[i].idxBorderColor < 0;
		rep.check(borderGone, "T7 batch border colour disabled");
		// Block 3 (Hide "Scroll of Wisdom") was not selected: untouched bytes.
		rep.check(SerializeFilter(r).find("    BaseType \"Scroll of Wisdom\"\r\n") != std::string::npos,
		          "T7 unselected block untouched");
	}

	// ---- T8: custom zone + export/import ---------------------------------
	{
		FilterFile f = ParseFilter(synthetic_crlf());
		FilterDocumentEditor doc;
		doc.Attach(&f);

		CustomZone z0 = FindCustomZone(f);
		rep.check(!z0.present(), "T8 no zone in fresh file");

		CustomZone z = EnsureCustomZone(doc);
		rep.check(z.present(), "T8 zone created");
		bool beforeFirst = !f.blocks.empty() && z.endLine < f.blocks[0].headerLineIdx;
		rep.check(beforeFirst, "T8 zone sits before first block header");

		// Export blocks 1+2 from a second file and import them.
		FilterFile src = ParseFilter(synthetic_crlf());
		std::string frag = ExportCustomRules(src, { 1, 2 }, "test");
		rep.check(frag.find("#pobtools-rules") == 0, "T8 export has metadata header");

		std::string err;
		int n = ImportCustomRules(doc, frag, &err);
		rep.check(n == 2, "T8 import adds 2 blocks", err + " n=" + std::to_string(n));
		int again = ImportCustomRules(doc, frag, &err);
		rep.check(again == 0, "T8 re-import skips duplicates", std::to_string(again));

		// The imported blocks live inside the zone.
		CustomZone z2 = FindCustomZone(f);
		int inZone = 0;
		for (const FilterBlock& b : f.blocks)
			if (b.headerLineIdx > z2.beginLine && b.headerLineIdx < z2.endLine) inZone++;
		rep.check(inZone == 2, "T8 imported blocks inside zone", std::to_string(inZone));

		// Round-trips cleanly and the zone survives a reparse.
		FilterFile r = ParseFilter(SerializeFilter(f));
		rep.check(FindCustomZone(r).present(), "T8 zone survives reparse");

		// Damaged zone repair: drop the end sentinel, re-ensure.
		CustomZone z3 = FindCustomZone(r);
		FilterDocumentEditor doc2;
		doc2.Attach(&r);
		doc2.RemoveLine(z3.endLine);
		rep.check(!FindCustomZone(r).present(), "T8 end sentinel removed");
		CustomZone z4 = EnsureCustomZone(doc2);
		rep.check(z4.present(), "T8 damaged zone repaired");
	}

	// ---- T9: rename + reference sync (real files under %TEMP%) -----------
	{
		auto writeFile = [](const std::wstring& p, const std::string& content) {
			std::ofstream o(p, std::ios::binary);
			o.write(content.data(), (std::streamsize)content.size());
		};
		auto readFile = [](const std::wstring& p) {
			std::ifstream in(p, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		};
		auto exists = [](const std::wstring& p) {
			DWORD a = GetFileAttributesW(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		};

		wchar_t tmpBuf[MAX_PATH];
		GetTempPathW(MAX_PATH, tmpBuf);
		std::wstring dir = std::wstring(tmpBuf) + L"pobtools_filter_selftest";
		CreateDirectoryW(dir.c_str(), nullptr);
		for (const wchar_t* n : { L"AAA.mp3", L"BBB.mp3", L"CCC.mp3", L"BBB (2).mp3", L"DDD.mp3" })
			DeleteFileW((dir + L"\\" + n).c_str());
		writeFile(dir + L"\\AAA.mp3", "aaa");
		writeFile(dir + L"\\BBB.mp3", "bbb");

		// Keep the user's persisted sound folder safe.
		std::wstring savedFolder = GetSoundFolder();
		struct FolderGuard {
			std::wstring saved;
			~FolderGuard() { SetSoundFolder(saved); }
		} guard{ savedFolder };

		SoundLibraryService svc;
		svc.Init(exeDir);
		svc.SetFolder(dir);
		rep.check(svc.files().size() == 2, "T9 scan finds 2 files",
		          std::to_string(svc.files().size()));

		std::string filterSrc =
			"Show\r\n"
			"    BaseType \"Divine Orb\"\r\n"
			"    CustomAlertSound \"AAA.mp3\" 300\r\n"
			"\r\n"
			"Show\r\n"
			"    BaseType \"Chaos Orb\"\r\n"
			"    CustomAlertSoundOptional \"sub\\AAA.mp3\"\r\n";
		FilterFile f = ParseFilter(filterSrc);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		rep.check(FindCustomSoundRefs(f, L"AAA.mp3").size() == 2, "T9 refs found (plain + subdir)");
		rep.check(FindCustomSoundRefs(f, L"aaa.MP3").size() == 2, "T9 ref match is case-insensitive");

		// Plain rename with reference sync.
		RenamePlanEntry e = svc.BuildSingleRename(L"AAA.mp3", L"CCC.mp3", &f);
		rep.check(e.state == RenamePlanEntry::State::Rename && e.refLines.size() == 2,
		          "T9 single-rename plan");
		std::vector<RenamePlanEntry> plan{ e };
		SoundLibraryService::ApplyResult r = svc.ApplyRenamePlan(plan, &doc);
		rep.check(r.renamed == 1 && r.refsUpdated == 2 && r.err.empty(), "T9 rename applied + refs synced",
		          r.err);
		rep.check(!exists(dir + L"\\AAA.mp3") && exists(dir + L"\\CCC.mp3"), "T9 disk state after rename");
		rep.check(f.dirty, "T9 model marked dirty (not saved)");
		std::string out = SerializeFilter(f);
		rep.check(out.find("CustomAlertSound \"CCC.mp3\" 300") != std::string::npos &&
		          out.find("CustomAlertSoundOptional \"sub\\CCC.mp3\"") != std::string::npos,
		          "T9 both refs rewritten, path prefix kept");

		// Conflict refused while unresolved.
		RenamePlanEntry c = svc.BuildSingleRename(L"CCC.mp3", L"BBB.mp3", nullptr);
		rep.check(c.state == RenamePlanEntry::State::Conflict, "T9 conflict detected");
		std::vector<RenamePlanEntry> plan2{ c };
		r = svc.ApplyRenamePlan(plan2, nullptr);
		rep.check(!r.err.empty() && exists(dir + L"\\CCC.mp3"), "T9 unresolved conflict refused");

		// Suffix resolution.
		plan2[0].resolution = RenamePlanEntry::Resolution::Suffix;
		r = svc.ApplyRenamePlan(plan2, nullptr);
		rep.check(r.renamed == 1 && exists(dir + L"\\BBB (2).mp3") && !exists(dir + L"\\CCC.mp3"),
		          "T9 suffix resolution", r.err);

		// Swap resolution: contents exchange, names stay.
		writeFile(dir + L"\\DDD.mp3", "ddd");
		svc.Rescan();
		RenamePlanEntry sw = svc.BuildSingleRename(L"BBB (2).mp3", L"DDD.mp3", nullptr);
		rep.check(sw.state == RenamePlanEntry::State::Conflict, "T9 swap starts as conflict");
		std::vector<RenamePlanEntry> plan3{ sw };
		plan3[0].resolution = RenamePlanEntry::Resolution::Swap;
		r = svc.ApplyRenamePlan(plan3, nullptr);
		rep.check(r.swapped == 1 && readFile(dir + L"\\DDD.mp3") == "aaa" &&
		          readFile(dir + L"\\BBB (2).mp3") == "ddd",
		          "T9 swap exchanges contents", r.err);

		// Rule-based batch plan over the remaining files (BBB.mp3, BBB (2).mp3,
		// DDD.mp3): two matches, {n} numbers them without colliding.
		svc.rules().clear();
		svc.rules().push_back(NamingRule{ "test", "BBB", "alert{n}.{ext}", true });
		std::vector<RenamePlanEntry> bp = svc.BuildRenamePlan(nullptr);
		rep.check(bp.size() == 2 &&
		          bp[0].newName == L"alert1.mp3" && bp[1].newName == L"alert2.mp3" &&
		          bp[0].state == RenamePlanEntry::State::Rename &&
		          bp[1].state == RenamePlanEntry::State::Rename,
		          "T9 batch plan expands {n}/{ext} without collisions",
		          "n=" + std::to_string(bp.size()));

		for (const wchar_t* n : { L"AAA.mp3", L"BBB.mp3", L"CCC.mp3", L"BBB (2).mp3", L"DDD.mp3" })
			DeleteFileW((dir + L"\\" + n).c_str());
		RemoveDirectoryW(dir.c_str());
	}

	// ---- T10: naming-rules json round-trip -------------------------------
	{
		std::vector<NamingRule> rules = {
			{ u8"神聖石音效", "divine", "divine{n}.{ext}", true },
			{ "manual-only", "", "boss.mp3", false },
		};
		std::string j = SoundRulesToJson(rules);
		std::vector<NamingRule> back;
		bool ok = SoundRulesFromJson(j, &back);
		rep.check(ok && back.size() == 2 && back[0].name == rules[0].name &&
		          back[0].match == "divine" && back[1].enabled == false,
		          "T10 rules json round-trip");
		std::vector<NamingRule> bad;
		rep.check(!SoundRulesFromJson("{not json!!", &bad) && bad.empty(),
		          "T10 bad json rejected without crash");
		rep.check(!SoundRulesFromJson("[]", &bad), "T10 wrong shape rejected");
	}

	// ---- T10b: 替換引用 (ReplaceSoundRefs) --------------------------------
	{
		std::string src =
			"Show\r\n\tBaseType \"A\"\r\n\tCustomAlertSound \"old.mp3\" 250\r\n"
			"Show\r\n\tBaseType \"B\"\r\n\tCustomAlertSound \"sub/old.mp3\" 100\r\n"
			"Show\r\n\tBaseType \"C\"\r\n\tCustomAlertSound \"other.mp3\" 300\r\n";
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		int n = ReplaceSoundRefs(&doc, L"old.mp3", L"new.mp3");
		rep.check(n == 2 && f.dirty, "T10b replace-refs rewrites both refs", std::to_string(n));
		std::string out = SerializeFilter(f);
		rep.check(out.find("CustomAlertSound \"new.mp3\" 250") != std::string::npos &&
		          out.find("CustomAlertSound \"sub/new.mp3\" 100") != std::string::npos &&
		          out.find("\"other.mp3\" 300") != std::string::npos,
		          "T10b volume/prefix kept, other refs untouched");
		rep.check(ReplaceSoundRefs(&doc, L"new.mp3", L"new.mp3") == 0,
		          "T10b same-name replace is a no-op");
	}

	// ---- T11: big-file health (the bundled NeverSink fixture) --------------
	// Reads Filters\default.filter next to the exe -- never the user's own
	// filters in Documents (a selftest does not touch the user's game files).
	{
		std::vector<FilterListEntry> found;
		{
			FilterListEntry e;
			e.path = exeDir + L"Filters\\default.filter";
			e.name = "Filters\\default.filter";
			if (GetFileAttributesW(e.path.c_str()) != INVALID_FILE_ATTRIBUTES) found.push_back(e);
		}
		if (found.empty()) {
			rep.note("T11 skipped: no Filters\\default.filter next to the exe");
		} else {
			bool ok = false;
			FilterFile f = LoadFilter(found.front().path, &ok);
			if (!ok) {
				rep.note("T11 skipped: could not read " + found.front().name);
			} else {
				std::ifstream in(found.front().path, std::ios::binary);
				std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				rep.check(SerializeFilter(f) == raw,
				          "T11 real filter round-trip byte-exact", found.front().name);
				rep.note("T11 " + found.front().name + ": " + std::to_string(f.blocks.size()) +
				         " blocks, " + std::to_string(f.lines.size()) + " lines");
			}
		}
	}

	// ---- T12: NeverSink header zh display ---------------------------------
	{
		rep.check(NeverSinkHeaderZh("%H2 $type->leveling->flasks->life $tier->t2") ==
		          u8"%H2 【練等·藥劑·生命】T2",
		          "T12 type+tier translated",
		          NeverSinkHeaderZh("%H2 $type->leveling->flasks->life $tier->t2"));
		rep.check(NeverSinkHeaderZh("$type->unknownseg->currency $tier->boots_life_based") ==
		          u8"【unknownseg·通貨】boots_life_based",
		          "T12 unknown segments stay English");
		rep.check(NeverSinkHeaderZh(u8"純註解標題 (no markers)") == u8"純註解標題 (no markers)",
		          "T12 non-NeverSink header untouched");
		rep.check(NeverSinkHeaderZh("$type->anyremaining $tier->restex") ==
		          u8"【其餘所有】其餘",
		          "T12 tier keyword translated");
	}

	// ---- T13: drop-preview evaluator --------------------------------------
	{
		const char* src =
			"Show\r\n"
			"\tBaseType == \"Divine Orb\"\r\n"
			"\tSetTextColor 71 255 0 255\r\n"
			"\tSetFontSize 45\r\n"
			"\tCustomAlertSound \"6veryvaluable.mp3\" 220\r\n"
			"\r\n"
			"Show\r\n"
			"\tHasExplicitMod \"Veiled\"\r\n"
			"\tSetTextColor 9 9 9\r\n"
			"\r\n"
			"Show\r\n"
			"\tContinue\r\n"
			"\tClass \"Currency\"\r\n"
			"\tPlayEffect Purple\r\n"
			"\r\n"
			"Show\r\n"
			"\tClass \"Currency\"\r\n"
			"\tSetTextColor 1 2 3\r\n"
			"\r\n"
			"Hide\r\n"
			"\tBaseType \"Scroll\"\r\n";
		FilterFile f = ParseFilter(src);
		FilterI18n emptyI18n;   // ClassNameEn falls back to the input

		PreviewItem divine; divine.baseType = "Divine Orb"; divine.classId = "Stackable Currency";
		PreviewResult r = EvaluatePreview(f, divine, emptyI18n);
		rep.check(r.matched && !r.hidden && r.text[0] == 71 && r.text[1] == 255 && r.fontSize == 45,
		          "T13 first match wins + style override");
		rep.check(r.customSound == "6veryvaluable.mp3" && r.customVol == 220,
		          "T13 custom sound + volume captured");

		PreviewItem chaos; chaos.baseType = "Chaos Orb"; chaos.classId = "Stackable Currency";
		PreviewResult rc = EvaluatePreview(f, chaos, emptyI18n);
		rep.check(rc.matched && rc.text[0] == 1 && rc.text[1] == 2 && rc.text[2] == 3,
		          "T13 substring Class match decides");
		rep.check(rc.playEffect == "Purple", "T13 Continue block styles then keeps going");
		bool sawUnknown = false;
		for (const std::string& k : rc.unknownConds) if (k == "HasExplicitMod") sawUnknown = true;
		rep.check(sawUnknown, "T13 unmodelled condition reported + treated false");

		PreviewItem wis; wis.baseType = "Scroll of Wisdom"; wis.classId = "Quest Items";
		PreviewResult rw = EvaluatePreview(f, wis, emptyI18n);
		rep.check(rw.matched && rw.hidden, "T13 Hide block hides");

		PreviewItem belt; belt.baseType = "Leather Belt"; belt.classId = "Belts"; belt.rarity = 2;
		PreviewResult ru = EvaluatePreview(f, belt, emptyI18n);
		rep.check(!ru.matched && !ru.hidden && ru.text[0] == 255 && ru.text[1] == 255 && ru.text[2] == 119,
		          "T13 no match -> rarity default style");

		PreviewItem synth;
		bool okS = SynthesizePreviewItem(f, 0, emptyI18n, {}, &synth);
		rep.check(okS && synth.baseType == "Divine Orb",
		          "T13 synthesize from exact BaseType block");
		PreviewResult rs = EvaluatePreview(f, synth, emptyI18n);
		rep.check(rs.matched && rs.blockIdx == 0, "T13 synthesized item lands on its block");
		rep.check(!SynthesizePreviewItem(f, 1, emptyI18n, {}, &synth),
		          "T13 synthesize refuses unmodelled block");
	}

	// ---- T14: item-import data layer (base metadata + zh parse tables) ----
	// Needs Data\ next to the exe (run from dist\, like the atlas selftest).
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		rep.check(i18n.loaded(), "T14 i18n loaded (run from dist\\ so Data\\ exists)");

		rep.check(i18n.DropLevelOf("Thicket Bow") == 56, "T14 Thicket Bow drop level 56",
		          std::to_string(i18n.DropLevelOf("Thicket Bow")));
		int w = 0, h = 0;
		rep.check(i18n.SizeOf("Thicket Bow", &w, &h) && w == 2 && h == 3,
		          "T14 Thicket Bow inventory 2x3");
		rep.check(i18n.DropLevelOf("Headhunter") == -1,
		          "T14 unique without drop data -> -1 (caller keeps default)");
		rep.check(!i18n.SizeOf("Abyss Scarab of Crystals", &w, &h),
		          "T14 GGPK-patched entry without size -> false (caller keeps default)");
		rep.check(i18n.DropLevelOf("No Such Item") == -1 && !i18n.SizeOf("No Such Item", &w, &h),
		          "T14 unknown item -> unknown metadata");

		const FilterI18n::ZhTables& z = i18n.Zh();
		rep.check(!z.header.empty() && !z.rarity.empty() && !z.status.empty() &&
		          !z.influence.empty() && !z.itemClass.empty(),
		          "T14 all five zh tables non-empty");
		auto eq = [](const std::unordered_map<std::string, std::string>& m,
		             const char* k, const char* v) {
			auto it = m.find(k);
			return it != m.end() && it->second == v;
		};
		rep.check(eq(z.header, u8"物品種類", "Item Class"), "T14 header table zh->en");
		rep.check(eq(z.rarity, u8"稀有", "Rare"), "T14 rarity table zh->en");
		rep.check(eq(z.status, u8"已汙染", "Corrupted"), "T14 status table zh->en");
		rep.check(eq(z.influence, u8"塑者之物", "Shaper Item"), "T14 influence table zh->en");
		rep.check(eq(z.header, u8"地圖階級", "Map Tier"), "T14 Map Tier supplement");
		rep.check(eq(z.header, u8"堆疊數量", "Stack Size"), "T14 Stack Size supplement");
		rep.check(eq(z.itemClass, u8"深淵珠寶", "Abyss Jewels"), "T14 Abyss Jewels supplement");
	}

	// ---- T15: pasted-item parser — real zh fixtures -----------------------
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		ItemLibrary libLoader;
		libLoader.Load(exeDir, i18n);
		const std::vector<LibItem>& lib = libLoader.items();
		rep.check(!lib.empty(), "T15 item library loaded");

		ImportedItem b = ParseGameItemText(kFxBoots, i18n, lib);
		rep.check(b.ok, "T15 boots parsed");
		rep.check(b.item.rarity == 2 && b.item.itemLevel == 84 && b.item.quality == 29,
		          "T15 boots rarity/ilvl/quality");
		rep.check(b.baseEn == "Paladin Boots" && b.item.classId == "Boots",
		          "T15 boots base + class", b.baseEn + "/" + b.item.classId);
		rep.check(b.item.sockets == 4 && b.item.linkedSockets == 2 &&
		          b.item.socketGroups.size() == 3 && b.item.socketGroups[1] == "GW",
		          "T15 boots sockets R G-W R -> {R,GW,R}");
		rep.check(b.item.influence == 0 && b.exarch && b.eater,
		          "T15 boots exarch/eater flagged, no HasInfluence bits");
		rep.check(b.item.dropLevel == 84 && b.item.width == 2 && b.item.height == 2,
		          "T15 boots drop level + size from item_meta");
		rep.check(b.item.identified && !b.item.corrupted, "T15 boots identified, not corrupted");
		bool warnedExarch = false;
		for (const ImportIssue& w : b.warnings)
			if (w.msg.find("HasSearingExarchImplicit") != std::string::npos) warnedExarch = true;
		rep.check(warnedExarch, "T15 exarch mark surfaced as warning");

		ImportedItem fl = ParseGameItemText(kFxFlask, i18n, lib);
		rep.check(fl.ok && fl.item.rarity == 3 && fl.baseEn == "Silver Flask" &&
		          fl.item.enchanted && fl.item.quality == 20 && fl.item.itemLevel == 85,
		          "T15 unique flask + enchant line", fl.baseEn);

		ImportedItem jw = ParseGameItemText(kFxJewel, i18n, lib);
		rep.check(jw.ok && jw.item.fractured && jw.baseEn == "Crimson Jewel" &&
		          jw.item.itemLevel == 69 && jw.item.sockets == 0,
		          "T15 fractured jewel marker in its own section", jw.baseEn);

		ImportedItem vb = ParseGameItemText(kFxVestigialBoots, i18n, lib);
		rep.check(vb.ok && vb.baseEn == "Stealth Boots" && vb.item.rarity == 3,
		          "T15 vestigial prefix stripped to real base", vb.baseEn);
		rep.check(vb.item.sockets == 3 && vb.item.linkedSockets == 3 &&
		          vb.item.socketGroups.size() == 1 && vb.item.socketGroups[0] == "WWW",
		          "T15 sockets W-W-W with trailing space tolerated");
	}

	// ---- T16: pasted-item parser — en client text -------------------------
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		ItemLibrary libLoader;
		libLoader.Load(exeDir, i18n);
		const std::vector<LibItem>& lib = libLoader.items();

		const char* enRare =
			"Item Class: Body Armours\n"
			"Rarity: Rare\n"
			"Corpse Shell\n"
			"Vaal Regalia\n"
			"--------\n"
			"Quality: +28% (augmented)\n"
			"Energy Shield: 505 (augmented)\n"
			"--------\n"
			"Requirements:\n"
			"Level: 68\n"
			"Int: 194\n"
			"--------\n"
			"Sockets: R-G-B W-W G\n"
			"--------\n"
			"Item Level: 86\n"
			"--------\n"
			"+1 to Level of Socketed Gems\n"
			"--------\n"
			"Corrupted\n";
		ImportedItem e = ParseGameItemText(enRare, i18n, lib);
		rep.check(e.ok && e.baseEn == "Vaal Regalia" && e.item.rarity == 2 &&
		          e.item.itemLevel == 86 && e.item.quality == 28 && e.item.corrupted,
		          "T16 en rare parsed", e.baseEn);
		rep.check(e.item.sockets == 6 && e.item.linkedSockets == 3 &&
		          e.item.socketGroups.size() == 3 && e.item.socketGroups[0] == "RGB",
		          "T16 en sockets R-G-B W-W G");
		rep.check(e.item.gemLevel == 1, "T16 requirements Level is NOT the gem level");

		const char* enGem =
			"Item Class: Skill Gems\n"
			"Rarity: Gem\n"
			"Superior Spectral Shield Throw\n"
			"--------\n"
			"Attack, Projectile, Physical\n"
			"Level: 20 (Max)\n"
			"Quality: +20% (augmented)\n"
			"--------\n"
			"Requirements:\n"
			"Level: 70\n"
			"Dex: 155\n"
			"--------\n"
			"Deals weapon damage as projectiles\n";
		ImportedItem g = ParseGameItemText(enGem, i18n, lib);
		rep.check(g.ok && g.baseEn == "Spectral Shield Throw" && g.item.gemLevel == 20 &&
		          g.item.quality == 20 && g.item.rarity == 0,
		          "T16 superior gem: level from its own section", g.baseEn);
		bool noIlvl = false;
		for (const ImportIssue& w : g.warnings)
			if (w.msg.find(u8"物品等級") != std::string::npos) noIlvl = true;
		rep.check(noIlvl, "T16 missing Item Level warned (gems have none)");

		const char* enCurrency =
			"Item Class: Stackable Currency\n"
			"Rarity: Currency\n"
			"Chaos Orb\n"
			"--------\n"
			"Stack Size: 5/20\n"
			"--------\n"
			"Reforges a rare item with new random modifiers\n";
		ImportedItem c = ParseGameItemText(enCurrency, i18n, lib);
		rep.check(c.ok && c.baseEn == "Chaos Orb" && c.item.stackSize == 5 &&
		          c.item.rarity == 0, "T16 currency Stack Size 5/20 -> 5", c.baseEn);

		const char* enMap =
			"Item Class: Maps\n"
			"Rarity: Normal\n"
			"Blighted Cage Map\n"
			"--------\n"
			"Map Tier: 14 (augmented)\n"
			"--------\n"
			"Item Level: 79\n";
		ImportedItem m = ParseGameItemText(enMap, i18n, lib);
		rep.check(m.ok && m.baseEn == "Cage Map" && m.item.blightedMap &&
		          m.item.mapTier == 14, "T16 Blighted prefix + Map Tier", m.baseEn);

		const char* enReplica =
			"Item Class: Belts\n"
			"Rarity: Unique\n"
			"Replica Headhunter\n"
			"Leather Belt\n"
			"--------\n"
			"Requirements:\n"
			"Level: 40\n"
			"--------\n"
			"Item Level: 83\n";
		ImportedItem rp = ParseGameItemText(enReplica, i18n, lib);
		rep.check(rp.ok && rp.baseEn == "Leather Belt" && rp.item.replica &&
		          rp.item.rarity == 3, "T16 Replica flag from the name line", rp.baseEn);

		const char* enMagic =
			"Item Class: Belts\n"
			"Rarity: Magic\n"
			"Sharpened Rustic Sash of the Whelpling\n"
			"--------\n"
			"Item Level: 12\n";
		ImportedItem mg = ParseGameItemText(enMagic, i18n, lib);
		rep.check(mg.ok && mg.baseEn == "Rustic Sash" && mg.item.rarity == 1,
		          "T16 magic composed name -> longest base substring", mg.baseEn);
	}

	// ---- T17: evaluator extensions (SocketGroup + influence bitmask) ------
	{
		FilterI18n emptyI18n;
		auto evalOne = [&emptyI18n](const std::string& cond, const PreviewItem& it) {
			FilterFile f = ParseFilter("Show\n\t" + cond + "\n\tSetFontSize 45\n");
			return EvaluatePreview(f, it, emptyI18n);
		};

		PreviewItem it;
		it.baseType = "Cobalt Jewel";
		it.classId = "Jewel";
		it.socketGroups = { "R", "GW", "R" };

		PreviewItem rgb = it;
		rgb.socketGroups = { "BGR", "W" };          // one linked group holding R+G+B

		rep.check(evalOne("SocketGroup \"RGB\"", rgb).matched,
		          "T17 SocketGroup RGB matches linked R+G+B in any order");
		rep.check(!evalOne("SocketGroup \"RGB\"", it).matched,
		          "T17 SocketGroup RGB needs the colours in ONE linked group");
		rep.check(evalOne("SocketGroup ! \"RGB\"", it).matched,
		          "T17 SocketGroup negation");
		PreviewItem five = it;
		five.socketGroups = { "GGGGR" };
		rep.check(evalOne("SocketGroup \"5GGG\"", five).matched,
		          "T17 SocketGroup 5GGG: 5-linked with 3 greens matches");
		rep.check(!evalOne("SocketGroup \"5GGG\"", rgb).matched,
		          "T17 SocketGroup 5GGG: 3-link does not match");
		rep.check(evalOne("SocketGroup == \"RGB\"", rgb).matched &&
		          !evalOne("SocketGroup == \"RG\"", rgb).matched,
		          "T17 SocketGroup == exact multiset");
		PreviewItem bare = it;
		bare.socketGroups.clear();
		rep.check(!evalOne("SocketGroup \"RGB\"", bare).matched,
		          "T17 unknown socket groups never match (plain-drop semantics)");
		rep.check(evalOne("SocketGroup \"RGB\"", bare).unknownConds.empty(),
		          "T17 empty socket groups are modelled, not 'unknown'");

		PreviewItem shaped = it;
		shaped.influence = kInfShaper;
		rep.check(evalOne("HasInfluence Shaper", shaped).matched,
		          "T17 HasInfluence bit match");
		rep.check(!evalOne("HasInfluence Elder", shaped).matched,
		          "T17 HasInfluence wrong bit no match");
		rep.check(evalOne("HasInfluence Elder Shaper", shaped).matched,
		          "T17 HasInfluence any-of");
		rep.check(evalOne("HasInfluence ! Shaper", it).matched &&
		          !evalOne("HasInfluence ! Shaper", shaped).matched,
		          "T17 HasInfluence negation");
		rep.check(evalOne("HasInfluence None", it).matched &&
		          !evalOne("HasInfluence None", shaped).matched,
		          "T17 HasInfluence None == uninfluenced (old behaviour kept)");
		rep.check(evalOne("ShaperItem True", shaped).matched &&
		          !evalOne("ElderItem True", shaped).matched &&
		          evalOne("ElderItem False", shaped).matched,
		          "T17 Shaper/ElderItem read the bitmask");

		PreviewResult ru = evalOne("Sockets >= \"AAAA\"", five);
		bool sawSockets = false;
		for (const std::string& k : ru.unknownConds) if (k == "Sockets") sawSockets = true;
		rep.check(!ru.matched && sawSockets,
		          "T17 Sockets colour syntax stays unmodelled + reported");
	}

	// ---- T18: end-to-end — pasted zh boots through a mini filter ----------
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		ItemLibrary libLoader;
		libLoader.Load(exeDir, i18n);
		ImportedItem b = ParseGameItemText(kFxBoots, i18n, libLoader.items());
		rep.check(b.ok, "T18 boots parsed");

		const char* src =
			"Show\n"
			"\tHasExplicitMod \"Veiled\"\n"
			"\tSetFontSize 40\n"
			"\n"
			"Show\n"
			"\tSocketGroup \"RGB\"\n"
			"\tSetFontSize 41\n"
			"\n"
			"Show\n"
			"\tHasInfluence Shaper Elder\n"
			"\tSetFontSize 42\n"
			"\n"
			"Show\n"
			"\tContinue\n"
			"\tDropLevel >= 65\n"
			"\tPlayEffect Purple\n"
			"\n"
			"Show\n"
			"\tRarity Rare\n"
			"\tItemLevel >= 80\n"
			"\tSetTextColor 1 2 3\n";
		FilterFile f = ParseFilter(src);
		PreviewResult res = EvaluatePreview(f, b.item, i18n);
		rep.check(res.matched && res.blockIdx == 4 && res.text[0] == 1 && res.text[1] == 2,
		          "T18 lands on Rare+ilvl block, skips unmodelled/unmatched ones",
		          std::to_string(res.blockIdx));
		rep.check(res.playEffect == "Purple",
		          "T18 DropLevel 84 passes the Continue block, beam applied");
		bool sawHem = false;
		for (const std::string& k : res.unknownConds) if (k == "HasExplicitMod") sawHem = true;
		rep.check(sawHem, "T18 HasExplicitMod reported as not simulated");
	}

	// ---- T19: baseline snapshot + "modified" ------------------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		const std::string before = SerializeFilter(f);
		doc.CaptureBaseline();
		rep.check(doc.HasBaseline() && SerializeFilter(f) == before && before == src,
		          "T19 capturing the baseline leaves the bytes alone");
		bool allSame = true;
		for (int i = 0; i < (int)f.blocks.size(); i++) allSame &= doc.BlockState(i) == BlockChange::Same;
		rep.check(allSame && doc.UnsavedBlockCount(false) == 0, "T19 fresh baseline: every block Same, 0 unsaved");

		const int fs = doc.FindLine(0, "SetFontSize");
		FilterSetValueInt(f.lines[fs], 0, 40);
		f.dirty = true;
		rep.check(doc.BlockState(0) == BlockChange::Modified && doc.BlockState(1) == BlockChange::Same &&
		          doc.BlockState(2) == BlockChange::Same && doc.BlockState(3) == BlockChange::Same,
		          "T19 a value edit marks only its block Modified");
		const FilterLine* bl = doc.BaselineLine(fs);
		rep.check(bl && FilterValueInt(*bl, 0, 0) == 45 && doc.LineChanged(fs),
		          "T19 the baseline line still says 45 (shown as 原本)",
		          bl ? FilterSerializeLine(*bl) : std::string("null"));
		rep.check(!doc.LineChanged(doc.FindLine(0, "SetTextColor")), "T19 an untouched line is not changed");
		rep.check(doc.UnsavedBlockCount(false) == 1, "T19 unsaved count = 1 block");

		FilterSetValueInt(f.lines[fs], 0, 45);
		rep.check(doc.BlockState(0) == BlockChange::Same, "T19 typing the old value back is Same again");
		rep.check(doc.UnsavedBlockCount(true) == 0 && !f.dirty, "T19 ...and settling marks the file clean");

		const int rl = doc.FindLine(0, "Rarity");
		doc.CommentOutLine(rl);
		f.dirty = true;
		const FilterLine* rb = doc.BaselineLine(rl);
		rep.check(doc.BlockState(0) == BlockChange::Modified && rb && rb->kind == FilterLineKind::Condition,
		          "T19 a disabled (#!) line keeps its identity: the baseline is the live line");
		doc.RestoreLine(rl);
		rep.check(doc.BlockState(0) == BlockChange::Same, "T19 restoring it is Same again");
	}

	// ---- T20: restore one block -> byte-exact round-trip -------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		doc.CaptureBaseline();
		// block 0: a value edit, an inserted line, a disabled line, Show -> Hide
		FilterSetValueInt(f.lines[doc.FindLine(0, "SetFontSize")], 0, 30);
		doc.InsertLine(0, "Quality", ">=", { FilterToken{ "20", false } });
		doc.CommentOutLine(doc.FindLine(0, "Rarity"));
		{
			FilterLine& h = f.lines[f.blocks[0].headerLineIdx];
			h.keyword = "Hide";
			h.dirty = true;
			f.blocks[0].hide = true;
		}
		// block 3: Hide -> Show
		{
			FilterLine& h = f.lines[f.blocks[3].headerLineIdx];
			h.keyword = "Show";
			h.dirty = true;
			f.blocks[3].hide = false;
		}
		f.dirty = true;
		rep.check(doc.BlockState(0) == BlockChange::Modified && doc.BlockState(3) == BlockChange::Modified &&
		          doc.UnsavedBlockCount(false) == 2, "T20 two blocks edited");
		const int nb = doc.RestoreBlock(0);
		rep.check(nb == 0 && doc.BlockState(0) == BlockChange::Same && !f.blocks[0].hide,
		          "T20 RestoreBlock puts block 0 back (index kept)", std::to_string(nb));
		rep.check(doc.BlockState(3) == BlockChange::Modified && f.dirty, "T20 ...and leaves the other block alone");
		doc.RestoreBlock(3);
		rep.check(SerializeFilter(f) == src, "T20 every edited block restored: the file is byte-for-byte the original");
		rep.check(!f.dirty && doc.UnsavedBlockCount(true) == 0, "T20 ...and clean again");
		// the restored lines are real lines again: edit + restore once more
		FilterSetValueInt(f.lines[doc.FindLine(0, "SetFontSize")], 0, 33);
		f.dirty = true;
		doc.RestoreBlock(0);
		rep.check(SerializeFilter(f) == src && !f.dirty, "T20 restore is repeatable");
	}

	// ---- T21: a new custom rule is Added, not Modified ----------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		doc.CaptureBaseline();
		CustomZone z = EnsureCustomZone(doc);
		const int nb = doc.CreateBlockAtLine(z.endLine, false, u8"PobTools custom rule");
		doc.InsertLine(nb, "BaseType", "", { FilterToken{ "Divine Orb", true } });
		rep.check(nb >= 0 && doc.BlockState(nb) == BlockChange::Added, "T21 new custom rule is Added (新增)");
		bool othersSame = true;
		for (int i = 0; i < (int)f.blocks.size(); i++)
			if (i != nb) othersSame &= doc.BlockState(i) == BlockChange::Same;
		rep.check(othersSame, "T21 the NeverSink blocks stay Same");
		rep.check(doc.RestoreBlock(nb) == -1, "T21 an added block has nothing to restore");
		rep.check(doc.UnsavedBlockCount(false) == 1, "T21 unsaved count = 1");
		const int nlines = 0;
		(void)nlines;
		doc.RemoveBlock(nb);
		rep.check(doc.RemovedBaselineBlocks() == 0 && doc.UnsavedBlockCount(true) == 1 && f.dirty,
		          "T21 deleting it again still leaves the zone markers unsaved");
		const int dup = doc.DuplicateBlock(1);
		rep.check(dup >= 0 && doc.BlockState(dup) == BlockChange::Added && doc.BlockState(1) == BlockChange::Same,
		          "T21 a duplicated block is Added, its source stays Same");
	}

	// ---- T22/T23/T24 need a scratch folder ---------------------------------
	wchar_t tmpBuf2[MAX_PATH] = L"";
	GetTempPathW(MAX_PATH, tmpBuf2);
	const std::wstring scratch = std::wstring(tmpBuf2) + L"pobtools_fe_st_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
	CreateDirectoryW(scratch.c_str(), nullptr);

	// ---- T22: external modification ----------------------------------------
	{
		const std::wstring path = scratch + L"watch.filter";
		auto writeFile = [](const std::wstring& p, const std::string& data) {
			std::ofstream o(p, std::ios::binary);
			o.write(data.data(), (std::streamsize)data.size());
		};
		auto bumpTime = [](const std::wstring& p, long long secs) {
			HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			FILETIME ft{};
			GetFileTime(h, nullptr, nullptr, &ft);
			ULARGE_INTEGER u;
			u.LowPart = ft.dwLowDateTime;
			u.HighPart = ft.dwHighDateTime;
			u.QuadPart += (unsigned long long)(secs * 10000000LL);
			ft.dwLowDateTime = u.LowPart;
			ft.dwHighDateTime = u.HighPart;
			const bool ok = SetFileTime(h, nullptr, nullptr, &ft) != 0;
			CloseHandle(h);
			return ok;
		};
		writeFile(path, synthetic_crlf());

		ExternalChangeWatch w;
		w.Reset(path);
		rep.check(!w.Poll(0.0), "T22 nothing changed yet");
		const bool bumped = bumpTime(path, 10);
		rep.check(bumped && !w.Poll(0.5), "T22 polls at most every 2 s");
		rep.check(w.Poll(2.5) && w.changed(), "T22 a new write time (outside write) is detected");
		w.Acknowledge();
		rep.check(!w.Poll(5.0), "T22 dismissed: the current stamp is the new normal");

		// the editor's own save must not report itself (EditorShell::Save shows
		// a toast, so it runs inside a scratch ImGui context)
		ImGuiContext* prevCtx = ImGui::GetCurrentContext();
		ImGuiContext* ctx = ImGui::CreateContext();
		ImGui::SetCurrentContext(ctx);
		{
			EditorShell sh;
			sh.exeDir = scratch;
			sh.testMode = true;   // no ini writes
			const bool opened = sh.OpenByPath(path, true);
			rep.check(opened && sh.watch.path() == path && sh.doc.HasBaseline(), "T22 OpenByPath starts the watch and the baseline");
			FilterSetValueInt(sh.model.lines[sh.doc.FindLine(0, "SetFontSize")], 0, 41);
			sh.model.dirty = true;
			rep.check(sh.UnsavedCount() == 1, "T22 one unsaved block before saving");
			const bool saved = sh.Save();
			rep.check(saved && !sh.watch.Poll(100.0), "T22 our own save is not reported as an outside change");
			rep.check(sh.UnsavedCount() == 0 && !sh.model.dirty &&
			          sh.doc.BlockState(0) == BlockChange::Same, "T22 after saving, the saved file is the new baseline");
			bumpTime(path, 20);
			rep.check(sh.watch.Poll(200.0), "T22 an outside write after our save is still caught");
		}
		ImGui::DestroyContext(ctx);
		ImGui::SetCurrentContext(prevCtx);
		DeleteFileW(path.c_str());
		DeleteFileW((path + L".bak").c_str());
	}

	// ---- T23: filter chip counts ---------------------------------------------
	{
		EditorShell sh;
		sh.testMode = true;
		sh.model = ParseFilter(synthetic_crlf());
		sh.loaded = true;
		sh.doc.Attach(&sh.model);
		sh.doc.CaptureBaseline();
		EdRebuildRows(sh);
		const ChipCounts c0 = sh.chipCounts;
		rep.check(c0.all == 4 && c0.shown == 3 && c0.hidden == 1 && c0.changed == 0 && c0.custom == 0,
		          "T23 counts on load: 4 all / 3 shown / 1 hidden / 0 changed / 0 custom",
		          std::to_string(c0.all) + "/" + std::to_string(c0.shown) + "/" + std::to_string(c0.hidden) + "/" +
		              std::to_string(c0.changed) + "/" + std::to_string(c0.custom));
		// a custom rule (added) + a value edit (modified)
		CustomZone z = EnsureCustomZone(sh.doc);
		const int nb = sh.doc.CreateBlockAtLine(z.endLine, false, u8"PobTools custom rule");
		sh.doc.InsertLine(nb, "BaseType", "", { FilterToken{ "Divine Orb", true } });
		EdRebuildRows(sh);
		int vaal = -1;
		for (int i = 0; i < (int)sh.model.blocks.size(); i++)
			for (int li : sh.model.blocks[i].lineIdx)
				if (sh.model.lines[li].keyword == "BaseType" && FilterHasValue(sh.model.lines[li], "Vaal Orb")) vaal = i;
		FilterSetValueInt(sh.model.lines[sh.doc.FindLine(vaal, "SetFontSize")], 0, 36);
		sh.model.dirty = true;
		EdRefreshRowStates(sh);
		const ChipCounts c1 = sh.chipCounts;
		rep.check(c1.all == 5 && c1.shown == 4 && c1.hidden == 1 && c1.changed == 2 && c1.custom == 1,
		          "T23 after an add + an edit: 5 / 4 / 1 / 2 changed / 1 custom",
		          std::to_string(c1.all) + "/" + std::to_string(c1.shown) + "/" + std::to_string(c1.hidden) + "/" +
		              std::to_string(c1.changed) + "/" + std::to_string(c1.custom));
		sh.chip = RuleChip::Changed;
		EdRebuildVisRows(sh);
		rep.check((int)sh.visRows.size() == 2, "T23 the 已修改 chip lists exactly those two", std::to_string(sh.visRows.size()));
		sh.chip = RuleChip::Hidden;
		EdRebuildVisRows(sh);
		rep.check((int)sh.visRows.size() == 1 && sh.model.blocks[sh.visRows[0]].hide, "T23 the 隱藏 chip lists the Hide block");
		// hiding one more block under the 隱藏 chip updates the list by itself
		SetBlockHide(sh, sh.model.blocks[vaal], true);
		EdRefreshRowStates(sh);
		rep.check((int)sh.visRows.size() == 2 && sh.chipCounts.hidden == 2, "T23 a show/hide change refreshes counts and the chip list");
		sh.chip = RuleChip::Custom;
		EdRebuildVisRows(sh);
		rep.check((int)sh.visRows.size() == 1 && sh.rows[sh.visRows[0]].custom, "T23 the 自訂 chip lists the custom rule");
		// visList: group headings interleaved, never a heading without a rule after it
		sh.chip = RuleChip::All;
		EdRebuildVisRows(sh);
		bool headingsOk = !sh.visList.empty() && sh.visList.front().block < 0;
		for (size_t i = 0; i + 1 < sh.visList.size(); i++)
			if (sh.visList[i].block < 0 && sh.visList[i + 1].block < 0) headingsOk = false;
		rep.check(headingsOk && sh.visList.back().block >= 0, "T23 the list interleaves group headings correctly");
	}

	// ---- T24: recently used colours --------------------------------------------
	{
		std::vector<std::uint32_t> list;
		for (int i = 0; i < 10; i++) {
			const int c[4] = { i, 10 + i, 20 + i, 255 };
			PushRecentColor(list, c);
		}
		const int again[4] = { 7, 17, 27, 255 };
		PushRecentColor(list, again);
		rep.check((int)list.size() == kRecentColorMax && list[0] == ((7u << 24) | (17u << 16) | (27u << 8) | 255u) &&
		          list[1] == ((9u << 24) | (19u << 16) | (29u << 8) | 255u),
		          "T24 newest first, at most 8, a reused colour moves to the front without a duplicate");
		int dups = 0;
		for (size_t i = 0; i < list.size(); i++)
			for (size_t j = i + 1; j < list.size(); j++) if (list[i] == list[j]) dups++;
		rep.check(dups == 0, "T24 no duplicates");

		EditorShell a;
		a.exeDir = scratch;
		a.recentColors = list;
		SaveEditorSettings(a);
		EditorShell b;
		b.exeDir = scratch;
		LoadEditorSettings(b);
		rep.check(b.recentColors == list, "T24 saved to pob-zh.ini and read back identically",
		          EncodeRecentColors(b.recentColors));
		rep.check(DecodeRecentColors("x;300,0,0,0;1,2,3,4;1,2,3,4") == std::vector<std::uint32_t>{ 0x01020304u },
		          "T24 garbage and out-of-range entries are dropped");
		EditorShell t;
		t.exeDir = scratch;
		t.testMode = true;
		t.recentColors = { 0xffffffffu };
		SaveEditorSettings(t);   // a test run writes nothing
		EditorShell r;
		r.exeDir = scratch;
		LoadEditorSettings(r);
		rep.check(r.recentColors == list, "T24 a test run does not write the ini");
		DeleteFileW((scratch + L"pob-zh.ini").c_str());
	}
	RemoveDirectoryW(scratch.c_str());
	rep.check(GetFileAttributesW(scratch.c_str()) == INVALID_FILE_ATTRIBUTES, "T24 the scratch folder is gone");

	// ---- T25: sound file names with a download suffix ------------------------
	{
		rep.check(SoundNameHasDownloadSuffix(L"6maps (1).mp3") && SoundNameHasDownloadSuffix(L"alert (12).wav") &&
		          !SoundNameHasDownloadSuffix(L"6maps.mp3") && !SoundNameHasDownloadSuffix(L"(1).mp3") &&
		          !SoundNameHasDownloadSuffix(L"map(1).mp3") && !SoundNameHasDownloadSuffix(L"map ().mp3"),
		          "T25 \" (n)\" download suffix detected, plain names not");
	}

	rep.note("failures=" + std::to_string(rep.failures));
	printf("%s", rep.text.c_str());

	std::ofstream out(exeDir + L"filter_selftest.txt", std::ios::binary);
	if (out) out.write(rep.text.data(), (std::streamsize)rep.text.size());

	return rep.failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// --filter-import-probe: the paste button's exact path, headless.

int RunFilterImportProbe(const std::wstring& exeDir, const std::wstring& itemFile,
                         const std::wstring& filterFile)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string repText;
	auto line = [&repText](const std::string& s) { repText += s + "\n"; };

	std::string text;
	if (!itemFile.empty()) {
		std::ifstream in(itemFile, std::ios::binary);
		text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	} else {
		text = ReadClipboardUtf8(nullptr);   // the same call the paste button makes
	}
	line("input: " + std::to_string(text.size()) + " bytes (" +
	     (itemFile.empty() ? "clipboard" : "file") + ")");

	FilterI18n i18n;
	i18n.Load(exeDir, "zh-rTW");
	ItemLibrary lib;
	lib.Load(exeDir, i18n);

	ImportedItem r = ParseGameItemText(text, i18n, lib.items());
	line("ok=" + std::to_string(r.ok ? 1 : 0));
	line("name='" + r.name + "'  base='" + r.baseRaw + "' -> '" + r.baseEn +
	     "'  classId='" + r.item.classId + "'");
	line("rarity=" + std::to_string(r.item.rarity) +
	     " ilvl=" + std::to_string(r.item.itemLevel) +
	     " drop=" + std::to_string(r.item.dropLevel) +
	     " q=" + std::to_string(r.item.quality) +
	     " size=" + std::to_string(r.item.width) + "x" + std::to_string(r.item.height) +
	     " sockets=" + std::to_string(r.item.sockets) +
	     "/" + std::to_string(r.item.linkedSockets));
	{
		std::string g = "groups=";
		for (const std::string& s : r.item.socketGroups) g += "[" + s + "]";
		g += "  influence=" + std::to_string(r.item.influence);
		g += std::string("  corrupted=") + (r.item.corrupted ? "1" : "0");
		g += std::string(" identified=") + (r.item.identified ? "1" : "0");
		g += std::string(" fractured=") + (r.item.fractured ? "1" : "0");
		g += std::string(" enchanted=") + (r.item.enchanted ? "1" : "0");
		line(g);
	}
	for (const ImportIssue& w : r.warnings) line(u8"warn: " + w.msg);

	int rc = r.ok ? 0 : 1;
	if (r.ok) {
		std::wstring fpath = filterFile.empty() ? exeDir + L"Filters\\default.filter"
		                                        : filterFile;
		bool okF = false;
		FilterFile f = LoadFilter(fpath, &okF);
		if (!okF) {
			line("filter: could not load (pass a .filter path as the 2nd argument)");
			rc = 2;
		} else {
			PreviewResult res = EvaluatePreview(f, r.item, i18n);
			line("filter blocks=" + std::to_string(f.blocks.size()));
			line(std::string("matched=") + (res.matched ? "1" : "0") +
			     " hidden=" + (res.hidden ? "1" : "0") +
			     " blockIdx=" + std::to_string(res.blockIdx) +
			     " fontSize=" + std::to_string(res.fontSize));
			if (res.blockIdx >= 0 && res.blockIdx < (int)f.blocks.size()) {
				line("rule: " + f.blocks[res.blockIdx].headerComment);
				line("rule zh: " + NeverSinkHeaderZh(f.blocks[res.blockIdx].headerComment));
			}
			if (!res.customSound.empty()) line("sound: " + res.customSound);
			if (!res.playEffect.empty()) line("beam: " + res.playEffect);
			if (!res.minimapIcon.empty()) line("icon: " + res.minimapIcon);
			std::string u = "unknownConds:";
			for (const std::string& k : res.unknownConds) u += " " + k;
			line(u);
		}
	}

	printf("%s", repText.c_str());
	std::ofstream out(exeDir + L"filter_import_probe.txt", std::ios::binary);
	if (out) out.write(repText.data(), (std::streamsize)repText.size());
	return rc;
}
