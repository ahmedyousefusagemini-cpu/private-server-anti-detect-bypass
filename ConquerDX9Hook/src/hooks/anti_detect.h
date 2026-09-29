#pragma once

// ============================================================================
// AntiDetect - placeholder for debugger-detection / Cheat-Engine-detection work
// ----------------------------------------------------------------------------
// This module is intentionally empty. It exists so the anti-detection code has
// a wired-in home: Install() runs once from the hook init thread, PerFrame()
// runs once per rendered frame from HookedEndScene.
//
// Intended hook points (not implemented):
//   * Debugger hiding: PEB.BeingDebugged, NtQueryInformationProcess
//     (ProcessDebugPort / ProcessDebugObjectHandle / ProcessDebugFlags),
//     IsDebuggerPresent, CheckRemoteDebuggerPresent, OutputDebugString timing.
//   * Cheat Engine evasion: hide the process/window from CE's EnumWindows and
//     OpenProcess scans, block handle access to the game process.
// ============================================================================

namespace AntiDetect {

	// Called once from HookInitializationThread after MinHook is initialized.
	void Install();

	// Called every frame from HookedEndScene (game thread).
	void PerFrame();
}
