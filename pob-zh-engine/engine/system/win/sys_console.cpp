// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: System Console
// Platform: Windows
//

#include "sys_local.h"

// =============
// Configuration
// =============

#define SCON_WIDTH 550
#define SCON_HEIGHT 500

#define SCON_STYLE WS_VISIBLE | WS_OVERLAPPED | WS_BORDER | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN

// ======================
// sys_IConsole Interface
// ======================

class sys_console_c: public sys_IConsole, public conPrintHook_c, public thread_c {
public:
	// Interface
	void	SetVisible(bool show);
	bool	IsVisible();
	void	SetForeground();
	void	SetTitle(const char* title);

	// Encapsulated
	sys_console_c(sys_IMain* sysHnd);
	~sys_console_c();

	sys_main_c* sys = nullptr;

	bool	shown = false;			// Is currently shown?
	HWND	hwMain = nullptr;		// Main window
	HWND	hwOut = nullptr;		// Output
	HFONT	font = nullptr;			// Font handle
	HBRUSH	bBackground = nullptr;	// Background brush

	static LRESULT __stdcall WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	volatile bool doRun;
	volatile bool isRunning;

	void	RunMessages(HWND hwnd = nullptr);
	void	ThreadProc();

	void	Print(std::u32string_view text);
	void	CopyToClipboard();

	void	ConPrintHook(const char* text);
	void	ConPrintClear();
};

sys_IConsole* sys_IConsole::GetHandle(sys_IMain* sysHnd)
{
	return new sys_console_c(sysHnd);
}

void sys_IConsole::FreeHandle(sys_IConsole* hnd)
{
	delete (sys_console_c*)hnd;
}

sys_console_c::sys_console_c(sys_IMain* sysHnd)
	: conPrintHook_c(sysHnd->con), sys((sys_main_c*)sysHnd), thread_c(sysHnd)
{
	isRunning = false;
	doRun = true;

	ThreadStart(true);
	while ( !isRunning );
}

void sys_console_c::RunMessages(HWND hwnd)
{
	// Flush message queue
	MSG msg;
	while (PeekMessage(&msg, hwnd, 0, 0, PM_NOREMOVE)) {
		if (GetMessage(&msg, hwnd, 0, 0) == 0) {
			sys->Exit();
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

void sys_console_c::ThreadProc()
{
	// Get info of the monitor containing the mouse cursor
	POINT curPos;
	GetCursorPos(&curPos);
	HMONITOR hMon = MonitorFromPoint(curPos, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO monInfo;
	monInfo.cbSize = sizeof(monInfo);
	GetMonitorInfo(hMon, &monInfo);

	// Find the window position and size
	RECT wrec;
	wrec.left = (monInfo.rcMonitor.right + monInfo.rcMonitor.left - SCON_WIDTH) / 2;
	wrec.top = (monInfo.rcMonitor.bottom + monInfo.rcMonitor.top - SCON_HEIGHT) / 2;
	wrec.right = wrec.left + SCON_WIDTH;
	wrec.bottom = wrec.top + SCON_HEIGHT;
	AdjustWindowRect(&wrec, SCON_STYLE, FALSE);

	// Register the class
	WNDCLASS conClass;
	memset(&conClass, 0, sizeof(WNDCLASS));
	conClass.hInstance			= sys->hinst;
	conClass.lpszClassName		= CFG_SCON_TITLE " Class";
	conClass.lpfnWndProc		= WndProc;
	conClass.hbrBackground		= CreateSolidBrush(CFG_SCON_WINBG);
	conClass.hCursor			= LoadCursor(NULL, IDC_ARROW);
	conClass.hIcon				= sys->icon;
	if (RegisterClass(&conClass) == 0) exit(0);

	// Create the system console window
	hwMain = CreateWindowExW(
		0, CFG_SCON_TITLE " Class", CFG_SCON_TITLE, SCON_STYLE, 
		wrec.left, wrec.top, wrec.right - wrec.left, wrec.bottom - wrec.top,
		NULL, NULL, sys->hinst, NULL
	);
	SetWindowLongPtr(hwMain, GWLP_USERDATA, (LONG_PTR)this);

	// Populate window
	hwOut = CreateWindowExW(
		WS_EX_CLIENTEDGE, L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
		10, 10, SCON_WIDTH - 20, SCON_HEIGHT - 20, 
		hwMain, NULL, sys->hinst, NULL
	);

	// Create the output window font
	font = CreateFont(
		12, 0, 0, 0, 
		FW_LIGHT, FALSE, FALSE, FALSE, 
		DEFAULT_CHARSET, 
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
		FIXED_PITCH|FF_MODERN, L"Lucida Console"
	);
	SetWindowFont(hwOut, font, FALSE);
	Edit_LimitText(hwOut, 0xFFFF);

	// Create the output window background brush
	bBackground = CreateSolidBrush(CFG_SCON_TEXTBG);
	InvalidateRect(hwOut, nullptr, TRUE);

	// Flush any messages created
	RunMessages();

	shown = true;

	InstallPrintHook();
	
	isRunning = true;
	while (doRun) {
		RunMessages(hwMain);
		sys->Sleep(1);
	}

	RemovePrintHook();

	// Delete font and brush
	DeleteObject(bBackground);
	DeleteObject(font);

	// Destroy window
	DestroyWindow(hwMain);
	UnregisterClass(CFG_SCON_TITLE " Class", sys->hinst);

	isRunning = false;

	// Flush windowless messages (Like WM_QUIT)
	RunMessages();
}

sys_console_c::~sys_console_c()
{
	doRun = false;
	while (isRunning);
}

// ========================
// Console Window Procedure
// ========================

LRESULT __stdcall sys_console_c::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	sys_console_c* conWin = (sys_console_c*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

	switch (msg) {
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORSTATIC:
		// Set text color of output window
		SetTextColor((HDC)wParam, CFG_SCON_TEXTCOL);
		SetBkColor((HDC)wParam, CFG_SCON_TEXTBG);
		return (LRESULT)conWin->bBackground;

	case WM_KEYUP:
		if (wParam != VK_ESCAPE) {
			break;
		}
	case WM_CLOSE:
		// Quit
		conWin->doRun = false;
		PostQuitMessage(0);
		return FALSE;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ==============
// System Console
// ==============

void sys_console_c::SetVisible(bool show)
{
	if (show) {
		if ( !shown ) {
			shown = true;

			// Show the window
			ShowWindow(hwMain, SW_SHOW);
			SetForegroundWindow(hwMain);

			// Select all text and replace with full text
			Edit_SetText(hwOut, L"");
			char* buffer = sys->con->BuildBuffer();
			std::u32string u32_text = IndexUTF8ToUTF32(buffer).text;

			Print(u32_text);
			delete buffer;
	
			RunMessages(hwMain);
		}
	} else {
		shown = false;
	
		// Hide the window
		ShowWindow(hwMain, SW_HIDE);
	}
}

void sys_console_c::SetForeground()
{
	if (shown) {
		SetForegroundWindow(hwMain);
	}
}

bool sys_console_c::IsVisible()
{
	return IsWindowVisible(hwMain);
}

void sys_console_c::SetTitle(const char* title)
{
	WCHAR* text = WidenUTF8String(title);
	SetWindowText(hwMain, (text && *text)? text : CFG_SCON_TITLE);
}

void sys_console_c::Print(std::u32string_view string_view_text)
{
	if ( !shown ) {
		return;
	}

	int escLen;
	const char32_t* text = string_view_text.data();

	// Find the required buffer length
	int len = 0;
	for (int b = 0; text[b]; b++) {
		if (text[b] == U'\n') {
			// Newline takes 2 characters
			len+= 2;
		} else if (escLen = IsColorEscape(&text[b])) {
			// Skip colour escapes
			b+= escLen - 1;
		} else {
			if (text[b] > UINT16_MAX) {
				// Higher codepoints will be separated into surrogate pairs
				len += 2;
			}
			else {
				len++;
			}
		}
	}

	// Parse into the buffer
	char16_t* winText = new char16_t[len + 1];
	char16_t* p = winText;
	for (int b = 0; text[b]; b++) {
		if (text[b] == L'\n') {
			// Append newline
			*(p++) = L'\r';
			*(p++) = L'\n';
		} else if (escLen = IsColorEscape(&text[b])) {
			// Skip colour escapes
			b+= escLen - 1;
		} else {
			// Add character
			if (text[b] > UINT16_MAX) { // Outside the BMP
				char16_t high_surrogate = ((text[b] - 0x10000) / 0x400) + 0xD800;
				char16_t low_surrogate = ((text[b] - 0x10000) % 0x400) + 0xDC00;
				*(p++) = high_surrogate;
				*(p++) = low_surrogate;
			}
			else {
				*(p++) = (char16_t)text[b];
			}
			
		}
	}
	winText[len] = 0;

	// Append to the output
	Edit_SetSel(hwOut, Edit_GetTextLength(hwOut), -1);
	Edit_ReplaceSel(hwOut, winText);
	Edit_Scroll(hwOut, 0xFFFF, 0);
	RunMessages(hwMain);
	delete[] winText;
}

void sys_console_c::CopyToClipboard()
{
	int len = GetWindowTextLength(hwOut);
	if (len) {
		HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
		if ( !hg ) return;
		WCHAR* cp = (WCHAR*)GlobalLock(hg);
		GetWindowTextW(hwOut, cp, len + 1);
		GlobalUnlock(hg);
		OpenClipboard(hwMain);
		EmptyClipboard();
		SetClipboardData(CF_TEXT, hg);
		CloseClipboard();
	}
}

void sys_console_c::ConPrintHook(const char* text)
{
	Print(IndexUTF8ToUTF32(text).text);
}

void sys_console_c::ConPrintClear()
{
	Edit_SetText(hwOut, L"");
}
