#include "warehouse_tool.h"

#include "error_log.h"
#include "filter_i18n.h"
#include "icon_manager.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_theme.h"
#include "warehouse_service.h"
#include "warehouse_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// 倉庫收益統計 — paste a POESESSID, pick tabs, snapshot the stash, watch the
// value move. The panel is drawing and bookkeeping only: every network byte and
// every price lives in WarehouseService's worker.

namespace {

const ImVec4 kWarn(0.95f, 0.66f, 0.25f, 1.0f);
const ImVec4 kBad(0.94f, 0.27f, 0.27f, 1.0f);
const ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);
const ImVec4 kDim(0.62f, 0.66f, 0.70f, 1.0f);

std::string NarrowUtf8(const std::wstring& w)
{
	std::string out;
	out.reserve(w.size());
	for (wchar_t c : w) out += (c > 0 && c < 128) ? (char)c : '?';
	return out;
}

long long NowUtc()
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return (long long)((t - 116444736000000000ull) / 10000000ull);
}

std::string FormatUtcLocal(long long utc)
{
	// Shown in the user's local time; the file stores UTC.
	FILETIME ft;
	unsigned long long t = (unsigned long long)utc * 10000000ull + 116444736000000000ull;
	ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFF);
	ft.dwHighDateTime = (DWORD)(t >> 32);
	FILETIME lt;
	FileTimeToLocalFileTime(&ft, &lt);
	SYSTEMTIME st;
	FileTimeToSystemTime(&lt, &st);
	char buf[32];
	snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", st.wMonth, st.wDay, st.wHour,
	         st.wMinute);
	return buf;
}

std::string FormatChaos(double v)
{
	char buf[48];
	if (v >= 1000 || v <= -1000) snprintf(buf, sizeof(buf), "%.0f", v);
	else snprintf(buf, sizeof(buf), "%.1f", v);
	return buf;
}

// One value, in the display currency. Divine mode converts only what is worth
// at least one divine -- a 0.8c essence shown as "0.00 d" reads as zero, so
// sub-divine values stay in chaos (the Wealthy-Exile convention). divineRate 0
// forces chaos for everything.
std::string FormatValue(double chaos, bool divine, double divineRate)
{
	const double mag = chaos < 0 ? -chaos : chaos;
	if (divine && divineRate > 0 && mag >= divineRate) {
		char buf[48];
		const double d = chaos / divineRate;
		if (mag >= 100 * divineRate) snprintf(buf, sizeof(buf), "%.0f d", d);
		else snprintf(buf, sizeof(buf), "%.1f d", d);
		return buf;
	}
	return FormatChaos(chaos) + " c";
}

class WarehousePanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& host) override
	{
		host_ = &host;
		exeDir_ = host.exeDir;
		state_.Load(exeDir_);
		history_.Load(exeDir_);
		i18n_.Load(exeDir_, NarrowUtf8(host.locale));
		icons_.Init(exeDir_);
		svc_.Init(exeDir_);
		// No network here: the launcher Inits every panel it opens (and the panel
		// selftest Inits all of them); requests wait for a click.
		return true;
	}

	void Frame() override
	{
		icons_.Pump();
		st_ = svc_.Poll();

		// A finished snapshot is folded into the history right here, so the file
		// on disk is always one frame behind at most.
		if (st_.snapshotReady) {
			Snapshot snap;
			if (svc_.TakeSnapshot(&snap)) {
				if (history_.sessionStartUtc == 0) history_.sessionStartUtc = snap.utc;
				history_.snaps.push_back(std::move(snap));
				history_.Prune(NowUtc());
				if (!history_.Save(exeDir_))
					PobLog::Error("warehouse", u8"快照歷史存檔失敗");
				svc_.AckDone();
				st_ = svc_.Poll();
			}
		}
		if (st_.tabsReady && tabs_.empty()) tabs_ = st_.tabs;
		else if (st_.tabsReady && st_.tabs.size() != tabs_.size()) tabs_ = st_.tabs;
		if (st_.leaguesReady) leagues_ = st_.leagues;

		maybeAutoSnapshot();

		const float scale = host_->scale;
		ImGui::BeginChild("##wh_settings", ImVec2(330.0f * scale, 0), true);
		drawSettings();
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##wh_main", ImVec2(0, 0), false);
		drawMain();
		ImGui::EndChild();
	}

	ToolCloseState RequestClose() override
	{
		saveState();
		if (!history_.snaps.empty()) history_.Save(exeDir_);
		close_ = ToolCloseState::Closed;
		return close_;
	}
	ToolCloseState CloseState() const override { return close_; }
	void AbortClose() override { close_ = ToolCloseState::Open; }

	void Shutdown() override
	{
		svc_.Shutdown();
		icons_.Shutdown();
	}

	PobUi::Density Density() const override { return PobUi::Density::Compact; }
	const char* PanelId() const override { return "warehouse"; }

private:
	bool busy() const
	{
		return st_.phase != WarehousePhase::Idle && st_.phase != WarehousePhase::Done &&
		       st_.phase != WarehousePhase::Error;
	}

	void saveState()
	{
		if (!state_.Save(exeDir_))
			PobLog::Error("warehouse", u8"設定存檔失敗");
	}

	void maybeAutoSnapshot()
	{
		if (state_.autoMinutes <= 0 || busy() || !st_.authOk) return;
		if (state_.sessid.empty() || state_.league.empty() || state_.selectedTabIds.empty())
			return;
		const long long last = history_.snaps.empty() ? 0 : history_.snaps.back().utc;
		if (last == 0) return; // the first snapshot is always a deliberate click
		if (NowUtc() - last < (long long)state_.autoMinutes * 60) return;
		// Done/Error linger until acknowledged; an auto shot must not wait for one.
		svc_.AckDone();
		requestSnapshot();
	}

	void requestSnapshot()
	{
		StashAuth a;
		a.accountName = state_.accountName;
		a.secret = state_.sessid;
		svc_.SetAuth(a);
		svc_.RequestSnapshot(state_.league, state_.selectedTabIds, true);
	}

	// ---- left column ------------------------------------------------------

	void drawSettings()
	{
		ImGui::TextColored(kWarn, u8"測試性質功能");
		ImGui::TextWrapped(u8"以 POESESSID 讀取國際服 (PoE1) 倉庫。session id 等同帳號"
		                   u8"登入權杖，僅以 Windows 使用者加密（DPAPI）存於本機，"
		                   u8"請勿分享給任何人。日後將改接官方授權通道。");
		ImGui::Separator();

		ImGui::Text(u8"帳號名稱");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputText("##wh_acct", &state_.accountName)) stateDirty_ = true;
		ImGui::TextColored(kDim, u8"帳號頁的名稱，如 Name#1234");

		ImGui::Text("POESESSID");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputText("##wh_sessid", &state_.sessid,
		                     ImGuiInputTextFlags_Password))
			stateDirty_ = true;
		ImGui::TextColored(kDim, u8"瀏覽器登入官網後 Cookie 內的 POESESSID");

		ImGui::BeginDisabled(busy() || state_.sessid.empty() || state_.accountName.empty());
		if (ImGui::Button(u8"驗證 session")) {
			StashAuth a;
			a.accountName = state_.accountName;
			a.secret = state_.sessid;
			svc_.SetAuth(a);
			svc_.AckDone();
			svc_.RequestVerify();
			saveState();
			stateDirty_ = false;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (st_.authOk) ImGui::TextColored(kGood, u8"有效");
		else if (st_.authFailed) ImGui::TextColored(kBad, u8"無效");
		else ImGui::TextColored(kDim, u8"未驗證");

		ImGui::Separator();
		ImGui::Text(u8"聯盟");
		ImGui::SetNextItemWidth(-40.0f * host_->scale);
		if (leagues_.empty()) {
			if (ImGui::InputText("##wh_league", &state_.league)) stateDirty_ = true;
		} else {
			if (ImGui::BeginCombo("##wh_league_c", state_.league.c_str())) {
				for (const std::string& l : leagues_) {
					if (ImGui::Selectable(l.c_str(), l == state_.league)) {
						if (state_.league != l) {
							state_.league = l;
							state_.selectedTabIds.clear();
							tabs_.clear();
							stateDirty_ = true;
						}
					}
				}
				ImGui::EndCombo();
			}
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(busy());
		if (ImGui::Button(u8"更新##leagues")) {
			svc_.AckDone();
			svc_.RequestLeagues();
		}
		ImGui::EndDisabled();

		ImGui::Separator();
		ImGui::BeginDisabled(busy() || !st_.authOk || state_.league.empty());
		if (ImGui::Button(u8"取得倉庫分頁清單")) {
			StashAuth a;
			a.accountName = state_.accountName;
			a.secret = state_.sessid;
			svc_.SetAuth(a);
			svc_.AckDone();
			svc_.RequestTabList(state_.league);
		}
		ImGui::EndDisabled();

		if (!tabs_.empty()) {
			ImGui::TextColored(kDim, u8"勾選要統計的分頁（%d 個已選）",
			                   (int)state_.selectedTabIds.size());
			ImGui::BeginChild("##wh_tabs", ImVec2(0, 220.0f * host_->scale), true);
			for (const StashTabInfo& t : tabs_) {
				bool sel = std::find(state_.selectedTabIds.begin(),
				                     state_.selectedTabIds.end(),
				                     t.id) != state_.selectedTabIds.end();
				std::string label = t.name + "##" + t.id;
				if (ImGui::Checkbox(label.c_str(), &sel)) {
					if (sel) {
						state_.selectedTabIds.push_back(t.id);
					} else {
						state_.selectedTabIds.erase(
						    std::remove(state_.selectedTabIds.begin(),
						                state_.selectedTabIds.end(), t.id),
						    state_.selectedTabIds.end());
					}
					stateDirty_ = true;
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("#%d %s", t.index, t.type.c_str());
			}
			ImGui::EndChild();
		}

		ImGui::Separator();
		ImGui::Text(u8"自動快照");
		static const int kMinuteChoices[] = { 0, 5, 10, 15, 30 };
		std::string autoLabel = state_.autoMinutes <= 0
		                            ? std::string(u8"關閉")
		                            : (std::to_string(state_.autoMinutes) + u8" 分鐘");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##wh_auto", autoLabel.c_str())) {
			for (int m : kMinuteChoices) {
				std::string lab = m == 0 ? std::string(u8"關閉")
				                         : (std::to_string(m) + u8" 分鐘");
				if (ImGui::Selectable(lab.c_str(), state_.autoMinutes == m)) {
					state_.autoMinutes = m;
					stateDirty_ = true;
					saveState();
					stateDirty_ = false;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::TextColored(kDim, u8"最短 5 分鐘：官方 API 有頻率限制");

		if (stateDirty_ && !busy()) {
			// Settings persist on change, same as the launcher's own rule; the
			// session id rides along as a DPAPI blob.
			saveState();
			stateDirty_ = false;
		}
	}

	// ---- right column -----------------------------------------------------

	// The selected snapshot (sidebar click), and the one taken just before it
	// -- the pair every "change" view compares.
	const Snapshot* selectedSnap() const
	{
		if (history_.snaps.empty()) return nullptr;
		if (selSnapUtc_ != 0)
			for (const Snapshot& s : history_.snaps)
				if (s.utc == selSnapUtc_) return &s;
		return &history_.snaps.back();
	}
	const Snapshot* snapBefore(const Snapshot* s) const
	{
		if (!s) return nullptr;
		const Snapshot* prev = nullptr;
		for (const Snapshot& c : history_.snaps) {
			if (c.utc >= s->utc) break;
			prev = &c;
		}
		return prev;
	}

	void drawMain()
	{
		const bool canSnap = !busy() && st_.authOk && !state_.league.empty() &&
		                     !state_.selectedTabIds.empty() && !state_.sessid.empty();
		ImGui::BeginDisabled(!canSnap);
		if (ImGui::Button(u8"立即快照", ImVec2(120.0f * host_->scale, 0))) {
			svc_.AckDone();
			requestSnapshot();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		drawStatusLine();

		if (history_.snaps.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(kDim, u8"尚無快照。填好左側設定後按「立即快照」。");
			return;
		}

		drawTopCards();
		drawCurve();

		const Snapshot* sel = selectedSnap();
		const Snapshot* prev = snapBefore(sel);

		// Wealthy-Exile split: snapshot timeline on the left, the selected
		// snapshot's content on the right.
		ImGui::BeginChild("##wh_snapside", ImVec2(185.0f * host_->scale, 0), true);
		drawSnapSidebar(sel);
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##wh_content", ImVec2(0, 0), false);
		static const char* kCats[] = { u8"全部",   u8"通貨", u8"寶石", u8"命運卡",
			                           u8"傳奇",   u8"地圖", u8"其他", u8"未估價" };
		ImGui::SetNextItemWidth(110.0f * host_->scale);
		if (ImGui::Combo(u8"類別", &catFilter_, kCats, 8)) filterDirty_ = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(200.0f * host_->scale);
		if (ImGui::InputTextWithHint("##wh_search", u8"搜尋名稱…", &search_))
			filterDirty_ = true;
		if (ImGui::BeginTabBar("##wh_tabsbar")) {
			if (ImGui::BeginTabItem(u8"變化")) {
				drawChanges(prev, sel);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(u8"持有明細")) {
				drawLines(sel);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::EndChild();
	}

	void drawStatusLine()
	{
		switch (st_.phase) {
		case WarehousePhase::Idle:
			ImGui::TextColored(kDim, u8"待命");
			break;
		case WarehousePhase::FetchingTabs:
			ImGui::TextColored(kWarn, u8"%s (%d/%d)", st_.message.c_str(), st_.tabsDone,
			                   st_.tabsTotal);
			break;
		case WarehousePhase::Error:
			ImGui::TextColored(kBad, "%s", st_.message.c_str());
			if (st_.authFailed) {
				ImGui::SameLine();
				ImGui::TextColored(kWarn, u8"請重新輸入 POESESSID");
			}
			break;
		case WarehousePhase::Done:
			ImGui::TextColored(kGood, "%s", st_.message.c_str());
			break;
		default:
			ImGui::TextColored(kWarn, "%s", st_.message.c_str());
			break;
		}
	}

	// One "label: value" row inside a card, value right-aligned and coloured.
	void cardRow(const char* label, const std::string& value, const ImVec4* col)
	{
		ImGui::TextColored(kDim, "%s", label);
		ImGui::SameLine(110.0f * host_->scale);
		if (col) ImGui::TextColored(*col, "%s", value.c_str());
		else ImGui::TextUnformatted(value.c_str());
	}

	std::string signedValue(double chaos, double rate)
	{
		return (chaos >= 0 ? "+" : "") + FormatValue(chaos, state_.showDivine, rate);
	}

	// The Wealthy-Exile top strip: a session summary card and a breakdown card.
	void drawTopCards()
	{
		const Snapshot& latest = history_.snaps.back();
		const Snapshot* start = history_.FindByUtc(history_.sessionStartUtc);
		if (!start) start = &history_.snaps.front();
		const double rate = latest.divineRate;
		const SnapshotDiff d = DiffSnapshots(*start, latest);

		double revenue = 0, cost = 0;
		for (const SnapshotDiffLine& l : d.gained) revenue += l.dChaos;
		for (const SnapshotDiffLine& l : d.lost) cost += l.dChaos; // negative

		// Tall enough for every row the snapshot card draws: title + separator +
		// six label rows, plus the child's own padding. Derived from the real
		// line height so a font or scale change cannot clip the bottom rows.
		const float cardH = ImGui::GetTextLineHeightWithSpacing() * 7.0f +
		                    ImGui::GetStyle().WindowPadding.y * 2.0f +
		                    10.0f * host_->scale;
		const float half = (ImGui::GetContentRegionAvail().x - 8.0f * host_->scale) * 0.5f;

		ImGui::BeginChild("##wh_card_snap", ImVec2(half, cardH), true);
		ImGui::TextUnformatted(u8"快照區間");
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 110.0f * host_->scale);
		ImGui::TextColored(d.chaosPerHour >= 0 ? kGood : kBad, "%s/hr",
		                   signedValue(d.chaosPerHour, rate).c_str());
		ImGui::Separator();
		cardRow(u8"起點", FormatUtcLocal(start->utc), nullptr);
		cardRow(u8"最新", FormatUtcLocal(latest.utc), nullptr);
		{
			int mins = (int)((latest.utc - start->utc) / 60);
			char buf[32];
			snprintf(buf, sizeof(buf), "%dh %02dm", mins / 60, mins % 60);
			cardRow(u8"經過", buf, nullptr);
		}
		cardRow(u8"收益", signedValue(revenue, rate), &kGood);
		cardRow(u8"支出", signedValue(cost, rate), &kBad);
		{
			const ImVec4& col = d.dTotalChaos >= 0 ? kGood : kBad;
			cardRow(u8"淨值", signedValue(d.dTotalChaos, rate), &col);
		}
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::BeginChild("##wh_card_break", ImVec2(0, cardH), true);
		ImGui::TextUnformatted(u8"明細（自起點的主要變動）");
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 150.0f * host_->scale);
		ImGui::TextColored(kDim, u8"總值 %s",
		                   FormatValue(latest.totalChaos, state_.showDivine, rate).c_str());
		ImGui::Separator();
		// Both lists are |dChaos|-descending; a two-pointer merge yields the
		// overall top movers without re-sorting.
		size_t gi = 0, li = 0;
		for (int shown = 0; shown < 4; shown++) {
			const SnapshotDiffLine* pick = nullptr;
			double ga = gi < d.gained.size() ? d.gained[gi].dChaos : -1;
			double la = li < d.lost.size() ? -d.lost[li].dChaos : -1;
			if (ga <= 0 && la <= 0) break;
			if (ga >= la) pick = &d.gained[gi++];
			else pick = &d.lost[li++];
			drawIconCell(pick->icon);
			ImGui::SameLine();
			const std::string zh = i18n_.DisplayName(pick->dispEn);
			ImGui::TextUnformatted(zh.c_str());
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 90.0f * host_->scale);
			ImGui::TextColored(pick->dChaos >= 0 ? kGood : kBad, "%s",
			                   signedValue(pick->dChaos, rate).c_str());
		}
		if (latest.unpricedKinds > 0)
			ImGui::TextColored(kDim, u8"另有 %d 種未估價", latest.unpricedKinds);
		ImGui::EndChild();
	}

	void drawCurve()
	{
		if (history_.snaps.size() < 2) return;
		// At most 256 points: an even-stride sample keeps the shape; the tooltip
		// below reads from the sampled arrays so hover and pixels agree.
		const size_t n = history_.snaps.size();
		const size_t stride = (n + 255) / 256;
		curvePts_.clear();
		curveUtc_.clear();
		for (size_t i = 0; i < n; i += stride) {
			curvePts_.push_back((float)history_.snaps[i].totalChaos);
			curveUtc_.push_back(history_.snaps[i].utc);
		}
		if (curveUtc_.back() != history_.snaps.back().utc) {
			curvePts_.push_back((float)history_.snaps.back().totalChaos);
			curveUtc_.push_back(history_.snaps.back().utc);
		}
		float lo = curvePts_[0], hi = curvePts_[0];
		for (float v : curvePts_) {
			lo = v < lo ? v : lo;
			hi = v > hi ? v : hi;
		}
		if (hi <= lo) hi = lo + 1.0f;

		// Hand-drawn area chart instead of PlotLines: the line takes the trend's
		// colour (up = green, down = red) with a translucent fill underneath.
		const float w = ImGui::GetContentRegionAvail().x;
		const float hgt = 64.0f * host_->scale;
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##wh_curve", ImVec2(w, hgt));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec4& base = curvePts_.back() >= curvePts_.front() ? kGood : kBad;
		const ImU32 lineCol = ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, 0.95f));
		const ImU32 fillCol = ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, 0.16f));
		dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + hgt),
		                  ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
		const int pts = (int)curvePts_.size();
		const float pad = 4.0f * host_->scale;
		auto ptAt = [&](int i) {
			float fx = pts > 1 ? (float)i / (float)(pts - 1) : 0.0f;
			float fy = (curvePts_[i] - lo) / (hi - lo);
			return ImVec2(p0.x + fx * w, p0.y + (1.0f - fy) * (hgt - 2.0f * pad) + pad);
		};
		const float baseY = p0.y + hgt - 2.0f;
		for (int i = 0; i + 1 < pts; i++) {
			ImVec2 a = ptAt(i), b = ptAt(i + 1);
			dl->AddQuadFilled(ImVec2(a.x, baseY), ImVec2(b.x, baseY), b, a, fillCol);
		}
		for (int i = 0; i + 1 < pts; i++)
			dl->AddLine(ptAt(i), ptAt(i + 1), lineCol, 2.0f);

		if (ImGui::IsItemHovered()) {
			float fx = (ImGui::GetIO().MousePos.x - p0.x) / (w > 1.0f ? w : 1.0f);
			int idx = (int)(fx * (pts - 1) + 0.5f);
			idx = idx < 0 ? 0 : idx >= pts ? pts - 1 : idx;
			// A dot marks the hovered sample so the tooltip has an anchor.
			dl->AddCircleFilled(ptAt(idx), 3.5f, lineCol);
			ImGui::SetTooltip("%s\n%s c", FormatUtcLocal(curveUtc_[idx]).c_str(),
			                  FormatChaos(curvePts_[idx]).c_str());
		}
	}

	void drawIconCell(const std::string& artPath)
	{
		if (artPath.empty()) {
			ImGui::Dummy(ImVec2(20.0f * host_->scale, 20.0f * host_->scale));
			return;
		}
		icons_.RequestPath(artPath);
		unsigned tex = icons_.TextureByPath(artPath);
		if (tex)
			ImGui::Image((ImTextureID)(intptr_t)tex,
			             ImVec2(20.0f * host_->scale, 20.0f * host_->scale));
		else
			ImGui::Dummy(ImVec2(20.0f * host_->scale, 20.0f * host_->scale));
	}

	// Category = the price-key's namespace, so the filter can never disagree
	// with how a line was priced.
	static bool LineInCategory(const SnapshotLine& l, int cat)
	{
		switch (cat) {
		case 1: return l.key.rfind("currency|", 0) == 0;
		case 2: return l.key.rfind("gem|", 0) == 0;
		case 3: return l.key.rfind("card|", 0) == 0;
		case 4: return l.key.rfind("unique|", 0) == 0;
		case 5: return l.key.rfind("map|", 0) == 0;
		case 6: return l.key.rfind("other|", 0) == 0;
		case 7: return !l.priced;
		default: return true;
		}
	}

	static std::string LowerAscii(std::string s)
	{
		for (char& c : s)
			if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
		return s;
	}

	// The rightmost cell of both tables: one click per language. Callers wrap
	// each row in PushID, which is what keeps these buttons' IDs apart.
	void drawCopyCell(const std::string& dispEn)
	{
		const std::string zh = i18n_.DisplayName(dispEn);
		if (ImGui::SmallButton(u8"中文")) ImGui::SetClipboardText(zh.c_str());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"複製:%s", zh.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"英文")) ImGui::SetClipboardText(dispEn.c_str());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"複製:%s", dispEn.c_str());
	}

	// The name cell shared by the detail and diff tables: 繁中 text, English
	// tooltip, and a right-click menu that copies either.
	void drawNameCell(const std::string& dispEn)
	{
		const std::string zh = i18n_.DisplayName(dispEn);
		ImGui::TextUnformatted(zh.c_str());
		if (zh != dispEn && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dispEn.c_str());
		if (ImGui::BeginPopupContextItem("##name_ctx")) {
			if (ImGui::MenuItem(u8"複製名稱")) ImGui::SetClipboardText(zh.c_str());
			if (zh != dispEn && ImGui::MenuItem(u8"複製英文名稱"))
				ImGui::SetClipboardText(dispEn.c_str());
			ImGui::EndPopup();
		}
	}

	void drawLines(const Snapshot* latest)
	{
		if (!latest) {
			ImGui::TextColored(kDim, u8"尚無快照。");
			return;
		}

		// Value-sorted, filtered view of the selected snapshot; rebuilt when the
		// snapshot or the filter changes.
		if (linesForUtc_ != latest->utc || filterDirty_) {
			const std::string needle = LowerAscii(search_);
			sortedLines_.clear();
			for (size_t i = 0; i < latest->lines.size(); i++) {
				const SnapshotLine& l = latest->lines[i];
				if (!LineInCategory(l, catFilter_)) continue;
				if (!needle.empty()) {
					// English matches case-insensitively; the 繁中 name matches
					// bytewise (CJK has no case to fold).
					if (LowerAscii(l.dispEn).find(needle) == std::string::npos &&
					    i18n_.DisplayName(l.dispEn).find(search_) == std::string::npos)
						continue;
				}
				sortedLines_.push_back((int)i);
			}
			linesForUtc_ = latest->utc;
			filterDirty_ = false;
		}
		const double rate = latest->divineRate;
		if (ImGui::BeginTable("##wh_lines", 6,
		                      ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
		                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("##ic",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_NoSort,
			                        24.0f * host_->scale);
			ImGui::TableSetupColumn(u8"物品", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(u8"數量",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        70.0f * host_->scale);
			ImGui::TableSetupColumn(u8"單價",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        90.0f * host_->scale);
			ImGui::TableSetupColumn(u8"小計",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_DefaultSort |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        100.0f * host_->scale);
			ImGui::TableSetupColumn(u8"複製",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_NoSort,
			                        96.0f * host_->scale);
			ImGui::TableHeadersRow();

			// Sorted per frame from the click-selected header (list is small).
			if (const ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
				if (sp->SpecsCount > 0) {
					const ImGuiTableColumnSortSpecs& s0 = sp->Specs[0];
					const bool asc = s0.SortDirection != ImGuiSortDirection_Descending;
					std::stable_sort(sortedLines_.begin(), sortedLines_.end(),
					                 [&](int a, int b) {
						                 const SnapshotLine& x = latest->lines[a];
						                 const SnapshotLine& y = latest->lines[b];
						                 int cmp = 0;
						                 switch (s0.ColumnIndex) {
						                 case 1:
							                 cmp = i18n_.DisplayName(x.dispEn)
							                           .compare(i18n_.DisplayName(y.dispEn));
							                 break;
						                 case 2:
							                 cmp = x.count < y.count ? -1
							                       : x.count > y.count ? 1 : 0;
							                 break;
						                 case 3:
							                 cmp = x.chaosEach < y.chaosEach ? -1
							                       : x.chaosEach > y.chaosEach ? 1 : 0;
							                 break;
						                 default:
							                 cmp = x.chaosTotal < y.chaosTotal ? -1
							                       : x.chaosTotal > y.chaosTotal ? 1 : 0;
							                 break;
						                 }
						                 if (cmp == 0) cmp = x.key.compare(y.key);
						                 return asc ? cmp < 0 : cmp > 0;
					                 });
				}
			}

			ImGuiListClipper clip;
			clip.Begin((int)sortedLines_.size());
			while (clip.Step()) {
				for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
					const SnapshotLine& l = latest->lines[sortedLines_[row]];
					ImGui::TableNextRow();
					ImGui::PushID(row);
					ImGui::TableNextColumn();
					drawIconCell(l.icon);
					ImGui::TableNextColumn();
					drawNameCell(l.dispEn);
					ImGui::TableNextColumn();
					ImGui::Text("%lld", l.count);
					ImGui::TableNextColumn();
					if (l.priced && l.estimated) {
						ImGui::TextColored(kDim, "~%s",
						                   FormatValue(l.chaosEach, state_.showDivine, rate).c_str());
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(u8"估計值：市場無批量掛牌，以保守下限 0.5c 計");
					} else if (l.priced) {
						ImGui::TextUnformatted(
						    FormatValue(l.chaosEach, state_.showDivine, rate).c_str());
					} else {
						ImGui::TextColored(kDim, u8"未估價");
					}
					ImGui::TableNextColumn();
					if (l.priced && l.estimated)
						ImGui::TextColored(kDim, "~%s",
						                   FormatValue(l.chaosTotal, state_.showDivine, rate).c_str());
					else if (l.priced)
						ImGui::TextUnformatted(
						    FormatValue(l.chaosTotal, state_.showDivine, rate).c_str());
					else
						ImGui::TextColored(kDim, "-");
					ImGui::TableNextColumn();
					drawCopyCell(l.dispEn);
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
	}

	bool diffLinePassesFilter(const SnapshotDiffLine& r)
	{
		switch (catFilter_) {
		case 1: if (r.key.rfind("currency|", 0) != 0) return false; break;
		case 2: if (r.key.rfind("gem|", 0) != 0) return false; break;
		case 3: if (r.key.rfind("card|", 0) != 0) return false; break;
		case 4: if (r.key.rfind("unique|", 0) != 0) return false; break;
		case 5: if (r.key.rfind("map|", 0) != 0) return false; break;
		case 6: if (r.key.rfind("other|", 0) != 0) return false; break;
		case 7: if (r.each != 0.0) return false; break; // no price known
		default: break;
		}
		if (!search_.empty()) {
			if (LowerAscii(r.dispEn).find(LowerAscii(search_)) == std::string::npos &&
			    i18n_.DisplayName(r.dispEn).find(search_) == std::string::npos)
				return false;
		}
		return true;
	}

	// The Wealthy-Exile change table: one merged list, biggest movers first.
	void drawChanges(const Snapshot* prev, const Snapshot* sel)
	{
		if (!sel || !prev) {
			ImGui::TextColored(kDim, u8"這是最早的快照，沒有更早的比較對象。");
			return;
		}
		SnapshotDiff d = DiffSnapshots(*prev, *sel);
		const double rate = sel->divineRate;
		const ImVec4& col = d.dTotalChaos >= 0 ? kGood : kBad;
		ImGui::TextColored(kDim, u8"與上一份（%s）相比：", FormatUtcLocal(prev->utc).c_str());
		ImGui::SameLine();
		ImGui::TextColored(col, "%s", signedValue(d.dTotalChaos, rate).c_str());

		std::vector<const SnapshotDiffLine*> rows;
		for (const SnapshotDiffLine& r : d.gained) rows.push_back(&r);
		for (const SnapshotDiffLine& r : d.lost) rows.push_back(&r);

		if (ImGui::BeginTable("##wh_changes", 6,
		                      ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
		                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("##ic",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_NoSort,
			                        24.0f * host_->scale);
			ImGui::TableSetupColumn(u8"物品", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(u8"數量",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        70.0f * host_->scale);
			ImGui::TableSetupColumn(u8"單價",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        90.0f * host_->scale);
			ImGui::TableSetupColumn(u8"變化",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_DefaultSort |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        100.0f * host_->scale);
			ImGui::TableSetupColumn(u8"複製",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_NoSort,
			                        96.0f * host_->scale);
			ImGui::TableHeadersRow();

			// The diff is recomputed per frame anyway, so the sort is applied per
			// frame too -- a few hundred rows cost nothing.
			if (const ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
				if (sp->SpecsCount > 0) {
					const ImGuiTableColumnSortSpecs& s0 = sp->Specs[0];
					const bool asc = s0.SortDirection != ImGuiSortDirection_Descending;
					std::stable_sort(rows.begin(), rows.end(),
					                 [&](const SnapshotDiffLine* a, const SnapshotDiffLine* b) {
						                 int cmp = 0;
						                 switch (s0.ColumnIndex) {
						                 case 1: {
							                 cmp = i18n_.DisplayName(a->dispEn)
							                           .compare(i18n_.DisplayName(b->dispEn));
							                 break;
						                 }
						                 case 2:
							                 cmp = a->dCount < b->dCount ? -1
							                       : a->dCount > b->dCount ? 1 : 0;
							                 break;
						                 case 3:
							                 cmp = a->each < b->each ? -1
							                       : a->each > b->each ? 1 : 0;
							                 break;
						                 default:
							                 cmp = a->dChaos < b->dChaos ? -1
							                       : a->dChaos > b->dChaos ? 1 : 0;
							                 break;
						                 }
						                 if (cmp == 0) cmp = a->key.compare(b->key);
						                 return asc ? cmp < 0 : cmp > 0;
					                 });
				}
			}
			int rowId = 0;
			for (const SnapshotDiffLine* r : rows) {
				if (!diffLinePassesFilter(*r)) continue;
				ImGui::TableNextRow();
				ImGui::PushID(rowId++);
				ImGui::TableNextColumn();
				drawIconCell(r->icon);
				ImGui::TableNextColumn();
				drawNameCell(r->dispEn);
				ImGui::TableNextColumn();
				ImGui::TextColored(r->dCount >= 0 ? kGood : kBad, "%+lld", r->dCount);
				ImGui::TableNextColumn();
				if (r->each > 0) {
					if (r->estimated)
						ImGui::TextColored(kDim, "~%s",
						                   FormatValue(r->each, state_.showDivine, rate).c_str());
					else
						ImGui::TextUnformatted(
						    FormatValue(r->each, state_.showDivine, rate).c_str());
				} else {
					ImGui::TextColored(kDim, "-");
				}
				ImGui::TableNextColumn();
				if (r->dChaos != 0)
					ImGui::TextColored(r->dChaos >= 0 ? kGood : kBad, "%s",
					                   signedValue(r->dChaos, rate).c_str());
				else
					ImGui::TextColored(kDim, "-");
				ImGui::TableNextColumn();
				drawCopyCell(r->dispEn);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// The left timeline: one row per snapshot, newest first, coloured by its
	// change against the one before it. Click selects; right-click for actions.
	void drawSnapSidebar(const Snapshot* sel)
	{
		ImGui::TextColored(kDim, u8"快照（%d）", (int)history_.snaps.size());
		ImGui::Separator();
		for (int i = (int)history_.snaps.size() - 1; i >= 0; i--) {
			const Snapshot& s = history_.snaps[i];
			const Snapshot* prev = i > 0 ? &history_.snaps[i - 1] : nullptr;
			const double delta = prev ? s.totalChaos - prev->totalChaos : 0.0;

			ImGui::PushID(i);
			char label[64];
			snprintf(label, sizeof(label), "%s##snap", FormatUtcLocal(s.utc).c_str());
			const bool isSel = sel && sel->utc == s.utc;
			if (ImGui::Selectable(label, isSel, 0,
			                      ImVec2(0, ImGui::GetTextLineHeightWithSpacing())))
				selSnapUtc_ = s.utc == history_.snaps.back().utc ? 0 : s.utc;
			if (ImGui::BeginPopupContextItem("##snap_ctx")) {
				if (s.utc != history_.sessionStartUtc && ImGui::MenuItem(u8"設為起點")) {
					history_.sessionStartUtc = s.utc;
					history_.Save(exeDir_);
				}
				ImGui::EndPopup();
			}
			// The change rides on the same row, right-aligned over the selectable.
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 62.0f * host_->scale);
			if (prev) {
				const std::string txt = (delta >= 0 ? "+" : "") + FormatChaos(delta);
				ImGui::TextColored(delta >= 0 ? kGood : kBad, "%s", txt.c_str());
			} else {
				ImGui::TextColored(kDim, "--");
			}
			if (s.utc == history_.sessionStartUtc) {
				ImGui::SameLine(0, 4.0f * host_->scale);
				ImGui::TextColored(kWarn, u8"起");
			}
			ImGui::PopID();
		}
	}

	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir_;

	WarehouseUiState state_;
	bool stateDirty_ = false;
	WarehouseHistory history_;
	WarehouseService svc_;
	WarehouseService::Status st_;
	FilterI18n i18n_;
	IconManager icons_;

	std::vector<StashTabInfo> tabs_;
	std::vector<std::string> leagues_;

	std::vector<float> curvePts_;
	std::vector<long long> curveUtc_;
	std::vector<int> sortedLines_;
	long long linesForUtc_ = 0;
	long long selSnapUtc_ = 0; // sidebar selection; 0 = follow the newest
	int catFilter_ = 0;
	std::string search_;
	bool filterDirty_ = false;

	ToolCloseState close_ = ToolCloseState::Open;
};

} // namespace

IToolPanel* CreateWarehousePanel()
{
	return new WarehousePanel();
}

void ShowWarehouseTool(const std::wstring& exeDir, const std::wstring& game,
                       const std::wstring& locale)
{
	WarehousePanel panel;
	ToolWindowDesc desc;
	// "PobTools — 倉庫收益"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 \xe5\x80\x89\xe5\xba\xab\xe6\x94\xb6\xe7\x9b\x8a";
	desc.defW = 1100;
	desc.defH = 760;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
