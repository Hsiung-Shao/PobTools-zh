#include "warehouse_tool.h"

#include "error_log.h"
#include "filter_i18n.h"
#include "icon_manager.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_theme.h"
#include "warehouse_format.h"
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

// "08/29(五) 18:03" for the snapshot card: the weekday reads at a glance (as on
// Wealthy Exile), the date keeps last week's Friday apart from this one.
std::string FormatUtcWeek(long long utc)
{
	static const char* const kWd[] = { u8"日", u8"一", u8"二", u8"三", u8"四", u8"五", u8"六" };
	const SYSTEMTIME st = LocalTimeOf(utc);
	char buf[48];
	snprintf(buf, sizeof(buf), u8"%02d/%02d(%s) %02d:%02d", st.wMonth, st.wDay,
	         kWd[st.wDayOfWeek % 7], st.wHour, st.wMinute);
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

// A dim note that wraps with its column instead of running under the edge.
void Hint(const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, kDim);
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
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

		// Which page: the host's own tab bar when it has one (the atlas planner's
		// 收益 / 說明 / 設定), else ours.
		Page page = Page::Revenue;
		if (embed_.page) {
			const int p = *embed_.page;
			page = p == 1 ? Page::Help : p == 2 ? Page::Settings : Page::Revenue;
		} else if (ImGui::BeginTabBar("##wh_pages")) {
			// A first run with no session id saved opens on the page that explains
			// what this is and what pasting one means.
			const ImGuiTabItemFlags open =
			    firstFrame_ && state_.sessid.empty() ? ImGuiTabItemFlags_SetSelected : 0;
			if (ImGui::BeginTabItem(u8"收益")) ImGui::EndTabItem();
			if (ImGui::BeginTabItem(u8"說明", nullptr, open)) {
				page = Page::Help;
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(u8"設定")) {
				page = Page::Settings;
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		firstFrame_ = false;

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

	void drawAutoHint()
	{
		if (state_.autoMinutes <= 0) {
			Hint(u8"開啟後依設定的間隔自動拍快照（面板開著就會運作，分頁沒顯示也一樣）。");
			return;
		}
		if (autoBlock_ == AutoBlock::Missing) {
			Hint(u8"暫停：需要 POESESSID、聯盟與至少一個分頁（在「設定」）。");
			return;
		}
		if (autoBlock_ == AutoBlock::Auth) {
			Hint(u8"暫停：session 無效，到「設定」重新驗證後恢復。");
			return;
		}
		if (autoDueUtc_ > 0) {
			const std::string t = std::string(u8"下次自動快照：") + FormatUtcLocal(autoDueUtc_);
			Hint(t.c_str());
		}
	}

	// ---- 收益 page -----------------------------------------------------------

	// Full width: a host's own content rides in the top row as a first card
	// (drawTopCards), not in a column of its own.
	void drawRevenuePage()
	{
		ImGui::BeginChild("##wh_main", ImVec2(0, 0), false);
		drawMain();
		ImGui::EndChild();
	}

	// 聯盟 and its refresh, on the 設定 page beside the tab list it decides.
	void drawLeaguePicker()
	{
		ImGui::Text(u8"聯盟");
		// Leave exactly the button's own width: a fixed 40px cut 「更新」 in half
		// at larger font sizes.
		const ImGuiStyle& sty = ImGui::GetStyle();
		ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(u8"更新").x + sty.FramePadding.x * 2.0f +
		                          sty.ItemSpacing.x));
		if (leagues_.empty()) {
			if (ImGui::InputText("##wh_league", &state_.sel().league)) stateDirty_ = true;
		} else {
			if (ImGui::BeginCombo("##wh_league_c", state_.sel().league.c_str())) {
				for (const std::string& l : leagues_) {
					if (ImGui::Selectable(l.c_str(), l == state_.sel().league)) {
						if (state_.sel().league != l) {
							state_.sel().league = l;
							state_.sel().tabIds.clear();
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
	}

	void drawAutoPicker()
	{
		ImGui::Text(u8"自動快照");
		auto autoLabel = [](int minutes) {
			return minutes <= 0 ? std::string(u8"關閉")
			                    : std::string(u8"每 ") + std::to_string(minutes / 60) + u8" 小時";
		};
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##wh_auto", autoLabel(state_.autoMinutes).c_str())) {
			for (int h = 0; h <= 6; h++) {
				const int m = h * 60;
				if (ImGui::Selectable(autoLabel(m).c_str(), state_.autoMinutes == m)) {
					// Switching it on starts the count; changing the interval does not.
					if (state_.autoMinutes == 0 && m > 0) autoArmedUtc_ = NowUtc();
					state_.autoMinutes = m;
					stateDirty_ = true;
				}
			}
			ImGui::EndCombo();
		}
		drawAutoHint();
	}

	// ---- 說明 page -----------------------------------------------------------

	// What this tool does, how to drive it, and -- the part that matters before
	// anyone publishes it -- what pasting a POESESSID means, what GGG has said
	// about that, and exactly what this program does and does not do with it.
	// Prose lives here rather than in a data file: every other tool panel keeps
	// its strings in the .cpp too, and the launcher's string table carries only
	// the button label.
	void drawHelpPage()
	{
		ImGui::BeginChild("##wh_help", ImVec2(0, 0), false);
		// Wrap to this child, not to the window: the page is drawn inside the
		// atlas planner's embed as well, where the two differ.
		ImGui::PushTextWrapPos(0.0f);
		auto para = [](const char* s) { ImGui::TextWrapped("%s", s); };
		// Bullet + SameLine + TextWrapped: wrapped lines align under the text
		// instead of running back under the bullet.
		auto item = [](const char* s) {
			ImGui::Bullet();
			ImGui::SameLine();
			ImGui::TextWrapped("%s", s);
		};

		ImGui::SeparatorText(u8"這是什麼");
		para(u8"把你勾選的倉庫分頁拍成快照，之後每拍一次就與上一次比對，算出這段期間倉庫"
		     u8"價值的變化。變化拆成兩部分：數量變化（東西真的多了或少了）與市價波動"
		     u8"（東西沒動，行情變了）。估價來自 poe.ninja，目前只支援 PoE1 國際服。");
		para(u8"在輿圖策略裡，一段收益紀錄可以綁進方案，和每張地圖的成本一起算出刷圖收益"
		     u8"／小時，並隨分享碼帶給別人。");

		ImGui::SeparatorText(u8"快速上手");
		item(u8"到「設定」頁填入帳號名稱（官網帳號頁上的名稱，含 #1234）。");
		item(u8"貼上 POESESSID，按「驗證 session」確認可用。");
		item(u8"選好聯盟，按「取得倉庫分頁清單」，勾選要統計的分頁。");
		item(u8"回「收益」頁按「立即快照」。");
		item(u8"第一次快照只是起點；拍過第二次之後才會有變化與收益數字。");

		ImGui::SeparatorText(u8"POESESSID 怎麼取得");
		item(u8"用瀏覽器登入 www.pathofexile.com。");
		item(u8"按 F12 開開發者工具 → 應用程式（Application）/ 儲存空間 → Cookies → "
		     u8"https://www.pathofexile.com。");
		item(u8"複製 POESESSID 的值（32 個十六進位字元）。");
		ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
		para(u8"這串等同你的登入權杖：拿到的人不需要密碼就能以你的身分登入官網。不要貼給"
		     u8"任何人，包含我們。在官網登出會讓它立刻失效，也就等於收回這個工具的存取權。");
		ImGui::PopStyleColor();

		ImGui::SeparatorText(u8"風險與官方立場");
		ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
		para(u8"GGG 沒有為第三方工具提供 session 授權，官方開發者也公開表示過：不要把 "
		     u8"POESESSID 放進任何第三方程式。");
		ImGui::PopStyleColor();
		item(u8"本功能是官方 OAuth 申請核准前的測試通道。核准之後會改用官方授權，這個貼上 "
		     u8"session id 的做法就會退場。");
		item(u8"目前找不到因為第三方工具「唯讀讀取倉庫」而被停權的案例（已知的停權都與自動"
		     u8"化操作有關），但這不代表官方允許，政策也可能改變。");
		item(u8"覺得不妥就不要用這個功能，PobTools 其他功能完全不受影響。");

		ImGui::SeparatorText(u8"這個工具怎麼處理你的 session id");
		item(u8"只放進送往 pathofexile.com 的 Cookie 標頭，不會出現在網址、記錄檔、錯誤報告"
		     u8"或自檢報告裡。");
		item(u8"全程只發 GET，只讀角色清單與倉庫分頁；不會替你發文、改設定或動用點數。");
		item(u8"不去翻瀏覽器的 cookie 資料庫，一律由你自己貼上。");
		item(u8"不上傳到任何伺服器（包含我們自己的），沒有任何統計或遙測。");
		item(u8"輸入框以圓點遮蔽；預設不記住，勾了「記住」才以 Windows 使用者加密（DPAPI）"
		     u8"存在本機，換使用者或換電腦都解不開。");

		ImGui::SeparatorText(u8"請求頻率與限流");
		item(u8"快照最多 10 分鐘一次：手動與自動共用，所有視窗與行程也共用同一份限制。");
		item(u8"同一次快照裡，每個分頁的請求至少間隔 2 秒，不會一次打一堆。");
		item(u8"收到 429 就停手，照伺服器指定的 Retry-After 等待；伺服器回報額度快滿時會自動"
		     u8"放慢。");
		item(u8"GGG 的請求額度是整個帳號一池：官網、交易站工具和這個功能共用。快照期間盡量"
		     u8"別同時在交易站狂刷。");

		ImGui::SeparatorText(u8"資料放在哪、怎麼清除");
		item(u8"設定：PobTools\\warehouse_ui.json（session id 只以 DPAPI 密文存在，而且勾了"
		     u8"「記住」才會寫進去）。");
		item(u8"快照歷史：PobTools\\warehouse\\，依聯盟分檔。");
		item(u8"限流狀態：PobTools\\warehouse_guard.json。");
		item(u8"「設定」頁的「清除」按鈕會立刻把 session id 從記憶體與檔案裡抹掉；帳號名稱、"
		     u8"勾選的分頁與快照歷史都保留。");
		item(u8"不勾「記住」時，session id 只留在記憶體：程式關掉就沒了，下次要重貼。程式開著"
		     u8"的期間，自動快照照常運作。");

		ImGui::Dummy(ImVec2(0.0f, 8.0f * host_->scale));
		Hint(u8"設定都在「設定」分頁。");
		ImGui::PopTextWrapPos();
		ImGui::EndChild();
	}

	// ---- 設定 page -----------------------------------------------------------

	void drawSettingsPage()
	{
		// Two columns: account, league, schedule (and the host's record buttons)
		// on the left; the stash tab list -- the page's longest thing -- filling
		// the right.
		const float scale = host_->scale;
		const float leftW = (std::min)(
		    420.0f * scale, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
		ImGui::BeginChild("##wh_settings", ImVec2(leftW, 0), false);
		ImGui::TextColored(kWarn, u8"測試性質功能");
		ImGui::TextWrapped(u8"以 POESESSID 讀取國際服 PoE1 倉庫。session id 等同帳號"
		                   u8"登入權杖，請勿分享給任何人；GGG 官方不建議把它交給第三方程式，"
		                   u8"日後將改接官方授權通道。詳見「說明」分頁。");
		ImGui::Separator();

		ImGui::Text(u8"帳號名稱");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputText("##wh_acct", &state_.accountName)) stateDirty_ = true;
		Hint(u8"帳號頁的名稱，如 Name#1234");

		ImGui::Text("POESESSID");
		// Leave room for 清除 on the same line.
		const ImGuiStyle& sty = ImGui::GetStyle();
		ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(u8"清除").x + sty.FramePadding.x * 2.0f +
		                          sty.ItemSpacing.x));
		if (ImGui::InputText("##wh_sessid", &state_.sessid,
		                     ImGuiInputTextFlags_Password)) {
			stateDirty_ = true;
			secretDirty_ = true;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(state_.sessid.empty());
		if (ImGui::Button(u8"清除##wh_sessid_clear")) clearSessid();
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(u8"立刻從記憶體與設定檔清除 session id\n"
			                  u8"（帳號名稱、勾選的分頁與快照歷史保留）");
		Hint(u8"瀏覽器登入官網後 Cookie 內的 POESESSID");

		if (ImGui::Checkbox(u8"記住（以 Windows 使用者加密存於本機）",
		                    &state_.rememberSessid)) {
			// Turning it off must not wait for the next settings change: save now,
			// which overwrites the blob on disk with an empty field.
			stateDirty_ = true;
			secretDirty_ = true;
			persistIfDirty();
		}
		Hint(state_.rememberSessid
		         ? u8"session id 以 DPAPI 加密存在 warehouse_ui.json，換使用者或換電腦都解不開"
		         : u8"不記住：session id 只留在記憶體，關掉程式就沒了，下次要重貼");

		ImGui::BeginDisabled(busy() || state_.sessid.empty() || state_.accountName.empty());
		if (ImGui::Button(u8"驗證 session")) {
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
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (st_.authOk) ImGui::TextColored(kGood, u8"有效");
		else if (st_.authFailed) ImGui::TextColored(kBad, u8"無效");
		else ImGui::TextColored(kDim, u8"未驗證");

		ImGui::Separator();
		drawLeaguePicker();
		ImGui::Separator();
		drawAutoPicker();
		if (embed_.settingsBottom) {
			ImGui::Separator();
			embed_.settingsBottom();
		}
		ImGui::EndChild(); // ##wh_settings

		ImGui::SameLine();
		const float rightW = (std::min)(ImGui::GetContentRegionAvail().x, 480.0f * scale);
		ImGui::BeginChild("##wh_settabs", ImVec2(rightW, 0), false);
		ImGui::Text(u8"倉庫分頁");
		// Tab ids are per league, so this is always the league picked on the
		// left. The fetch checks the session itself -- no separate verify first.
		ImGui::BeginDisabled(busy() || state_.sessid.empty() || state_.accountName.empty() ||
		                     state_.sel().league.empty());
		if (ImGui::Button(u8"取得倉庫分頁清單")) {
			StashAuth a;
			a.accountName = state_.accountName;
			a.secret = state_.sessid;
			svc_.SetAuth(a);
			svc_.AckDone();
			svc_.RequestTabList(state_.sel().league);
		}
		ImGui::EndDisabled();

		if (!tabs_.empty()) {
			ImGui::TextColored(kDim, u8"勾選要統計的分頁（%d 個已選）",
			                   (int)state_.sel().tabIds.size());
			// The rest of the page: the list is the page's main content now.
			ImGui::BeginChild("##wh_tabs",
			                  ImVec2(0, (std::max)(ImGui::GetContentRegionAvail().y,
			                                       160.0f * host_->scale)),
			                  true);
			for (const StashTabInfo& t : tabs_) {
				bool sel = std::find(state_.sel().tabIds.begin(),
				                     state_.sel().tabIds.end(),
				                     t.id) != state_.sel().tabIds.end();
				std::string label = t.name + "##" + t.id;
				if (ImGui::Checkbox(label.c_str(), &sel)) {
					if (sel) {
						state_.sel().tabIds.push_back(t.id);
					} else {
						state_.sel().tabIds.erase(
						    std::remove(state_.sel().tabIds.begin(),
						                state_.sel().tabIds.end(), t.id),
						    state_.sel().tabIds.end());
					}
					stateDirty_ = true;
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("#%d %s", t.index, t.type.c_str());
			}
			ImGui::EndChild();
		} else if (!state_.sel().tabIds.empty()) {
			// Picked in an earlier run; the list itself is fetched on demand.
			ImGui::TextColored(kDim, u8"已選 %d 個分頁（取得清單後可修改）",
			                   (int)state_.sel().tabIds.size());
		}
		ImGui::EndChild(); // ##wh_settabs
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
		// The shared guard's word, as of its last 2-second refresh (a click
		// re-reads it under the lock anyway).
		const long long now = NowUtc();
		const long long nextOk = WarehouseNextSnapshotUtc(guard_);
		const bool cooling = now < nextOk;
		// No verify-first: the snapshot job checks the session itself and says
		// so when it is dead (the verify button lives on the 設定 page now).
		const bool canSnap = !busy() && !state_.sel().league.empty() &&
		                     !state_.sel().tabIds.empty() && !state_.sessid.empty() &&
		                     !state_.accountName.empty() && !cooling;
		ImGui::BeginDisabled(!canSnap);
		if (ImGui::Button(u8"立即快照", ImVec2(120.0f * host_->scale, 0))) {
			svc_.AckDone();
			requestSnapshot();
		}
		ImGui::EndDisabled();
		if (cooling) {
			const long long left = nextOk - now;
			const bool paused = guard_.blockedUntilUtc >= nextOk;
			ImGui::SameLine();
			ImGui::TextColored(kWarn, "%s %lld:%02lld", paused ? u8"限流" : u8"冷卻", left / 60,
			                   left % 60);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", paused ? u8"GGG 要求暫停請求（HTTP 429 或額度用盡），"
				                                 u8"期間不送出任何倉庫請求"
				                               : u8"帳號請求額度整個帳號共用：兩次快照至少間隔 "
				                                 u8"10 分鐘，所有視窗共用這個限制");
		}
		ImGui::SameLine();
		drawStatusLine();

		if (history_.snaps.empty()) {
			ImGui::Spacing();
			// The host's card (the per-map cost) needs no snapshot: keep it.
			if (embed_.topCard) {
				const ImGuiStyle& sty = ImGui::GetStyle();
				drawHostCard(threeCardWidth(),
				             (embed_.topCardHeight ? embed_.topCardHeight() : 0.0f) +
				                 sty.WindowPadding.y * 2.0f);
			}
			ImGui::TextColored(kDim, u8"尚無快照。在「設定」填好帳號並勾選分頁後按「立即快照」。");
			return;
		}

		drawTopCards();
		drawCurve();

		const Snapshot* sel = selectedSnap();
		const Snapshot* prev = snapBefore(sel);

		// Wealthy-Exile split: snapshot timeline on the left, the selected
		// snapshot's content on the right.
		ImGui::BeginChild("##wh_snapside", ImVec2(260.0f * host_->scale, 0), true);
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
			// What is missing mostly lives on the 設定 page now: say where.
			if (state_.accountName.empty() || state_.sessid.empty())
				ImGui::TextColored(kWarn, u8"請到「設定」填寫帳號名稱與 POESESSID");
			else if (state_.sel().league.empty())
				ImGui::TextColored(kWarn, u8"請到「設定」選擇聯盟");
			else if (state_.sel().tabIds.empty())
				ImGui::TextColored(kWarn, u8"請到「設定」勾選要統計的倉庫分頁");
			else
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
				ImGui::TextColored(kWarn, u8"請到「設定」重新輸入 POESESSID");
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

	// One key/value pair in a column of the snapshot card: label left, value
	// right-aligned to the column's edge (the Wealthy-Exile card). Placed in
	// SCREEN coordinates on purpose: these rows live inside BeginGroup, and
	// SameLine(x) there adds the group's own offset a second time -- which
	// pushed the right column's values clean out of the card.
	// Returns whether the mouse is over the row, for an explaining tooltip.
	bool cardKv(const char* label, const std::string& value, float colW, const ImVec4* col)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::TextColored(kDim, "%s", label);
		ImGui::SetCursorScreenPos(ImVec2(p.x + colW - ImGui::CalcTextSize(value.c_str()).x, p.y));
		if (col) ImGui::TextColored(*col, "%s", value.c_str());
		else ImGui::TextUnformatted(value.c_str());
		return ImGui::IsWindowHovered() &&
		       ImGui::IsMouseHoveringRect(p, ImVec2(p.x + colW, p.y + ImGui::GetTextLineHeight()));
	}

	// The thin rule above a card column's total row.
	void cardRule(float colW)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float y = p.y + 2.0f * host_->scale;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y), ImVec2(p.x + colW, y),
		                                    ImGui::GetColorU32(ImGuiCol_Separator));
		ImGui::Dummy(ImVec2(colW, 5.0f * host_->scale));
	}

	std::string signedValue(double chaos, double rate)
	{
		return (chaos >= 0 ? "+" : "") + FormatValue(chaos, state_.showDivine, rate);
	}

	// Width of the cost and snapshot cards in the three-card row. The breakdown
	// is a short list, so it takes less (user request, 2026-09-12): about a
	// quarter, never under 260 unless a third is narrower still.
	float threeCardWidth() const
	{
		const float usable = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f;
		const float breakW =
		    (std::max)(usable * 0.24f, (std::min)(usable / 3.0f, 260.0f * host_->scale));
		return (usable - breakW) * 0.5f;
	}

	void drawHostCard(float w, float h)
	{
		ImGui::BeginChild("##wh_card_host", ImVec2(w, h), true);
		embed_.topCard();
		ImGui::EndChild();
	}

	// The Wealthy-Exile top strip: a session summary card and a breakdown card
	// (after the host's card, when it has one).
	void drawTopCards()
	{
		const Snapshot& latest = history_.snaps.back();
		const Snapshot* start = history_.FindByUtc(history_.sessionStartUtc);
		if (!start) start = &history_.snaps.front();
		const double rate = latest.divineRate;
		const SnapshotDiff d = DiffSnapshots(*start, latest);

		int shotsIn = 0; // snapshots inside the interval, both ends included
		for (const Snapshot& s : history_.snaps)
			if (s.utc >= start->utc && s.utc <= latest.utc) shotsIn++;

		// Tall enough for both cards, from the real line height so a font or
		// scale change cannot clip a row: the snapshot card is a title plus four
		// key/value rows and a rule; the breakdown card a title, four icon rows
		// and the unpriced note.
		const ImGuiStyle& sty = ImGui::GetStyle();
		const float lh = ImGui::GetTextLineHeightWithSpacing();
		const float iconRow = (std::max)(lh, 20.0f * host_->scale + sty.ItemSpacing.y);
		const float snapNeed = lh * 6.0f + 8.0f * host_->scale;
		const float breakNeed = lh * 2.0f + iconRow * 4.0f + lh;
		// A host card (the atlas planner's per-map cost) leads a row of three,
		// and the row takes the tallest content (user layout, 2026-09-12).
		const bool hostCard = (bool)embed_.topCard;
		const float hostNeed = hostCard && embed_.topCardHeight ? embed_.topCardHeight() : 0.0f;
		const float cardH = (std::max)((std::max)(snapNeed, breakNeed), hostNeed) +
		                    sty.WindowPadding.y * 2.0f;
		const float cardW = hostCard ? threeCardWidth()
		                             : (ImGui::GetContentRegionAvail().x - 8.0f * host_->scale) * 0.5f;
		if (hostCard) {
			drawHostCard(cardW, cardH);
			ImGui::SameLine();
		}

		// Wealthy-Exile layout: the rate top-right, times on the left, money on
		// the right, a rule over each column's total.
		ImGui::BeginChild("##wh_card_snap", ImVec2(cardW, cardH), true);
		ImGui::TextUnformatted(u8"快照區間");
		{
			// Top right: what farming earned per hour -- count changes only. The
			// market moving stock already held is money, but not the maps' doing.
			const char* lbl = u8"刷圖收益";
			const std::string val =
			    d.summaryOnly ? std::string("--/hr") : signedValue(d.farmPerHour, rate) + "/hr";
			const float w = ImGui::CalcTextSize(lbl).x + sty.ItemSpacing.x +
			                ImGui::CalcTextSize(val.c_str()).x;
			ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
			ImGui::TextColored(kDim, "%s", lbl);
			ImGui::SameLine();
			ImGui::TextColored(d.farmPerHour >= 0 ? kGood : kBad, "%s", val.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"（收益 − 支出）÷ 經過時間，只算數量變化；市價波動不算在內");
		}
		ImGui::Spacing();
		const float gap = 28.0f * host_->scale;
		const float colW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
		char dur[32];
		{
			const int mins = (int)((latest.utc - start->utc) / 60);
			snprintf(dur, sizeof(dur), "%dh %02dm", mins / 60, mins % 60);
		}
		ImGui::BeginGroup();
		cardKv(u8"起點", FormatUtcWeek(start->utc), colW, nullptr);
		cardKv(u8"最新", FormatUtcWeek(latest.utc), colW, nullptr);
		cardKv(u8"快照", std::to_string(shotsIn) + u8" 份", colW, nullptr);
		cardRule(colW);
		cardKv(u8"經過", dur, colW, nullptr);
		ImGui::EndGroup();
		ImGui::SameLine(0, gap);
		// Money split by cause: 收益 / 支出 are count changes (farmed, spent),
		// 市價波動 the market re-pricing stock held throughout. Before the split,
		// a divine-price dip on a held stack read as spending.
		ImGui::BeginGroup();
		if (d.summaryOnly) {
			cardKv(u8"收益", "-", colW, nullptr);
			cardKv(u8"支出", "-", colW, nullptr);
			cardKv(u8"市價波動", "-", colW, nullptr);
		} else {
			if (cardKv(u8"收益", WhFmt::FormatDivChaos(d.qtyGain, rate, false), colW, nullptr))
				ImGui::SetTooltip(u8"起點之後多出來的物品：數量增加 × 目前單價");
			if (cardKv(u8"支出", WhFmt::FormatDivChaos(-d.qtyLoss, rate, false), colW, nullptr))
				ImGui::SetTooltip(u8"起點之後用掉或賣掉的物品：數量減少 × 單價");
			if (cardKv(u8"市價波動", WhFmt::FormatDivChaos(d.priceMove, rate, true), colW, nullptr))
				ImGui::SetTooltip(u8"起點時就持有的數量 × 單價漲跌；\n"
				                  u8"報價消失（變成未估價）的物品也算在這裡");
		}
		cardRule(colW);
		{
			const ImVec4& col = d.dTotalChaos >= 0 ? kGood : kBad;
			if (cardKv(u8"淨值", WhFmt::FormatDivChaos(d.dTotalChaos, rate, true), colW, &col))
				ImGui::SetTooltip(u8"倉庫總值的變化 = 收益 − 支出 ± 市價波動");
		}
		ImGui::EndGroup();
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::BeginChild("##wh_card_break", ImVec2(0, cardH), true);
		ImGui::TextUnformatted(u8"明細（自起點的主要增減）");
		{
			const std::string totalTxt =
			    std::string(u8"總值 ") + FormatValue(latest.totalChaos, state_.showDivine, rate);
			ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(totalTxt.c_str()).x);
			ImGui::TextColored(kDim, "%s", totalTxt.c_str());
		}
		ImGui::Separator();
		// The biggest count changes by value -- what was farmed and what was
		// spent. Market moves on held stock are the 變化 table's 市價波動 column.
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
		for (size_t mi = 0; mi < shownN; mi++) {
			const SnapshotDiffLine* pick = movers[mi];
			drawIconCell(pick->icon);
			ImGui::SameLine();
			const std::string val = signedValue(pick->dQtyChaos, rate);
			const float valW = ImGui::CalcTextSize(val.c_str()).x;
			// A long name is cut with an ellipsis before the value instead of
			// running underneath it; the full name is in the tooltip.
			const std::string zh = i18n_.DisplayName(pick->dispEn);
			const float nameMax =
			    ImGui::GetContentRegionMax().x - valW - 12.0f * host_->scale - ImGui::GetCursorPosX();
			const std::string nameShown = Ellipsize(zh, nameMax);
			ImGui::TextUnformatted(nameShown.c_str());
			if (nameShown != zh && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", zh.c_str());
			ImGui::SameLine(ImGui::GetContentRegionMax().x - valW);
			ImGui::TextColored(pick->dQtyChaos >= 0 ? kGood : kBad, "%s", val.c_str());
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
		const float realLo = lo, realHi = hi; // before the flat-line guard below
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

		// Scale labels in divine, like every other value on the card (user
		// request), at today's rate.
		{
			const double rateNow = history_.snaps.back().divineRate;
			const ImU32 labelCol = ImGui::GetColorU32(kDim);
			const float lx = p0.x + 6.0f * host_->scale;
			dl->AddText(ImVec2(lx, p0.y + 2.0f * host_->scale), labelCol,
			            FormatValue(realHi, true, rateNow).c_str());
			if (realHi > realLo)
				dl->AddText(ImVec2(lx, p0.y + hgt - ImGui::GetTextLineHeight() - 2.0f * host_->scale),
				            labelCol, FormatValue(realLo, true, rateNow).c_str());
		}

		if (ImGui::IsItemHovered()) {
			float fx = (ImGui::GetIO().MousePos.x - p0.x) / (w > 1.0f ? w : 1.0f);
			int idx = (int)(fx * (pts - 1) + 0.5f);
			idx = idx < 0 ? 0 : idx >= pts ? pts - 1 : idx;
			// A dot marks the hovered sample so the tooltip has an anchor.
			dl->AddCircleFilled(ptAt(idx), 3.5f, lineCol);
			// In divine at that snapshot's own rate.
			ImGui::SetTooltip("%s\n%s", FormatUtcLocal(curveUtc_[idx]).c_str(),
			                  FormatValue(curvePts_[idx], true, curveRate_[idx]).c_str());
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

	// Fixed table columns sized from what they hold rather than a constant: at
	// a larger font (with the scrollbar taking its share) the copy buttons were
	// squeezed half out of view.
	float fitCol(const char* header, const char* sample) const
	{
		// The header also carries the sort arrow.
		const float h = ImGui::CalcTextSize(header).x + ImGui::GetFontSize();
		const float s = ImGui::CalcTextSize(sample).x;
		return (h > s ? h : s) + ImGui::GetStyle().FramePadding.x * 2.0f;
	}
	float copyColW() const
	{
		const ImGuiStyle& sty = ImGui::GetStyle();
		return ImGui::CalcTextSize(u8"中文").x + ImGui::CalcTextSize(u8"英文").x +
		       sty.FramePadding.x * 4.0f + sty.ItemSpacing.x + 4.0f;
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
		if (latest->summary) {
			char msg[160];
			snprintf(msg, sizeof(msg),
			         u8"此快照已精簡為摘要，只保留總值（原有 %d 種物品）。"
			         u8"最近 %d 份與起點快照保留完整明細。",
			         latest->lineCount, WarehouseHistory::kFullKept);
			Hint(msg);
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
			                        fitCol(u8"數量", "+999999"));
			ImGui::TableSetupColumn(u8"單價",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"單價", "~9999.9 c"));
			ImGui::TableSetupColumn(u8"小計",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_DefaultSort |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"小計", "~9999.9 d"));
			ImGui::TableSetupColumn(u8"複製",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_NoSort,
			                        copyColW());
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
		if (d.summaryOnly) {
			Hint(sel->summary ? u8"此快照已精簡為摘要，只保留總值，無法逐項比較。"
			                  : u8"上一份快照已精簡為摘要，只能比較總值。");
			return;
		}
		ImGui::SameLine();
		ImGui::TextColored(kDim, u8"（增減 %s、市價 %s）",
		                   signedValue(d.qtyGain + d.qtyLoss, rate).c_str(),
		                   signedValue(d.priceMove, rate).c_str());

		std::vector<const SnapshotDiffLine*> rows;
		for (const SnapshotDiffLine& r : d.gained) rows.push_back(&r);
		for (const SnapshotDiffLine& r : d.lost) rows.push_back(&r);

		if (ImGui::BeginTable("##wh_changes", 7,
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
			                        fitCol(u8"數量", "+999999"));
			ImGui::TableSetupColumn(u8"單價",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"單價", "~9999.9 c"));
			ImGui::TableSetupColumn(u8"增減價值",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_DefaultSort |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"增減價值", "+9999.9 d"));
			ImGui::TableSetupColumn(u8"市價波動",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_PreferSortDescending,
			                        fitCol(u8"市價波動", "+9999.9 d"));
			ImGui::TableSetupColumn(u8"複製",
			                        ImGuiTableColumnFlags_WidthFixed |
			                            ImGuiTableColumnFlags_NoSort,
			                        copyColW());
			// TableHeadersRow spelled out, for a tooltip on the two money columns.
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			for (int c = 0; c < 7; c++) {
				if (!ImGui::TableSetColumnIndex(c)) continue;
				ImGui::PushID(c);
				ImGui::TableHeader(ImGui::TableGetColumnName(c));
				ImGui::PopID();
				if (c == 4 && ImGui::IsItemHovered())
					ImGui::SetTooltip(u8"數量變化 × 單價：刷到或用掉的價值");
				else if (c == 5 && ImGui::IsItemHovered())
					ImGui::SetTooltip(u8"原本就持有的數量 × 單價漲跌；報價消失的物品也算在這裡");
			}

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
						                 case 5:
							                 cmp = a->dPriceChaos < b->dPriceChaos ? -1
							                       : a->dPriceChaos > b->dPriceChaos ? 1 : 0;
							                 break;
						                 default:
							                 cmp = a->dQtyChaos < b->dQtyChaos ? -1
							                       : a->dQtyChaos > b->dQtyChaos ? 1 : 0;
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
				// Half a cent and under is float noise from the split, not money.
				auto moneyCell = [&](double v) {
					if (v > 0.005 || v < -0.005)
						ImGui::TextColored(v >= 0 ? kGood : kBad, "%s", signedValue(v, rate).c_str());
					else
						ImGui::TextColored(kDim, "-");
				};
				ImGui::TableNextColumn();
				moneyCell(r->dQtyChaos);
				ImGui::TableNextColumn();
				moneyCell(r->dPriceChaos);
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
			const std::string date = FormatUtcLocal(s.utc);
			// The change, uniformly in divine (user request): raw chaos numbers at
			// this width were unreadable (+214262). Chaos only when no rate is known.
			std::string txt = "--";
			if (prev) {
				if (s.divineRate > 0) {
					const double dv = delta / s.divineRate;
					const double mag = dv < 0 ? -dv : dv;
					char buf[32];
					snprintf(buf, sizeof(buf),
					         mag >= 100 ? "%+.0f d" : mag >= 1 ? "%+.1f d" : "%+.2f d", dv);
					txt = buf;
				} else {
					txt = (delta >= 0 ? "+" : "") + FormatChaos(delta) + " c";
				}
			}
			const ImVec4& deltaCol = !prev ? kDim : delta >= 0 ? kGood : kBad;
			const bool isSel = sel && sel->utc == s.utc;
			const float x0 = ImGui::GetCursorPosX();
			// A summary (old snapshot thinned to its totals) is drawn dim.
			if (s.summary) ImGui::PushStyleColor(ImGuiCol_Text, kDim);
			if (ImGui::Selectable((date + "##snap").c_str(), isSel, 0,
			                      ImVec2(0, ImGui::GetTextLineHeightWithSpacing())))
				selSnapUtc_ = s.utc == history_.snaps.back().utc ? 0 : s.utc;
			if (s.summary) ImGui::PopStyleColor();
			if (s.summary && ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"摘要快照：只保留總值（原有 %d 種物品）", s.lineCount);
			if (ImGui::BeginPopupContextItem("##snap_ctx")) {
				if (s.utc == history_.sessionStartUtc)
					ImGui::TextDisabled(u8"目前的起點");
				else if (ImGui::MenuItem(u8"設為起點", nullptr, false, !s.summary))
					setStartRequest_ = s.utc; // applied after the frame: s points into history_
				if (s.summary) ImGui::TextDisabled(u8"摘要快照沒有逐項明細，不能當起點");
				ImGui::EndPopup();
			}
			// Date on the left with the session-start mark right after it, the
			// change right-aligned: both measured, so they cannot run into each
			// other at any font size.
			if (s.utc == history_.sessionStartUtc) {
				ImGui::SameLine(x0 + ImGui::CalcTextSize(date.c_str()).x + 6.0f * host_->scale);
				ImGui::TextColored(kWarn, u8"起");
			}
			ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(txt.c_str()).x);
			ImGui::TextColored(deltaCol, "%s", txt.c_str());
			ImGui::PopID();
		}
	}

	// The three pages, whether our own tab bar or a host's picks them. The host
	// speaks in ints (WarehouseEmbed::page) so warehouse_tool.h need not know
	// this type.
	enum class Page { Revenue, Help, Settings };

	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir_;
	WarehouseEmbed embed_;   // what an embedding host adds; empty when standalone
	bool firstFrame_ = true; // our own tab bar picks its opening page once

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
	FilterI18n i18n_;
	IconManager icons_;

	std::vector<StashTabInfo> tabs_;
	std::vector<std::string> leagues_;

	std::vector<float> curvePts_;
	std::vector<long long> curveUtc_;
	std::vector<double> curveRate_; // each sample's own divine rate
	std::vector<int> sortedLines_;
	long long linesForUtc_ = 0;
	long long selSnapUtc_ = 0; // sidebar selection; 0 = follow the newest
	int catFilter_ = 0;
	long long setStartRequest_ = 0; // "設為起點" picked this frame; applied after drawing
	double lastStatCheck_ = -10.0;  // ImGui time of the last history-file stat
	FILETIME histWrite_{};          // history file's last-write time as last seen
	std::string search_;
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
	desc.defW = 1100;
	desc.defH = 760;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
