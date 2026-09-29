#include <windows.h>
#include "anti_detect.h"
#include "log.h"

namespace AntiDetect {

	void Install()
	{
		HookLog("[AntiDetect] Install() - stub, no hooks yet");
		// TODO: install debugger-hiding hooks here.
		// TODO: install Cheat Engine evasion hooks here.
	}

	void PerFrame()
	{
		// TODO: per-frame anti-detect work (called from HookedEndScene).
	}
}
