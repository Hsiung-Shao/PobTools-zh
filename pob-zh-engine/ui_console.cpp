// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// Module: UI Console
//

#include "ui_local.h"

#include <fmt/core.h>
#include <string_view>

// =============
// Configuration
// =============

#define UI_CONMOVETIME 100.0f	// Movement time

// =====================
// ui_IConsole Interface
// =====================

class ui_console_c: public ui_IConsole, public conInputHandler_c {
public:
	// Interface
	void	Toggle();
	void	Hide();
	bool	KeyEvent(int key, int type);
	void	Render();

	// Encapsulated
	ui_console_c(ui_main_c* ui);

	ui_main_c* ui = nullptr;
	sys_IMain* sys = nullptr;
	r_IRenderer* renderer = nullptr;

	conVar_c* con_fontSize = nullptr;

	enum {	
		MODE_UP,		// Up (hidden)
		MODE_MV_DOWN,	// Moving down
		MODE_DOWN,		// Down (shown)
		MODE_MV_UP		// Moving up
	} mode;
	int		moveStart = 0;

	char	input[1024] = {};
	int		caret = 0;
	void	SetConInput(char* newInput, int newCaret);
};

ui_IConsole* ui_IConsole::GetHandle(ui_main_c* ui)
{
	return new ui_console_c(ui);
}

void ui_IConsole::FreeHandle(ui_IConsole* hnd)
{
	delete (ui_console_c*)hnd;
}

ui_console_c::ui_console_c(ui_main_c* ui)
	: conInputHandler_c(ui->sys->con), ui(ui), sys(ui->sys), renderer(ui->renderer)
{
	con_fontSize = sys->con->Cvar_Add("con_fontSize", CV_ARCHIVE|CV_CLAMP, "14", 12, 32);

	mode = MODE_UP;
	RefreshConInput();
}

// ===============
// Console Handler
// ===============

void ui_console_c::Toggle()
{
	switch (mode) {
	case MODE_UP:
		// Start moving down
		mode = MODE_MV_DOWN;
		moveStart = sys->GetTime();
		break;
	case MODE_DOWN:
		// Start moving up and reset buffer scroll position
		mode = MODE_MV_UP;
		moveStart = sys->GetTime();
		sys->con->Scroll(CBSC_BOTTOM);
		break;
	default:
		break;
	}
}

void ui_console_c::Hide()
{
	mode = MODE_UP;
}

bool ui_console_c::KeyEvent(int key, int type)
{
	if (type == KE_KEYDOWN && (key == '`' || key == KEY_F1) && sys->IsKeyDown(KEY_CTRL)) {
		Toggle();
		return true;
	}

	if (mode == MODE_MV_DOWN || mode == MODE_DOWN) {
		ConInputKeyEvent(key, type);
		return true;
	}
	return false;
}

void ui_console_c::Render()
{
	float uiw = (float)renderer->VirtualScreenWidth();
	float uih = (float)renderer->VirtualScreenHeight();

	// Find y position

	float y = (float)floor(uih/2.0f);
	int movetime = sys->GetTime() - moveStart;

	switch (mode) {
	case MODE_UP:
		// Don't render when not visible at all
		return;
	case MODE_MV_DOWN:
		if (movetime >= UI_CONMOVETIME) {
			// Reached bottom
			mode = MODE_DOWN;
		} else {
			// Still moving
			y*= movetime / UI_CONMOVETIME;
		}
		break;
	case MODE_MV_UP:
		if (movetime >= UI_CONMOVETIME) {
			// Reached top
			mode = MODE_UP;
			ClearConInput();
			return;
		} else {
			// Still moving
			y*= 1 - movetime / UI_CONMOVETIME;
		}
		break;
	default:
		break;
	}

	int fontSize = con_fontSize->intVal;
	float backHeight = y + fontSize;

	// Draw the background of the console
	renderer->DrawColor(colorBlack);
	renderer->DrawImage(NULL, { 0, 0 }, { uiw, backHeight });
	renderer->DrawColor(0xFF007700);
	renderer->DrawImage(NULL, { 0, backHeight - fontSize / 2.0f }, { uiw, fontSize / 2.0f });

	float basey = y - (float)fontSize;
	float liney;

	// Draw info strings
	liney = basey;
	renderer->DrawString(0, liney, F_RIGHT, fontSize, colorGreen, F_FIXED, CFG_VERSION);
	liney-= fontSize;
	int memTotal = lua_gc(ui->L, LUA_GCCOUNT, 0);
	for (dword i = 0; i < ui->subScriptSize; i++) {
		if (ui->subScriptList[i]) {
			memTotal+= ui->subScriptList[i]->GetScriptMemory();
		}
	}
	renderer->DrawStringFormat(0, liney, F_RIGHT, fontSize, colorWhite, F_FIXED, "%dkB used by Lua", memTotal);
#ifdef _MEMTRAK_H 
	liney-= fontSize;
	renderer->DrawStringFormat(0, liney, F_RIGHT, fontSize, colorWhite, F_FIXED, "%dkB tracking overhead", _memTrak_getOverhead()>>10);
	liney-= fontSize;
	renderer->DrawStringFormat(0, liney, F_RIGHT, fontSize, colorWhite, F_FIXED, "%d allocations", _memTrak_getAllocCount());
	liney-= fontSize;
	renderer->DrawStringFormat(0, liney, F_RIGHT, fontSize, colorWhite, F_FIXED, "%dkB used by engine", _memTrak_getAllocSize()>>10);
#endif

	// Draw the text lines
	liney = basey - fontSize;
	int index = -1;
	const char* l;
	while (liney >= 0 && (l = sys->con->EnumLines(&index))) {
		renderer->DrawString(0, liney, F_LEFT, fontSize, colorWhite, F_FIXED, l);
		liney-= fontSize;
	}

	// Generate input caret string
	size_t caretPad = 0;
	for (int c = 0; c < caret; c++) {
		int escLen = IsColorEscape(&input[c]);
		if (escLen) {
			c+= escLen - 1;
		} else {
			++caretPad;
		}
	}
	std::string caretStr(caretPad, ' ');
	caretStr += "_";

	// Draw prompt, input text, and caret
	renderer->DrawString(0, basey, F_LEFT, fontSize, colorWhite, F_FIXED, "]");
	renderer->DrawStringFormat(fontSize * 0.66f, basey, F_LEFT, fontSize, colorWhite, F_FIXED, "%s", input);
	renderer->DrawString(fontSize * 0.66f, basey, F_LEFT, fontSize, colorWhite, F_FIXED, caretStr.c_str());
}

void ui_console_c::SetConInput(char* newInput, int newCaret)
{
	std::string_view nextInput = newInput;
	if (nextInput.size() >= 1024) {
		nextInput = nextInput.substr(0, 1023);
	}
	memcpy(input, nextInput.data(), nextInput.size());
	input[nextInput.size()] = '\0';
	caret = newCaret; // TOOD(LV): bounds-check new caret?
}
