/*
 * D3DX9_43.dll proxy (DLL side-loading) - entry point.
 *
 * How it works:
 *   1. Rename the game's original D3DX9_43.dll to D3DX9_43_org.dll (same folder).
 *   2. Build this project (Release|x86) -> output is D3DX9_43.dll.
 *   3. Drop the built D3DX9_43.dll next to Conquer.exe.
 *
 * The game loads our DLL instead of the real one. Every D3DX export is a
 * linker-level forwarder (see proxy.cpp), so all D3DX calls pass straight
 * through to the original D3DX9_43_org.dll while DllMain() runs our code.
 * No function signatures or wrappers are needed - the loader resolves the
 * forwarders, and this module only has to be present.
 *
 * DllMain runs under the loader lock, so keep it trivial: never create a
 * window, load another module, or wait on a thread from here. If you add
 * payload code, hand it to a worker thread (CreateThread) and do the real
 * work there.
 */

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID reserved)
{
	(void)reserved;

	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// The proxy is a pure forwarder, so there is no payload to run here.
		DisableThreadLibraryCalls(moduleHandle);
		break;

	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
