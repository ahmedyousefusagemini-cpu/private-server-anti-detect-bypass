#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include "log.h"

void HookLog(const char* fmt, ...)
{
	char exePath[MAX_PATH] = { 0 };
	if (!GetModuleFileNameA(NULL, exePath, MAX_PATH)) return;
	char* s = strrchr(exePath, '\\'); if (s) *(s + 1) = 0;
	char logPath[MAX_PATH]; _snprintf_s(logPath, _TRUNCATE, "%shook_init.log", exePath);
	FILE* f = nullptr; if (fopen_s(&f, logPath, "a") != 0 || !f) return;
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fprintf(f, "\n"); fclose(f);
}
