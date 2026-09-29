// GetProcessId / QueryFullProcessImageNameA need at least Vista.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "anti_detect.h"
#include "log.h"
#include "MinHook.h"

// ============================================================================
// AntiDetect - neutralise Conquer.exe's anti-cheat / anti-debug checks
// ----------------------------------------------------------------------------
// Analysis of Conquer.exe (build 7952, image base 0x400000) found:
//
//   * CAntiCheatMgr  (3drole\anticheatmgr.cpp) - the anti-cheat manager.
//     Its Process() runs from the shell dialog (myshell\myshelldlg.cpp:1478,
//     __except handler Catch_00a405f2) and from the main tick FUN_011341fc.
//   * CMsgCheatingProgram - client -> server "you are cheating" report.
//   * A periodic detector FUN_01086d90 @ 0x01086d90 walks every process with
//     CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) + Process32First/Next and
//     matches "MSDEV.EXE" (strstr) plus the byte-spelled names "zfws.exe" and
//     "zfhook.exe"; on a hit it reports (FUN_00d95103, event 0xc5b6a) and then
//     punishes (FUN_010d9966).
//
// The client imports the whole detection API set STATICALLY, so hooking the
// real functions with MinHook catches every call site - whether the client
// calls the import thunk directly or resolves it at runtime via
// GetProcAddress. IAT slots confirmed in the binary:
//     IsDebuggerPresent        IAT 0x0150022C
//     TerminateProcess         IAT 0x01500138
//     CreateToolhelp32Snapshot IAT 0x01500168
//
// Strategy: make every detection primitive return "clean", and refuse the
// client's self-terminate punish path.
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// Blocklist - process image names (lower-case, substring match) treated as a
// cheat/debugger tool. Filtering these out of process and window enumeration
// hides the tool from the scanner.
// ---------------------------------------------------------------------------
const char* const kBlockedProcessSubstrings[] = {
	"msdev.exe",            // VS6 debugger  - confirmed in FUN_01086d90
	"zfws.exe",             // confirmed in FUN_01086d90
	"zfhook.exe",           // confirmed in FUN_01086d90
	"cheatengine",          // Cheat Engine
	"cheat engine",
	"ollydbg",
	"x64dbg",
	"x32dbg",
	"windbg",
	"idaq",
	"ida64",
	"idag",
	"processhacker",
	"procexp",
	"scylla",
	"artmoney",
	"tsearch",
	"frida",
	"wireshark",
	"charles",
};

// Window class / title fragments that identify a tool window.
const char* const kBlockedWindowSubstrings[] = {
	"cheat engine",
	"cheatengine",
	"tfrmmainwindow",       // Cheat Engine's main window class (Lazarus)
	"ollydbg",
	"x64dbg",
	"x32dbg",
	"windbg",
	"scylla",
	"artmoney",
	"process hacker",
};

bool ContainsInsensitive(const char* haystack, const char* needleLower)
{
	if (!haystack || !needleLower) return false;
	size_t nlen = strlen(needleLower);
	if (nlen == 0) return false;
	for (const char* p = haystack; *p; ++p)
	{
		size_t i = 0;
		while (i < nlen && p[i])
		{
			char c = p[i];
			if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
			if (c != needleLower[i]) break;
			++i;
		}
		if (i == nlen) return true;
	}
	return false;
}

bool IsBlockedProcessNameA(const char* exeName)
{
	for (size_t i = 0; i < _countof(kBlockedProcessSubstrings); ++i)
		if (ContainsInsensitive(exeName, kBlockedProcessSubstrings[i]))
			return true;
	return false;
}

// kernel32 exports the unsuffixed "Process32First"/"Process32Next" as the ANSI
// variant and "Process32FirstW"/"Process32NextW" as the wide one (there is no
// Process32FirstA export). Conquer.exe uses the ANSI one - FUN_01086d90 compares
// the name with strlen/strstr - but both are hooked.
bool IsBlockedProcessNameW(const wchar_t* exeName)
{
	if (!exeName) return false;
	char narrow[MAX_PATH];
	if (WideCharToMultiByte(CP_ACP, 0, exeName, -1, narrow, sizeof(narrow), NULL, NULL) <= 0)
		return false;
	return IsBlockedProcessNameA(narrow);
}

bool IsBlockedWindowString(const char* text)
{
	if (!text) return false;
	for (size_t i = 0; i < _countof(kBlockedWindowSubstrings); ++i)
		if (ContainsInsensitive(text, kBlockedWindowSubstrings[i]))
			return true;
	return false;
}

// The real GetClassNameA, captured during Install(). IsBlockedWindow calls it
// directly so the class-name check never recurses into our own hook.
typedef int (WINAPI *GetClassNameA_t)(HWND, LPSTR, int);
GetClassNameA_t g_realGetClassNameA = nullptr;

// Owning process image name of a window (empty when it cannot be determined).
void GetWindowProcessName(HWND hwnd, char* out, size_t outSize)
{
	out[0] = '\0';
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid || pid == GetCurrentProcessId()) return;

	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h) return;
	DWORD len = (DWORD)outSize;
	if (!QueryFullProcessImageNameA(h, 0, out, &len))
		out[0] = '\0';
	CloseHandle(h);
}

// A window belongs to a blocked tool if either its owning process or its
// class name matches the blocklist.
bool IsBlockedWindow(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd)) return false;

	char exe[MAX_PATH];
	GetWindowProcessName(hwnd, exe, sizeof(exe));
	if (exe[0])
	{
		const char* base = strrchr(exe, '\\');
		if (IsBlockedProcessNameA(base ? base + 1 : exe)) return true;
	}

	char cls[256];
	cls[0] = '\0';
	// Use the captured original: GetClassNameA is itself hooked below.
	if (g_realGetClassNameA)
		g_realGetClassNameA(hwnd, cls, sizeof(cls));
	if (IsBlockedWindowString(cls)) return true;

	return false;
}

// ---------------------------------------------------------------------------
// Original function pointers
// ---------------------------------------------------------------------------
// tlhelp32.h names the ANSI structure PROCESSENTRY32 and, under UNICODE,
// #defines PROCESSENTRY32 -> PROCESSENTRY32W. There is no PROCESSENTRY32A /
// LPPROCESSENTRY32A, so declare the ANSI layout here. It is identical to the
// wide one except szExeFile is CHAR, and szExeFile sits at offset 0x24 in both
// on x86.
struct ProcessEntry32A
{
	DWORD     dwSize;
	DWORD     cntUsage;
	DWORD     th32ProcessID;
	ULONG_PTR th32DefaultHeapID;
	DWORD     th32ModuleID;
	DWORD     cntThreads;
	DWORD     th32ParentProcessID;
	LONG      pcPriClassBase;
	DWORD     dwFlags;
	CHAR      szExeFile[MAX_PATH];
};
typedef ProcessEntry32A* LPProcessEntry32A;

typedef BOOL (WINAPI *IsDebuggerPresent_t)(void);
typedef BOOL (WINAPI *CheckRemoteDebuggerPresent_t)(HANDLE, PBOOL);
typedef void (WINAPI *OutputDebugStringA_t)(LPCSTR);
typedef void (WINAPI *OutputDebugStringW_t)(LPCWSTR);
typedef BOOL (WINAPI *Process32FirstA_t)(HANDLE, LPProcessEntry32A);
typedef BOOL (WINAPI *Process32FirstW_t)(HANDLE, LPPROCESSENTRY32W);
typedef BOOL (WINAPI *Process32NextA_t)(HANDLE, LPProcessEntry32A);
typedef BOOL (WINAPI *Process32NextW_t)(HANDLE, LPPROCESSENTRY32W);
typedef BOOL (WINAPI *TerminateProcess_t)(HANDLE, UINT);
typedef BOOL (WINAPI *EnumWindows_t)(WNDENUMPROC, LPARAM);
typedef BOOL (WINAPI *EnumChildWindows_t)(HWND, WNDENUMPROC, LPARAM);
typedef HWND (WINAPI *FindWindowA_t)(LPCSTR, LPCSTR);
typedef HWND (WINAPI *FindWindowW_t)(LPCWSTR, LPCWSTR);
typedef int  (WINAPI *GetWindowTextA_t)(HWND, LPSTR, int);
typedef int  (WINAPI *GetWindowTextW_t)(HWND, LPWSTR, int);
typedef LONG (NTAPI  *NtQueryInformationProcess_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

IsDebuggerPresent_t          g_realIsDebuggerPresent          = nullptr;
CheckRemoteDebuggerPresent_t g_realCheckRemoteDebuggerPresent = nullptr;
OutputDebugStringA_t         g_realOutputDebugStringA         = nullptr;
OutputDebugStringW_t         g_realOutputDebugStringW         = nullptr;
Process32FirstA_t             g_realProcess32FirstA             = nullptr;
Process32FirstW_t             g_realProcess32FirstW             = nullptr;
Process32NextA_t              g_realProcess32NextA              = nullptr;
Process32NextW_t              g_realProcess32NextW              = nullptr;
TerminateProcess_t           g_realTerminateProcess           = nullptr;
EnumWindows_t                g_realEnumWindows                = nullptr;
EnumChildWindows_t           g_realEnumChildWindows           = nullptr;
FindWindowA_t                g_realFindWindowA                = nullptr;
FindWindowW_t                g_realFindWindowW                = nullptr;
GetWindowTextA_t             g_realGetWindowTextA             = nullptr;
GetWindowTextW_t             g_realGetWindowTextW             = nullptr;
NtQueryInformationProcess_t  g_realNtQueryInformationProcess  = nullptr;

// ---------------------------------------------------------------------------
// Debugger hiding
// ---------------------------------------------------------------------------
// Clears PEB->BeingDebugged and the heap-debug bits of PEB->NtGlobalFlag.
// Defeats detectors that read the PEB directly instead of calling an API.
void ClearPebDebugFlags()
{
#if defined(_WIN64)
	BYTE* peb = (BYTE*)__readgsqword(0x60);
#else
	BYTE* peb = (BYTE*)__readfsdword(0x30);
#endif
	if (!peb) return;
	peb[0x02] = 0;                    // PEB->BeingDebugged
	*(DWORD*)(peb + 0x68) &= ~0x70u;  // PEB->NtGlobalFlag (FLG_HEAP_*)
}

BOOL WINAPI HookedIsDebuggerPresent(void)
{
	return FALSE;
}

BOOL WINAPI HookedCheckRemoteDebuggerPresent(HANDLE, PBOOL pbDebuggerPresent)
{
	if (pbDebuggerPresent) *pbDebuggerPresent = FALSE;
	return TRUE;
}

// Neutralises the classic OutputDebugStringA + GetLastError() probe: with no
// debugger the real call fails and sets last-error; the hook makes it a silent
// success so the check always reads "not debugged".
void WINAPI HookedOutputDebugStringA(LPCSTR)
{
	SetLastError(ERROR_SUCCESS);
}

void WINAPI HookedOutputDebugStringW(LPCWSTR)
{
	SetLastError(ERROR_SUCCESS);
}

LONG NTAPI HookedNtQueryInformationProcess(HANDLE process, ULONG infoClass,
	PVOID info, ULONG infoLength, PULONG returnLength)
{
	LONG status = g_realNtQueryInformationProcess(process, infoClass, info, infoLength, returnLength);

	DWORD pid = GetProcessId(process);
	if (pid != GetCurrentProcessId() && process != (HANDLE)(LONG_PTR)-1)
		return status;

	switch (infoClass)
	{
	case 7:    // ProcessDebugPort         - 0 == no debugger
		if (info && infoLength >= sizeof(ULONG_PTR)) *(ULONG_PTR*)info = 0;
		break;
	case 0x1E: // ProcessDebugObjectHandle - 0 == no debug object
		if (info && infoLength >= sizeof(ULONG_PTR)) *(ULONG_PTR*)info = 0;
		break;
	case 0x1F: // ProcessDebugFlags        - 1 == "no debugger"
		if (info && infoLength >= sizeof(ULONG)) *(ULONG*)info = 1;
		break;
	default:
		break;
	}
	return status;
}

// ---------------------------------------------------------------------------
// Process enumeration filter
// ---------------------------------------------------------------------------
// The scanner (FUN_01086d90) only inspects the entries it is handed, so
// skipping blocked names hides the tool completely.
BOOL WINAPI HookedProcess32FirstA(HANDLE snapshot, LPProcessEntry32A entry)
{
	if (!g_realProcess32FirstA(snapshot, entry)) return FALSE;
	while (IsBlockedProcessNameA(entry->szExeFile))
	{
		if (!g_realProcess32NextA(snapshot, entry)) return FALSE;
	}
	return TRUE;
}

BOOL WINAPI HookedProcess32NextA(HANDLE snapshot, LPProcessEntry32A entry)
{
	while (g_realProcess32NextA(snapshot, entry))
	{
		if (!IsBlockedProcessNameA(entry->szExeFile)) return TRUE;
	}
	return FALSE;
}

BOOL WINAPI HookedProcess32FirstW(HANDLE snapshot, LPPROCESSENTRY32W entry)
{
	if (!g_realProcess32FirstW(snapshot, entry)) return FALSE;
	while (IsBlockedProcessNameW(entry->szExeFile))
	{
		if (!g_realProcess32NextW(snapshot, entry)) return FALSE;
	}
	return TRUE;
}

BOOL WINAPI HookedProcess32NextW(HANDLE snapshot, LPPROCESSENTRY32W entry)
{
	while (g_realProcess32NextW(snapshot, entry))
	{
		if (!IsBlockedProcessNameW(entry->szExeFile)) return TRUE;
	}
	return FALSE;
}

// ---------------------------------------------------------------------------
// Window enumeration / lookup filter
// ---------------------------------------------------------------------------
struct EnumContext
{
	WNDENUMPROC userProc;
	LPARAM      userParam;
};

BOOL CALLBACK EnumWindowsFilter(HWND hwnd, LPARAM lParam)
{
	EnumContext* ctx = (EnumContext*)lParam;
	if (IsBlockedWindow(hwnd)) return TRUE;   // hide it from the caller
	return ctx->userProc(hwnd, ctx->userParam);
}

BOOL CALLBACK EnumChildWindowsFilter(HWND hwnd, LPARAM lParam)
{
	EnumContext* ctx = (EnumContext*)lParam;
	if (IsBlockedWindow(hwnd)) return TRUE;
	return ctx->userProc(hwnd, ctx->userParam);
}

BOOL WINAPI HookedEnumWindows(WNDENUMPROC proc, LPARAM lParam)
{
	if (!proc) return g_realEnumWindows(proc, lParam);
	EnumContext ctx;
	ctx.userProc = proc;
	ctx.userParam = lParam;
	return g_realEnumWindows(EnumWindowsFilter, (LPARAM)&ctx);
}

BOOL WINAPI HookedEnumChildWindows(HWND parent, WNDENUMPROC proc, LPARAM lParam)
{
	if (!proc) return g_realEnumChildWindows(parent, proc, lParam);
	EnumContext ctx;
	ctx.userProc = proc;
	ctx.userParam = lParam;
	return g_realEnumChildWindows(parent, EnumChildWindowsFilter, (LPARAM)&ctx);
}

HWND WINAPI HookedFindWindowA(LPCSTR className, LPCSTR windowName)
{
	if (IsBlockedWindowString(className) || IsBlockedWindowString(windowName))
		return NULL;
	return g_realFindWindowA(className, windowName);
}

HWND WINAPI HookedFindWindowW(LPCWSTR className, LPCWSTR windowName)
{
	char cls[256] = { 0 };
	char name[256] = { 0 };
	if (className)  WideCharToMultiByte(CP_ACP, 0, className, -1, cls, sizeof(cls), NULL, NULL);
	if (windowName) WideCharToMultiByte(CP_ACP, 0, windowName, -1, name, sizeof(name), NULL, NULL);
	if (IsBlockedWindowString(cls) || IsBlockedWindowString(name))
		return NULL;
	return g_realFindWindowW(className, windowName);
}

int WINAPI HookedGetWindowTextA(HWND hwnd, LPSTR text, int maxCount)
{
	if (IsBlockedWindow(hwnd))
	{
		if (text && maxCount > 0) text[0] = '\0';
		return 0;
	}
	return g_realGetWindowTextA(hwnd, text, maxCount);
}

int WINAPI HookedGetWindowTextW(HWND hwnd, LPWSTR text, int maxCount)
{
	if (IsBlockedWindow(hwnd))
	{
		if (text && maxCount > 0) text[0] = L'\0';
		return 0;
	}
	return g_realGetWindowTextW(hwnd, text, maxCount);
}

int WINAPI HookedGetClassNameA(HWND hwnd, LPSTR text, int maxCount)
{
	if (IsBlockedWindow(hwnd))
	{
		if (text && maxCount > 0) text[0] = '\0';
		return 0;
	}
	return g_realGetClassNameA(hwnd, text, maxCount);
}

// ---------------------------------------------------------------------------
// Kill-path block
// ---------------------------------------------------------------------------
// The client imports TerminateProcess (IAT 0x01500138) for its punish path.
// Refuse to let the process terminate itself.
BOOL WINAPI HookedTerminateProcess(HANDLE process, UINT exitCode)
{
	// (HANDLE)-1 is the current-process pseudo handle.
	if (process == (HANDLE)(LONG_PTR)-1 || GetProcessId(process) == GetCurrentProcessId())
	{
		HookLog("[AntiDetect] blocked TerminateProcess on self (code %u)", exitCode);
		return TRUE;
	}
	return g_realTerminateProcess(process, exitCode);
}

// ---------------------------------------------------------------------------
// Hook installation helper
// ---------------------------------------------------------------------------
void InstallHook(const char* module, const char* name, LPVOID detour, LPVOID* original)
{
	HMODULE mod = GetModuleHandleA(module);
	if (!mod)
	{
		HookLog("[AntiDetect] %s not loaded - skipping %s", module, name);
		return;
	}
	LPVOID target = (LPVOID)GetProcAddress(mod, name);
	if (!target)
	{
		HookLog("[AntiDetect] %s!%s not found", module, name);
		return;
	}
	MH_STATUS st = MH_CreateHook(target, detour, original);
	if (st == MH_ERROR_ALREADY_CREATED || st == MH_ERROR_ENABLED)
	{
		// e.g. "Process32First" and "Process32FirstA" can alias one address.
		HookLog("[AntiDetect] %s!%s already hooked - skipped", module, name);
		return;
	}
	if (st != MH_OK)
	{
		HookLog("[AntiDetect] CreateHook %s failed (%d)", name, st);
		return;
	}
	st = MH_EnableHook(target);
	HookLog("[AntiDetect] hooked %s!%s @ %p -> %d", module, name, target, st);
}

} // namespace

namespace AntiDetect {

	void Install()
	{
		ClearPebDebugFlags();

		// --- debugger hiding ---
		InstallHook("kernel32.dll", "IsDebuggerPresent",
			(LPVOID)HookedIsDebuggerPresent, (LPVOID*)&g_realIsDebuggerPresent);
		InstallHook("kernel32.dll", "CheckRemoteDebuggerPresent",
			(LPVOID)HookedCheckRemoteDebuggerPresent, (LPVOID*)&g_realCheckRemoteDebuggerPresent);
		InstallHook("kernel32.dll", "OutputDebugStringA",
			(LPVOID)HookedOutputDebugStringA, (LPVOID*)&g_realOutputDebugStringA);
		InstallHook("kernel32.dll", "OutputDebugStringW",
			(LPVOID)HookedOutputDebugStringW, (LPVOID*)&g_realOutputDebugStringW);
		InstallHook("ntdll.dll", "NtQueryInformationProcess",
			(LPVOID)HookedNtQueryInformationProcess, (LPVOID*)&g_realNtQueryInformationProcess);

		// --- process enumeration filter ---
		// Conquer.exe imports the unsuffixed (ANSI) names; hook the plain and
		// the W exports so every code path is covered. GetProcAddress returns
		// NULL for names that do not exist and InstallHook skips them.
		InstallHook("kernel32.dll", "Process32First",
			(LPVOID)HookedProcess32FirstA, (LPVOID*)&g_realProcess32FirstA);
		InstallHook("kernel32.dll", "Process32FirstA",
			(LPVOID)HookedProcess32FirstA, (LPVOID*)&g_realProcess32FirstA);
		InstallHook("kernel32.dll", "Process32FirstW",
			(LPVOID)HookedProcess32FirstW, (LPVOID*)&g_realProcess32FirstW);
		InstallHook("kernel32.dll", "Process32Next",
			(LPVOID)HookedProcess32NextA, (LPVOID*)&g_realProcess32NextA);
		InstallHook("kernel32.dll", "Process32NextA",
			(LPVOID)HookedProcess32NextA, (LPVOID*)&g_realProcess32NextA);
		InstallHook("kernel32.dll", "Process32NextW",
			(LPVOID)HookedProcess32NextW, (LPVOID*)&g_realProcess32NextW);

		// --- window enumeration / lookup filter ---
		InstallHook("user32.dll", "EnumWindows",
			(LPVOID)HookedEnumWindows, (LPVOID*)&g_realEnumWindows);
		InstallHook("user32.dll", "EnumChildWindows",
			(LPVOID)HookedEnumChildWindows, (LPVOID*)&g_realEnumChildWindows);
		InstallHook("user32.dll", "FindWindowA",
			(LPVOID)HookedFindWindowA, (LPVOID*)&g_realFindWindowA);
		InstallHook("user32.dll", "FindWindowW",
			(LPVOID)HookedFindWindowW, (LPVOID*)&g_realFindWindowW);
		InstallHook("user32.dll", "GetWindowTextA",
			(LPVOID)HookedGetWindowTextA, (LPVOID*)&g_realGetWindowTextA);
		InstallHook("user32.dll", "GetWindowTextW",
			(LPVOID)HookedGetWindowTextW, (LPVOID*)&g_realGetWindowTextW);
		InstallHook("user32.dll", "GetClassNameA",
			(LPVOID)HookedGetClassNameA, (LPVOID*)&g_realGetClassNameA);

		// --- kill-path block ---
		InstallHook("kernel32.dll", "TerminateProcess",
			(LPVOID)HookedTerminateProcess, (LPVOID*)&g_realTerminateProcess);

		HookLog("[AntiDetect] Install() complete");
	}

	void PerFrame()
	{
		// Re-assert the PEB flags about once a second: attaching a debugger (or
		// a CRT re-init) can set them again after Install().
		static DWORD lastTick = 0;
		DWORD now = GetTickCount();
		if (now - lastTick < 1000)
			return;
		lastTick = now;
		ClearPebDebugFlags();
	}
}
