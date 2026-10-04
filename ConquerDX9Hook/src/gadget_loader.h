/*
 * gadget_loader.h - bring the Frida Gadget into the host process.
 *
 * The proxy DLL (D3DX9_43.dll) is loaded by the game very early, under the
 * loader lock. Loading the Gadget has to happen off that lock, so the entry
 * point hands the work to a worker thread and returns immediately.
 *
 * The Gadget is the in-process, embeddable build of Frida. Once mapped it
 * boots from its own DllMain, reads its sidecar .config and opens a Frida
 * service (by default a listener on 127.0.0.1:27042) that a normal Frida
 * client can attach to.
 */

#ifndef DX9HOOK_GADGET_LOADER_H
#define DX9HOOK_GADGET_LOADER_H

#include <windows.h>

namespace dx9hook {

// Spawns the worker thread that loads the Gadget sidecar. Returns immediately;
// never blocks and never loads anything on the calling thread, so it is safe
// to call from DllMain(DLL_PROCESS_ATTACH).
//
// proxyModule is this proxy DLL's HMODULE. The Gadget is looked up next to it
// by default, unless the DX9HOOK_GADGET environment variable names an
// absolute path to use instead.
void StartGadgetLoader(HMODULE proxyModule);

}  // namespace dx9hook

#endif  // DX9HOOK_GADGET_LOADER_H
