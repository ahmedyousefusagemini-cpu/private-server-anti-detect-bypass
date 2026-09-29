#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>

typedef HRESULT(WINAPI* EndSceneFunc)(LPDIRECT3DDEVICE9);

typedef HRESULT(WINAPI* ResetFunc)(LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS*);


struct GameWindowInfo {
	HWND parentWindowHandle;          
	HWND gameWindowHandle;            
	WNDPROC originalWindowProcedure;  
	LPDIRECT3DDEVICE9 direct3DDevice;  

	GameWindowInfo() : parentWindowHandle(NULL), gameWindowHandle(NULL), originalWindowProcedure(NULL),
		direct3DDevice(nullptr) {}
};
