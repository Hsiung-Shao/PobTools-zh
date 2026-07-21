// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Core Config
//

#include "common.h"
#include "system.h"

#include "core_config.h"

#include <fmt/core.h>

// =======================
// core_IConfig Interface
// =======================

class core_config_c: public core_IConfig, public conPrintHook_c, public conCmdHandler_c {
public:
	// Interface
	bool	LoadConfig(std::filesystem::path const& cfgName);
	bool	SaveConfig(std::filesystem::path const& cfgName);

	// Encapsulated
	core_config_c(sys_IMain* sysHnd);

	sys_IMain* sys;

	void	C_Set(IConsole* conHnd, args_c &args);
	void	C_SetA(IConsole* conHnd, args_c &args);
	void	C_Toggle(IConsole* conHnd, args_c &args);
	void	C_CmdList(IConsole* conHnd, args_c &args);
	void	C_CvarList(IConsole* conHnd, args_c &args);
	void	C_Clear(IConsole* conHnd, args_c &args);
	void	C_Exec(IConsole* conHnd, args_c &args);
	void	C_Exit(IConsole* conHnd, args_c &args);
	void	C_Restart(IConsole* conHnd, args_c &args);
	void	C_MemReport(IConsole* conHnd, args_c &args);

	conVar_c* con_log;
	bool	logOpen;
	fileOutputStream_c logFile;

	void	ConPrintHook(const char* text);
};

core_IConfig* core_IConfig::GetHandle(sys_IMain* sysHnd)
{
	return new core_config_c(sysHnd);
}

void core_IConfig::FreeHandle(core_IConfig* hnd)
{
	delete (core_config_c*)hnd;
}

core_config_c::core_config_c(sys_IMain* sysHnd)
	: conPrintHook_c(sysHnd->con), conCmdHandler_c(sysHnd->con), sys(sysHnd)
{
	Cmd_Add("set", 2, "<cvar_name> <cvar_value>", this, &core_config_c::C_Set);
	Cmd_Add("seta", 2, "<cvar_name> <cvar_value>", this, &core_config_c::C_SetA);
	Cmd_Add("toggle", 1, "<cvar_name>", this, &core_config_c::C_Toggle);
	Cmd_Add("cmdList", 0, "", this, &core_config_c::C_CmdList);
	Cmd_Add("cvarList", 0, "", this, &core_config_c::C_CvarList);
	Cmd_Add("clear", 0, "", this, &core_config_c::C_Clear);
	Cmd_Add("exec", 1, "<configname>", this, &core_config_c::C_Exec);
	Cmd_Add("exit", 0, "", this, &core_config_c::C_Exit);
	Cmd_Add("quit", 0, "", this, &core_config_c::C_Exit);
	Cmd_Add("restart", 0, "", this, &core_config_c::C_Restart);
	Cmd_Add("memreport", 0, "", this, &core_config_c::C_MemReport);

	con_log = sys->con->Cvar_Add("con_log", CV_ARCHIVE, "0");
	logOpen = false;
	InstallPrintHook();
}

// ==============
// Basic Commands
// ==============

void core_config_c::C_Set(IConsole* conHnd, args_c &args)
{
	conVar_c* cv = conHnd->Cvar_Add(args[1], CV_SET, "");
	cv->Set(args[2]);
}

void core_config_c::C_SetA(IConsole* conHnd, args_c &args)
{
	conVar_c* cv = conHnd->Cvar_Add(args[1], CV_SET|CV_ARCHIVE, "");
	cv->Set(args[2]);
}

void core_config_c::C_Toggle(IConsole* conHnd, args_c &args)
{
	conVar_c* cv = conHnd->Cvar_Ptr(args[1]);
	if (cv) {
		// Toggle it
		cv->Toggle();
	} else {
		// Oops.
		conHnd->Printf("Cvar '%s' does not exist.\n", args[1]);
	}
}

void core_config_c::C_CmdList(IConsole* conHnd, args_c &args)
{
	int index = -1;
	while (conCmd_c* cmd = conHnd->EnumCmd(&index)) {
		conHnd->Print(fmt::format(" {} {}\n", cmd->name, cmd->usage).c_str());
	}
}

void core_config_c::C_CvarList(IConsole* conHnd, args_c &args)
{
	int index = -1;
	while (conVar_c* cv = conHnd->EnumCvar(&index)) {
		conHnd->Print(fmt::format("{}{}{}  {} = \"{}\"\n", cv->flags & CV_ARCHIVE? 'A':' ', cv->flags & CV_READONLY? 'R':' ', cv->flags & CV_CLAMP? 'C':' ', cv->name, cv->strVal).c_str());
	}
}

void core_config_c::C_Clear(IConsole* conHnd, args_c &args)
{
	conHnd->Clear();
}

void core_config_c::C_Exec(IConsole* conHnd, args_c &args)
{
	LoadConfig(args[1]);
}

void core_config_c::C_Exit(IConsole* conHnd, args_c &args)
{
	sys->Exit();
}

void core_config_c::C_Restart(IConsole* conHnd, args_c &args)
{
	sys->Restart();
}

void core_config_c::C_MemReport(IConsole* conHnd, args_c &args)
{
#ifdef _MEMTRAK_H
	_memTrak_memReport("memreport.log");
	conHnd->Printf("Memory report saved to memreport.log\n");
#else
	conHnd->Printf("Memory report not available in Release builds.\n");
#endif
}

// ============
// Config Files
// ============

bool core_config_c::LoadConfig(std::filesystem::path const& cfgName)
{
	// Make sure it has .cfg extension
	auto fileName = cfgName;
	fileName.replace_extension(".cfg");

	sys->con->Print(fmt::format("Executing {}\n", fileName.generic_u8string()).c_str());

	// Read the config file
	fileInputStream_c f;
	if (f.FileOpen(fileName, true)) {
		sys->con->Warning("config file not found");
		return true;
	}
	size_t flen = f.GetLen();
	char* cfg = new char[flen+2];
	f.Read(cfg, flen);
	cfg[flen] = '\n';
	cfg[flen+1] = 0;
	f.FileClose();

	// Parse the config text
	char* line = cfg;
	for (char* i = cfg; *i; i++) {
		if (*i == '\r' || *i == '\n') {
			// Remove comments and null terminate
			*i = 0;
			char *comm = strstr(line, "//");
			if (comm) {
				*comm = 0;
			}

			// Execute if there's anything left
			if (*line) {
				sys->con->Execute(line);
			}
			line = i + 1;
		}
	}

	delete[] cfg;
	return false;
}

bool core_config_c::SaveConfig(std::filesystem::path const& cfgName)
{
	// Make sure it has .cfg extension
	auto fileName = cfgName;
	fileName.replace_extension(".cfg");

	sys->con->Print(fmt::format("Saving {}\n", fileName.generic_u8string()).c_str());

	// Open the config file
	fileOutputStream_c f;
	if (f.FileOpen(fileName, false)) {
		sys->con->Warning("couldnt write config file");
		return true;
	}

	// Write archived cvars
	int index = -1;
	while (conVar_c* cv = sys->con->EnumCvar(&index)) {
		if (cv->flags & CV_ARCHIVE) {
			f.FilePrintf("set %s \"%s\"\n", cv->name.c_str(), cv->strVal.c_str());
		}
	}

	f.FileClose();
	return false;
}

// ===============
// Console Logging
// ===============

void core_config_c::ConPrintHook(const char* text)
{
	if (con_log->intVal) {
		if (logOpen == false) {
			logFile.FileOpen(std::filesystem::u8path(CFG_LOGFILE), false);
			logOpen = true;
			logFile.FilePrintf("Log opened.\n");
		}
		logFile.Write(text, strlen(text));
		logFile.FileFlush();
	} else {
		if (logOpen) {
			logFile.FileClose();
			logOpen = false;
		}
	}
}
