/*
 * gadget_loader.cpp - load the Frida Gadget sidecar from a worker thread.
 *
 * Why a worker thread
 * -------------------
 * DllMain runs with the loader lock held. Calling LoadLibrary from there
 * re-enters the loader and deadlocks. The Gadget also does real work in its
 * own DllMain (it reads its config and spins up a listener thread), so it must
 * not be mapped while we are holding the lock either. CreateThread from
 * DllMain is explicitly allowed by the loader, and the new thread starts with
 * the lock free, so that is where the load happens.
 *
 * Why LoadLibraryEx with an explicit search path
 * ---------------------------------------------
 * A bare LoadLibrary("D3DX9_43_44.dll") resolves against the process search
 * order, which on a game that ships in a writable folder means a planted
 * DLL next to Conquer.exe could be picked up instead. Naming the file
 * absolutely and restricting the search to the Gadget's own directory plus
 * System32 removes that. It also keeps the Gadget's own dependencies out of
 * the game directory.
 *
 * The Gadget needs no configuration from us: it derives its config path from
 * its own module name, so D3DX9_43_44.dll reads D3DX9_43_44.config from the
 * same folder. Renaming one means renaming the other.
 */

#include "gadget_loader.h"

#include <cstdio>
#include <string>

namespace {

// Default sidecar name. Kept beside the real D3DX9_43_org.dll so the pair
// blends into a folder that already holds D3DX9_43.dll and D3DX9_43_org.dll.
const wchar_t* const kGadgetFileName = L"D3DX9_43_44.dll";

// Escape hatch for testing: point the loader at a different Gadget build.
const wchar_t* const kGadgetEnvVar = L"DX9HOOK_GADGET";

// ASCII marker so build tooling can prove this translation unit is linked in.
// A forwarder-only build of this DLL looks perfectly healthy - it loads, the
// game runs, every D3DX call works - and simply never starts Frida. build.bat
// greps the linked image for this string and fails loudly if it is absent, so
// a stale source tree can never ship silently again. Referenced below so the
// optimiser cannot discard it.
const char kLoaderMarker[] = "DX9HOOK_GADGET_LOADER_V1";

void Trace(const wchar_t* message)
{
	// Visible in DebugView / a debugger's output window. Deliberately not
	// MessageBoxW: this runs in a game and must never block on UI.
	OutputDebugStringW(message);
}

// Directory of the module that contains `module`, with a trailing separator.
std::wstring ModuleDirectory(HMODULE module)
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
	{
		return std::wstring();
	}

	std::wstring full(path, length);
	const std::wstring::size_type slash = full.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
	{
		return std::wstring();
	}
	return full.substr(0, slash + 1);
}

std::wstring EnvironmentOverride()
{
	wchar_t buffer[32768] = {};
	const DWORD length = GetEnvironmentVariableW(kGadgetEnvVar, buffer, 32768);
	if (length == 0 || length >= 32768)
	{
		return std::wstring();
	}
	return std::wstring(buffer, length);
}

DWORD WINAPI LoaderThread(LPVOID parameter)
{
	const HMODULE proxyModule = static_cast<HMODULE>(parameter);

	// Emits kLoaderMarker as a side effect: this call is what keeps the marker
	// in the linked image for build.bat to find.
	OutputDebugStringA("[dx9hook] gadget loader active: " kLoaderMarker "\n");

	std::wstring gadgetPath = EnvironmentOverride();
	if (gadgetPath.empty())
	{
		const std::wstring directory = ModuleDirectory(proxyModule);
		if (directory.empty())
		{
			Trace(L"[dx9hook] could not resolve the proxy's own directory; Gadget not loaded\n");
			return 1;
		}
		gadgetPath = directory + kGadgetFileName;
	}

	if (GetFileAttributesW(gadgetPath.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		// Expected on a plain proxy deployment: the game runs, Frida is simply
		// absent. Not an error worth interrupting anything over.
		Trace((L"[dx9hook] Gadget not found, skipping: " + gadgetPath + L"\n").c_str());
		return 1;
	}

	// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires an absolute path, which
	// gadgetPath already is. Together with SYSTEM32 it means: the Gadget's own
	// folder for its dependencies, System32 for the Windows APIs it imports,
	// and nothing else.
	HMODULE gadgetModule = LoadLibraryExW(
		gadgetPath.c_str(),
		nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

	// Before KB2533623 the LOAD_LIBRARY_SEARCH_* flags are not understood and
	// the call fails outright. A plain absolute-path load is still better than
	// not loading at all, so retry that way rather than give up.
	if (gadgetModule == nullptr && GetLastError() == ERROR_INVALID_PARAMETER)
	{
		gadgetModule = LoadLibraryW(gadgetPath.c_str());
	}

	if (gadgetModule == nullptr)
	{
		wchar_t message[512] = {};
		_snwprintf_s(
			message, _TRUNCATE,
			L"[dx9hook] LoadLibraryEx failed for %s (error %lu)\n",
			gadgetPath.c_str(), GetLastError());
		Trace(message);
		return 1;
	}

	Trace((L"[dx9hook] Frida Gadget loaded from " + gadgetPath + L"\n").c_str());
	return 0;
}

}  // namespace

namespace dx9hook {

void StartGadgetLoader(HMODULE proxyModule)
{
	const HANDLE thread = CreateThread(nullptr, 0, &LoaderThread, proxyModule, 0, nullptr);
	if (thread == nullptr)
	{
		Trace(L"[dx9hook] CreateThread failed; Gadget not loaded\n");
		return;
	}

	// We never join it: the thread either loads the Gadget and exits, or gives
	// up and exits. Closing the handle here just drops our reference.
	CloseHandle(thread);
}

}  // namespace dx9hook
