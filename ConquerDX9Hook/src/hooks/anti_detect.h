#pragma once

// ============================================================================
// AntiDetect - debugger / Cheat-Engine detection evasion for Conquer.exe
// ----------------------------------------------------------------------------
// The client ships several detection layers; the serious one is ndac.dll
// (H:\client\ndac.dll, 18.7 MB, VM-obfuscated), which was observed running
// IMMEDIATELY after the process entry point - i.e. before an init thread
// created from DllMain can run, because such threads only start once the
// loader lock is released.
//
// So the hooks are installed in two passes:
//
//   Install()      - called from DllMain, synchronously, before any of the
//                    client's code runs. Must not call LoadLibrary (that can
//                    deadlock under the loader lock), so it only hooks modules
//                    that are already mapped.
//   InstallLate()  - called from HookInitializationThread once the loader lock
//                    is gone. May LoadLibrary, and re-runs the whole set to
//                    fill in anything the early pass had to skip. Idempotent:
//                    already-created hooks are skipped.
//   PerFrame()     - called every frame from HookedEndScene.
//
// Hook set: PEB scrub (BeingDebugged / NtGlobalFlag), IsDebuggerPresent,
// CheckRemoteDebuggerPresent, OutputDebugStringA/W, NtQueryInformationProcess,
// Process32First/Next, OpenProcess, EnumProcesses, GetModuleBaseNameA/W,
// EnumWindows, EnumChildWindows, FindWindowA/W, GetWindowTextA/W,
// GetClassNameA, TerminateProcess.
// ============================================================================

namespace AntiDetect {

	// Early pass - called from DllMain (DLL_PROCESS_ATTACH). Safe under the
	// loader lock: never calls LoadLibrary.
	void Install();

	// Full pass - called from HookInitializationThread after the loader lock is
	// released. May LoadLibrary, and re-runs the hook set to fill any gaps left
	// by the early pass. Idempotent.
	void InstallLate();

	// Called every frame from HookedEndScene (game thread).
	void PerFrame();
}
