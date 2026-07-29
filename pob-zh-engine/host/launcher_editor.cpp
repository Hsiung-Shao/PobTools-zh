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
	static const char* kGames[2] = { "poe1", "poe2" };
	static const char* kLocales[1] = { "zh-rTW" };
	int gi = (game == L"poe2") ? 1 : 0;
	int li = 0;  // only zh-rTW is offered
	(void)locale;

	EditorModel model = LoadModel(exeDir, kGames[gi], kLocales[li]);

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

	std::string status;

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

	auto reload = [&]() {
		model = LoadModel(exeDir, kGames[gi], kLocales[li]);
		fileFilter = 0;
		rebuildFilter();
		missScanned = false;
		misses.clear(); missTrans.clear(); missTarget.clear();
		status.clear();
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
		ImGui::SetNextItemWidth(90 * scale);
		if (ImGui::Combo("##game", &gi, kGames, 2)) reload();
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110 * scale);
		if (ImGui::Combo("##locale", &li, kLocales, 1)) reload();

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
		if (ImGui::Button(u8"重新載入")) reload();

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
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (ImGui::InputText("##v", &e.value))
							model.files[e.fileIdx].dirty = true;
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
					ImGui::SetNextItemWidth(-FLT_MIN);
					{
						int& tgt = missTarget[i];
						std::string preview = (tgt >= 0 && tgt < (int)model.files.size())
							? model.files[tgt].name : u8"選擇檔案";
						if (ImGui::BeginCombo("##tgt", preview.c_str())) {
							for (size_t f = 0; f < model.files.size(); f++)
								if (ImGui::Selectable(model.files[f].name.c_str(), tgt == (int)f)) tgt = (int)f;
							ImGui::EndCombo();
						}
					}

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

		// --- unsaved-changes guard on close ---
		if (glfwWindowShouldClose(win)) {
			if (DirtyCount(model) > 0) {
				glfwSetWindowShouldClose(win, GLFW_FALSE);
				ImGui::OpenPopup(u8"未儲存變更");
			} else {
				running = false;
			}
		}
		if (ImGui::BeginPopupModal(u8"未儲存變更", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted(u8"有尚未儲存的翻譯變更。");
			ImGui::Spacing();
			if (ImGui::Button(u8"儲存並關閉")) {
				std::string err;
				SaveAll(model, &err);
				ImGui::CloseCurrentPopup();
				running = false;
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"直接關閉")) { ImGui::CloseCurrentPopup(); running = false; }
			ImGui::SameLine();
			if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
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
