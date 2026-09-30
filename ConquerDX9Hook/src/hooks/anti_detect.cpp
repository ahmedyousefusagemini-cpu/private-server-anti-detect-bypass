// GetProcessId / QueryFullProcessImageNameA need at least Vista.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
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

// Defined below; used by the blocklist helpers to record who is looking.
void LogBlockedHit(const char* source, const char* name);

bool IsBlockedProcessNameA(const char* exeName)
{
	for (size_t i = 0; i < _countof(kBlockedProcessSubstrings); ++i)
		if (ContainsInsensitive(exeName, kBlockedProcessSubstrings[i]))
		{
			LogBlockedHit("proc", exeName);
			return true;
		}
	return false;
}

// ---------------------------------------------------------------------------
// Diagnostic: record WHO looks for a blocked tool
// ---------------------------------------------------------------------------
// The blocklist match is rare, so logging only on a hit is cheap. The stack
// backtrace tells us which module is doing the looking, which is how we find
// out which detector is responsible for the Cheat Engine kill.
void LogBlockedHit(const char* source, const char* name)
{
	void* frames[8] = { 0 };
	USHORT count = CaptureStackBackTrace(2, 8, frames, NULL);

	char who[4][MAX_PATH];
	for (int i = 0; i < 4; ++i)
	{
		who[i][0] = '\0';
		if (i >= (int)count || !frames[i]) continue;

		HMODULE mod = NULL;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)frames[i], &mod) && mod)
		{
			char path[MAX_PATH] = { 0 };
			GetModuleFileNameA(mod, path, sizeof(path));
			const char* base = strrchr(path, '\\');
			_snprintf_s(who[i], sizeof(who[i]), _TRUNCATE, "%s+0x%X",
				base ? base + 1 : path,
				(unsigned)((BYTE*)frames[i] - (BYTE*)mod));
		}
		else
		{
			_snprintf_s(who[i], sizeof(who[i]), _TRUNCATE, "0x%p", frames[i]);
		}
	}

	HookLog("[AntiDetect] BLOCKED-HIT src=%s name='%s' <- %s <- %s <- %s <- %s",
		source, name, who[0], who[1], who[2], who[3]);
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
		{
			LogBlockedHit("window", text);
			return true;
		}
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
// PSAPI. ndac.dll imports these statically, so they must be covered: this is
// how a running Cheat Engine is found (EnumProcesses -> OpenProcess ->
// GetModuleBaseNameW, matched against a name list).
typedef BOOL  (WINAPI *EnumProcesses_t)(DWORD*, DWORD, DWORD*);
typedef DWORD (WINAPI *GetModuleBaseNameA_t)(HANDLE, HMODULE, LPSTR, DWORD);
typedef DWORD (WINAPI *GetModuleBaseNameW_t)(HANDLE, HMODULE, LPWSTR, DWORD);
typedef HANDLE(WINAPI *OpenProcess_t)(DWORD, BOOL, DWORD);

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
EnumProcesses_t              g_realEnumProcesses              = nullptr;
GetModuleBaseNameA_t         g_realGetModuleBaseNameA         = nullptr;
GetModuleBaseNameW_t         g_realGetModuleBaseNameW         = nullptr;
OpenProcess_t                g_realOpenProcess                = nullptr;

// ---------------------------------------------------------------------------
// Safe handle -> PID resolution
// ---------------------------------------------------------------------------
// IMPORTANT: never call GetProcessId() from inside the NtQueryInformationProcess
// hook. GetProcessId is itself implemented on top of NtQueryInformationProcess,
// so doing so recurses into our own hook until the stack overflows and the game
// dies instantly. Resolve the PID through the original (trampoline) instead.
struct ProcessBasicInformation_t
{
	LONG      ExitStatus;
	PVOID     PebBaseAddress;
	ULONG_PTR AffinityMask;
	LONG      BasePriority;
	ULONG_PTR UniqueProcessId;
	ULONG_PTR InheritedFromUniqueProcessId;
};

DWORD GetPidSafe(HANDLE process)
{
	// (HANDLE)-1 is the current-process pseudo handle; GetCurrentProcessId()
	// reads the TEB and never calls NtQueryInformationProcess.
	if (!process || process == (HANDLE)(LONG_PTR)-1 || process == GetCurrentProcess())
		return GetCurrentProcessId();

	if (!g_realNtQueryInformationProcess)
		return 0;

	ProcessBasicInformation_t pbi;
	memset(&pbi, 0, sizeof(pbi));
	// ProcessBasicInformation == 0. Calling the trampoline does not recurse.
	if (g_realNtQueryInformationProcess(process, 0, &pbi, sizeof(pbi), NULL) < 0)
		return 0;
	return (DWORD)(ULONG_PTR)pbi.UniqueProcessId;
}

// ---------------------------------------------------------------------------
// PID -> image name, and "is this a tool we are hiding?"
// ---------------------------------------------------------------------------
// Uses only unhooked calls: g_realOpenProcess (trampoline) + the plain
// QueryFullProcessImageNameA + CloseHandle. Never OpenProcess directly - that
// is itself hooked below.
bool GetPidImageName(DWORD pid, char* out, size_t outSize)
{
	out[0] = '\0';
	if (!pid || pid == GetCurrentProcessId()) return false;
	if (!g_realOpenProcess) return false;

	HANDLE h = g_realOpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h) return false;
	DWORD len = (DWORD)outSize;
	BOOL ok = QueryFullProcessImageNameA(h, 0, out, &len);
	CloseHandle(h);
	if (!ok) out[0] = '\0';
	return ok != FALSE;
}

// Fails open: if the name cannot be resolved we do not hide anything, so a
// resolution failure can never break legitimate callers.
bool IsBlockedPid(DWORD pid)
{
	char name[MAX_PATH];
	if (!GetPidImageName(pid, name, sizeof(name))) return false;
	const char* base = strrchr(name, '\\');
	return IsBlockedProcessNameA(base ? base + 1 : name);
}

// Replacement shown to the scanner instead of the real tool name.
const char kBenignNameA[] = "svchost.exe";
const wchar_t kBenignNameW[] = L"svchost.exe";

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

// NOTE: a continuous PEB-scrub watchdog thread was tried here and REVERTED.
// It made things strictly worse - the client died within ~4 s of resuming
// instead of reaching ~500 MB. ndac.dll imports Thread32First/Thread32Next
// (thread enumeration), so adding a second thread to the process handed it an
// easy signal. Keep the process's thread count unchanged: scrub the PEB only
// from Install() and from the existing EndScene-driven PerFrame().

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

	// Only the debug-info classes need touching and they are rare, so the
	// common path costs nothing extra.
	if (infoClass != 7 && infoClass != 0x1E && infoClass != 0x1F)
		return status;

	if (GetPidSafe(process) != GetCurrentProcessId())
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
// PSAPI process-scan filter
// ---------------------------------------------------------------------------
// ndac.dll's scanner (RTTI: CProcessStringInfoStream / CProcessModuleInfoStream
// / CWindowInfoStream) walks processes with EnumProcesses, opens each one and
// reads its base name against a list. Hide the tool at every step of that
// chain - this is the path a merely-running Cheat Engine is caught by.
BOOL WINAPI HookedEnumProcesses(DWORD* pids, DWORD cb, LPDWORD needed)
{
	if (!g_realEnumProcesses(pids, cb, needed)) return FALSE;
	if (!pids || !needed) return TRUE;

	DWORD count = *needed / sizeof(DWORD);
	DWORD write = 0;
	for (DWORD i = 0; i < count; ++i)
	{
		if (!IsBlockedPid(pids[i]))
			pids[write++] = pids[i];
	}
	*needed = write * sizeof(DWORD);
	return TRUE;
}

DWORD WINAPI HookedGetModuleBaseNameA(HANDLE process, HMODULE module, LPSTR name, DWORD size)
{
	DWORD r = g_realGetModuleBaseNameA(process, module, name, size);
	if (r && name && size > 0 && IsBlockedProcessNameA(name))
		lstrcpynA(name, kBenignNameA, (int)size);   // harmless name, no match
	return r;
}

DWORD WINAPI HookedGetModuleBaseNameW(HANDLE process, HMODULE module, LPWSTR name, DWORD size)
{
	DWORD r = g_realGetModuleBaseNameW(process, module, name, size);
	if (r && name && size > 0 && IsBlockedProcessNameW(name))
		lstrcpynW(name, kBenignNameW, (int)size);
	return r;
}

// Deny the scanner a handle to the tool in the first place.
HANDLE WINAPI HookedOpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
	if (IsBlockedPid(pid))
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return NULL;
	}
	return g_realOpenProcess(access, inherit, pid);
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
	// GetPidSafe, not GetProcessId: the latter goes through
	// NtQueryInformationProcess and would recurse into our hook.
	if (GetPidSafe(process) == GetCurrentProcessId())
	{
		HookLog("[AntiDetect] blocked TerminateProcess on self (code %u)", exitCode);
		return TRUE;
	}
	return g_realTerminateProcess(process, exitCode);
}

// ---------------------------------------------------------------------------
// Hook installation helper
// ---------------------------------------------------------------------------
// The early pass runs inside DllMain under the loader lock, where LoadLibrary
// can deadlock. It only hooks modules that are already mapped; InstallLate()
// re-runs with this enabled to fill any gaps.
bool g_allowLoadLibrary = true;

void InstallHook(const char* module, const char* name, LPVOID detour, LPVOID* original)
{
	HMODULE mod = GetModuleHandleA(module);
	if (!mod && g_allowLoadLibrary)
		mod = LoadLibraryA(module);
	if (!mod)
	{
		HookLog("[AntiDetect] %s not mapped - skipping %s", module, name);
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

	// Shared by Install() and InstallLate(); idempotent (InstallHook skips hooks
	// that already exist), so the late pass only fills gaps.
	static void InstallAllHooks()
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

		// --- PSAPI process-scan filter ---
		// OpenProcess first: IsBlockedPid resolves image names through the
		// g_realOpenProcess trampoline, so it must exist before the filters that
		// depend on it go live.
		InstallHook("kernel32.dll", "OpenProcess",
			(LPVOID)HookedOpenProcess, (LPVOID*)&g_realOpenProcess);
		InstallHook("psapi.dll", "EnumProcesses",
			(LPVOID)HookedEnumProcesses, (LPVOID*)&g_realEnumProcesses);
		InstallHook("psapi.dll", "GetModuleBaseNameA",
			(LPVOID)HookedGetModuleBaseNameA, (LPVOID*)&g_realGetModuleBaseNameA);
		InstallHook("psapi.dll", "GetModuleBaseNameW",
			(LPVOID)HookedGetModuleBaseNameW, (LPVOID*)&g_realGetModuleBaseNameW);

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

	}

	// Early pass: called from DllMain, synchronously, before any of the client's
	// code runs. Threads created inside DllMain do not start until the loader
	// lock is released (which happens after the entry point), and the client's
	// anti-cheat (ndac.dll) was observed running immediately after the entry
	// point - so installing from the init thread loses that race.
	void Install()
	{
		g_allowLoadLibrary = false;   // LoadLibrary can deadlock under the loader lock

		MH_STATUS initStatus = MH_Initialize();
		if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
			HookLog("[AntiDetect] MH_Initialize failed (%d)", initStatus);

		InstallAllHooks();
		HookLog("[AntiDetect] Install() (early, from DllMain) complete");
	}

	// Full pass: called from the init thread once the loader lock is gone. May
	// LoadLibrary, and re-runs the whole set to fill anything the early pass had
	// to skip.
	void InstallLate()
	{
		g_allowLoadLibrary = true;
		InstallAllHooks();
		HookLog("[AntiDetect] InstallLate() complete");
	}

	// -----------------------------------------------------------------------
	// Neutralise ndac.dll's INT 1 self-terminate
	// -----------------------------------------------------------------------
	// ndac.dll kills the client once it decides a debugger is present. The stub
	// it runs (found by disassembling ndac+0x245DA3 at runtime) is:
	//
	//     CD 01                    int 1          <- raises the unhandled exception
	//     E8 ?? ?? ?? ??           call ...
	//     89 84 2A 02 00 C3 FF     mov [edx+ebp-0x3CFFFE], eax
	//     58                       pop eax
	//     05 E5 7F 0B 00           add eax, 0xB7FE5     (position-independent stub)
	//     FF E0                    jmp eax
	//
	// We scan ndac's executable sections for that fixed tail and replace the
	// leading `CD 01` with two NOPs, so the path becomes a no-op instead of a
	// process kill.
	//
	// Runs from the EXISTING init thread on purpose - adding a thread was
	// measured to make the client die faster (ndac imports Thread32First/
	// Thread32Next and enumerates threads).
	//
	// CAVEAT: ndac is a VM protector with encrypted sections; it may
	// integrity-check its own code, in which case this patch trips a different
	// detection. Returns the number of sites patched (0 = not found / not yet
	// loaded).
	int PatchNdacInt1()
	{
		HMODULE ndac = GetModuleHandleA("ndac.dll");
		if (!ndac) return 0;

		PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)ndac;
		if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
		PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)ndac + dos->e_lfanew);
		if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return 0;

		// Fixed 15-byte tail that immediately follows the INT 1 stub.
		static const unsigned char kTail[15] = {
			0x89, 0x84, 0x2A, 0x02, 0x00, 0xC3, 0xFF,   // mov [edx+ebp-0x3CFFFE], eax
			0x58,                                       // pop eax
			0x05, 0xE5, 0x7F, 0x0B, 0x00,               // add eax, 0xB7FE5
			0xFF, 0xE0                                  // jmp eax
		};

		int patched = 0;
		PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);

		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			// Only executable sections can hold the stub, and they are readable.
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

			BYTE* base = (BYTE*)ndac + sec[i].VirtualAddress;
			DWORD size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
			if (size <= sizeof(kTail)) continue;

			for (DWORD off = 0; off + sizeof(kTail) <= size; ++off)
			{
				if (memcmp(base + off, kTail, sizeof(kTail)) != 0) continue;

				// The tail starts 7 bytes into the stub, so the INT 1 is at
				// off-7 and the `E8` call opcode at off-5.
				if (off < 7) continue;
				BYTE* stub = base + off - 7;
				if (stub[0] != 0xCD || stub[1] != 0x01 || stub[2] != 0xE8) continue;

				DWORD oldProtect = 0;
				if (!VirtualProtect(stub, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
				{
					HookLog("[AntiDetect] ndac INT1: VirtualProtect failed (%lu)", GetLastError());
					continue;
				}
				stub[0] = 0x90;   // NOP
				stub[1] = 0x90;   // NOP
				VirtualProtect(stub, 2, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), stub, 2);

				++patched;
				HookLog("[AntiDetect] ndac INT1 patched at %p (ndac+0x%X)",
					(void*)stub, (unsigned)((BYTE*)stub - (BYTE*)ndac));
			}
		}

		if (patched)
			HookLog("[AntiDetect] ndac INT1: %d site(s) neutralised", patched);
		return patched;
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
