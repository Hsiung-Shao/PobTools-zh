#include "warehouse_tool.h"

#include "error_log.h"
#include "filter_i18n.h"
#include "icon_manager.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "warehouse_format.h"
#include "warehouse_service.h"
#include "warehouse_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 倉庫收益統計 — paste a POESESSID, pick tabs, snapshot the stash, watch the
// value move. The panel is drawing and bookkeeping only: every network byte and
// every price lives in WarehouseService's worker.
//
// Drawn with the design system (ui_widgets.h, 2026-10-08): an action row, four
// KPI cards, the host's cost card beside 主要增減, the value chart, and the
// snapshot list beside the detail table.

namespace {

namespace Tok = PobUi::Tok;
using PobUi::D;

ImVec4 V(std::uint32_t c) { return PobUi::TokV4(c); }

float SmallPx()
{
	const PobUi::WidgetFonts& f = PobUi::Fonts();
	return f.small ? f.small->FontSize : ImGui::GetFontSize();
}

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


// Shown in the user's local time; the file stores UTC.
SYSTEMTIME LocalTimeOf(long long utc)
{
	FILETIME ft;
	unsigned long long t = (unsigned long long)utc * 10000000ull + 116444736000000000ull;
	ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFF);
	ft.dwHighDateTime = (DWORD)(t >> 32);
	FILETIME lt;
	FileTimeToLocalFileTime(&ft, &lt);
	SYSTEMTIME st;
	FileTimeToSystemTime(&lt, &st);
	return st;
}

std::string FormatUtcLocal(long long utc)
{
	const SYSTEMTIME st = LocalTimeOf(utc);
	char buf[32];
	snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", st.wMonth, st.wDay, st.wHour,
	         st.wMinute);
	return buf;
}

// The longest prefix of `s` that fits `maxW` pixels with a trailing "…", cut on
// a UTF-8 boundary; unchanged when it already fits. For names that would
// otherwise run underneath the value beside them.
std::string Ellipsize(const std::string& s, float maxW)
{
	if (maxW <= 0.0f || ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
	const char* ell = u8"…";
	const float ellW = ImGui::CalcTextSize(ell).x;
	std::string out;
	size_t i = 0;
	while (i < s.size()) {
		const unsigned char c = (unsigned char)s[i];
		const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
		const std::string next = out + s.substr(i, n);
		if (ImGui::CalcTextSize(next.c_str()).x + ellW > maxW) break;
		out = next;
		i += n;
	}
	return out + ell;
}

// A muted note in the small face that wraps at `width` (the column or card it
// sits in) instead of running under the edge.
void HintWrapped(const char* text, float width)
{
	PobUi::Hint(text, width > 0.0f ? width : ImGui::GetContentRegionAvail().x);
}

// Text right-aligned in what is left of the current line (a table cell).
void RightText(const char* text, const ImVec4& col)
{
	const float w = ImGui::CalcTextSize(text).x;
	const float avail = ImGui::GetContentRegionAvail().x;
	if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
	ImGui::TextColored(col, "%s", text);
}

// Shared with the atlas planner's cost card (warehouse_format.h), so a value
// reads the same in both windows.
using WhFmt::FormatChaos;
using WhFmt::FormatValue;

class WarehousePanel : public IToolPanel {
public:
	WarehousePanel() = default;
	explicit WarehousePanel(const WarehouseEmbed& e) : embed_(e) {}

	bool Init(const ToolPanelHost& host) override
	{
		host_ = &host;
		exeDir_ = host.exeDir;
		// Copy follows the interface language: 繁中 copies the Chinese name,
		// everything else the English one (user rule, 2026-10-08).
		copyZh_ = host.locale == L"zh-rTW";
		state_.Load(exeDir_);
		// PoE1 only for now: no channel can read a PoE2 stash (the legacy
		// endpoints silently ignore realm=poe2 and answer for PoE1; the OAuth
		// stash API is PoE1-only -- verified 2026-09-11). The per-game plumbing
		// and the PoE2 price tables stay, ready for a PoE2 stash API.
		state_.game = "poe1";
		// One history file per league since 2026-09-11. The old single file is
		// split once, by whichever panel starts first (a repeat is a no-op).
		if (WarehouseHistory::MigrateLegacy(exeDir_, state_.game) < 0)
			PobLog::Error("warehouse", u8"舊快照歷史搬移失敗，原檔保留");
		historyLeague_ = state_.sel().league;
		history_.Load(exeDir_, state_.game, historyLeague_);
		// Auto snapshots count from opening (user rule): an old snapshot never
		// makes one fire the moment the panel appears.
		autoArmedUtc_ = NowUtc();
		guard_ = WarehouseGuardLoad(exeDir_);
		i18n_.Load(exeDir_, NarrowUtf8(host.locale));
		icons_.Init(exeDir_);
		svc_.Init(exeDir_);
		svc_.SetGame(state_.game);
		// Standalone: a first run with no session id saved opens on the page that
		// explains what this is and what pasting one means.
		ownPage_ = state_.sessid.empty() ? 1 : 0;
		// Test aid (POBTOOLS_WAREHOUSE_PAGE, with POBTOOLS_TOOL_SHOT): open the
		// standalone window on a page. Embedded, the host picks the page.
		if (!embed_.page) {
			wchar_t pg[32] = L"";
			const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_WAREHOUSE_PAGE", pg, 32);
			const std::wstring page = (n > 0 && n < 32) ? std::wstring(pg) : std::wstring();
			if (page == L"profit") ownPage_ = 0;
			else if (page == L"help") ownPage_ = 1;
			else if (page == L"settings") ownPage_ = 2;
		}
		// No network here: the launcher Inits every panel it opens (and the panel
		// selftest Inits all of them); requests wait for a click.
		return true;
	}

	// What the panel must do whether or not it is drawn this frame: take the
	// worker's status, follow the league, fold a finished snapshot in, and keep
	// the auto-snapshot schedule. Frame and RunDeferred both call it -- hosts
	// call RunDeferred every frame, also for a panel whose tab is hidden (the
	// atlas planner's embed included) -- and it runs once per frame.
	void pump()
	{
		const int frame = ImGui::GetFrameCount();
		if (frame == pumpedFrame_) return;
		pumpedFrame_ = frame;
		st_ = svc_.Poll();
		// History is per league: picking another league shows that league's file.
		if (state_.sel().league != historyLeague_) switchHistoryLeague();
		reloadIfChanged();

		// A finished snapshot is folded into the history right here, so the file
		// on disk is always one frame behind at most. Read-modify-write: another
		// panel may share this history file.
		if (st_.snapshotReady) {
			Snapshot snap;
			if (svc_.TakeSnapshot(&snap)) {
				// Into the league it was taken in: the picker may have moved on
				// while the tabs were being fetched.
				const std::string lg = snap.league.empty() ? historyLeague_ : snap.league;
				if (!history_.AppendAndSave(exeDir_, state_.game, lg, snap))
					PobLog::Error("warehouse", u8"快照歷史存檔失敗");
				if (lg != historyLeague_) history_.Load(exeDir_, state_.game, historyLeague_);
				svc_.AckDone();
				st_ = svc_.Poll();
			}
		}
		if (st_.tabsReady && tabs_.empty()) tabs_ = st_.tabs;
		else if (st_.tabsReady && st_.tabs.size() != tabs_.size()) tabs_ = st_.tabs;
		if (st_.leaguesReady) leagues_ = st_.leagues;
		maybeAutoSnapshot();
	}

	void RunDeferred() override { pump(); }

	void Frame() override
	{
		icons_.Pump();
		pump();

		// A job that just finished says so once, as a toast; a failure stays on
		// the revenue page as a banner until the next action.
		if (st_.phase == WarehousePhase::Done && lastPhase_ != WarehousePhase::Done &&
		    !st_.message.empty())
			PobUi::ShowToast(st_.message.c_str(), PobUi::Tone::Ok);
		lastPhase_ = st_.phase;

		// Which page: the host's own tab bar when it has one (the atlas planner's
		// 收益 / 說明 / 設定), else ours.
		Page page = Page::Revenue;
		if (embed_.page) {
			const int p = *embed_.page;
			page = p == 1 ? Page::Help : p == 2 ? Page::Settings : Page::Revenue;
		} else {
			const char* tabs[3] = { u8"收益", u8"說明", u8"設定" };
			ownPage_ = PobUi::PageTabs("##wh_pages", ownPage_, tabs, 3);
			ImGui::Dummy(ImVec2(0, D(4.0f)));
			page = ownPage_ == 1 ? Page::Help : ownPage_ == 2 ? Page::Settings : Page::Revenue;
		}

		switch (page) {
		case Page::Help: drawHelpPage(); break;
		case Page::Settings: drawSettingsPage(); break;
		default: drawRevenuePage(); break;
		}
		persistIfDirty();

		// Applied after drawing: the write reloads history_, and the frame above
		// held pointers into it.
		if (setStartRequest_) {
			history_.SetSessionStartAndSave(exeDir_, state_.game, historyLeague_, setStartRequest_);
			setStartRequest_ = 0;
		}
	}

	ToolCloseState RequestClose() override
	{
		// History is not saved here: every change already went to disk as a
		// read-modify-write, and saving this copy could erase a snapshot another
		// panel took since. A session id typed but not yet persisted counts as a
		// credential save; anything else must not touch the file's own.
		saveState(secretDirty_);
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

	// The history file may also be written by another panel (the standalone
	// tool and the atlas planner's embed). Writes are read-modify-write, so
	// nothing is lost; this is what makes the other panel's snapshots SHOW UP
	// here within a couple of seconds.
	void reloadIfChanged()
	{
		const double now = ImGui::GetTime();
		if (now - lastStatCheck_ < 2.0) return;
		lastStatCheck_ = now;
		// Tiny file: another panel or process may have started a snapshot or been
		// told to wait.
		guard_ = WarehouseGuardLoad(exeDir_);
		WIN32_FILE_ATTRIBUTE_DATA fa{};
		if (!GetFileAttributesExW(
		        WarehouseHistory::PathOf(exeDir_, state_.game, historyLeague_).c_str(),
		        GetFileExInfoStandard, &fa))
			return;
		if (CompareFileTime(&fa.ftLastWriteTime, &histWrite_) == 0) return;
		histWrite_ = fa.ftLastWriteTime;
		history_.Load(exeDir_, state_.game, historyLeague_);
		linesForUtc_ = 0;
		filterDirty_ = true;
	}

	// The league picker moved: drop what is shown and let reloadIfChanged read
	// the new league's file this same frame (a league never snapshotted has no
	// file, and then the view is simply empty).
	void switchHistoryLeague()
	{
		historyLeague_ = state_.sel().league;
		history_ = WarehouseHistory{};
		histWrite_ = FILETIME{};
		lastStatCheck_ = -10.0;
		selSnapUtc_ = 0;
		linesForUtc_ = 0;
		filterDirty_ = true;
	}

	// withSecret: this save is about the session id itself (typing it, the 記住
	// box, the 清除 button). Everything else must leave the file's credential
	// fields alone -- another panel may have cleared them since we loaded, and
	// this copy is then stale (see WarehouseUiState::Save).
	void saveState(bool withSecret)
	{
		if (!state_.Save(exeDir_, withSecret))
			PobLog::Error("warehouse", u8"設定存檔失敗");
		if (withSecret) secretDirty_ = false;
	}

	// The 清除 button: the session id leaves this panel, the worker and the
	// settings file at once, and the session verdict goes with it. Everything
	// else the player set up -- account name, league, tab picks, snapshot
	// history -- stays. Saved even when the save would otherwise be deferred:
	// "cleared" must mean the file no longer has it.
	void clearSessid()
	{
		WarehouseWipe(state_.sessid);
		svc_.ForgetAuth();
		st_.authOk = false;
		st_.authFailed = false;
		saveState(true);
		stateDirty_ = false;
	}

	// Every snapshot -- the button or the schedule -- claims the shared guard
	// first: one start per 10 minutes across all panels and processes, and
	// none while the server has asked for a pause. False = refused.
	bool requestSnapshot()
	{
		long long next = 0;
		const bool granted = WarehouseClaimSnapshot(exeDir_, NowUtc(), &next);
		guard_ = WarehouseGuardLoad(exeDir_);
		if (!granted) return false;
		StashAuth a;
		a.accountName = state_.accountName;
		a.secret = state_.sessid;
		svc_.SetAuth(a);
		svc_.RequestSnapshot(state_.sel().league, state_.sel().tabIds, true);
		return true;
	}

	void maybeAutoSnapshot()
	{
		autoBlock_ = AutoBlock::None;
		autoDueUtc_ = 0;
		if (state_.autoMinutes <= 0) return;
		if (state_.sessid.empty() || state_.sel().league.empty() || state_.sel().tabIds.empty()) {
			autoBlock_ = AutoBlock::Missing;
			return;
		}
		// A dead session needs the player; retrying it on a timer would only
		// spend the account's requests. A successful verify resumes.
		if (st_.authFailed) {
			autoBlock_ = AutoBlock::Auth;
			return;
		}
		// A failed attempt counts like the arming: the next try waits a full
		// interval instead of retrying the moment the cooldown ends.
		const long long armed = (std::max)(autoArmedUtc_, autoLastAttemptUtc_);
		const long long latest = history_.snaps.empty() ? 0 : history_.snaps.back().utc;
		autoDueUtc_ = WarehouseAutoDueUtc(latest, armed, state_.autoMinutes, guard_);
		if (busy() || NowUtc() < autoDueUtc_) return;
		// Done/Error linger until acknowledged; an auto shot must not wait for one.
		svc_.AckDone();
		if (requestSnapshot()) autoLastAttemptUtc_ = NowUtc();
	}

	// The schedule in one sentence, for the 設定 page's 自動快照 row.
	std::string autoHintText() const
	{
		if (state_.autoMinutes <= 0)
			return u8"開啟後依設定的間隔自動拍快照（面板開著就會運作，分頁沒顯示也一樣）。";
		if (autoBlock_ == AutoBlock::Missing)
			return u8"暫停：需要 POESESSID、聯盟與至少一個分頁（在「設定」）。";
		if (autoBlock_ == AutoBlock::Auth) return u8"暫停：session 無效，到「設定」重新驗證後恢復。";
		if (autoDueUtc_ > 0) return std::string(u8"下次自動快照：") + FormatUtcLocal(autoDueUtc_);
		return std::string();
	}

	// The 「前往設定」 of a setup banner: the host's page when it owns the tabs.
	void goSettings()
	{
		if (embed_.page) {
			if (embed_.goPage) embed_.goPage(2);
		} else {
			ownPage_ = 2;
		}
	}

	// ---- 收益 page -----------------------------------------------------------

	void drawRevenuePage()
	{
		ImGui::BeginChild("##wh_main", ImVec2(0, 0), false);
		drawMain();
		ImGui::EndChild();
	}

	// ---- 說明 page -----------------------------------------------------------

	// What this tool does, how to drive it, and -- the part that matters before
	// anyone publishes it -- what pasting a POESESSID means, what GGG has said
	// about that, and exactly what this program does and does not do with it.
	// Prose lives here rather than in a data file: every other tool panel keeps
	// its strings in the .cpp too, and the launcher's string table carries only
	// the button label. One card per section (design system, 2026-10-08).
	void drawHelpPage()
	{
		ImGui::BeginChild("##wh_help", ImVec2(0, 0), false);
		const float w = (std::min)(ImGui::GetContentRegionAvail().x, D(820.0f));
		auto para = [](const char* s) { ImGui::TextWrapped("%s", s); };
		auto warn = [](const char* s) {
			ImGui::PushStyleColor(ImGuiCol_Text, V(Tok::Warning));
			ImGui::TextWrapped("%s", s);
			ImGui::PopStyleColor();
		};
		// Bullet + SameLine + TextWrapped: wrapped lines align under the text
		// instead of running back under the bullet.
		auto item = [](const char* s) {
			ImGui::Bullet();
			ImGui::SameLine();
			ImGui::TextWrapped("%s", s);
		};
		auto section = [&](const char* id, const char* title) {
			PobUi::CardBegin(id, nullptr, title, nullptr, true, w);
		};
		auto end = [] {
			PobUi::CardEnd();
			ImGui::Dummy(ImVec2(0, D(4.0f)));
		};

		section("##h_what", u8"這是什麼");
		para(u8"把你勾選的倉庫分頁拍成快照，之後每拍一次就與上一次比對，算出這段期間倉庫"
		     u8"價值的變化。變化拆成兩部分：數量變化（東西真的多了或少了）與市價波動"
		     u8"（東西沒動，行情變了）。估價來自 poe.ninja，目前只支援 PoE1 國際服。");
		para(u8"在輿圖策略裡，一段收益紀錄可以綁進方案，和每張地圖的成本一起算出刷圖收益"
		     u8"／小時，並隨分享碼帶給別人。");
		end();

		section("##h_start", u8"快速上手");
		item(u8"到「設定」頁填入帳號名稱（官網帳號頁上的名稱，含 #1234）。");
		item(u8"貼上 POESESSID，按「驗證 session」確認可用。");
		item(u8"選好聯盟，按「取得倉庫分頁清單」，勾選要統計的分頁。");
		item(u8"回「收益」頁按「立即快照」。");
		item(u8"第一次快照只是起點；拍過第二次之後才會有變化與收益數字。");
		end();

		section("##h_sessid", u8"POESESSID 怎麼取得");
		item(u8"用瀏覽器登入 www.pathofexile.com。");
		// "›", not an arrow: the tool window's faces have no U+2190 block, and
		// the arrows drew as "?" (seen on the 2026-10-08 screenshots).
		item(u8"按 F12 開開發者工具 › 應用程式（Application）/ 儲存空間 › Cookies › "
		     u8"https://www.pathofexile.com。");
		item(u8"複製 POESESSID 的值（32 個十六進位字元）。");
		warn(u8"這串等同你的登入權杖：拿到的人不需要密碼就能以你的身分登入官網。不要貼給"
		     u8"任何人，包含我們。在官網登出會讓它立刻失效，也就等於收回這個工具的存取權。");
		end();

		section("##h_risk", u8"風險與官方立場");
		warn(u8"GGG 沒有為第三方工具提供 session 授權，官方開發者也公開表示過：不要把 "
		     u8"POESESSID 放進任何第三方程式。");
		item(u8"本功能是官方 OAuth 申請核准前的測試通道。核准之後會改用官方授權，這個貼上 "
		     u8"session id 的做法就會退場。");
		item(u8"目前找不到因為第三方工具「唯讀讀取倉庫」而被停權的案例（已知的停權都與自動"
		     u8"化操作有關），但這不代表官方允許，政策也可能改變。");
		item(u8"覺得不妥就不要用這個功能，PobTools 其他功能完全不受影響。");
		end();

		section("##h_handle", u8"這個工具怎麼處理你的 session id");
		item(u8"只放進送往 pathofexile.com 的 Cookie 標頭，不會出現在網址、記錄檔、錯誤報告"
		     u8"或自檢報告裡。");
		item(u8"全程只發 GET，只讀角色清單與倉庫分頁；不會替你發文、改設定或動用點數。");
		item(u8"不去翻瀏覽器的 cookie 資料庫，一律由你自己貼上。");
		item(u8"不上傳到任何伺服器（包含我們自己的），沒有任何統計或遙測。");
		item(u8"輸入框以圓點遮蔽；預設不記住，勾了「記住」才以 Windows 使用者加密（DPAPI）"
		     u8"存在本機，換使用者或換電腦都解不開。");
		end();

		section("##h_rate", u8"請求頻率與限流");
		item(u8"快照最多 10 分鐘一次：手動與自動共用，所有視窗與行程也共用同一份限制。");
		item(u8"同一次快照裡，每個分頁的請求至少間隔 2 秒，不會一次打一堆。");
		item(u8"收到 429 就停手，照伺服器指定的 Retry-After 等待；伺服器回報額度快滿時會自動"
		     u8"放慢。");
		item(u8"GGG 的請求額度是整個帳號一池：官網、交易站工具和這個功能共用。快照期間盡量"
		     u8"別同時在交易站狂刷。");
		end();

		section("##h_data", u8"資料放在哪、怎麼清除");
		item(u8"設定：PobTools\\warehouse_ui.json（session id 只以 DPAPI 密文存在，而且勾了"
		     u8"「記住」才會寫進去）。");
		item(u8"快照歷史：PobTools\\warehouse\\，依聯盟分檔。");
		item(u8"限流狀態：PobTools\\warehouse_guard.json。");
		item(u8"「設定」頁的「清除」按鈕會立刻把 session id 從記憶體與檔案裡抹掉；帳號名稱、"
		     u8"勾選的分頁與快照歷史都保留。");
		item(u8"不勾「記住」時，session id 只留在記憶體：程式關掉就沒了，下次要重貼。程式開著"
		     u8"的期間，自動快照照常運作。");
		end();

		PobUi::Hint(u8"設定都在「設定」分頁。");
		ImGui::EndChild();
	}

	// ---- 設定 page -----------------------------------------------------------

	// The session verdict as a pill: the same words on the 設定 page and in the
	// revenue page's action row.
	void sessionPill(bool withLeague)
	{
		PobUi::Tone tone = PobUi::Tone::Idle;
		std::string txt = u8"session 未驗證";
		if (st_.authOk) {
			tone = PobUi::Tone::Ok;
			txt = u8"session 有效";
		} else if (st_.authFailed) {
			tone = PobUi::Tone::Bad;
			txt = u8"session 無效";
		}
		if (withLeague && !state_.sel().league.empty()) txt += u8" · " + state_.sel().league;
		PobUi::StatusPill(tone, txt.c_str());
	}

	void drawSettingsPage()
	{
		ImGui::BeginChild("##wh_settings_page", ImVec2(0, 0), false);
		PobUi::Banner("##wh_testing", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, u8"測試性質功能",
		              u8"以 POESESSID 讀取國際服 PoE1 倉庫。session id 等同帳號"
		              u8"登入權杖，請勿分享給任何人；GGG 官方不建議把它交給第三方程式，"
		              u8"日後將改接官方授權通道。詳見「說明」分頁。",
		              false, nullptr, false);
		ImGui::Dummy(ImVec2(0, D(4.0f)));

		// Two columns: account, league, schedule (and the host's record buttons)
		// on the left; the stash tab list -- the page's longest thing -- filling
		// the right.
		const float gap = D(12.0f);
		const float avail = ImGui::GetContentRegionAvail().x;
		const float leftW = (std::min)(D(560.0f), (avail - gap) * 0.58f);
		const float rightW = (std::min)(avail - leftW - gap, D(480.0f));
		const ImVec2 top = ImGui::GetCursorScreenPos();
		const float pageBottom = top.y + ImGui::GetContentRegionAvail().y;

		ImGui::BeginGroup();
		drawAccountCard(leftW);
		if (embed_.settingsBottom) {
			ImGui::Dummy(ImVec2(0, D(4.0f)));
			PobUi::CardBegin("##wh_rec", nullptr, nullptr, nullptr, true, leftW);
			embed_.settingsBottom();
			PobUi::CardEnd();
		}
		ImGui::EndGroup();

		ImGui::SetCursorScreenPos(ImVec2(top.x + leftW + gap, top.y));
		drawTabsCard(rightW, pageBottom);
		ImGui::EndChild();
	}

	void drawAccountCard(float w)
	{
		const float gap = PobUi::RowGap();
		PobUi::CardBegin("##wh_account", nullptr, u8"帳號", nullptr, false, w);

		const float fieldW = D(240.0f);
		PobUi::RowBegin(u8"帳號名稱", u8"帳號頁的名稱，如 Name#1234", fieldW);
		PobUi::PushControlFrame();
		ImGui::SetNextItemWidth(fieldW);
		if (ImGui::InputText("##wh_acct", &state_.accountName)) stateDirty_ = true;
		PobUi::PopControlFrame();
		PobUi::RowEnd();

		{
			const float clearW = PobUi::ButtonWidth(u8"清除");
			const float inW = D(180.0f);
			PobUi::RowBegin("POESESSID", u8"瀏覽器登入官網後 Cookie 內的 POESESSID", inW + gap + clearW);
			PobUi::PushControlFrame();
			ImGui::SetNextItemWidth(inW);
			if (ImGui::InputText("##wh_sessid", &state_.sessid, ImGuiInputTextFlags_Password)) {
				stateDirty_ = true;
				secretDirty_ = true;
			}
			PobUi::PopControlFrame();
			ImGui::SameLine(0, gap);
			if (PobUi::Button(u8"清除##wh_sessid_clear", PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, 0.0f,
			                  !state_.sessid.empty()))
				clearSessid();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				PobUi::Tooltip(u8"立刻從記憶體與設定檔清除 session id\n"
				               u8"（帳號名稱、勾選的分頁與快照歷史保留）");
			PobUi::RowEnd();
		}

		PobUi::RowBegin(u8"記住（以 Windows 使用者加密存於本機）",
		                state_.rememberSessid
		                    ? u8"session id 以 DPAPI 加密存在 warehouse_ui.json，換使用者或換電腦都解不開"
		                    : u8"不記住：session id 只留在記憶體，關掉程式就沒了，下次要重貼",
		                PobUi::SwitchWidth());
		if (PobUi::Switch("##wh_remember", &state_.rememberSessid)) {
			// Turning it off must not wait for the next settings change: save now,
			// which overwrites the blob on disk with an empty field.
			stateDirty_ = true;
			secretDirty_ = true;
			persistIfDirty();
		}
		PobUi::RowEnd();

		{
			const char* verify = u8"驗證 session";
			const float pillW = PobUi::PillWidth(u8"session 未驗證");
			PobUi::RowBegin(u8"驗證", nullptr, pillW + gap + PobUi::ButtonWidth(verify));
			sessionPill(false);
			ImGui::SameLine(0, gap);
			if (PobUi::Button(verify, PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, 0.0f,
			                  !busy() && !state_.sessid.empty() && !state_.accountName.empty())) {
				StashAuth a;
				a.accountName = state_.accountName;
				a.secret = state_.sessid;
				svc_.SetAuth(a);
				svc_.AckDone();
				svc_.RequestVerify();
				// The button's whole point is the credential the player just typed.
				saveState(true);
				stateDirty_ = false;
			}
			PobUi::RowEnd();
		}

		drawLeagueRow();
		drawAutoRow();
		PobUi::CardEnd();
	}

	// 聯盟 and its refresh, on the 設定 page beside the tab list it decides.
	void drawLeagueRow()
	{
		const float gap = PobUi::RowGap();
		const char* refresh = u8"更新##leagues";
		const float selW = D(200.0f);
		PobUi::RowBegin(u8"聯盟", nullptr, selW + gap + PobUi::ButtonWidth(refresh));
		if (leagues_.empty()) {
			PobUi::PushControlFrame();
			ImGui::SetNextItemWidth(selW);
			if (ImGui::InputText("##wh_league", &state_.sel().league)) stateDirty_ = true;
			PobUi::PopControlFrame();
		} else {
			// The saved league stays pickable even when the server no longer lists it.
			std::vector<std::string> names = leagues_;
			const std::string cur = state_.sel().league;
			if (!cur.empty() && std::find(names.begin(), names.end(), cur) == names.end())
				names.insert(names.begin(), cur);
			std::vector<const char*> labels;
			int sel = -1;
			for (size_t i = 0; i < names.size(); i++) {
				labels.push_back(names[i].c_str());
				if (names[i] == cur) sel = (int)i;
			}
			if (PobUi::Select("##wh_league_c", &sel, labels.data(), nullptr, (int)labels.size(), selW) &&
			    sel >= 0 && names[sel] != cur) {
				state_.sel().league = names[sel];
				state_.sel().tabIds.clear();
				tabs_.clear();
				stateDirty_ = true;
			}
		}
		ImGui::SameLine(0, gap);
		if (PobUi::Button(refresh, PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, 0.0f, !busy())) {
			svc_.AckDone();
			svc_.RequestLeagues();
		}
		PobUi::RowEnd();
	}

	void drawAutoRow()
	{
		static const char* const kAuto[] = { u8"關閉",       u8"每 1 小時", u8"每 2 小時", u8"每 3 小時",
			                                 u8"每 4 小時", u8"每 5 小時", u8"每 6 小時" };
		const std::string hint = autoHintText();
		const float selW = D(160.0f);
		PobUi::RowBegin(u8"自動快照", hint.empty() ? nullptr : hint.c_str(), selW);
		int idx = (std::max)(0, (std::min)(6, state_.autoMinutes / 60));
		if (PobUi::Select("##wh_auto", &idx, kAuto, nullptr, 7, selW)) {
			const int m = idx * 60;
			// Switching it on starts the count; changing the interval does not.
			if (state_.autoMinutes == 0 && m > 0) autoArmedUtc_ = NowUtc();
			state_.autoMinutes = m;
			stateDirty_ = true;
		}
		PobUi::RowEnd();
	}

	// `bottomY`: the page's bottom edge (screen y); the list fills down to it.
	void drawTabsCard(float w, float bottomY)
	{
		char note[64];
		snprintf(note, sizeof(note), u8"%d 個已選", (int)state_.sel().tabIds.size());
		PobUi::CardBegin("##wh_tabs_card", nullptr, u8"倉庫分頁", note, true, w);
		const float innerW = PobUi::CardInnerWidth();
		// Tab ids are per league, so this is always the league picked on the
		// left. The fetch checks the session itself -- no separate verify first.
		if (PobUi::Button(u8"取得倉庫分頁清單", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f,
		                  !busy() && !state_.sessid.empty() && !state_.accountName.empty() &&
		                      !state_.sel().league.empty())) {
			StashAuth a;
			a.accountName = state_.accountName;
			a.secret = state_.sessid;
			svc_.SetAuth(a);
			svc_.AckDone();
			svc_.RequestTabList(state_.sel().league);
		}
		if (!tabs_.empty()) {
			PobUi::Hint(u8"勾選要統計的分頁");
			// The rest of the page: the list is the column's main content.
			const float listH = (std::max)(bottomY - ImGui::GetCursorScreenPos().y - D(24.0f), D(160.0f));
			ImGui::BeginChild("##wh_tabs", ImVec2(innerW, listH), false);
			for (const StashTabInfo& t : tabs_) {
				bool sel = std::find(state_.sel().tabIds.begin(), state_.sel().tabIds.end(), t.id) !=
				           state_.sel().tabIds.end();
				std::string label = t.name + "##" + t.id;
				if (ImGui::Checkbox(label.c_str(), &sel)) {
					if (sel) {
						state_.sel().tabIds.push_back(t.id);
					} else {
						state_.sel().tabIds.erase(
						    std::remove(state_.sel().tabIds.begin(), state_.sel().tabIds.end(), t.id),
						    state_.sel().tabIds.end());
					}
					stateDirty_ = true;
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("#%d %s", t.index, t.type.c_str());
			}
			ImGui::EndChild();
		} else if (!state_.sel().tabIds.empty()) {
			// Picked in an earlier run; the list itself is fetched on demand.
			char msg[96];
			snprintf(msg, sizeof(msg), u8"已選 %d 個分頁（取得清單後可修改）", (int)state_.sel().tabIds.size());
			HintWrapped(msg, innerW);
		} else {
			HintWrapped(u8"先填好帳號、POESESSID 與聯盟，再取得分頁清單。", innerW);
		}
		PobUi::CardEnd();
	}

	// Every frame, whichever page is on screen.
	void persistIfDirty()
	{
		if (stateDirty_ && !busy()) {
			// Settings persist on change, same as the launcher's own rule; the
			// session id rides along as a DPAPI blob -- but only when this panel
			// is the one that changed it.
			saveState(secretDirty_);
			stateDirty_ = false;
		}
	}

	// ---- revenue page -------------------------------------------------------

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

	std::string signedValue(double chaos, double rate) const
	{
		return (chaos >= 0 ? "+" : "") + FormatValue(chaos, state_.showDivine, rate);
	}

	// What is missing before a snapshot can be taken, "" when nothing is.
	const char* setupMissing() const
	{
		if (state_.accountName.empty() || state_.sessid.empty())
			return u8"請到「設定」填寫帳號名稱與 POESESSID";
		if (state_.sel().league.empty()) return u8"請到「設定」選擇聯盟";
		if (state_.sel().tabIds.empty()) return u8"請到「設定」勾選要統計的倉庫分頁";
		return "";
	}

	void drawMain()
	{
		drawActionRow();

		// Setup and failures, as banners under the row: what is wrong and the way
		// to the page that fixes it.
		const char* missing = setupMissing();
		const char* goLabel = (embed_.page && !embed_.goPage) ? nullptr : u8"前往設定";
		if (st_.phase == WarehousePhase::Error) {
			ImGui::Dummy(ImVec2(0, D(2.0f)));
			if (PobUi::Banner("##wh_err", PobUi::BannerTone::Bad, PobIcon::CircleX, st_.message.c_str(),
			                  st_.authFailed ? u8"請到「設定」重新輸入 POESESSID" : nullptr, false,
			                  st_.authFailed ? goLabel : nullptr, false) == PobUi::BannerResult::Action)
				goSettings();
		} else if (*missing && !busy()) {
			ImGui::Dummy(ImVec2(0, D(2.0f)));
			if (PobUi::Banner("##wh_setup", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, missing, nullptr,
			                  false, goLabel, false) == PobUi::BannerResult::Action)
				goSettings();
		}
		ImGui::Dummy(ImVec2(0, D(4.0f)));

		if (history_.snaps.empty()) {
			// The host's card (the per-map cost) needs no snapshot: keep it.
			const float gap = D(12.0f);
			const float avail = ImGui::GetContentRegionAvail().x;
			float emptyW = avail;
			if (embed_.topCard) {
				const float w1 = std::floor((avail - gap) * 1.15f / 2.15f);
				emptyW = avail - w1 - gap;
				PobUi::CardBegin("##wh_cost", nullptr, nullptr, nullptr, true, w1);
				embed_.topCard();
				PobUi::CardEnd();
				ImGui::SameLine(0, gap);
			}
			PobUi::CardBegin("##wh_empty", nullptr, u8"尚無快照", nullptr, true, emptyW);
			HintWrapped(u8"尚無快照。在「設定」填好帳號並勾選分頁後按「立即快照」。", PobUi::CardInnerWidth());
			PobUi::CardEnd();
			return;
		}

		const Snapshot& latest = history_.snaps.back();
		const Snapshot* start = history_.FindByUtc(history_.sessionStartUtc);
		if (!start) start = &history_.snaps.front();
		const SnapshotDiff d = DiffSnapshots(*start, latest);

		drawKpis(d, *start, latest);
		ImGui::Dummy(ImVec2(0, D(2.0f)));

		// Second row: the host's cost card (or, standalone, the chart) beside
		// 主要增減, 1.15 : 1 as in the design; each as tall as the taller.
		const float gap = D(12.0f);
		const float avail = ImGui::GetContentRegionAvail().x;
		const float w1 = std::floor((avail - gap) * 1.15f / 2.15f);
		const float w2 = avail - w1 - gap;
		float nat1 = 0.0f;
		if (embed_.topCard) {
			PobUi::CardBegin("##wh_cost", nullptr, nullptr, nullptr, true, w1, row2MinH_);
			embed_.topCard();
			PobUi::CardEnd();
			nat1 = PobUi::CardNaturalHeight();
		} else {
			drawCurveCard(w1, row2MinH_, (std::max)(D(96.0f), moversNat_ - curveChrome_));
			nat1 = PobUi::CardNaturalHeight();
		}
		ImGui::SameLine(0, gap);
		drawMoversCard(d, latest, w2, row2MinH_);
		moversNat_ = PobUi::CardNaturalHeight();
		row2MinH_ = (std::max)(nat1, moversNat_);

		if (embed_.topCard) {
			ImGui::Dummy(ImVec2(0, D(2.0f)));
			drawCurveCard(avail, 0.0f, D(80.0f));
		}
		ImGui::Dummy(ImVec2(0, D(2.0f)));
		drawBottom();
	}

	// 立即快照 | session pill | last / next snapshot ...... host's bind button.
	void drawActionRow()
	{
		// The shared guard's word, as of its last 2-second refresh (a click
		// re-reads it under the lock anyway).
		const long long now = NowUtc();
		const long long nextOk = WarehouseNextSnapshotUtc(guard_);
		const bool cooling = now < nextOk;
		// No verify-first: the snapshot job checks the session itself and says
		// so when it is dead (the verify button lives on the 設定 page).
		const bool canSnap = !busy() && !*setupMissing() && !cooling;

		const ImVec2 row = ImGui::GetCursorScreenPos();
		const float rowW = ImGui::GetContentRegionAvail().x;
		const float rowH = PobUi::ControlH();
		const float gap = D(12.0f);
		if (PobUi::Button(u8"立即快照", PobUi::BtnKind::Primary, PobUi::BtnSize::Md, PobIcon::Camera, 0.0f, canSnap)) {
			svc_.AckDone();
			requestSnapshot();
		}
		if (!canSnap && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			const char* why = busy() ? u8"正在處理上一個請求" : *setupMissing() ? setupMissing()
			                                                                   : u8"冷卻中：兩次快照至少間隔 10 分鐘";
			PobUi::Tooltip(why);
		}
		float x = ImGui::GetItemRectMax().x + gap;

		// The worker's progress while it runs, else the session verdict.
		ImGui::SetCursorScreenPos(ImVec2(x, row.y + std::floor((rowH - D(24.0f)) * 0.5f)));
		if (busy()) {
			std::string msg = st_.message.empty() ? std::string(u8"處理中") : st_.message;
			if (st_.phase == WarehousePhase::FetchingTabs && st_.tabsTotal > 0) {
				char buf[32];
				snprintf(buf, sizeof(buf), " (%d/%d)", st_.tabsDone, st_.tabsTotal);
				msg += buf;
			}
			PobUi::StatusPill(PobUi::Tone::Run, msg.c_str());
		} else {
			sessionPill(true);
		}
		x = ImGui::GetItemRectMax().x + gap;

		// When the last one was taken, and when the next may be.
		std::string hint = history_.snaps.empty() ? std::string(u8"尚無快照")
		                                          : std::string(u8"上次快照 ") + FormatUtcLocal(history_.snaps.back().utc);
		const char* hintTip = nullptr;
		if (cooling) {
			const long long left = nextOk - now;
			const bool paused = guard_.blockedUntilUtc >= nextOk;
			char buf[48];
			snprintf(buf, sizeof(buf), u8" · %s %lld:%02lld", paused ? u8"限流" : u8"冷卻", left / 60, left % 60);
			hint += buf;
			hintTip = paused ? u8"GGG 要求暫停請求（HTTP 429 或額度用盡），期間不送出任何倉庫請求"
			                 : u8"帳號請求額度整個帳號共用：兩次快照至少間隔 10 分鐘，所有視窗共用這個限制";
		} else if (state_.autoMinutes <= 0) {
			hint += u8" · 自動快照關閉";
		} else if (autoBlock_ != AutoBlock::None) {
			hint += u8" · 自動快照暫停";
		} else {
			hint += u8" · 自動快照每 " + std::to_string(state_.autoMinutes / 60) + u8" 小時";
		}
		const std::string autoTip = autoHintText();
		if (!hintTip && state_.autoMinutes > 0 && !autoTip.empty()) hintTip = autoTip.c_str();
		ImGui::SetCursorScreenPos(ImVec2(x, row.y + std::floor((rowH - SmallPx()) * 0.5f)));
		PobUi::Hint(hint.c_str());
		if (hintTip && ImGui::IsItemHovered()) PobUi::Tooltip(hintTip);
		x = ImGui::GetItemRectMax().x + gap;

		// The host's end of the row (the atlas planner's bind button), right-aligned.
		if (embed_.topBar) {
			const float w = embed_.topBarWidth ? embed_.topBarWidth() : 0.0f;
			ImGui::SetCursorScreenPos(ImVec2((std::max)(x, row.x + rowW - w), row.y));
			embed_.topBar();
		}
		ImGui::SetCursorScreenPos(row);
		ImGui::Dummy(ImVec2(rowW, rowH));
	}

	// A big figure (ToolPanelHost::big, digits and money only) with an optional
	// small unit after it on the same baseline.
	void bigFigure(const std::string& value, std::uint32_t col, const char* unit = nullptr)
	{
		ImFont* big = host_->big ? host_->big : ImGui::GetFont();
		ImFont* small = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::PushFont(big);
		ImGui::TextColored(V(col), "%s", value.c_str());
		ImGui::PopFont();
		if (unit && *unit) {
			const float x = ImGui::GetItemRectMax().x + D(4.0f);
			ImGui::SetCursorScreenPos(ImVec2(x, p.y + big->Ascent - small->Ascent));
			PobUi::Hint(unit);
			ImGui::SetCursorScreenPos(p);
			ImGui::Dummy(ImVec2(1.0f, big->FontSize));
		}
	}

	// 「316 小時 33 分」: the numbers big, the units small.
	void bigDuration(long long seconds)
	{
		const long long mins = seconds / 60;
		ImFont* big = host_->big ? host_->big : ImGui::GetFont();
		ImFont* small = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float unitY = p.y + big->Ascent - small->Ascent;
		float x = p.x;
		auto num = [&](long long v) {
			ImGui::SetCursorScreenPos(ImVec2(x, p.y));
			ImGui::PushFont(big);
			ImGui::Text("%lld", v);
			ImGui::PopFont();
			x = ImGui::GetItemRectMax().x + D(4.0f);
		};
		auto unit = [&](const char* u) {
			ImGui::SetCursorScreenPos(ImVec2(x, unitY));
			PobUi::Hint(u);
			x = ImGui::GetItemRectMax().x + D(6.0f);
		};
		num(mins / 60);
		unit(u8"小時");
		num(mins % 60);
		unit(u8"分");
		ImGui::SetCursorScreenPos(p);
		ImGui::Dummy(ImVec2(x - p.x, big->FontSize));
	}

	// Four KPI cards: what farming earned per hour, the net change, the market,
	// and the interval itself. Equal widths; two by two when the row is narrow.
	void drawKpis(const SnapshotDiff& d, const Snapshot& start, const Snapshot& latest)
	{
		const double rate = latest.divineRate;
		int shotsIn = 0; // snapshots inside the interval, both ends included
		for (const Snapshot& s : history_.snaps)
			if (s.utc >= start.utc && s.utc <= latest.utc) shotsIn++;

		const float gap = D(12.0f);
		const float avail = ImGui::GetContentRegionAvail().x;
		const int cols = avail >= D(4 * 200.0f) ? 4 : 2;
		const float cw = std::floor((avail - gap * (float)(cols - 1)) / (float)cols);
		float tallest = 0.0f;
		auto card = [&](int i, const char* id) {
			if (i % cols != 0) ImGui::SameLine(0, gap);
			PobUi::CardBegin(id, nullptr, nullptr, nullptr, true, cw, kpiMinH_);
		};
		auto done = [&](const char* tip) {
			PobUi::CardEnd();
			tallest = (std::max)(tallest, PobUi::CardNaturalHeight());
			if (tip && ImGui::IsItemHovered()) PobUi::Tooltip(tip);
		};
		const std::uint32_t good = Tok::Success, bad = Tok::Danger;

		// What farming earned per hour -- count changes only. The market moving
		// stock already held is money, but not the maps' doing.
		card(0, "##kpi_farm");
		PobUi::Hint(u8"刷圖收益");
		if (d.summaryOnly) bigFigure("--", Tok::TextMuted, u8"/ 小時");
		else bigFigure(signedValue(d.farmPerHour, rate), d.farmPerHour >= 0 ? good : bad, u8"/ 小時");
		PobUi::Hint(u8"只算數量增減，不含市價漲跌");
		done(u8"（收益 − 支出）÷ 經過時間，只算數量變化；市價波動不算在內");

		// Money split by cause: 收益 / 支出 are count changes (farmed, spent),
		// 市價波動 the market re-pricing stock held throughout.
		card(1, "##kpi_net");
		PobUi::Hint(u8"區間淨值變化");
		bigFigure(WhFmt::FormatDivChaos(d.dTotalChaos, rate, true), d.dTotalChaos >= 0 ? good : bad);
		{
			const std::string split =
			    d.summaryOnly ? std::string(u8"收益 - · 支出 -")
			                  : std::string(u8"收益 ") + WhFmt::FormatDivChaos(d.qtyGain, rate, false) +
			                        u8" · 支出 " + WhFmt::FormatDivChaos(-d.qtyLoss, rate, false);
			PobUi::Hint(split.c_str());
		}
		done(u8"倉庫總值的變化 = 收益 − 支出 ± 市價波動\n"
		     u8"收益：起點之後多出來的物品，數量增加 × 目前單價\n"
		     u8"支出：起點之後用掉或賣掉的物品，數量減少 × 單價");

		card(2, "##kpi_market");
		PobUi::Hint(u8"市價波動");
		if (d.summaryOnly) bigFigure("-", Tok::TextMuted);
		else bigFigure(WhFmt::FormatDivChaos(d.priceMove, rate, true), Tok::Text);
		PobUi::Hint(u8"持有物品漲跌，不算進收益");
		done(u8"起點時就持有的數量 × 單價漲跌；\n報價消失（變成未估價）的物品也算在這裡");

		card(3, "##kpi_span");
		PobUi::Hint(u8"快照區間");
		bigDuration(latest.utc - start.utc);
		{
			const std::string span = FormatUtcLocal(start.utc) + u8" ～ " + FormatUtcLocal(latest.utc) + u8" · " +
			                         std::to_string(shotsIn) + u8" 份";
			PobUi::Hint(span.c_str());
		}
		done(u8"起點（快照清單右鍵可改）到最新一份快照");

		kpiMinH_ = tallest;
	}

	// One heading line inside a padded card: title, a muted note after it, and
	// a right-aligned figure.
	void cardHead(const char* title, const char* note, const std::string& right, std::uint32_t rightCol)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		PobUi::Heading(title);
		const float h = ImGui::GetItemRectSize().y;
		if (note && *note) {
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMax().x + D(10.0f), p.y + std::floor((h - SmallPx()) * 0.5f)));
			PobUi::Hint(note);
		}
		if (!right.empty()) {
			const float w = ImGui::CalcTextSize(right.c_str()).x;
			ImGui::SetCursorScreenPos(ImVec2(PobUi::CardInnerX() + PobUi::CardInnerWidth() - w,
			                                 p.y + std::floor((h - ImGui::GetFontSize()) * 0.5f)));
			ImGui::TextColored(V(rightCol), "%s", right.c_str());
		}
		ImGui::SetCursorScreenPos(p);
		ImGui::Dummy(ImVec2(1.0f, h));
		ImGui::Dummy(ImVec2(0, D(2.0f)));
	}

	// 主要增減: the biggest count changes since the start, by value -- what was
	// farmed and what was spent. Market moves on held stock are the 變化
	// table's 市價波動 column.
	void drawMoversCard(const SnapshotDiff& d, const Snapshot& latest, float w, float minH)
	{
		const double rate = latest.divineRate;
		PobUi::CardBegin("##wh_movers", nullptr, nullptr, nullptr, true, w, minH);
		cardHead(u8"主要增減", u8"自起點起",
		         std::string(u8"總值 ") + FormatValue(latest.totalChaos, state_.showDivine, rate), Tok::TextMuted);
		const float innerX = PobUi::CardInnerX(), innerW = PobUi::CardInnerWidth();
		std::vector<const SnapshotDiffLine*> movers;
		for (const SnapshotDiffLine& l : d.gained)
			if (l.dQtyChaos != 0) movers.push_back(&l);
		for (const SnapshotDiffLine& l : d.lost)
			if (l.dQtyChaos != 0) movers.push_back(&l);
		const size_t shownN = (std::min)(movers.size(), (size_t)4);
		std::partial_sort(movers.begin(), movers.begin() + shownN, movers.end(),
		                  [](const SnapshotDiffLine* x, const SnapshotDiffLine* y) {
			                  const double ax = x->dQtyChaos < 0 ? -x->dQtyChaos : x->dQtyChaos;
			                  const double ay = y->dQtyChaos < 0 ? -y->dQtyChaos : y->dQtyChaos;
			                  return ax > ay;
		                  });
		if (d.summaryOnly) HintWrapped(u8"起點或最新快照已精簡為摘要，無法逐項比較。", innerW);
		else if (shownN == 0) HintWrapped(u8"起點之後沒有數量變化。", innerW);
		const float rowH = (std::max)(ImGui::GetTextLineHeight(), D(22.0f)) + D(6.0f);
		for (size_t mi = 0; mi < shownN; mi++) {
			const SnapshotDiffLine* pick = movers[mi];
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float textY = p.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f);
			ImGui::SetCursorScreenPos(ImVec2(innerX, p.y + std::floor((rowH - D(22.0f)) * 0.5f)));
			drawIcon(pick->icon, D(22.0f));
			const std::string val = signedValue(pick->dQtyChaos, rate);
			const float valW = ImGui::CalcTextSize(val.c_str()).x;
			// A long name is cut with an ellipsis before the value instead of
			// running underneath it; the full name is in the tooltip.
			const std::string zh = i18n_.DisplayName(pick->dispEn);
			const float nameX = innerX + D(22.0f) + D(8.0f);
			const std::string nameShown = Ellipsize(zh, innerX + innerW - valW - D(12.0f) - nameX);
			ImGui::SetCursorScreenPos(ImVec2(nameX, textY));
			ImGui::TextUnformatted(nameShown.c_str());
			if (ImGui::IsItemHovered()) PobUi::Tooltip(zh == pick->dispEn ? zh.c_str() : (zh + "\n" + pick->dispEn).c_str());
			ImGui::SetCursorScreenPos(ImVec2(innerX + innerW - valW, textY));
			ImGui::TextColored(V(pick->dQtyChaos >= 0 ? Tok::Success : Tok::Danger), "%s", val.c_str());
			ImGui::SetCursorScreenPos(p);
			ImGui::Dummy(ImVec2(innerW, rowH));
		}
		if (latest.unpricedKinds > 0) {
			char buf[64];
			snprintf(buf, sizeof(buf), u8"另有 %d 種未估價", latest.unpricedKinds);
			PobUi::Hint(buf);
		}
		PobUi::CardEnd();
	}

	// 總值走勢: a card of its own -- title and "start ～ now" on top, three ticks
	// on the left (0 / half / top), the line and its fill, the session start in
	// warning and the newest point in success. No label sits on the plot.
	void drawCurveCard(float w, float minH, float plotH)
	{
		PobUi::CardBegin("##wh_curve_card", nullptr, nullptr, nullptr, true, w, minH);
		const double rateNow = history_.snaps.back().divineRate;
		{
			std::string range;
			if (history_.snaps.size() >= 2)
				range = FormatValue(history_.snaps.front().totalChaos, true, rateNow) + u8" ～ " +
				        FormatValue(history_.snaps.back().totalChaos, true, rateNow);
			cardHead(u8"總值走勢", nullptr, range, Tok::TextMuted);
		}
		const float innerX = PobUi::CardInnerX(), innerW = PobUi::CardInnerWidth();
		if (history_.snaps.size() < 2) {
			HintWrapped(u8"拍過兩份快照之後才有走勢。", innerW);
			PobUi::CardEnd();
			curveChrome_ = PobUi::CardNaturalHeight();
			return;
		}
		// At most 256 points: an even-stride sample keeps the shape; the tooltip
		// below reads from the sampled arrays so hover and pixels agree.
		const size_t n = history_.snaps.size();
		const size_t stride = (n + 255) / 256;
		curvePts_.clear();
		curveUtc_.clear();
		curveRate_.clear();
		for (size_t i = 0; i < n; i += stride) {
			curvePts_.push_back((float)history_.snaps[i].totalChaos);
			curveUtc_.push_back(history_.snaps[i].utc);
			curveRate_.push_back(history_.snaps[i].divineRate);
		}
		if (curveUtc_.back() != history_.snaps.back().utc) {
			curvePts_.push_back((float)history_.snaps.back().totalChaos);
			curveUtc_.push_back(history_.snaps.back().utc);
			curveRate_.push_back(history_.snaps.back().divineRate);
		}
		float lo = curvePts_[0], hi = curvePts_[0];
		for (float v : curvePts_) {
			lo = v < lo ? v : lo;
			hi = v > hi ? v : hi;
		}
		// The axis starts at zero (a stash is never worth less), so the slope
		// reads as the real proportion; a negative value would extend it down.
		if (lo > 0.0f) lo = 0.0f;
		if (hi <= lo) hi = lo + 1.0f;

		// Ticks, in divine like every other value on the page, at today's rate.
		const float mid = lo + (hi - lo) * 0.5f;
		const std::string tHi = FormatValue(hi, true, rateNow), tMid = FormatValue(mid, true, rateNow),
		                  tLo = lo == 0.0f ? std::string("0") : FormatValue(lo, true, rateNow);
		ImFont* small = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const float sPx = small->FontSize;
		auto sw = [&](const std::string& s) { return small->CalcTextSizeA(sPx, FLT_MAX, 0.0f, s.c_str()).x; };
		const float axisW = (std::max)((std::max)(sw(tHi), sw(tMid)), sw(tLo)) + D(8.0f);

		const ImVec2 top = ImGui::GetCursorScreenPos();
		const ImVec2 p0(innerX + axisW, top.y);
		const float pw = innerW - axisW;
		ImGui::SetCursorScreenPos(p0);
		ImGui::InvisibleButton("##wh_curve", ImVec2(pw, plotH));
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float pad = D(5.0f);
		auto yOf = [&](float v) { return p0.y + pad + (1.0f - (v - lo) / (hi - lo)) * (plotH - 2.0f * pad); };
		// Grid and tick labels.
		const float tickVals[3] = { hi, mid, lo };
		const std::string* tickTxt[3] = { &tHi, &tMid, &tLo };
		for (int t = 0; t < 3; t++) {
			const float y = std::floor(yOf(tickVals[t])) + 0.5f;
			dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + pw, y), Tok::BorderSubtle, 1.0f);
			const float tw = sw(*tickTxt[t]);
			dl->AddText(small, sPx, ImVec2(p0.x - D(8.0f) - tw, y - sPx * 0.5f), Tok::TextMuted, tickTxt[t]->c_str());
		}
		const int pts = (int)curvePts_.size();
		auto ptAt = [&](int i) {
			const float fx = pts > 1 ? (float)i / (float)(pts - 1) : 0.0f;
			return ImVec2(p0.x + fx * pw, yOf(curvePts_[i]));
		};
		const std::uint32_t base = curvePts_.back() >= curvePts_.front() ? Tok::Success : Tok::Danger;
		const ImU32 lineCol = base;
		const ImU32 fillCol = (base & 0x00FFFFFFu) | (0x24u << 24); // ~14 %
		const float baseY = yOf(lo);
		// One quad per segment, without anti-aliased edges: AA fringes on the
		// shared sides showed as a vertical seam at every sample.
		const ImDrawListFlags keepFlags = dl->Flags;
		dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
		for (int i = 0; i + 1 < pts; i++) {
			const ImVec2 a = ptAt(i), b = ptAt(i + 1);
			dl->AddQuadFilled(ImVec2(a.x, baseY), ImVec2(b.x, baseY), b, a, fillCol);
		}
		dl->Flags = keepFlags;
		for (int i = 0; i + 1 < pts; i++) dl->AddLine(ptAt(i), ptAt(i + 1), lineCol, 2.0f);
		// The session start (the sample at or just before it) and the newest one.
		int startIdx = 0;
		for (int i = 0; i < pts; i++)
			if (curveUtc_[i] <= history_.sessionStartUtc) startIdx = i;
		const float dotR = D(4.0f);
		if (history_.sessionStartUtc > 0) dl->AddCircleFilled(ptAt(startIdx), dotR, Tok::Warning, 16);
		dl->AddCircleFilled(ptAt(pts - 1), dotR, Tok::Success, 16);

		if (hovered) {
			const float fx = (ImGui::GetIO().MousePos.x - p0.x) / (pw > 1.0f ? pw : 1.0f);
			int idx = (int)(fx * (pts - 1) + 0.5f);
			idx = idx < 0 ? 0 : idx >= pts ? pts - 1 : idx;
			// A ring marks the hovered sample so the tooltip has an anchor.
			dl->AddCircle(ptAt(idx), dotR + D(2.0f), Tok::Text, 16, 1.5f);
			// In divine at that snapshot's own rate.
			const std::string tip = FormatUtcLocal(curveUtc_[idx]) + "\n" + FormatValue(curvePts_[idx], true, curveRate_[idx]);
			PobUi::Tooltip(tip.c_str());
		}
		ImGui::SetCursorScreenPos(ImVec2(innerX, top.y));
		ImGui::Dummy(ImVec2(innerW, plotH));
		PobUi::CardEnd();
		// What the card needs besides its plot, so the standalone chart can grow
		// its plot to the height of 主要增減 beside it.
		curveChrome_ = PobUi::CardNaturalHeight() - plotH;
	}

	// The bottom of the page: the snapshot list on the left, the selected
	// snapshot's detail on the right, both filling what is left of the height.
	void drawBottom()
	{
		const Snapshot* sel = selectedSnap();
		const Snapshot* prev = snapBefore(sel);
		const float gap = D(12.0f);
		const float h = (std::max)(ImGui::GetContentRegionAvail().y, D(260.0f));
		const float leftW = D(240.0f); // date + the 起點 pill + the change, side by side
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float availW = ImGui::GetContentRegionAvail().x;

		// The list sits on a card; the card is drawn here because the list
		// scrolls (a child window), which a PobUi card cannot hold.
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, p + ImVec2(leftW, h), Tok::Surface1, D(8.0f));
		dl->AddRect(p, p + ImVec2(leftW, h), Tok::Border, D(8.0f), 0, 1.0f);
		const float pad = D(8.0f);
		ImGui::SetCursorScreenPos(p + ImVec2(pad, pad));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		ImGui::BeginChild("##wh_snapside", ImVec2(leftW - pad * 2.0f, h - pad * 2.0f), false);
		drawSnapSidebar(sel);
		ImGui::EndChild();

		ImGui::SetCursorScreenPos(ImVec2(p.x + leftW + gap, p.y));
		ImGui::BeginChild("##wh_content", ImVec2(availW - leftW - gap, h), false);
		drawDetail(prev, sel);
		ImGui::EndChild();
		ImGui::PopStyleColor();

		ImGui::SetCursorScreenPos(p);
		ImGui::Dummy(ImVec2(availW, h));
	}

	// The filter row over the table: view, category, search, and -- on 變化 --
	// what the selected snapshot changed against the one before it.
	void drawDetail(const Snapshot* prev, const Snapshot* sel)
	{
		static const char* const kViews[] = { u8"變化", u8"持有明細" };
		static const char* const kCats[] = { u8"全部類別", u8"通貨", u8"寶石", u8"命運卡",
			                                 u8"傳奇",     u8"地圖", u8"其他", u8"未估價" };
		const float gap = D(8.0f);
		const ImVec2 row = ImGui::GetCursorScreenPos();
		const float rowW = ImGui::GetContentRegionAvail().x;
		PobUi::Segmented("##wh_view", &detailView_, kViews, 2);
		ImGui::SameLine(0, gap);
		if (PobUi::Select("##wh_cat", &catFilter_, kCats, nullptr, 8, D(130.0f))) filterDirty_ = true;
		ImGui::SameLine(0, gap);

		// "與 09/11 20:59 相比 +16.9d", right-aligned; the search takes the rest.
		std::string cmpLabel, cmpValue;
		double cmpDelta = 0.0;
		SnapshotDiff diff;
		const bool changes = detailView_ == 0;
		if (changes && sel && prev) {
			diff = DiffSnapshots(*prev, *sel);
			cmpDelta = diff.dTotalChaos;
			cmpLabel = std::string(u8"與 ") + FormatUtcLocal(prev->utc) + u8" 相比";
			cmpValue = signedValue(cmpDelta, sel->divineRate);
		}
		ImFont* small = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const float labelW = cmpLabel.empty() ? 0.0f : small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, cmpLabel.c_str()).x;
		const float valueW = cmpValue.empty() ? 0.0f : ImGui::CalcTextSize(cmpValue.c_str()).x;
		const float cmpW = cmpLabel.empty() ? 0.0f : labelW + D(6.0f) + valueW;
		const float searchX = ImGui::GetCursorScreenPos().x;
		const float searchW = (std::max)(D(120.0f), row.x + rowW - searchX - (cmpW > 0 ? cmpW + D(12.0f) : 0.0f));
		if (PobUi::SearchField("##wh_search", searchBuf_, (int)sizeof(searchBuf_), u8"搜尋名稱…", searchW)) {
			search_ = searchBuf_;
			filterDirty_ = true;
		}
		if (cmpW > 0.0f) {
			const float h = PobUi::ControlH();
			const float x = row.x + rowW - cmpW;
			ImGui::SetCursorScreenPos(ImVec2(x, row.y + std::floor((h - small->FontSize) * 0.5f)));
			PobUi::Hint(cmpLabel.c_str());
			const bool hov = ImGui::IsItemHovered();
			ImGui::SetCursorScreenPos(ImVec2(x + labelW + D(6.0f), row.y + std::floor((h - ImGui::GetFontSize()) * 0.5f)));
			ImGui::TextColored(V(cmpDelta >= 0 ? Tok::Success : Tok::Danger), "%s", cmpValue.c_str());
			if ((hov || ImGui::IsItemHovered()) && !diff.summaryOnly) {
				const std::string tip = std::string(u8"增減 ") + signedValue(diff.qtyGain + diff.qtyLoss, sel->divineRate) +
				                        u8"、市價 " + signedValue(diff.priceMove, sel->divineRate);
				PobUi::Tooltip(tip.c_str());
			}
		}
		ImGui::SetCursorScreenPos(row);
		ImGui::Dummy(ImVec2(rowW, PobUi::ControlH()));
		ImGui::Dummy(ImVec2(0, D(2.0f)));

		if (changes) drawChanges(prev, sel, diff);
		else drawLines(sel);
	}

	void drawIcon(const std::string& artPath, float size)
	{
		if (!artPath.empty()) icons_.RequestPath(artPath);
		const unsigned tex = artPath.empty() ? 0 : icons_.TextureByPath(artPath);
		if (tex) {
			ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(size, size));
		} else {
			// No art, or not downloaded yet: the design's grey square holds the place.
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(size, size), Tok::Surface3, D(4.0f));
			ImGui::Dummy(ImVec2(size, size));
		}
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

	// Fixed table columns sized from what they hold rather than a constant: at
	// a larger font (with the scrollbar taking its share) a fixed width cut the
	// figures.
	float fitCol(const char* header, const char* sample) const
	{
		// The header also carries the sort arrow.
		const float h = ImGui::CalcTextSize(header).x + ImGui::GetFontSize();
		const float s = ImGui::CalcTextSize(sample).x;
		return (h > s ? h : s) + ImGui::GetStyle().CellPadding.x * 2.0f;
	}

	// The two tables share their look (design system): a muted header on
	// surface-1, rows split by border-subtle, no zebra.
	void pushTableStyle()
	{
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, V(Tok::Surface1));
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, V(Tok::BorderSubtle));
		ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, V(Tok::Border));
	}
	void popTableStyle() { ImGui::PopStyleColor(3); }

	// The header row spelled out: labels muted, numeric columns right-aligned
	// (before the sort arrow), and a tooltip where a column needs explaining.
	void headerRow(int cols, const bool* rightAlign, const char* const* tips)
	{
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		const float arrowW = std::floor(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x);
		for (int c = 0; c < cols; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			const char* label = ImGui::TableGetColumnName(c);
			const bool sortable = !(ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_NoSort);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float cellW = ImGui::GetContentRegionAvail().x;
			ImGui::PushID(c);
			ImGui::TableHeader("##hdr");
			ImGui::PopID();
			const bool hov = ImGui::IsItemHovered();
			if (label && label[0] != '#') {
				const float tw = ImGui::CalcTextSize(label).x;
				const float x = rightAlign[c] ? p.x + cellW - tw - (sortable ? arrowW : 0.0f) : p.x;
				ImGui::GetWindowDrawList()->AddText(ImVec2(x, p.y), Tok::TextMuted, label);
			}
			if (hov && tips && tips[c]) PobUi::Tooltip(tips[c]);
		}
	}

	// One click copies the name in the interface language (user rule,
	// 2026-10-08): the Chinese name under 繁中, the English one otherwise.
	// Callers wrap each row in PushID, which keeps the buttons' IDs apart.
	void copyCell(const std::string& dispEn)
	{
		const std::string name = copyZh_ ? i18n_.DisplayName(dispEn) : dispEn;
		const float s = ImGui::GetTextLineHeight();
		const float avail = ImGui::GetContentRegionAvail().x;
		if (avail > s) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - s) * 0.5f);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool click = ImGui::InvisibleButton("##copy", ImVec2(s, s));
		const bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (hov) dl->AddRectFilled(p, p + ImVec2(s, s), Tok::Surface3, D(4.0f));
		const std::uint32_t col = hov ? Tok::Text : Tok::AccentText;
		if (PobUi::Fonts().icons) {
			const float px = SmallPx();
			PobUi::IconAt(dl, p + ImVec2((s - px) * 0.5f, (s - px) * 0.5f), PobIcon::Copy, col, px);
		} else {
			// No icon font: two offset squares, the usual copy glyph.
			const float q = std::floor(s * 0.38f), o = std::floor(s * 0.18f);
			const ImVec2 c0 = p + ImVec2(s * 0.5f - q * 0.5f - o * 0.5f, s * 0.5f - q * 0.5f - o * 0.5f);
			dl->AddRect(c0, c0 + ImVec2(q, q), col, 1.0f);
			dl->AddRect(c0 + ImVec2(o, o), c0 + ImVec2(o + q, o + q), col, 1.0f);
		}
		if (hov) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			PobUi::Tooltip((std::string(u8"複製：") + name).c_str());
		}
		if (click) {
			ImGui::SetClipboardText(name.c_str());
			PobUi::ShowToast((std::string(u8"已複製：") + name).c_str(), PobUi::Tone::Ok);
		}
	}

	// The name cell shared by the detail and diff tables: the display name, the
	// English one on hover, and a right-click menu that copies either.
	void drawNameCell(const std::string& dispEn, bool dim)
	{
		const std::string zh = i18n_.DisplayName(dispEn);
		if (dim) ImGui::TextColored(V(Tok::TextMuted), "%s", zh.c_str());
		else ImGui::TextUnformatted(zh.c_str());
		if (zh != dispEn && ImGui::IsItemHovered()) PobUi::Tooltip(dispEn.c_str());
		if (ImGui::BeginPopupContextItem("##name_ctx")) {
			if (ImGui::MenuItem(u8"複製名稱")) ImGui::SetClipboardText(zh.c_str());
			if (zh != dispEn && ImGui::MenuItem(u8"複製英文名稱")) ImGui::SetClipboardText(dispEn.c_str());
			ImGui::EndPopup();
		}
	}

	void drawLines(const Snapshot* latest)
	{
		if (!latest) {
			PobUi::Hint(u8"尚無快照。");
			return;
		}
		if (latest->summary) {
			char msg[160];
			snprintf(msg, sizeof(msg),
			         u8"此快照已精簡為摘要，只保留總值（原有 %d 種物品）。"
			         u8"最近 %d 份與起點快照保留完整明細。",
			         latest->lineCount, WarehouseHistory::kFullKept);
			HintWrapped(msg, 0.0f);
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
		const float iconSz = (std::min)(ImGui::GetTextLineHeight(), D(20.0f));
		pushTableStyle();
		if (ImGui::BeginTable("##wh_lines", 6,
		                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("##ic", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
			                        iconSz + ImGui::GetStyle().CellPadding.x * 2.0f);
			ImGui::TableSetupColumn(u8"物品", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(u8"數量",
			                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"數量", "999999"));
			ImGui::TableSetupColumn(u8"單價",
			                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"單價", "~9999.9 c"));
			ImGui::TableSetupColumn(u8"小計",
			                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"小計", "~9999.9 d"));
			ImGui::TableSetupColumn("##copy", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
			                        ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.x * 2.0f);
			static const bool kRight[6] = { false, false, true, true, true, false };
			headerRow(6, kRight, nullptr);

			// Sorted per frame from the click-selected header (list is small).
			if (const ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
				if (sp->SpecsCount > 0) {
					const ImGuiTableColumnSortSpecs& s0 = sp->Specs[0];
					const bool asc = s0.SortDirection != ImGuiSortDirection_Descending;
					std::stable_sort(sortedLines_.begin(), sortedLines_.end(), [&](int a, int b) {
						const SnapshotLine& x = latest->lines[a];
						const SnapshotLine& y = latest->lines[b];
						int cmp = 0;
						switch (s0.ColumnIndex) {
						case 1: cmp = i18n_.DisplayName(x.dispEn).compare(i18n_.DisplayName(y.dispEn)); break;
						case 2: cmp = x.count < y.count ? -1 : x.count > y.count ? 1 : 0; break;
						case 3: cmp = x.chaosEach < y.chaosEach ? -1 : x.chaosEach > y.chaosEach ? 1 : 0; break;
						default: cmp = x.chaosTotal < y.chaosTotal ? -1 : x.chaosTotal > y.chaosTotal ? 1 : 0; break;
						}
						if (cmp == 0) cmp = x.key.compare(y.key);
						return asc ? cmp < 0 : cmp > 0;
					});
				}
			}

			const ImVec4 text = V(Tok::Text), muted = V(Tok::TextMuted), faint = V(Tok::TextFaint);
			ImGuiListClipper clip;
			clip.Begin((int)sortedLines_.size());
			while (clip.Step()) {
				for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
					const SnapshotLine& l = latest->lines[sortedLines_[row]];
					ImGui::TableNextRow();
					ImGui::PushID(row);
					ImGui::TableNextColumn();
					drawIcon(l.icon, iconSz);
					ImGui::TableNextColumn();
					drawNameCell(l.dispEn, !l.priced);
					ImGui::TableNextColumn();
					RightText(std::to_string(l.count).c_str(), text);
					ImGui::TableNextColumn();
					if (l.priced && l.estimated) {
						RightText(("~" + FormatValue(l.chaosEach, state_.showDivine, rate)).c_str(), muted);
						if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"估計值：市場無批量掛牌，以保守下限 0.5c 計");
					} else if (l.priced) {
						RightText(FormatValue(l.chaosEach, state_.showDivine, rate).c_str(), text);
					} else {
						RightText(u8"未估價", faint);
					}
					ImGui::TableNextColumn();
					if (l.priced && l.estimated)
						RightText(("~" + FormatValue(l.chaosTotal, state_.showDivine, rate)).c_str(), muted);
					else if (l.priced)
						RightText(FormatValue(l.chaosTotal, state_.showDivine, rate).c_str(), text);
					else
						RightText(u8"—", faint);
					ImGui::TableNextColumn();
					copyCell(l.dispEn);
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
		popTableStyle();
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
	// `d` is prev -> sel, already computed by the filter row.
	void drawChanges(const Snapshot* prev, const Snapshot* sel, const SnapshotDiff& d)
	{
		if (!sel || !prev) {
			PobUi::Hint(u8"這是最早的快照，沒有更早的比較對象。");
			return;
		}
		const double rate = sel->divineRate;
		if (d.summaryOnly) {
			HintWrapped(sel->summary ? u8"此快照已精簡為摘要，只保留總值，無法逐項比較。"
			                         : u8"上一份快照已精簡為摘要，只能比較總值。",
			            0.0f);
			return;
		}

		std::vector<const SnapshotDiffLine*> rows;
		for (const SnapshotDiffLine& r : d.gained) rows.push_back(&r);
		for (const SnapshotDiffLine& r : d.lost) rows.push_back(&r);

		const float iconSz = (std::min)(ImGui::GetTextLineHeight(), D(20.0f));
		pushTableStyle();
		if (ImGui::BeginTable("##wh_changes", 7,
		                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("##ic", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
			                        iconSz + ImGui::GetStyle().CellPadding.x * 2.0f);
			ImGui::TableSetupColumn(u8"物品", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(u8"數量",
			                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"數量", "+999999"));
			ImGui::TableSetupColumn(u8"單價",
			                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"單價", "~9999.9 c"));
			ImGui::TableSetupColumn(u8"增減價值",
			                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"增減價值", "+9999.9 d"));
			ImGui::TableSetupColumn(u8"市價波動",
			                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"市價波動", "+9999.9 d"));
			ImGui::TableSetupColumn("##copy", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
			                        ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.x * 2.0f);
			static const bool kRight[7] = { false, false, true, true, true, true, false };
			static const char* const kTips[7] = { nullptr, nullptr, nullptr, nullptr,
				                                  u8"數量變化 × 單價：刷到或用掉的價值",
				                                  u8"原本就持有的數量 × 單價漲跌；報價消失的物品也算在這裡",
				                                  nullptr };
			headerRow(7, kRight, kTips);

			// The diff is recomputed per frame anyway, so the sort is applied per
			// frame too -- a few hundred rows cost nothing.
			if (const ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
				if (sp->SpecsCount > 0) {
					const ImGuiTableColumnSortSpecs& s0 = sp->Specs[0];
					const bool asc = s0.SortDirection != ImGuiSortDirection_Descending;
					std::stable_sort(rows.begin(), rows.end(), [&](const SnapshotDiffLine* a, const SnapshotDiffLine* b) {
						int cmp = 0;
						switch (s0.ColumnIndex) {
						case 1: cmp = i18n_.DisplayName(a->dispEn).compare(i18n_.DisplayName(b->dispEn)); break;
						case 2: cmp = a->dCount < b->dCount ? -1 : a->dCount > b->dCount ? 1 : 0; break;
						case 3: cmp = a->each < b->each ? -1 : a->each > b->each ? 1 : 0; break;
						case 5: cmp = a->dPriceChaos < b->dPriceChaos ? -1 : a->dPriceChaos > b->dPriceChaos ? 1 : 0; break;
						default: cmp = a->dQtyChaos < b->dQtyChaos ? -1 : a->dQtyChaos > b->dQtyChaos ? 1 : 0; break;
						}
						if (cmp == 0) cmp = a->key.compare(b->key);
						return asc ? cmp < 0 : cmp > 0;
					});
				}
			}
			const ImVec4 text = V(Tok::Text), muted = V(Tok::TextMuted), faint = V(Tok::TextFaint);
			const ImVec4 good = V(Tok::Success), bad = V(Tok::Danger);
			int rowId = 0;
			for (const SnapshotDiffLine* r : rows) {
				if (!diffLinePassesFilter(*r)) continue;
				ImGui::TableNextRow();
				ImGui::PushID(rowId++);
				ImGui::TableNextColumn();
				drawIcon(r->icon, iconSz);
				ImGui::TableNextColumn();
				drawNameCell(r->dispEn, r->each <= 0.0);
				ImGui::TableNextColumn();
				{
					char buf[32];
					snprintf(buf, sizeof(buf), "%+lld", r->dCount);
					RightText(buf, r->dCount >= 0 ? good : bad);
				}
				ImGui::TableNextColumn();
				if (r->each > 0) {
					if (r->estimated) RightText(("~" + FormatValue(r->each, state_.showDivine, rate)).c_str(), muted);
					else RightText(FormatValue(r->each, state_.showDivine, rate).c_str(), text);
				} else {
					RightText(u8"—", faint);
				}
				// Half a cent and under is float noise from the split, not money.
				auto moneyCell = [&](double v) {
					if (v > 0.005 || v < -0.005) RightText(signedValue(v, rate).c_str(), v >= 0 ? good : bad);
					else RightText(u8"—", faint);
				};
				ImGui::TableNextColumn();
				moneyCell(r->dQtyChaos);
				ImGui::TableNextColumn();
				moneyCell(r->dPriceChaos);
				ImGui::TableNextColumn();
				copyCell(r->dispEn);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		popTableStyle();
	}

	// The left timeline: one row per snapshot, newest first, coloured by its
	// change against the one before it. Click selects; right-click for actions.
	void drawSnapSidebar(const Snapshot* sel)
	{
		{
			char head[64];
			snprintf(head, sizeof(head), u8"快照 %d 份 · 右鍵設為起點", (int)history_.snaps.size());
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + D(6.0f));
			PobUi::Overline(head);
		}
		const float rowH = (std::max)(ImGui::GetTextLineHeight(), D(24.0f)) + D(6.0f);
		ImGui::PushStyleColor(ImGuiCol_Header, V(Tok::AccentSoft));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, V(Tok::Surface3));
		ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
		for (int i = (int)history_.snaps.size() - 1; i >= 0; i--) {
			const Snapshot& s = history_.snaps[i];
			const Snapshot* prev = i > 0 ? &history_.snaps[i - 1] : nullptr;
			const double delta = prev ? s.totalChaos - prev->totalChaos : 0.0;

			ImGui::PushID(i);
			const std::string date = FormatUtcLocal(s.utc);
			// The change, uniformly in divine (user request): raw chaos numbers at
			// this width were unreadable (+214262). Chaos only when no rate is known.
			std::string txt = "--";
			if (prev) {
				if (s.divineRate > 0) {
					const double dv = delta / s.divineRate;
					const double mag = dv < 0 ? -dv : dv;
					char buf[32];
					snprintf(buf, sizeof(buf), mag >= 100 ? "%+.0f d" : mag >= 1 ? "%+.1f d" : "%+.2f d", dv);
					txt = buf;
				} else {
					txt = (delta >= 0 ? "+" : "") + FormatChaos(delta) + " c";
				}
			}
			const std::uint32_t deltaCol = !prev ? Tok::TextFaint : delta >= 0 ? Tok::Success : Tok::Danger;
			const bool isSel = sel && sel->utc == s.utc;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			if (ImGui::Selectable("##snap", isSel, 0, ImVec2(0, rowH)))
				selSnapUtc_ = s.utc == history_.snaps.back().utc ? 0 : s.utc;
			const bool hov = ImGui::IsItemHovered();
			if (ImGui::BeginPopupContextItem("##snap_ctx")) {
				if (s.utc == history_.sessionStartUtc)
					ImGui::TextDisabled(u8"目前的起點");
				else if (ImGui::MenuItem(u8"設為起點", nullptr, false, !s.summary))
					setStartRequest_ = s.utc; // applied after the frame: s points into history_
				if (s.summary) ImGui::TextDisabled(u8"摘要快照沒有逐項明細，不能當起點");
				ImGui::EndPopup();
			}
			if (s.summary && hov) {
				char tip[96];
				snprintf(tip, sizeof(tip), u8"摘要快照：只保留總值（原有 %d 種物品）", s.lineCount);
				PobUi::Tooltip(tip);
			}
			// Date left (dim for a summary), the start pill after it, the change
			// right-aligned: all measured, so nothing runs into anything.
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float ty = p.y + std::floor((rowH - ImGui::GetFontSize()) * 0.5f);
			const float x0 = p.x + D(6.0f);
			dl->AddText(ImVec2(x0, ty), s.summary ? Tok::TextMuted : Tok::Text, date.c_str());
			const float tw = ImGui::CalcTextSize(txt.c_str()).x;
			dl->AddText(ImVec2(p.x + w - D(6.0f) - tw, ty), deltaCol, txt.c_str());
			if (s.utc == history_.sessionStartUtc) {
				const ImVec2 keep = ImGui::GetCursorScreenPos();
				ImGui::SetCursorScreenPos(ImVec2(x0 + ImGui::CalcTextSize(date.c_str()).x + D(8.0f),
				                                 p.y + std::floor((rowH - D(24.0f)) * 0.5f)));
				PobUi::StatusPill(PobUi::Tone::Warn, u8"起點");
				ImGui::SetCursorScreenPos(keep);
			}
			ImGui::PopID();
		}
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
	}

	// The three pages, whether our own tab bar or a host's picks them. The host
	// speaks in ints (WarehouseEmbed::page) so warehouse_tool.h need not know
	// this type.
	enum class Page { Revenue, Help, Settings };

	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir_;
	WarehouseEmbed embed_;   // what an embedding host adds; empty when standalone
	int ownPage_ = 0;        // standalone: 0 收益, 1 說明, 2 設定
	bool copyZh_ = false;    // the interface is 繁中: copy buttons copy the Chinese name

	WarehouseUiState state_;
	bool stateDirty_ = false;
	bool secretDirty_ = false; // the pending save is about the session id itself
	WarehouseHistory history_;
	std::string historyLeague_; // the league whose file history_ holds
	WarehouseGuard guard_;      // the shared request guard, refreshed every 2 s
	int pumpedFrame_ = -1;      // pump() runs once per frame
	long long autoArmedUtc_ = 0;       // panel opened / auto switched on
	long long autoLastAttemptUtc_ = 0; // last auto start this panel made
	long long autoDueUtc_ = 0;         // for the hint; 0 = not scheduled
	enum class AutoBlock { None, Missing, Auth } autoBlock_ = AutoBlock::None;
	WarehouseService svc_;
	WarehouseService::Status st_;
	WarehousePhase lastPhase_ = WarehousePhase::Idle; // for the "done" toast
	FilterI18n i18n_;
	IconManager icons_;

	std::vector<StashTabInfo> tabs_;
	std::vector<std::string> leagues_;

	// Side-by-side cards share a height: each row's tallest content as measured
	// last frame (ImGui 1.90 has no child auto-resize to lean on).
	float kpiMinH_ = 0.0f;
	float row2MinH_ = 0.0f;
	float moversNat_ = 0.0f;   // 主要增減's own height: the standalone chart fills to it
	float curveChrome_ = 0.0f; // the chart card's height minus its plot

	std::vector<float> curvePts_;
	std::vector<long long> curveUtc_;
	std::vector<double> curveRate_; // each sample's own divine rate
	std::vector<int> sortedLines_;
	long long linesForUtc_ = 0;
	long long selSnapUtc_ = 0; // sidebar selection; 0 = follow the newest
	int detailView_ = 0;       // 0 變化, 1 持有明細
	int catFilter_ = 0;
	long long setStartRequest_ = 0; // "設為起點" picked this frame; applied after drawing
	double lastStatCheck_ = -10.0;  // ImGui time of the last history-file stat
	FILETIME histWrite_{};          // history file's last-write time as last seen
	char searchBuf_[128] = "";
	std::string search_;            // searchBuf_, as the filters read it
	bool filterDirty_ = false;

	ToolCloseState close_ = ToolCloseState::Open;
};

} // namespace

IToolPanel* CreateWarehousePanel()
{
	return new WarehousePanel();
}

IToolPanel* CreateWarehousePanelEmbedded(const WarehouseEmbed& embed)
{
	return new WarehousePanel(embed);
}

void ShowWarehouseTool(const std::wstring& exeDir, const std::wstring& game,
                       const std::wstring& locale)
{
	WarehousePanel panel;
	ToolWindowDesc desc;
	// "PobTools — 倉庫收益"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 \xe5\x80\x89\xe5\xba\xab\xe6\x94\xb6\xe7\x9b\x8a";
	desc.defW = 1280;
	desc.defH = 880;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
