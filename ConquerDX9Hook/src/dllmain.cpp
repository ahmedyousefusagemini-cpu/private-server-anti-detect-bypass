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
 * window, load another module, or wait on a thread from here. The payload is
 * therefore handed to a worker thread, which does the real work.
 *
 * The payload is the Frida Gadget: a sidecar DLL (D3DX9_43_44.dll by default)
 * that boots Frida inside this process. See gadget_loader.h.
 */

#include <windows.h>

#include "gadget_loader.h"

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID reserved)
{
	(void)reserved;

	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(moduleHandle);

		// Hand the Gadget load to a worker thread; this returns immediately.
		// If the sidecar is absent the game simply runs without Frida.
		dx9hook::StartGadgetLoader(moduleHandle);
		break;

	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
