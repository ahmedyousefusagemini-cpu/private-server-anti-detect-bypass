#pragma once

// Shared diagnostic log. Appends to "hook_init.log" next to the game exe
// (the process that loaded this proxy DLL).
void HookLog(const char* fmt, ...);
