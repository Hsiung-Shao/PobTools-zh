#include "boss_profit_tool.h"

#include "boss_profit.h"
#include "error_log.h"
#include "filter_i18n.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "warehouse_format.h"
#include "warehouse_price_feed.h"
#include "warehouse_state.h" // WarehouseSavedLeague

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h> // TableSetColumnSortDirection: restoring a saved sort has no public call

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// 王戰收益 — every boss, what it drops and what a kill is worth at poe.ninja prices. Drawing and
// bookkeeping only: the numbers come from ComputeBossVariant, the prices from NinjaPriceFeed's worker.

namespace {

namespace Tok = PobUi::Tok;
using PobUi::D;

ImVec4 V(std::uint32_t c) { return PobUi::TokV4(c); }

std::string NarrowUtf8(const std::wstring& w)
{
	std::string out;
	for (wchar_t c : w) out += (c > 0 && c < 128) ? (char)c : '?';
	return out;
}

std::string LowerAscii(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	return s;
}

bool ReadAll(const std::wstring& path, std::string* out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	*out = ss.str();
	return true;
}

// pad: keeps a table's last column clear of the scrollbar edge
void RightText(const char* text, const ImVec4& col, float pad = 0.0f)
{
	const float w = ImGui::CalcTextSize(text).x;
	const float avail = ImGui::GetContentRegionAvail().x - pad;
	if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
	ImGui::TextColored(col, "%s", text);
}

// A rounded surface-1 block with inner padding, same look as PobUi's cards but fixed height so
// the table inside can scroll.
void BeginPane(const char* id, const ImVec2& size)
{
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, D(8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(16.0f), D(14.0f)));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, V(Tok::Surface1));
	ImGui::PushStyleColor(ImGuiCol_Border, V(Tok::Border));
	ImGui::BeginChild(id, size, true);
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
}

constexpr double kMaxPrice = 1e9;    // chaos; nothing in the game is worth more
constexpr double kMaxMinutes = 1440; // a kill does not take a day

class BossProfitPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& host) override
	{
		host_ = &host;
		exeDir_ = host.exeDir;
		std::string body, err;
		if (!ReadAll(exeDir_ + L"Data\\boss_drops_poe1.json", &body) || !LoadBossDb(body, &db_, &err)) {
			initError_ = std::string(u8"王戰掉落資料讀取失敗（Data\\boss_drops_poe1.json）") +
			             (err.empty() ? "" : u8"：" + err);
			PobLog::Error("bossprofit", initError_.c_str());
			return false;
		}
		// A missing or broken overrides file is simply "nothing typed yet".
		if (ReadAll(overridesPath(), &body) && !ParseBossOverrides(body, &ov_, &err)) ov_ = {};
		if (ov_.sort.empty()) ov_.sort = { -2 }; // profit, highest first
		i18n_.Load(exeDir_, NarrowUtf8(host.locale));
		league_ = WarehouseSavedLeague(exeDir_); // empty = poe.ninja's current league
		feed_.Init(exeDir_);                     // no network until the first frame is drawn
		// Test aid (POBTOOLS_BOSS_PAGE=help, with POBTOOLS_TOOL_SHOT): open on the help page.
		wchar_t pg[8] = L"";
		if (GetEnvironmentVariableW(L"POBTOOLS_BOSS_PAGE", pg, 8) == 4 && std::wstring(pg) == L"help") page_ = 1;
		for (int b = 0; b < (int)db_.bosses.size(); b++)
			for (int v = 0; v < (int)db_.bosses[b].variants.size(); v++) rows_.push_back({ b, v, {} });
		return true;
	}
	const char* InitError() const override { return initError_.c_str(); }

	void Frame() override
	{
		if (!requested_) {
			feed_.Request("poe1", league_, false);
			requested_ = true;
		}
		const NinjaPriceFeed::Status ps = feed_.Poll();
		if (ps.prices != prices_ || dirty_) {
			prices_ = ps.prices;
			recompute();
		}
		static const char* const kPages[2] = { u8"收益", u8"說明" };
		page_ = PobUi::PageTabs("##boss_pages", page_, kPages, 2);
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		if (page_ == 1) {
			drawHelpPage();
			return; // nothing on this page edits, so no save is pending
		}
		drawToolbar(ps);
		ImGui::Dummy(ImVec2(0, D(8.0f)));

		const float footH = ImGui::GetTextLineHeightWithSpacing() + D(8.0f);
		const float h = (std::max)(D(200.0f), ImGui::GetContentRegionAvail().y - footH);
		const float w = ImGui::GetContentRegionAvail().x;
		const float gap = D(12.0f);
		const float leftW = std::floor((w - gap) * 0.54f);
		BeginPane("##boss_list", ImVec2(leftW, h));
		drawList(ps.divineRate);
		ImGui::EndChild();
		ImGui::SameLine(0, gap);
		BeginPane("##boss_detail", ImVec2(w - leftW - gap, h));
		drawDetail(ps.divineRate);
		ImGui::EndChild();

		PobUi::Hint(u8"掉率為社群統計，非官方數值　·　價格來自 poe.ninja，掛單少於 5 筆不採用　·　掉率或單價填 0 恢復預設");
		ImGui::SameLine(0, D(8.0f));
		PobUi::InfoTip((std::string(u8"掉率來源：") + db_.source + u8"\n傳奇為已鑑定掛單價，王掉的是未鑑定品，高浮動傳奇會偏高\n按住 Shift 點欄位標題可加上次要排序").c_str());

		if (saveDue_) {
			saveDue_ = false;
			saveOverrides();
		}
	}

	ToolCloseState RequestClose() override
	{
		close_ = ToolCloseState::Closed;
		return close_;
	}
	ToolCloseState CloseState() const override { return close_; }
	void AbortClose() override { close_ = ToolCloseState::Open; }
	void Shutdown() override { feed_.Shutdown(); }
	const char* PanelId() const override { return "bossprofit"; }

private:
	struct Row {
		int b = 0, v = 0;
		BossResult r;
	};

	std::wstring overridesPath() const { return exeDir_ + L"PobTools\\boss_profit_ui.json"; }

	void saveOverrides()
	{
		CreateDirectoryW((exeDir_ + L"PobTools").c_str(), nullptr);
		std::ofstream f(overridesPath(), std::ios::binary | std::ios::trunc);
		const std::string s = SerializeBossOverrides(ov_);
		if (!f || !f.write(s.data(), (std::streamsize)s.size()))
			PobLog::Error("bossprofit", u8"手動修改存檔失敗（PobTools\\boss_profit_ui.json）");
	}

	void changed()
	{
		dirty_ = true;
		saveDue_ = true;
	}

	void recompute()
	{
		dirty_ = false;
		auto lookup = [this](const std::string& key, NinjaPrice* out) {
			if (!prices_) return false;
			auto it = prices_->find(key);
			if (it == prices_->end()) return false;
			*out = it->second;
			return true;
		};
		for (Row& r : rows_) {
			const Boss& b = db_.bosses[r.b];
			r.r = ComputeBossVariant(b, b.variants[r.v], lookup, ov_);
		}
	}

	// What the tool shows, how to drive it and what the totals leave out. Prose lives here like
	// the other tool panels' strings.
	void drawHelpPage()
	{
		ImGui::BeginChild("##boss_help", ImVec2(0, 0), false);
		const float w = (std::min)(ImGui::GetContentRegionAvail().x, D(820.0f));
		auto para = [](const char* s) { ImGui::TextWrapped("%s", s); };
		// Bullet + SameLine + TextWrapped: wrapped lines align under the text, not the bullet.
		auto item = [](const char* s) {
			ImGui::Bullet();
			ImGui::SameLine();
			ImGui::TextWrapped("%s", s);
		};
		auto section = [&](const char* id, const char* title) { PobUi::CardBegin(id, nullptr, title, nullptr, true, w); };
		auto end = [] {
			PobUi::CardEnd();
			ImGui::Dummy(ImVec2(0, D(4.0f)));
		};

		section("##h_what", u8"這是什麼");
		para(u8"列出各王的掉落與門票，用市價算出打一場的期望收益與利潤，"
		     u8"讓你比較哪隻王划算。數字是長期平均，不是單場保證。");
		end();

		section("##h_use", u8"怎麼用");
		item(u8"左邊是王的清單。點欄位標題排序，按住 Shift 再點可加上次要排序。");
		item(u8"上方可篩選「全部／普通／Uber」與「有門票／無門票」，搜尋框可打中文或英文王名。");
		item(u8"點一隻王，右邊顯示它的掉落與門票，以及一場的收益、門票成本、利潤與 ROI。");
		item(u8"在「擊殺耗時」填你打一場要幾分鐘，就會算出每小時利潤。");
		item(u8"「掉率」與「單價」都可以直接改成你自己的數字，改過的會換顏色。填 0 恢復預設值。");
		item(u8"「重置此王修改」會清掉這隻王的手動掉率、單價與擊殺耗時。");
		item(u8"價格每15分鐘更新一次，也可以按「重新整理價格」。聯盟沿用倉庫收益的設定，沒設定就用目前賽季。");
		end();

		section("##h_math", u8"數字怎麼算");
		item(u8"期望值 = 掉率 × 單價。例：掉率 5%、單價 200 c，期望值是 10 c。");
		item(u8"收益 = 所有計入的掉落期望值加總。");
		item(u8"利潤 = 收益 - 門票成本。不需要門票的王只顯示收益。");
		item(u8"ROI = 利潤 ÷ 門票成本。");
		item(u8"每小時 = 利潤 × 60 ÷ 擊殺耗時（分鐘）。");
		end();

		section("##h_skip", u8"哪些東西沒有算進去");
		item(u8"掉率「未知」的物品：來源沒有這項的掉率數字。它會列在清單裡，但不計入收益，期望值顯示「—」。"
		     u8"在掉率欄填入你認為的數字後就會計入。所以有未知項目的王，實際收益會比畫面上高。");
		item(u8"「無報價」的物品：市價來源沒有這項的價格，或掛單少於5筆。同樣不計入收益；"
		     u8"在單價欄手填後計入。王清單的「缺價」欄是這類項目的數量。");
		item(u8"門票無報價：門票成本會少算，利潤因此偏高，並且不計 ROI。手填門票單價即可。");
		item(u8"隱藏昇華版的禁忌血肉、禁忌烈焰：poe.ninja 沒有分開報價，需要自己填單價。");
		end();

		section("##h_limit", u8"資料來源與限制");
		item(u8"掉率是社群統計，不是官方數值，改版後可能過時。");
		item(u8"傳奇的價格是已鑑定掛單價；王掉的是未鑑定品，數值浮動大的傳奇會被高估。");
		item(u8"價格來自 poe.ninja，目前只支援 PoE1 國際服。");
		para((std::string(u8"掉率來源：") + db_.source).c_str());
		end();
		ImGui::EndChild();
	}

	const std::string& bossName(const Row& r) const
	{
		const Boss& b = db_.bosses[r.b];
		return b.zh.empty() ? b.en : b.zh;
	}
	// The source's variant names ("Uber Sirus", "Regular Dread") are its own labels, not game text:
	// show the tier instead, and nothing for a boss with one variant.
	std::string variantLabel(const Row& r) const
	{
		const Boss& b = db_.bosses[r.b];
		if (b.variants.size() < 2) return std::string();
		const BossVariant& v = b.variants[r.v];
		// 使用者裁定(2026-11-20): Uber 照玩家說法不翻; 無暇 is the Flawless Breachstone's item name.
		if (v.name.rfind("Flawless", 0) == 0) return u8"無暇";
		return v.uber ? "Uber" : u8"普通";
	}
	std::string rowName(const Row& r) const
	{
		const std::string v = variantLabel(r);
		return v.empty() ? bossName(r) : bossName(r) + u8" · " + v;
	}

	std::string money(double chaos, double rate, bool plus = false) const
	{
		std::string s = WhFmt::FormatValue(chaos, true, rate);
		return (plus && chaos > 0 ? "+" : "") + s;
	}

	void drawToolbar(const NinjaPriceFeed::Status& ps)
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(V(Tok::TextMuted), "%s", u8"聯盟");
		ImGui::SameLine(0, D(8.0f));
		ImGui::TextUnformatted((ps.league.empty() ? (league_.empty() ? std::string(u8"目前賽季") : league_) : ps.league).c_str());
		ImGui::SameLine(0, D(12.0f));
		if (ps.busy) PobUi::StatusPill(PobUi::Tone::Run, u8"讀取價格中");
		else if (!ps.error.empty() && !ps.prices) PobUi::StatusPill(PobUi::Tone::Bad, u8"價格讀取失敗");
		else if (!ps.error.empty()) PobUi::StatusPill(PobUi::Tone::Warn, u8"價格未更新");
		else if (ps.prices) PobUi::StatusPill(PobUi::Tone::Ok, u8"價格已載入");
		if (!ps.error.empty() && ImGui::IsItemHovered()) PobUi::Tooltip(ps.error.c_str());
		if (ps.divineRate > 0) {
			ImGui::SameLine(0, D(12.0f));
			char buf[64];
			snprintf(buf, sizeof(buf), u8"1 神聖石 = %.0f c", ps.divineRate);
			ImGui::TextColored(V(Tok::TextMuted), "%s", buf);
		}
		ImGui::SameLine(0, D(12.0f));
		if (PobUi::Button(u8"重新整理價格", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Refresh, 0.0f, !ps.busy))
			feed_.Request("poe1", league_, true);

		static const char* const kKind[3] = { u8"全部", u8"普通", "Uber" };
		static const char* const kTicket[3] = { u8"不限門票", u8"有門票", u8"無門票" };
		PobUi::Segmented("##boss_kind", &kindFilter_, kKind, 3);
		ImGui::SameLine(0, D(12.0f));
		PobUi::Segmented("##boss_ticket", &ticketFilter_, kTicket, 3);
		ImGui::SameLine(0, D(12.0f));
		PobUi::SearchField("##boss_search", search_, (int)sizeof(search_), u8"搜尋王",
		                   (std::max)(D(160.0f), ImGui::GetContentRegionAvail().x));
	}

	bool visible(const Row& r) const
	{
		const Boss& b = db_.bosses[r.b];
		const BossVariant& v = b.variants[r.v];
		if (kindFilter_ == 1 && v.uber) return false;
		if (kindFilter_ == 2 && !v.uber) return false;
		if (ticketFilter_ == 1 && v.tickets.empty()) return false;
		if (ticketFilter_ == 2 && !v.tickets.empty()) return false;
		if (search_[0]) {
			// English folds case; the row as shown (Chinese name, 普通/Uber) matches bytewise.
			const std::string needle = LowerAscii(search_);
			if (LowerAscii(b.en + " " + v.name).find(needle) == std::string::npos &&
			    rowName(r).find(search_) == std::string::npos)
				return false;
		}
		return true;
	}

	void drawList(double rate)
	{
		std::vector<int> order;
		for (int i = 0; i < (int)rows_.size(); i++)
			if (visible(rows_[i])) order.push_back(i);

		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, V(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, V(Tok::BorderSubtle));
		if (ImGui::BeginTable("##bosses", kBossSortCols,
		                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable |
		                          ImGuiTableFlags_SortMulti | ImGuiTableFlags_SizingFixedFit)) {
			const ImGuiTableColumnFlags num = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(u8"王", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(u8"利潤", num);
			ImGui::TableSetupColumn("ROI", num);
			ImGui::TableSetupColumn(u8"收益", num);
			ImGui::TableSetupColumn(u8"門票", num);
			// Columns with nothing to show in the current list are hidden, not filled with dashes.
			bool anyHour = false, anyMissing = false;
			// (over every row, not the filtered ones: a column must not come and go while searching)
			for (const Row& r : rows_) {
				anyHour |= r.r.hasPerHour;
				anyMissing |= r.r.unpricedDrops > 0 || r.r.ticketIncomplete;
			}
			ImGui::TableSetupColumn(u8"每小時", num | (anyHour ? 0 : ImGuiTableColumnFlags_Disabled));
			ImGui::TableSetupColumn(u8"缺價", num | (anyMissing ? 0 : ImGuiTableColumnFlags_Disabled));
			ImGui::TableHeadersRow();

			if (!sortRestored_) { // after the header row: the columns' sort directions are laid out by then
				sortRestored_ = true;
				bool first = true;
				for (int c : ov_.sort) {
					ImGui::TableSetColumnSortDirection(std::abs(c) - 1, c < 0 ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending,
					                                   !first);
					first = false;
				}
			}
			if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
				if (sp->SpecsDirty) {
					sp->SpecsDirty = false;
					std::vector<int> now;
					for (int k = 0; k < sp->SpecsCount; k++)
						now.push_back((sp->Specs[k].ColumnIndex + 1) *
						              (sp->Specs[k].SortDirection == ImGuiSortDirection_Descending ? -1 : 1));
					if (now != ov_.sort) {
						ov_.sort = now;
						saveDue_ = true;
					}
				}
				// A value a row does not have (no ROI, no kill time) sorts last either way; so does a
				// profit that is overstated because a ticket has no price.
				auto val = [&](int col, int i, bool* has) -> double {
					const BossResult& r = rows_[i].r;
					*has = true;
					switch (col) {
					case 2: *has = r.hasRoi; return r.roiPct;
					case 3: return r.revenue;
					case 4: return r.ticketCost;
					case 5: *has = r.hasPerHour && !r.ticketIncomplete; return r.profitPerHour;
					case 6: return r.unpricedDrops + (r.ticketIncomplete ? 1 : 0);
					default: *has = !r.ticketIncomplete; return r.profit;
					}
				};
				std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
					for (int k = 0; k < sp->SpecsCount; k++) { // first column decides, the next breaks its ties
						const int col = sp->Specs[k].ColumnIndex;
						const bool asc = sp->Specs[k].SortDirection != ImGuiSortDirection_Descending;
						if (col == 0) {
							const int c = rowName(rows_[a]).compare(rowName(rows_[b]));
							if (c != 0) return asc ? c < 0 : c > 0;
							continue;
						}
						bool ha, hb;
						const double x = val(col, a, &ha), y = val(col, b, &hb);
						if (ha != hb) return ha;
						if (!ha || x == y) continue;
						return asc ? x < y : x > y;
					}
					return false;
				});
			}

			const ImVec4 text = V(Tok::Text), muted = V(Tok::TextMuted), faint = V(Tok::TextFaint);
			const ImVec4 good = V(Tok::Success), bad = V(Tok::Danger);
			for (int i : order) {
				const Row& row = rows_[i];
				const BossResult& r = row.r;
				ImGui::TableNextRow();
				ImGui::PushID(i);
				ImGui::TableNextColumn();
				if (ImGui::Selectable("##row", sel_ == i,
				                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
					sel_ = i;
				ImGui::SameLine(0, 0);
				ImGui::TextUnformatted(bossName(row).c_str());
				if (const std::string vl = variantLabel(row); !vl.empty()) {
					ImGui::SameLine(0, D(8.0f));
					ImGui::TextColored(muted, "%s", vl.c_str());
				}
				ImGui::TableNextColumn();
				RightText(money(r.profit, rate, true).c_str(),
				          r.ticketIncomplete || r.profit == 0 ? muted : r.profit > 0 ? good : bad);
				ImGui::TableNextColumn();
				if (r.hasRoi) {
					char buf[32];
					snprintf(buf, sizeof(buf), "%+.0f%%", r.roiPct);
					RightText(buf, muted);
				} else {
					RightText(u8"—", faint);
				}
				ImGui::TableNextColumn();
				RightText(money(r.revenue, rate).c_str(), muted);
				ImGui::TableNextColumn();
				if (!r.hasTicket) RightText(u8"—", faint);
				else if (r.ticketIncomplete)
					// an unpriced ticket is not free: never show a bare "0 c"
					RightText(r.ticketCost > 0 ? (u8"≥ " + money(r.ticketCost, rate)).c_str() : u8"無報價", V(Tok::Warning));
				else RightText(money(r.ticketCost, rate).c_str(), muted);
				ImGui::TableNextColumn();
				if (r.hasPerHour)
					RightText(money(r.profitPerHour, rate, true).c_str(), r.ticketIncomplete ? faint : text);
				else RightText(u8"—", faint);
				ImGui::TableNextColumn();
				if (r.unpricedDrops > 0 || r.ticketIncomplete) {
					char buf[32];
					snprintf(buf, sizeof(buf), "%d", r.unpricedDrops + (r.ticketIncomplete ? 1 : 0));
					// only a missing ticket price makes the profit wrong; missing drops just understate it
					RightText(buf, r.ticketIncomplete ? V(Tok::Warning) : faint, D(8.0f));
					if (ImGui::IsItemHovered())
						PobUi::Tooltip(r.ticketIncomplete ? u8"門票有項目沒有報價：利潤偏高、不計 ROI。可在右側手填單價。"
						                                  : u8"沒有報價的掉落不計入收益。可在右側手填單價。");
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(2);
	}

	// A number box that reports a committed edit. Returns true when the user finished typing.
	bool numberCell(const char* id, double* v, const char* fmt, bool manual)
	{
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (manual) ImGui::PushStyleColor(ImGuiCol_Text, V(Tok::AccentText));
		ImGui::InputDouble(id, v, 0.0, 0.0, fmt);
		if (manual) ImGui::PopStyleColor();
		// An empty format leaves the box blank; the word is painted over it, so it never sits in the
		// edit buffer where typed digits would land next to it and fail to parse.
		if (!fmt[0] && !ImGui::IsItemActive())
			ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin() + ImGui::GetStyle().FramePadding,
			                                    ImGui::GetColorU32(V(Tok::TextMuted)), u8"未知");
		return ImGui::IsItemDeactivatedAfterEdit();
	}

	void stat(const char* label, const std::string& value, const ImVec4& col)
	{
		ImGui::TableNextColumn();
		PobUi::Overline(label);
		ImGui::TextColored(col, "%s", value.c_str());
	}

	void drawDetail(double rate)
	{
		if (sel_ < 0 || sel_ >= (int)rows_.size()) {
			PobUi::Hint(u8"從左側選一隻王，查看掉落與門票");
			return;
		}
		const Row& row = rows_[sel_];
		const Boss& b = db_.bosses[row.b];
		const BossVariant& v = b.variants[row.v];
		const BossResult& r = row.r;
		const std::string vid = BossVariantId(b, v);
		const ImVec4 text = V(Tok::Text), muted = V(Tok::TextMuted), faint = V(Tok::TextFaint);

		PobUi::Heading(bossName(row).c_str());
		{
			const std::string vl = variantLabel(row);
			const std::string sub = (b.zh.empty() ? std::string() : b.en) + (!b.zh.empty() && !vl.empty() ? u8" · " : "") + vl;
			if (!sub.empty()) ImGui::TextColored(muted, "%s", sub.c_str());
		}
		ImGui::Dummy(ImVec2(0, D(12.0f)));

		// The one figure the panel exists for; everything else on this side explains it.
		PobUi::Overline(!r.hasTicket ? u8"一場收益（不需門票）" : r.ticketIncomplete ? u8"一場利潤（門票缺價，偏高）" : u8"一場利潤");
		ImGui::PushFont(host_->big ? host_->big : ImGui::GetFont()); // digits and money glyphs only
		ImGui::TextColored(r.ticketIncomplete || r.profit == 0 ? muted : r.profit > 0 ? V(Tok::Success) : V(Tok::Danger), "%s",
		                   money(r.profit, rate, true).c_str());
		ImGui::PopFont();
		ImGui::Dummy(ImVec2(0, D(8.0f)));

		if (ImGui::BeginTable("##stats", 4, ImGuiTableFlags_SizingStretchSame)) {
			stat(u8"收益", money(r.revenue, rate), text);
			if (!r.hasTicket) stat(u8"門票", u8"—", faint);
			else if (r.ticketIncomplete)
				stat(u8"門票", r.ticketCost > 0 ? u8"≥ " + money(r.ticketCost, rate) : std::string(u8"無報價"), V(Tok::Warning));
			else stat(u8"門票", money(r.ticketCost, rate), text);
			char roi[32] = u8"—";
			if (r.hasRoi) snprintf(roi, sizeof(roi), "%+.0f%%", r.roiPct);
			stat("ROI", roi, r.hasRoi ? text : faint);
			stat(u8"每小時", r.hasPerHour ? money(r.profitPerHour, rate, true) : std::string(u8"—"),
			     r.hasPerHour && !r.ticketIncomplete ? text : faint);
			ImGui::EndTable();
		}
		ImGui::Dummy(ImVec2(0, D(8.0f)));

		ImGui::PushStyleColor(ImGuiCol_FrameBg, V(Tok::Surface2));
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(muted, "%s", u8"擊殺耗時");
		ImGui::SameLine(0, D(8.0f));
		auto ks = ov_.killSeconds.find(vid);
		double minutes = ks == ov_.killSeconds.end() ? 0.0 : ks->second / 60.0;
		ImGui::SetNextItemWidth(D(72.0f));
		ImGui::InputDouble("##kill", &minutes, 0.0, 0.0, "%.1f");
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			if (minutes > 0 && minutes <= kMaxMinutes) ov_.killSeconds[vid] = (int)std::lround(minutes * 60.0);
			else ov_.killSeconds.erase(vid);
			changed();
		}
		ImGui::SameLine(0, D(8.0f));
		ImGui::TextColored(muted, "%s", u8"分鐘");
		ImGui::SameLine(0, D(16.0f));
		if (PobUi::Button(u8"重置此王修改", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::RotateCcw)) {
			ResetBossVariantOverrides(&ov_, b, v);
			changed();
		}
		ImGui::Dummy(ImVec2(0, D(4.0f)));

		char tabDrops[48], tabTickets[48];
		snprintf(tabDrops, sizeof(tabDrops), u8"掉落 %d", (int)r.drops.size());
		snprintf(tabTickets, sizeof(tabTickets), u8"門票 %d", (int)r.tickets.size());
		const char* const tabs[2] = { tabDrops, tabTickets };
		detailPage_ = PobUi::PageTabs("##boss_detail_tabs", detailPage_, tabs, 2);
		const bool drops = detailPage_ == 0;
		const std::vector<BossLine>& lines = drops ? r.drops : r.tickets;
		if (lines.empty()) {
			ImGui::PopStyleColor();
			PobUi::Hint(drops ? u8"沒有掉落資料" : u8"這隻王不需要門票");
			return;
		}
		double top = 0.0; // the biggest contributor gets the accent, the rest stay neutral
		for (const BossLine& l : lines) top = (std::max)(top, l.value);
		const double total = drops ? r.revenue : r.ticketCost;

		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, V(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, V(Tok::BorderSubtle));
		if (ImGui::BeginTable(drops ? "##drops" : "##tickets", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(u8"物品", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(drops ? u8"掉率 %" : u8"數量", ImGuiTableColumnFlags_WidthFixed, D(80.0f));
			ImGui::TableSetupColumn(u8"單價 c", ImGuiTableColumnFlags_WidthFixed, D(104.0f));
			ImGui::TableSetupColumn(drops ? u8"期望值" : u8"小計", ImGuiTableColumnFlags_WidthFixed, D(120.0f));
			ImGui::TableHeadersRow();
			for (int i = 0; i < (int)lines.size(); i++) {
				const BossLine& l = lines[i];
				const bool manual = l.src == BossPriceSrc::Manual;
				ImGui::TableNextRow();
				ImGui::PushID(i);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				// "Tombfist (1 Abyssal Socket)": translate the name, keep the variant note as written.
				const size_t paren = l.en.find(" (");
				const std::string base = paren == std::string::npos ? l.en : l.en.substr(0, paren);
				const std::string name = i18n_.DisplayName(base) + (paren == std::string::npos ? "" : l.en.substr(paren));
				ImGui::TextUnformatted(name.c_str());
				if (ImGui::IsItemHovered()) {
					std::string tip = l.en;
					if (drops && !v.drops[i].note.empty()) tip += "\n" + v.drops[i].note;
					PobUi::Tooltip(tip.c_str());
				}

				ImGui::TableNextColumn();
				if (drops) {
					double pct = l.ratePct;
					// An edit that leaves the shown number as it was (cleared box, typed and undone) is not an override.
					if (numberCell("##rate", &pct, pct > 0 ? "%.2f" : "" /* unknown: reads 未知 */, l.rateManual) && (l.rateManual || pct != l.ratePct)) {
						const std::string key = vid + "|" + l.en;
						if (pct > 0 && pct <= 100) ov_.rate[key] = pct;
						else ov_.rate.erase(key); // out of range = back to the shipped rate
						changed();
					}
					if (ImGui::IsItemHovered()) {
						if (l.rateManual) PobUi::Tooltip(u8"手動值，填 0 恢復預設");
						else if (l.ratePct <= 0) PobUi::Tooltip(u8"掉率未知，不計入收益。填入掉率後計入");
						else if (!v.drops[i].rateText.empty())
							PobUi::Tooltip((std::string(u8"來源標示為近似值：") + v.drops[i].rateText).c_str());
					}
				} else {
					char buf[32];
					snprintf(buf, sizeof(buf), "%g", l.qty);
					RightText(buf, text);
				}

				ImGui::TableNextColumn();
				double unit = l.unit;
				if (numberCell("##unit", &unit, "%.1f", manual) && (manual || unit != l.unit)) {
					// "<= max" also rejects inf, which would serialise as null and void the whole file
					if (unit > 0 && unit <= kMaxPrice) ov_.price[l.priceKey] = unit;
					else ov_.price.erase(l.priceKey); // 0 or less = back to the market price
					changed();
				}
				if (manual && ImGui::IsItemHovered()) PobUi::Tooltip(u8"手動價，填 0 恢復 poe.ninja 價");

				ImGui::TableNextColumn();
				const float cellW = ImGui::GetContentRegionAvail().x - D(8.0f);
				const float cellR = ImGui::GetCursorScreenPos().x + cellW;
				if (drops && l.ratePct <= 0) {
					RightText(u8"—", V(Tok::TextMuted), D(8.0f));
				} else if (l.src == BossPriceSrc::None) {
					RightText(u8"無報價", V(Tok::Warning), D(8.0f));
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"poe.ninja 沒有這項的價格，或掛單少於 5 筆。可在「單價」欄手填");
				} else {
					RightText(money(l.value, rate).c_str(), text, D(8.0f));
					if (total > 0 && l.value > 0) { // share of the total, right-anchored under its number
						const float y = ImGui::GetItemRectMax().y + D(2.0f);
						const float bw = (std::max)(D(2.0f), cellW * (float)(l.value / total));
						ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cellR - bw, y), ImVec2(cellR, y + D(2.0f)),
						                                          l.value == top ? Tok::Accent : Tok::BorderStrong);
					}
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(3);
	}

	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir_;
	std::string initError_;
	BossDb db_;
	BossOverrides ov_;
	FilterI18n i18n_;
	NinjaPriceFeed feed_;
	std::shared_ptr<const NinjaPriceFeed::PriceTable> prices_;
	std::string league_;
	std::vector<Row> rows_;
	int page_ = 0; // 0 收益, 1 說明
	bool requested_ = false, dirty_ = true, saveDue_ = false, sortRestored_ = false;
	int sel_ = 0, kindFilter_ = 0, ticketFilter_ = 0, detailPage_ = 0;
	char search_[96] = "";
	ToolCloseState close_ = ToolCloseState::Open;
};

} // namespace

IToolPanel* CreateBossProfitPanel()
{
	return new BossProfitPanel();
}

void ShowBossProfitTool(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale)
{
	BossProfitPanel panel;
	ToolWindowDesc desc;
	desc.titleUtf8 = u8"PobTools — 王戰收益";
	desc.defW = 1280;
	desc.defH = 820;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
