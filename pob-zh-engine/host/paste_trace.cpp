// Diagnostics for the Chinese paste path: make the engine say what it did.
//
// reverse_one_line() is a chain of ~15 fallback rules over a dictionary merged
// from eight files, last file wins. From outside you see only "Chinese in,
// something out", so every question about coverage has to be answered by
// inference -- and inference kept being wrong: in one session three separate
// coverage figures were produced and all three were measuring something other
// than what they claimed (a fixture that silently excluded literal-number
// entries, a de-duplicated fixture that made a collision check vacuously true,
// and a Python re-implementation of the engine's hashing that did not match it).
//
// The fix is not a better inference. It is to have the engine emit, per line,
// which rule fired and which file's copy of the key won. Two entry points:
//
//   --paste-trace <in.txt> [out.tsv]   one item, line by line
//   --paste-census [out.tsv]           every dictionary entry's Chinese
//
// The census deliberately makes no pass/fail judgement. It records rule +
// winning file + output and leaves the classifying to analysis, because a
// premature pass/fail criterion is exactly what produced the wrong numbers.

#include "paste_trace.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "editor_data.h"
#include "../translate/translation_manager.h"

namespace {

std::wstring exe_dir()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring s(path);
	size_t slash = s.find_last_of(L'\\');
	return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::string tsv_safe(const std::string& s)
{
	std::string r;
	r.reserve(s.size());
	for (char c : s) r += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
	return r;
}

// Replace {N} / {N:+d} with a value. Two shapes are probed because the
// dictionary is inconsistent about where the percent sign lives: some templates
// read "{0}% chance" and others "{0} chance" with the percent inside the value.
// Substituting the wrong shape changes the hashed key and would be reported as
// a miss that does not exist -- the same class of instrumentation artefact this
// tool exists to eliminate, so both are emitted and analysis takes the better.
std::string fill_placeholders(const std::string& s, const char* value)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '{' && i + 2 < s.size()) {
			size_t j = i + 1;
			while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
			if (j > i + 1 && j < s.size()) {
				size_t close = (s[j] == '}') ? j : s.find('}', j);
				if (close != std::string::npos && close - i <= 12) {
					out += value;
					i = close;
					continue;
				}
			}
		}
		out += s[i];
	}
	return out;
}

bool has_placeholder(const std::string& s)
{
	for (size_t i = 0; i + 2 < s.size(); i++) {
		if (s[i] != '{') continue;
		size_t j = i + 1;
		while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
		if (j > i + 1 && j < s.size() && (s[j] == '}' || s[j] == ':')) return true;
	}
	return false;
}

// Runs one probe with tracing on and returns the trace rows it produced.
std::string trace_once(const std::string& text)
{
	translation_trace_begin();
	char* out = translation_reverse_text(text.c_str());
	std::string rows = translation_trace_get();
	if (out) translation_free(out);
	translation_trace_end();
	return rows;
}

FILE* open_out(const wchar_t* path, const char* header)
{
	FILE* f = nullptr;
	if (path && *path) _wfopen_s(&f, path, L"wb");
	if (!f) return nullptr;
	fputs("\xEF\xBB\xBF", f);   // BOM: these files get opened in Excel
	fputs(header, f);
	return f;
}

} // namespace

int RunPasteTrace(const std::wstring& inPath, const std::wstring& outPath)
{
	SetEnvironmentVariableA("POB_LOCALE", "zh-rTW");
	translation_init();

	std::ifstream in(narrow(inPath).c_str(), std::ios::binary);
	if (!in) {
		printf("cannot open %ls\n", inPath.c_str());
		return 2;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	std::string text = ss.str();
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text = text.substr(3);

	std::string rows = trace_once(text);

	FILE* f = open_out(outPath.c_str(), "rule\tkind\tkey\tfile\tin\tout\n");
	if (f) {
		fwrite(rows.data(), 1, rows.size(), f);
		fclose(f);
		printf("wrote %ls\n", outPath.c_str());
	} else {
		printf("rule\tkind\tkey\tfile\tin\tout\n%s", rows.c_str());
	}
	return 0;
}

int RunPasteCensus(const std::wstring& outPath, const std::string& shell)
{
	SetEnvironmentVariableA("POB_LOCALE", "zh-rTW");
	translation_init();

	EditorModel model = LoadModel(exe_dir(), "poe1", "zh-rTW");
	if (!model.localeExists) {
		printf("no poe1/zh-rTW dictionary next to the exe\n");
		return 2;
	}
	printf("census over %d entries in %d files\n",
	       (int)model.entries.size(), (int)model.files.size());

	// `id` is the entry's index in the model. Without it the two probe shapes of
	// one entry have to be re-associated downstream by string surgery, and that
	// surgery has already produced one wrong figure -- the sentinel and the
	// template's own '%' are indistinguishable after the fact.
	FILE* f = open_out(outPath.c_str(),
	                   "id\tsrc_file\tshape\trule\tkind\twinner_file\tmatched_key\tprobe\town_en\tgot_en\n");
	if (!f) {
		printf("cannot write %ls\n", outPath.c_str());
		return 2;
	}

	// A modifier never arrives alone: reverse_one_line's rarity/name handling and
	// the flavour pass both need context, so each probe is wrapped in the minimal
	// item shell a real paste would have.
	//
	// Which shell matters now that section kind drives the rules. The default one
	// has no Item Level line, so the grammar declines to label anything and every
	// probe runs the Unknown path -- that is deliberate: comparing this census
	// across two builds is what proves "Unknown means unchanged behaviour" over
	// the whole dictionary. `shell` picks a different context to measure instead:
	//
	//   plain     (default) no Item Level -> every line Unknown
	//   mods      probe sits in an annotated modifier section
	//   property  probe sits in the property block, before Item Level
	const char* kShellHead = "\xe7\xa8\x80\xe6\x9c\x89\xe5\xba\xa6: "
	                         "\xe7\xa8\x80\xe6\x9c\x89\n";   // "稀有度: 稀有\n"
	const char* kShellName = "New Item\nCrimson Jewel\n--------\n";
	// "物品等級: 83\n--------\n{ 前綴 }\n"   (前綴 = prefix marker)
	const char* kShellItemLevel = "\xe7\x89\xa9\xe5\x93\x81\xe7\xad\x89\xe7\xb4\x9a: 83\n"
	                              "--------\n";
	const char* kShellAnnotation = "{ \xe5\x89\x8d\xe7\xb6\xb4 }\n";
	printf("shell = %s\n", shell.c_str());

	int done = 0;
	int id = -1;
	int skippedMultiline = 0;
	for (const EditorEntry& raw : model.entries) {
		id++;
		if (raw.value.empty() || raw.key.empty()) continue;

		// The dictionary stores GGG's "[Attack|攻擊]" markup verbatim, but the
		// game renders it away before the text ever reaches a clipboard. Probing
		// with the markup left in reported 6,439 entries as unmatched that a real
		// paste would never present that way -- an artefact of the instrument,
		// not a defect in the engine. Rendered here with the ENGINE's own
		// function so the probe cannot drift from what it is measuring.
		EditorEntry e = raw;
		e.value = translation_strip_ggg_markup(raw.value.c_str());
		e.key = translation_strip_ggg_markup(raw.key.c_str());
		const char* srcFile = (e.fileIdx >= 0 && e.fileIdx < (int)model.files.size())
		                          ? model.files[e.fileIdx].name.c_str() : "?";
		const bool ph = has_placeholder(e.value);
		const char* shapes[2] = { "7654321", "7654321%" };
		const int nShapes = ph ? 2 : 1;

		for (int si = 0; si < nShapes; si++) {
			std::string probe = ph ? fill_placeholders(e.value, shapes[si]) : e.value;
			std::string ownEn = ph ? fill_placeholders(e.key, shapes[si]) : e.key;
			// A dictionary value may hold GGG's own "\n" (497 of them do). The game
			// renders those as two separate lines, so there is no single paste line
			// they correspond to and no single row that could describe one. Skipped
			// rather than folded into one line, and counted, because a census that
			// quietly drops rows reads as coverage it does not have.
			if (probe.find('\n') != std::string::npos) { skippedMultiline++; continue; }

			// The probe is not always the last line, so its row index is counted
			// from the shell rather than assumed -- one trace row per input line.
			std::string text = std::string(kShellHead) + kShellName;   // 4 lines
			size_t probeRow = 4;
			if (shell == "mods") {
				text += std::string(kShellItemLevel) + kShellAnnotation; // 3 lines
				probeRow += 3;
			}
			text += probe + "\n";
			if (shell == "property") text += std::string("--------\n") + kShellItemLevel;

			std::string rows = trace_once(text);
			std::vector<std::string> rowv;
			{
				size_t p = 0;
				while (p < rows.size()) {
					size_t nl = rows.find('\n', p);
					if (nl == std::string::npos) nl = rows.size();
					rowv.push_back(rows.substr(p, nl - p));
					p = nl + 1;
				}
			}
			std::string row = probeRow < rowv.size() ? rowv[probeRow] : std::string();
			while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();

			// row is: rule \t kind \t key \t file \t in \t out
			std::string parts[6];
			size_t p = 0;
			for (int i = 0; i < 6 && p <= row.size(); i++) {
				size_t t = row.find('\t', p);
				parts[i] = row.substr(p, t == std::string::npos ? std::string::npos : t - p);
				if (t == std::string::npos) break;
				p = t + 1;
			}

			fprintf(f, "%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			        id, srcFile, ph ? (si == 0 ? "plain" : "pct") : "literal",
			        parts[0].c_str(), parts[1].c_str(), parts[3].c_str(),
			        tsv_safe(parts[2]).c_str(),
			        tsv_safe(probe).c_str(), tsv_safe(ownEn).c_str(),
			        tsv_safe(parts[5]).c_str());
		}
		if (++done % 20000 == 0) printf("  %d...\n", done);
	}
	fclose(f);
	printf("skipped %d probe(s) whose value contains GGG's own newline\n", skippedMultiline);
	printf("wrote %ls\n", outPath.c_str());
	return 0;
}
