#include "launcher_editor.h"
#include "editor_data.h"
#include "launcher_config.h" // ResolveConfiguredFontPath
#include "ui_theme.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <vector>

static const float kFontSize = 18.0f;

// ---- helpers ---------------------------------------------------------------

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static std::vector<unsigned char> read_file(const std::wstring& path)
{
	std::vector<unsigned char> data;
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return data;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 30)) {
		data.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr) || read != data.size())
			data.clear();
	}
	CloseHandle(h);
	return data;
}

// Case-insensitive (ASCII) substring test. needleLower must already be lower.
static bool contains_ci(const std::string& hay, const std::string& needleLower)
{
	if (needleLower.empty()) return true;
	const size_t n = needleLower.size();
	if (hay.size() < n) return false;
	for (size_t i = 0; i + n <= hay.size(); i++) {
		size_t j = 0;
		for (; j < n; j++) {
			char c = hay[i + j];
			if (c >= 'A' && c <= 'Z') c += 32;
			if (c != needleLower[j]) break;
		}
		if (j == n) return true;
	}
	return false;
}

static std::string to_lower_ascii(const std::string& s)
{
	std::string r = s;
	for (char& c : r) if (c >= 'A' && c <= 'Z') c += 32;
	return r;
}

// Stable per-file badge colour: cheap hash of the file name.
static ImU32 BadgeColor(const std::string& name)
{
	unsigned h = 2166136261u;
	for (char ch : name) { h ^= (unsigned char)ch; h *= 16777619u; }
	float r = 0.45f + 0.35f * ((h & 0xFF) / 255.0f);
	float g = 0.45f + 0.35f * (((h >> 8) & 0xFF) / 255.0f);
	float b = 0.45f + 0.35f * (((h >> 16) & 0xFF) / 255.0f);
	return ImColor(r, g, b, 1.0f);
}

static void BadgeText(const std::string& label, ImU32 col)
{
	ImGui::PushStyleColor(ImGuiCol_Text, col);
	ImGui::TextUnformatted(label.c_str());
	ImGui::PopStyleColor();
}

// ---- POB colour escapes ------------------------------------------------------
// POB marks colour inline: ^xRRGGBB for a literal colour, ^0..^9 for a palette
// index. The dictionaries carry them verbatim (218 entries do) because the
// engine hands the codes straight back to POB, so the editor has to show them
// the way POB will draw them rather than as raw escape text.
//
// Palette and parsing rules mirror engine/common/common.cpp IsColorEscape /
// ReadColorEscape and the colorEscape[] table — kept identical on purpose; a
// preview that invents its own colours would be worse than none.
static const ImVec4 kPobPalette[10] = {
	ImVec4(0.0f, 0.0f, 0.0f, 1.0f), // ^0 black
	ImVec4(1.0f, 0.0f, 0.0f, 1.0f), // ^1 red
	ImVec4(0.0f, 1.0f, 0.0f, 1.0f), // ^2 green
	ImVec4(0.0f, 0.0f, 1.0f, 1.0f), // ^3 blue
	ImVec4(1.0f, 1.0f, 0.0f, 1.0f), // ^4 yellow
	ImVec4(1.0f, 0.0f, 1.0f, 1.0f), // ^5 purple
	ImVec4(0.0f, 1.0f, 1.0f, 1.0f), // ^6 aqua
	ImVec4(1.0f, 1.0f, 1.0f, 1.0f), // ^7 white
	ImVec4(0.7f, 0.7f, 0.7f, 1.0f), // ^8 gray
	ImVec4(0.4f, 0.4f, 0.4f, 1.0f), // ^9 dark gray
};

static bool is_hex_digit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Length of the escape at s[i], or 0 when there is none. 2 for ^N, 8 for ^xRRGGBB.
static int pob_escape_len(const std::string& s, size_t i)
{
	if (i >= s.size() || s[i] != '^') return 0;
	if (i + 1 >= s.size()) return 0;
	char d = s[i + 1];
	if (d >= '0' && d <= '9') return 2;
	if (d == 'x' || d == 'X') {
		if (i + 7 >= s.size()) return 0;
		for (int c = 0; c < 6; c++)
			if (!is_hex_digit(s[i + 2 + c])) return 0;
		return 8;
	}
	return 0;
}

static bool has_pob_color(const std::string& s)
{
	for (size_t i = 0; i < s.size(); i++)
		if (pob_escape_len(s, i)) return true;
	return false;
}

// Draw `s` the way POB would: escapes consumed, following text tinted.
// `base` is the colour to start from (the surrounding text colour).
static void TextPobColored(const std::string& s, const ImVec4& base)
{
	ImVec4 cur = base;
	std::string run;
	bool first = true;
	auto flush = [&]() {
		if (run.empty()) return;
		if (!first) ImGui::SameLine(0, 0);
		ImGui::TextColored(cur, "%s", run.c_str());
		first = false;
		run.clear();
	};
	for (size_t i = 0; i < s.size();) {
		int esc = pob_escape_len(s, i);
		if (!esc) { run += s[i++]; continue; }
		flush();
		if (esc == 2) {
			cur = kPobPalette[s[i + 1] - '0'];
		} else {
			auto hex = [&](size_t off) {
				int v = 0;
				for (size_t k = 0; k < 2; k++) {
					char c = s[off + k];
					v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
				}
				return v / 255.0f;
			};
			cur = ImVec4(hex(i + 2), hex(i + 4), hex(i + 6), 1.0f);
		}
		i += esc;
	}
	flush();
	if (first) ImGui::TextUnformatted(""); // keep the row height when empty
}

// Target-file picker, ordered the way the ENGINE loads the files rather than
// alphabetically. Which file you write to decides whether the edit does anything
// at all: the dictionary is one flat map merged in meta.json's load_order and the
// last file to define a key wins. poe1 and poe2 order the same files differently,
// so the name alone tells the user nothing.
static void TargetFileCombo(const EditorModel& model, const char* id, int& target, float scale)
{
	const std::vector<int> order = FileIdxInLoadOrder(model);
	auto label = [&](int fi) {
		const EditorFile& f = model.files[fi];
		if (f.order < 0) return f.name + u8"（未列入 load_order，引擎不會載入）";
		return std::to_string(f.order + 1) + ". " + f.name;
	};
	// highest-ranked listed file = the one that wins every collision
	int lastListed = -1;
	for (int fi : order) if (model.files[fi].order >= 0) lastListed = fi;

	ImGui::SetNextItemWidth(230 * scale);
	const std::string preview = (target >= 0 && target < (int)model.files.size())
		? label(target) : std::string(u8"（選擇檔案）");
	if (!ImGui::BeginCombo(id, preview.c_str())) return;
	bool separated = false;
	for (int fi : order) {
		const bool unlisted = model.files[fi].order < 0;
		if (unlisted && !separated) { ImGui::Separator(); separated = true; }
		// An unlisted file is never merged, so writing there is guaranteed to do
		// nothing -- the least detectable failure of all. Do not offer it.
		ImGui::BeginDisabled(unlisted);
		if (ImGui::Selectable(label(fi).c_str(), fi == target)) target = fi;
		ImGui::EndDisabled();
		if (fi == lastListed) {
			ImGui::SameLine();
			ImGui::TextDisabled(u8"← 最後載入，會蓋過前面");
		}
	}
	ImGui::EndCombo();
}

// ---- editor ----------------------------------------------------------------

void ShowEditor(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale)
{
	if (!glfwInit()) {
		MessageBoxW(nullptr, L"無法初始化 GLFW，翻譯編輯器無法顯示。", L"PobTools", MB_ICONERROR | MB_OK);
		return;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

	float scale = 1.0f;
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (monitor) {
		float sx = 1.0f, sy = 1.0f;
		glfwGetMonitorContentScale(monitor, &sx, &sy);
		scale = sx > 0.0f ? sx : 1.0f;
	}
	// Roomier default: the entry list is three columns and the missing-string
	// tab is four, so 1100x720 left both cramped. Clamped to the monitor work
	// area (not the raw resolution) so the title bar stays reachable and the
	// window still fits on a small laptop screen.
	int winW = (int)(1440 * scale);
	int winH = (int)(900 * scale);
	if (monitor) {
		int wx = 0, wy = 0, ww = 0, wh = 0;
		glfwGetMonitorWorkarea(monitor, &wx, &wy, &ww, &wh);
		if (ww > 0 && wh > 0) {
			if (winW > ww - (int)(40 * scale)) winW = ww - (int)(40 * scale);
			if (winH > wh - (int)(60 * scale)) winH = wh - (int)(60 * scale);
		}
	}
	if (winW < 900) winW = 900;
	if (winH < 600) winH = 600;

	GLFWwindow* win = glfwCreateWindow(winW, winH, "PobTools \xe2\x80\x94 \xe7\xbf\xbb\xe8\xad\xaf\xe7\xb7\xa8\xe8\xbc\xaf\xe5\x99\xa8", nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立翻譯編輯器視窗。", L"PobTools", MB_ICONERROR | MB_OK);
		return;
	}
	if (monitor) {
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		if (mode) glfwSetWindowPos(win, (mode->width - winW) / 2, (mode->height - winH) / 2);
	}
	glfwMakeContextCurrent(win);
	glfwSwapInterval(1);
	glfwShowWindow(win);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	PobUi::ApplyTheme(scale, PobUi::Density::Compact);

	// Full CJK + Korean atlas so any dictionary text renders and any character
	// can be typed via IME (the launcher only builds glyphs for its own labels).
	std::vector<unsigned char> ttf = read_file(ResolveConfiguredFontPath(exeDir));
	ImFont* font = nullptr;
	bool cjkOk = false;
	if (!ttf.empty()) {
		ImGuiIO& io = ImGui::GetIO();
		static ImVector<ImWchar> ranges;
		ranges.clear();
		ImFontGlyphRangesBuilder b;
		b.AddRanges(io.Fonts->GetGlyphRangesDefault());
		b.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
		b.AddRanges(io.Fonts->GetGlyphRangesKorean());
		b.BuildRanges(&ranges);
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		// CJK atlas is dense (~21k glyphs). Disable oversampling so the texture
		// stays well under the GLES2/ANGLE max-texture-size limit (oversampling
		// would ~4x the area and can silently drop glyphs on some GPUs).
		cfg.OversampleH = 1;
		cfg.OversampleV = 1;
		cfg.PixelSnapH = true;
		// Cap atlas width so it grows in height rather than exceeding GL limits.
		io.Fonts->TexDesiredWidth = 4096;
		font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), kFontSize * scale, &cfg, ranges.Data);
		if (io.Fonts->Build() && font)
			cjkOk = font->FindGlyphNoFallback((ImWchar)0x555F /* 啟 */) != nullptr;
	}
	if (!font) font = ImGui::GetIO().Fonts->AddFontDefault();

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	// --- state ---
	// "launcher" is the launcher's own labels. It is listed here rather than
	// special-cased anywhere because Data\launcher\<locale>\ has the same shape as
	// Data\poe1\<locale>\ — meta.json with a load_order plus dictionary files.
	static const char* kGames[kDictSlotCount] = { "poe1", "poe2", "launcher" };
	int gi = 0;
	for (int i = 0; i < kDictSlotCount; i++)
		if (game == DictSlotFolder((DictSlot)i)) { gi = i; break; }

	// The dictionaries the ENGINE will read, per slot. Editing the built-in copy
	// while POB reads an external one is the one failure this whole feature has to
	// avoid, so the editor resolves the same settings the launcher does. Resolved
	// once for all three: switching the combo must not re-read the ini and pick up
	// a half-finished edit made in another window.
	const LauncherConfig editCfg = LoadLauncherConfig(exeDir + L"pob-zh.ini");
	DictDirInfo slotDir[kDictSlotCount];
	for (int i = 0; i < kDictSlotCount; i++)
		slotDir[i] = ResolveDictDir(exeDir, (DictSlot)i, editCfg.dataDir[i]);
	auto slotRoot = [&]() { return slotDir[gi].root; };

	// Locales come from disk, not a hardcoded list: the caller's choice used to
	// be discarded outright, so opening the editor from an "en" launcher silently
	// edited the Chinese dictionary.
	std::vector<std::string> locales = ListLocales(slotRoot());
	std::string status;
	int li = 0;
	{
		const std::string want = narrow(locale);
		auto it = std::find(locales.begin(), locales.end(), want);
		if (it != locales.end()) {
			li = (int)(it - locales.begin());
		} else {
			auto zh = std::find(locales.begin(), locales.end(), std::string("zh-rTW"));
			li = (zh != locales.end()) ? (int)(zh - locales.begin()) : 0;
			if (!want.empty())
				status = u8"此語系（" + want + u8"）沒有翻譯資料，已改開 " + locales[li];
		}
	}

	EditorModel model = LoadModel(slotRoot(), locales[li]);
	auto noteExternal = [&]() {
		if (slotDir[gi].status == DataDirStatus::External)
			status = u8"編輯的是外部翻譯資料夾：" + narrow(slotRoot());
	};
	if (status.empty()) noteExternal();

	std::string search;       // raw search text
	std::string searchLower;  // cached lowercase
	int fileFilter = 0;       // 0 = all, else file index + 1
	std::vector<size_t> filtered;

	// one-shot requests to bring a tab to the front (the toolbar button and
	// the scan action both need it; ImGui clears the flag after one frame)
	bool focusMiss = false;
	bool focusEntries = false;
	bool missScanned = false;
	bool missLogFound = false;
	bool missShowReverse = false;
	std::vector<MissEntry> misses;
	std::vector<std::string> missTrans;
	std::vector<int> missTarget;

	// "Add entry" row (above the table, not inside it: the table is virtualised
	// with ImGuiListClipper over 110k rows and its indices are used directly).
	std::string newKey, newVal;
	int newTarget = -1;
	std::vector<int> newKeyOwners;      // files already defining newKey, in load order
	std::string newKeyOwnersFor;        // the key newKeyOwners was computed for
	bool focusNewKey = false;

	// Expanded editor for a long / multi-line value. The table is virtualised with
	// ImGuiListClipper, which requires uniform row heights, so a multi-line box
	// cannot go in the cell -- it opens as a modal instead. -1 = closed.
	int bigIdx = -1;
	std::string bigText;
	bool openBig = false;

	// An action waiting for the unsaved-changes prompt. All four entry points
	// (game combo, locale combo, reload button, window close) need the same
	// guard, so they set a pending action instead of each opening their own.
	enum class Pending { None, SwitchGame, SwitchLocale, Reload, Close };
	Pending pending = Pending::None;
	int pendingGi = gi, pendingLi = li;

	auto rebuildFilter = [&]() {
		filtered.clear();
		filtered.reserve(model.entries.size());
		for (size_t i = 0; i < model.entries.size(); i++) {
			const EditorEntry& e = model.entries[i];
			if (fileFilter != 0 && e.fileIdx != fileFilter - 1) continue;
			if (!searchLower.empty() &&
				!contains_ci(e.key, searchLower) && !contains_ci(e.value, searchLower))
				continue;
			filtered.push_back(i);
		}
	};

	// Unconditional reload. Callers are responsible for the dirty check — this
	// stays the raw operation so the guard has exactly one implementation.
	auto reload = [&]() {
		locales = ListLocales(slotRoot());
		if (li >= (int)locales.size()) li = 0;
		model = LoadModel(slotRoot(), locales[li]);
		fileFilter = 0;
		rebuildFilter();
		missScanned = false;
		misses.clear(); missTrans.clear(); missTarget.clear();
		newKeyOwners.clear(); newKeyOwnersFor.clear(); newTarget = -1;
		status.clear();
		noteExternal(); // each slot has its own folder: say which one this is
	};

	auto applyPending = [&]() {
		if (pending == Pending::SwitchGame)        gi = pendingGi;
		else if (pending == Pending::SwitchLocale) li = pendingLi;
		reload();
	};

	auto runScan = [&]() {
		misses = ScanMisses(exeDir, model, &missLogFound);
		missScanned = true;
		int uiIdx = FindFileIdx(model, "ui.json");
		if (uiIdx < 0) uiIdx = model.files.empty() ? -1 : 0;
		missTrans.assign(misses.size(), std::string());
		missTarget.assign(misses.size(), uiIdx);
	};

	rebuildFilter();

	bool running = true;
	while (running) {
		glfwPollEvents();
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGui::PushFont(font);

		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##editor", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

		// --- data scope ---
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(PobUi::Accent(), u8"翻譯資料庫");
		ImGui::SameLine(0, 18 * scale);
		// Both combos roll their own selection BACK when there are unsaved edits
		// and hand the choice to the prompt instead. ImGui::Combo writes the new
		// index immediately, so without the rollback the box would show a game
		// that is not loaded while the prompt is up.
		ImGui::SetNextItemWidth(110 * scale);
		{
			int prev = gi;
			if (ImGui::Combo("##game", &gi, kGames, kDictSlotCount)) {
				if (DirtyCount(model) > 0) { pendingGi = gi; gi = prev; pending = Pending::SwitchGame; }
				else reload();
			}
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110 * scale);
		{
			int prev = li;
			if (ImGui::BeginCombo("##locale", locales[li].c_str())) {
				for (size_t i = 0; i < locales.size(); i++) {
					if (ImGui::Selectable(locales[i].c_str(), (int)i == li)) li = (int)i;
				}
				ImGui::EndCombo();
			}
			if (li != prev) {
				if (DirtyCount(model) > 0) { pendingLi = li; li = prev; pending = Pending::SwitchLocale; }
				else reload();
			}
		}

		ImGui::SameLine(0, 18 * scale);
		ImGui::TextDisabled(u8"資料範圍");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(180 * scale);
		{
			std::string preview = (fileFilter == 0) ? u8"全部檔案" : model.files[fileFilter - 1].name;
			if (ImGui::BeginCombo("##filefilter", preview.c_str())) {
				if (ImGui::Selectable(u8"全部檔案", fileFilter == 0)) { fileFilter = 0; rebuildFilter(); }
				for (size_t i = 0; i < model.files.size(); i++) {
					bool sel = (fileFilter == (int)i + 1);
					if (ImGui::Selectable(model.files[i].name.c_str(), sel)) { fileFilter = (int)i + 1; rebuildFilter(); }
				}
				ImGui::EndCombo();
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// --- search and commands ---
		float commandW = 330 * scale;
		float searchW = ImGui::GetContentRegionAvail().x - commandW;
		if (searchW < 220 * scale) searchW = 220 * scale;
		ImGui::SetNextItemWidth(searchW);
		if (ImGui::InputTextWithHint("##search", u8"搜尋 key 或翻譯…", &search)) {
			searchLower = to_lower_ascii(search);
			rebuildFilter();
		}

		ImGui::SameLine();
		if (ImGui::Button(u8"缺漏掃描")) { focusMiss = true; runScan(); }
		ImGui::SameLine();
		int dirty = DirtyCount(model);
		{
			std::string saveLabel = dirty > 0 ? (std::string(u8"儲存全部 (") + std::to_string(dirty) + ")") : u8"儲存全部";
			ImGui::BeginDisabled(dirty == 0);
			PobUi::PushPrimaryButton();
			if (ImGui::Button(saveLabel.c_str())) {
				std::string err;
				int saved = SaveAll(model, &err);
				status = err.empty() ? (std::string(u8"已儲存 ") + std::to_string(saved) + u8" 個檔案")
				                     : (std::string(u8"儲存失敗：") + err);
			}
			PobUi::PopButtonStyle();
			ImGui::EndDisabled();
		}
		ImGui::SameLine();
		if (ImGui::Button(u8"重新載入")) {
			if (DirtyCount(model) > 0) pending = Pending::Reload;
			else reload();
		}

		// --- status strip ---
		ImGui::Spacing();
		if (!cjkOk) {
			// Kept ASCII: when the CJK atlas failed, Chinese glyphs cannot render.
			ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f),
				"[!] CJK font atlas not loaded (Fonts\\FZ_ZY.ttf missing or texture too large). Chinese cannot display/input.");
		}
		if (!model.localeExists) {
			ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), u8"此語系尚無翻譯資料：%s", narrow(model.dataDir).c_str());
		} else {
			ImGui::TextDisabled(u8"%zu 個檔案  ·  顯示 %zu / %zu 筆",
				model.files.size(), filtered.size(), model.entries.size());
			if (dirty > 0) {
				ImGui::SameLine(0, 16 * scale);
				ImGui::TextColored(PobUi::StatusColor(PobUi::StatusTone::Warning), u8"%d 筆待儲存", dirty);
			}
			if (!status.empty()) {
				ImGui::SameLine(0, 16 * scale);
				PobUi::StatusTone tone = status.find(u8"失敗") == std::string::npos
					? PobUi::StatusTone::Success : PobUi::StatusTone::Error;
				ImGui::TextColored(PobUi::StatusColor(tone), "%s", status.c_str());
			}
		}
		ImGui::Spacing();

		// --- tabs: entries / missing-string scan ------------------------------
		// The scan used to live in its own floating window, which left it a
		// fraction of the editor's width — too narrow for a long English
		// string plus a file picker plus a translation box on one row. As a
		// tab it gets the whole window.
		if (model.localeExists && ImGui::BeginTabBar("##editortabs")) {

		ImGuiTabItemFlags entriesFlags = focusEntries ? ImGuiTabItemFlags_SetSelected : 0;
		focusEntries = false;
		if (ImGui::BeginTabItem(u8"翻譯條目", nullptr, entriesFlags)) {

		// --- add a new entry --------------------------------------------------
		// Above the table on purpose: the table is an ImGuiListClipper over 110k
		// rows whose indices address `filtered` directly, and the warning below
		// needs a second line that would not fit in a cell anyway.
		//
		// Until now a key could only be added if the ENGINE had already logged it
		// as missing, so anything that never rendered was unreachable.
		{
			if (newTarget < 0 || newTarget >= (int)model.files.size()) {
				int def = FindFileIdx(model, "ui.json");
				newTarget = (def >= 0) ? def : (model.files.empty() ? -1 : 0);
			}
			if (newKeyOwnersFor != newKey) {   // cache: a linear scan of 110k rows
				newKeyOwners = FilesContaining(model, newKey);
				newKeyOwnersFor = newKey;
			}
			const int winner = newKeyOwners.empty() ? -1 : newKeyOwners.back();
			const bool inTarget = std::find(newKeyOwners.begin(), newKeyOwners.end(),
			                                newTarget) != newKeyOwners.end();

			ImGui::TextDisabled(u8"新增");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(280 * scale);
			if (focusNewKey) { ImGui::SetKeyboardFocusHere(); focusNewKey = false; }
			ImGui::InputTextWithHint("##newkey", u8"Key（英文，需與 POB 完全相同）", &newKey);
			ImGui::SameLine();
			ImGui::TextDisabled(u8"寫入");
			ImGui::SameLine();
			TargetFileCombo(model, "##newtarget", newTarget, scale);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90 * scale);
			const bool entered = ImGui::InputTextWithHint("##newval", u8"翻譯", &newVal,
			                                              ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			const bool canAdd = !newKey.empty() && !newVal.empty() && newTarget >= 0;
			ImGui::BeginDisabled(!canAdd);
			PobUi::PushPrimaryButton();
			const bool pressed = ImGui::Button(inTarget ? u8"覆蓋" : u8"新增", ImVec2(80 * scale, 0));
			PobUi::PopButtonStyle();
			ImGui::EndDisabled();

			if (canAdd && (pressed || entered)) {
				SetEntry(model, newTarget, newKey, newVal);
				status = std::string(inTarget ? u8"已覆蓋 " : u8"已新增 ") +
				         model.files[newTarget].name + u8"：" + newKey;
				search = newKey;                       // make the new row visible
				searchLower = to_lower_ascii(search);
				rebuildFilter();
				newKey.clear(); newVal.clear();
				newKeyOwners.clear(); newKeyOwnersFor.clear();
				focusNewKey = true;                    // ready for the next one
			}

			// Which copy the engine will actually use. Editing a shadowed copy
			// changes nothing, and nothing in the UI used to say so.
			if (newKey.empty()) {
				ImGui::TextDisabled(u8" ");
			} else if (newKeyOwners.empty()) {
				ImGui::TextDisabled(u8"新條目");
			} else if (inTarget) {
				std::string old;
				for (const EditorEntry& e : model.entries)
					if (e.fileIdx == newTarget && e.key == newKey) { old = e.value; break; }
				ImGui::TextColored(PobUi::StatusColor(PobUi::StatusTone::Warning),
					u8"%s 已有這個 key，將覆蓋現有翻譯：%s",
					model.files[newTarget].name.c_str(), old.c_str());
			} else {
				std::string others;
				for (int f : newKeyOwners) {
					if (!others.empty()) others += u8"、";
					others += model.files[f].name;
				}
				const bool targetWins = (newTarget >= 0 && winner >= 0 &&
				                         model.files[newTarget].order > model.files[winner].order);
				if (targetWins) {
					ImGui::TextDisabled(u8"此 key 也在 %s；引擎會採用你選的 %s",
						others.c_str(), model.files[newTarget].name.c_str());
				} else {
					ImGui::TextColored(PobUi::StatusColor(PobUi::StatusTone::Warning),
						u8"寫進 %s 不會生效，引擎會用 %s 的版本",
						model.files[newTarget].name.c_str(), model.files[winner].name.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton((std::string(u8"改寫入 ") +
					                        model.files[winner].name).c_str()))
						newTarget = winner;
				}
			}
		}
		ImGui::Spacing();

		// --- entry table (virtualized) ---
		{
			ImGuiTableFlags tflags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
			if (ImGui::BeginTable("##entries", 3, tflags, ImVec2(0, 0))) {
				ImGui::TableSetupColumn(u8"來源", ImGuiTableColumnFlags_WidthFixed, 70 * scale);
				ImGui::TableSetupColumn(u8"Key（英文）", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupColumn(u8"翻譯", ImGuiTableColumnFlags_WidthStretch, 0.55f);
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();

				ImGuiListClipper clipper;
				clipper.Begin((int)filtered.size());
				while (clipper.Step()) {
					for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
						size_t ei = filtered[row];
						EditorEntry& e = model.entries[ei];
						ImGui::TableNextRow();
						ImGui::PushID((int)ei);

						ImGui::TableSetColumnIndex(0);
						{
							std::string label = model.files[e.fileIdx].name;
							size_t dot = label.rfind(".json");
							if (dot != std::string::npos) label.erase(dot);
							BadgeText(label, BadgeColor(model.files[e.fileIdx].name));
						}

						ImGui::TableSetColumnIndex(1);
						// TextPobColored emits one item per colour run, so the
						// hover test has to cover the group, not the last run.
						ImGui::BeginGroup();
						if (has_pob_color(e.key))
							TextPobColored(e.key, ImGui::GetStyleColorVec4(ImGuiCol_Text));
						else
							ImGui::TextUnformatted(e.key.c_str());
						ImGui::EndGroup();
						if (ImGui::IsItemHovered() && ImGui::GetIO().KeyShift == false) {
							ImGui::BeginTooltip();
							ImGui::PushTextWrapPos(500 * scale);
							ImGui::TextUnformatted(e.key.c_str());
							ImGui::PopTextWrapPos();
							ImGui::EndTooltip();
						}

						ImGui::TableSetColumnIndex(2);
						// Long or multi-line values (the About paragraph is four
						// lines) get an expand button; the rule lives in
						// editor_data so it can be asserted headlessly.
						const bool big = NeedsExpandedEditor(e);
						ImGui::SetNextItemWidth(big ? -(28.0f * scale) : -FLT_MIN);
						if (ImGui::InputText("##v", &e.value))
							model.files[e.fileIdx].dirty = true;
						if (big) {
							ImGui::SameLine(0, 4 * scale);
							// Plain ASCII: this window builds its own glyph atlas
							// from the dictionary text, and a decorative arrow is
							// exactly the kind of character that renders as '?'.
							if (ImGui::SmallButton("...")) {
								bigIdx = (int)ei;
								bigText = e.value;
								openBig = true;
							}
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip(u8"展開編輯（內容較長或有換行）");
						}
						// The box has to keep the raw escapes so they can be
						// edited, so the rendered form goes underneath — this is
						// what POB will actually draw.
						if (has_pob_color(e.value))
							TextPobColored(e.value, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

						ImGui::PopID();
					}
				}
				ImGui::EndTable();
			}
		}
		ImGui::EndTabItem();
		}

		// --- missing-string scan ---------------------------------------------
		ImGuiTabItemFlags missFlags = focusMiss ? ImGuiTabItemFlags_SetSelected : 0;
		focusMiss = false;
		std::string missTabLabel = u8"缺漏掃描";
		if (missScanned && !misses.empty())
			missTabLabel += " (" + std::to_string(misses.size()) + ")";
		if (ImGui::BeginTabItem(missTabLabel.c_str(), nullptr, missFlags)) {
			if (ImGui::Button(u8"重新掃描")) runScan();
			ImGui::SameLine();
			ImGui::Checkbox(u8"顯示反查失敗 (REV)", &missShowReverse);
			ImGui::SameLine(0, 16 * scale);
			int removeIdx = -1;
			bool applyAll = false;
			if (ImGui::Button(u8"全部補上（已填寫者）")) applyAll = true;

			if (!missScanned) {
				ImGui::Spacing();
				ImGui::TextDisabled(u8"按「重新掃描」讀取 translate_misses.log。");
			} else if (!missLogFound) {
				ImGui::TextColored(ImVec4(0.94f, 0.67f, 0.27f, 1.0f),
					u8"找不到 translate_misses.log，請先啟動一次 POB 並操作介面。");
			}

			int shown = 0;
			for (const MissEntry& m : misses) if (missShowReverse || !m.reverse) shown++;
			ImGui::TextDisabled(u8"缺漏 %d 筆（共掃到 %zu）", shown, misses.size());
			ImGui::Spacing();

			// A table rather than a run of SameLine widgets: the English text
			// can be a whole sentence, and only a real column keeps the file
			// picker and the translation box aligned when it wraps.
			ImGuiTableFlags mflags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
			if (ImGui::BeginTable("##misses", 4, mflags, ImVec2(0, 0))) {
				ImGui::TableSetupColumn(u8"未翻譯字串", ImGuiTableColumnFlags_WidthStretch, 0.50f);
				ImGui::TableSetupColumn(u8"寫入檔案", ImGuiTableColumnFlags_WidthFixed, 150 * scale);
				ImGui::TableSetupColumn(u8"翻譯", ImGuiTableColumnFlags_WidthStretch, 0.50f);
				ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 64 * scale);
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();

				for (size_t i = 0; i < misses.size(); i++) {
					if (!missShowReverse && misses[i].reverse) continue;
					ImGui::TableNextRow();
					ImGui::PushID((int)i);

					ImGui::TableSetColumnIndex(0);
					if (misses[i].reverse) {
						BadgeText("REV", ImColor(0.85f, 0.6f, 0.3f, 1.0f));
						ImGui::SameLine();
					}
					if (has_pob_color(misses[i].text)) {
						TextPobColored(misses[i].text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
					} else {
						ImGui::PushTextWrapPos(0.0f);
						ImGui::TextUnformatted(misses[i].text.c_str());
						ImGui::PopTextWrapPos();
					}

					ImGui::TableSetColumnIndex(1);
					// Same picker as the add row: alphabetical order told the
					// user nothing about which copy the engine would read.
					TargetFileCombo(model, "##tgt", missTarget[i], scale);

					ImGui::TableSetColumnIndex(2);
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::InputTextWithHint("##mt", u8"輸入翻譯…", &missTrans[i]);

					ImGui::TableSetColumnIndex(3);
					bool canAdd = missTarget[i] >= 0 && !missTrans[i].empty();
					ImGui::BeginDisabled(!canAdd);
					if (ImGui::Button(u8"補上", ImVec2(-FLT_MIN, 0))) {
						SetEntry(model, missTarget[i], misses[i].text, missTrans[i]);
						removeIdx = (int)i;
					}
					ImGui::EndDisabled();

					ImGui::PopID();
				}
				ImGui::EndTable();
			}

			if (applyAll) {
				for (int i = (int)misses.size() - 1; i >= 0; i--) {
					if (missTarget[i] >= 0 && !missTrans[i].empty()) {
						SetEntry(model, missTarget[i], misses[i].text, missTrans[i]);
						misses.erase(misses.begin() + i);
						missTrans.erase(missTrans.begin() + i);
						missTarget.erase(missTarget.begin() + i);
					}
				}
				rebuildFilter();
			} else if (removeIdx >= 0) {
				misses.erase(misses.begin() + removeIdx);
				missTrans.erase(missTrans.begin() + removeIdx);
				missTarget.erase(missTarget.begin() + removeIdx);
				rebuildFilter();
			}
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
		}

		ImGui::End();

		// --- expanded value editor -------------------------------------------
		// Opened here, at the root ID level, for the same reason as the guard
		// below: OpenPopup inside the table's ID scope would never match this
		// BeginPopupModal. The row only records which entry to edit.
		if (openBig) { ImGui::OpenPopup(u8"編輯內容"); openBig = false; }
		if (ImGui::BeginPopupModal(u8"編輯內容", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			if (bigIdx >= 0 && bigIdx < (int)model.entries.size()) {
				EditorEntry& e = model.entries[bigIdx];
				ImGui::PushTextWrapPos(760 * scale);
				ImGui::TextDisabled(u8"Key（英文）");
				ImGui::TextUnformatted(e.key.c_str());
				ImGui::PopTextWrapPos();
				ImGui::Spacing();
				ImGui::TextDisabled(u8"翻譯（可多行；Enter 換行）");
				ImGui::InputTextMultiline("##bigval", &bigText,
				                          ImVec2(780 * scale, 240 * scale));
				if (has_pob_color(bigText)) {
					ImGui::TextDisabled(u8"顯示效果：");
					TextPobColored(bigText, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				}
				ImGui::Spacing();
				PobUi::PushPrimaryButton();
				if (ImGui::Button(u8"套用", ImVec2(110 * scale, 0))) {
					if (bigText != e.value) {
						e.value = bigText;
						model.files[e.fileIdx].dirty = true;
					}
					bigIdx = -1;
					ImGui::CloseCurrentPopup();
				}
				PobUi::PopButtonStyle();
				ImGui::SameLine();
				if (ImGui::Button(u8"取消", ImVec2(110 * scale, 0))) {
					bigIdx = -1;
					ImGui::CloseCurrentPopup();
				}
			} else {
				// The filter or a reload moved the ground under the popup.
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// --- unsaved-changes guard -------------------------------------------
		// Opened here, after End(), so the popup sits at the root ID level; the
		// combos above only set `pending`. Switching game used to discard unsaved
		// edits without a word.
		if (glfwWindowShouldClose(win)) {
			if (DirtyCount(model) > 0) {
				glfwSetWindowShouldClose(win, GLFW_FALSE);
				pending = Pending::Close;
			} else {
				running = false;
			}
		}
		if (pending != Pending::None && !ImGui::IsPopupOpen(u8"未儲存變更"))
			ImGui::OpenPopup(u8"未儲存變更");

		if (ImGui::BeginPopupModal(u8"未儲存變更", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text(u8"有 %d 個檔案尚未儲存。", DirtyCount(model));
			// Name the action: by the time the prompt appears the user may not
			// remember which button they pressed.
			switch (pending) {
				case Pending::SwitchGame:
					ImGui::TextUnformatted((std::string(u8"即將切換到 ") + kGames[pendingGi] +
					                        u8"，未儲存的修改會遺失。").c_str());
					break;
				case Pending::SwitchLocale:
					ImGui::TextUnformatted((std::string(u8"即將切換到 ") + locales[pendingLi] +
					                        u8"，未儲存的修改會遺失。").c_str());
					break;
				case Pending::Reload:
					ImGui::TextUnformatted(u8"即將重新載入，未儲存的修改會遺失。");
					break;
				default:
					ImGui::TextUnformatted(u8"即將關閉編輯器。");
					break;
			}
			ImGui::Spacing();

			const bool closing = (pending == Pending::Close);
			PobUi::PushPrimaryButton();
			if (ImGui::Button(closing ? u8"儲存並關閉" : u8"儲存後繼續")) {
				std::string err;
				SaveAll(model, &err);
				if (closing) running = false; else applyPending();
				pending = Pending::None;
				ImGui::CloseCurrentPopup();
			}
			PobUi::PopButtonStyle();
			ImGui::SameLine();
			// The only button here that throws work away.
			PobUi::PushDangerButton();
			if (ImGui::Button(closing ? u8"直接關閉" : u8"捨棄並繼續")) {
				if (closing) running = false; else applyPending();
				pending = Pending::None;
				ImGui::CloseCurrentPopup();
			}
			PobUi::PopButtonStyle();
			ImGui::SameLine();
			if (ImGui::Button(u8"取消")) {
				pending = Pending::None;   // combos already rolled themselves back
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGui::PopFont();
		ImGui::Render();
		int fbW = 0, fbH = 0;
		glfwGetFramebufferSize(win, &fbW, &fbH);
		glViewport(0, 0, fbW, fbH);
		glClearColor(0.043f, 0.063f, 0.078f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(win);
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
}
