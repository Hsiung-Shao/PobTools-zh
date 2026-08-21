/*
** startup_trace.cpp - see startup_trace.h
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "startup_trace.h"

static HANDLE     s_file = INVALID_HANDLE_VALUE;
static bool       s_began = false;
static std::mutex s_lock;

double startup_trace_now_ms(void)
{
	FILETIME created, exited, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER a, b;
	a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
	b.LowPart = now.dwLowDateTime;     b.HighPart = now.dwHighDateTime;
	if (b.QuadPart < a.QuadPart) return 0.0;
	return (double)(b.QuadPart - a.QuadPart) / 10000.0; /* 100ns -> ms */
}

void startup_trace_begin(const char* role)
{
	std::lock_guard<std::mutex> g(s_lock);
	if (s_began) return;
	s_began = true;

	/* <exe dir>\PobTools\startup_<role>.txt. Both processes ARE pob-zh.exe, so
	** the module path of the process (not of this DLL) is the install root. */
	wchar_t exe[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return;
	wchar_t* slash = wcsrchr(exe, L'\\');
	if (!slash) return;
	slash[1] = L'\0';

	wchar_t path[MAX_PATH + 64];
	wchar_t wrole[32];
	int i = 0;
	for (; role && role[i] && i < 31; i++) wrole[i] = (wchar_t)(unsigned char)role[i];
	wrole[i] = L'\0';
	swprintf_s(path, L"%sPobTools\\startup_%s.txt", exe, wrole);
	/* The folder normally exists (the updater and the tools log there); create
	** it for a brand-new install so the very first start is traced too. */
	wchar_t dir[MAX_PATH + 16];
	swprintf_s(dir, L"%sPobTools", exe);
	CreateDirectoryW(dir, nullptr);

	s_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
	                     FILE_ATTRIBUTE_NORMAL, nullptr);
	if (s_file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
		/* Another instance of the same role is alive and holds the plain name
		** (two POBs open at once). Its timeline stays intact; this one gets a
		** per-process file instead of vanishing. */
		swprintf_s(path, L"%sPobTools\\startup_%s_%lu.txt", exe, wrole, (unsigned long)GetCurrentProcessId());
		s_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
		                     FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	if (s_file == INVALID_HANDLE_VALUE) return;
	char head[160];
	int len = snprintf(head, sizeof(head), "# pob-zh startup timeline (%s); ms since process creation\r\n", role ? role : "?");
	DWORD w = 0;
	WriteFile(s_file, head, (DWORD)len, &w, nullptr);
}

void startup_trace_mark(const char* fmt, ...)
{
	if (!s_began || s_file == INVALID_HANDLE_VALUE) return;
	char body[512];
	va_list va;
	va_start(va, fmt);
	vsnprintf(body, sizeof(body), fmt, va);
	va_end(va);
	char line[600];
	int len = snprintf(line, sizeof(line), "+%9.1f ms  %s\r\n", startup_trace_now_ms(), body);
	if (len <= 0) return;
	std::lock_guard<std::mutex> g(s_lock);
	DWORD w = 0;
	/* No FlushFileBuffers: WriteFile already hands the line to the OS cache, which
	** survives a process crash; a disk flush per mark would cost more than some of
	** the stages being measured. */
	WriteFile(s_file, line, (DWORD)len, &w, nullptr);
}
