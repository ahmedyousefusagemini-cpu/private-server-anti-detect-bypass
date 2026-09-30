// ============================================================================
// PacketOverlay - in-game packet viewer for Conquer.exe
// ----------------------------------------------------------------------------
// A GDI-rendered window uploaded into a D3D9 texture and drawn as a
// screen-space quad. Everything is self-contained: no ImGui, no D3DX.
//
// The texture is D3DPOOL_MANAGED, so it survives device resets without any
// release/recreate dance. The DIB is only redrawn when something actually
// changed (new packet, scroll, selection, resize), and the quad is drawn
// every frame so the window stays on top of the scene.
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "packet_overlay.h"
#include "packet_capture.h"
#include "log.h"

namespace PacketOverlay {

using namespace PacketCapture;

namespace {

	// -----------------------------------------------------------------------
	// Layout
	// -----------------------------------------------------------------------
	const int kPanelW = 1000;
	const int kPanelH = 620;
	const int kTitleH = 26;
	const int kHeaderH = 20;
	const int kStatusH = 22;
	const int kDetailH = 214;
	const int kRowH = 16;
	const int kListTop = kTitleH + kHeaderH;
	const int kListHeight = kPanelH - kTitleH - kHeaderH - kStatusH - kDetailH;
	const int kVisibleRows = kListHeight / kRowH;

	const int kColSeq = 10;
	const int kColTime = 84;
	const int kColDir = 168;
	const int kColLen = 222;
	const int kColId = 272;
	const int kColPreview = 336;
	const int kColPreviewWidth = kPanelW - kColPreview - 12;

	const int kDetailTop = kTitleH + kHeaderH + kListHeight;
	const int kDetailTitleH = 20;
	const int kDetailLines = (kDetailH - kDetailTitleH - 6) / kRowH;

	// -----------------------------------------------------------------------
	// Colours (COLORREF is 0x00BBGGRR; the DIB is 32bpp BGRX)
	// -----------------------------------------------------------------------
	const COLORREF kColPanel = RGB(16, 18, 24);
	const COLORREF kColTitle = RGB(32, 37, 50);
	const COLORREF kColHeader = RGB(24, 27, 36);
	const COLORREF kColRowA = RGB(20, 22, 30);
	const COLORREF kColRowB = RGB(14, 16, 22);
	const COLORREF kColSelect = RGB(48, 66, 108);
	const COLORREF kColDetail = RGB(12, 14, 19);
	const COLORREF kColBorder = RGB(74, 84, 104);
	const COLORREF kColText = RGB(224, 228, 236);
	const COLORREF kColDim = RGB(126, 134, 148);
	const COLORREF kColSend = RGB(255, 170, 58);
	const COLORREF kColRecv = RGB(94, 220, 142);
	const COLORREF kColWarn = RGB(255, 96, 96);
	const COLORREF kColAccent = RGB(120, 180, 255);

	const uint32_t kPanelAlpha = 240;

	// -----------------------------------------------------------------------
	// State
	// -----------------------------------------------------------------------
	bool  g_visible = true;
	bool  g_visibleInitialised = false;
	bool  g_resourceErrorLogged = false;
	bool  g_dirty = true;
	bool  g_dragging = false;
	POINT g_dragCursor = { 0, 0 };
	int   g_panelX = 28;
	int   g_panelY = 28;

	long  g_scrollFromEnd = 0;   // rows scrolled up from the newest packet
	bool  g_hasSelection = false;
	uint32_t g_selectedSeq = 0;

	uint32_t g_lastTotal = 0;
	uint32_t g_startTick = 0;

	HWND g_renderWindow = nullptr;
	int  g_backbufferW = 0;
	int  g_backbufferH = 0;

	struct Resources
	{
		LPDIRECT3DTEXTURE9 texture;
		HDC      dc;
		HBITMAP  dib;
		HGDIOBJ  oldBitmap;
		HFONT    font;
		HGDIOBJ  oldFont;
		void*    bits;
		int      width;
		int      height;
		int      charW;
		int      charH;

		Resources() : texture(nullptr), dc(nullptr), dib(nullptr), oldBitmap(nullptr),
			font(nullptr), oldFont(nullptr), bits(nullptr), width(0), height(0),
			charW(8), charH(14) {}
	};

	Resources g_res;

	// -----------------------------------------------------------------------
	// GDI helpers
	// -----------------------------------------------------------------------
	void FillRectC(int x, int y, int w, int h, COLORREF colour)
	{
		RECT r;
		r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
		SetDCBrushColor(g_res.dc, colour);
		FillRect(g_res.dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
	}

	void DrawTextClipped(int x, int y, int maxWidth, COLORREF colour, const char* text)
	{
		if (!text || maxWidth <= 0) return;
		int charW = g_res.charW > 0 ? g_res.charW : 7;
		int maxChars = maxWidth / charW;
		if (maxChars <= 0) return;

		int length = (int)strlen(text);
		if (length > maxChars) length = maxChars;

		char buffer[1024];
		if (length > (int)sizeof(buffer) - 1) length = (int)sizeof(buffer) - 1;
		memcpy(buffer, text, length);
		buffer[length] = '\0';

		SetTextColor(g_res.dc, colour);
		SetBkMode(g_res.dc, TRANSPARENT);
		TextOutA(g_res.dc, x, y, buffer, length);
	}

	// GDI never writes the alpha byte, so after drawing we stamp the panel
	// alpha across the whole buffer.
	void ApplyAlpha(uint32_t alpha)
	{
		if (!g_res.bits) return;
		uint32_t* pixels = (uint32_t*)g_res.bits;
		int count = g_res.width * g_res.height;
		uint32_t a = alpha << 24;
		for (int i = 0; i < count; ++i)
			pixels[i] = (pixels[i] & 0x00FFFFFFu) | a;
	}

	void FormatClock(uint32_t tick, char* out, size_t outSize)
	{
		uint32_t elapsed = tick - g_startTick;
		uint32_t minutes = elapsed / 60000u;
		uint32_t seconds = (elapsed / 1000u) % 60u;
		uint32_t millis = elapsed % 1000u;
		_snprintf_s(out, outSize, _TRUNCATE, "%02u:%02u.%03u", minutes, seconds, millis);
	}

	// -----------------------------------------------------------------------
	// Row / detail drawing
	// -----------------------------------------------------------------------
	void BuildHexPreview(const Entry& entry, int maxChars, char* out, size_t outSize)
	{
		int bytes = entry.storedBytes;
		int needed = bytes > 0 ? bytes * 3 - 1 : 0;
		if (needed > maxChars) bytes = (maxChars + 1) / 3;

		int written = 0;
		for (int i = 0; i < bytes && written < (int)outSize - 4; ++i)
		{
			written += _snprintf_s(out + written, outSize - written, _TRUNCATE, "%02X", entry.data[i]);
			if (i + 1 < bytes && written < (int)outSize - 2)
			{
				out[written++] = ' ';
				out[written] = '\0';
			}
		}
		if (written == 0) out[0] = '\0';
	}

	void DrawListRows(const Snapshot& snapshot)
	{
		char text[512];

		long newest = (long)snapshot.total - 1;
		long first = newest - g_scrollFromEnd - (kVisibleRows - 1);

		for (int row = 0; row < kVisibleRows; ++row)
		{
			long seq = first + row;
			if (seq < 0) continue;

			long index = IndexOfSeq(snapshot, (uint32_t)seq);
			if (index < 0) continue;

			const Entry& entry = snapshot.ring[index];
			int y = kListTop + row * kRowH;

			bool selected = g_hasSelection && entry.seq == g_selectedSeq;
			COLORREF background = (row & 1) ? kColRowA : kColRowB;
			if (selected) background = kColSelect;
			FillRectC(1, y, kPanelW - 2, kRowH, background);

			COLORREF directionColour = (entry.direction == DirectionSend) ? kColSend : kColRecv;
			COLORREF bodyColour = selected ? RGB(255, 255, 255) : kColText;

			_snprintf_s(text, _TRUNCATE, "%06u", entry.seq);
			DrawTextClipped(kColSeq, y, 70, selected ? RGB(255, 255, 255) : kColDim, text);

			FormatClock(entry.tick, text, sizeof(text));
			DrawTextClipped(kColTime, y, 80, selected ? RGB(255, 255, 255) : kColDim, text);

			DrawTextClipped(kColDir, y, 50, directionColour,
				entry.direction == DirectionSend ? "SEND" : "RECV");

			_snprintf_s(text, _TRUNCATE, "%u", (unsigned)entry.length);
			DrawTextClipped(kColLen, y, 46, bodyColour, text);

			_snprintf_s(text, _TRUNCATE, "0x%04X", (unsigned)entry.messageId);
			DrawTextClipped(kColId, y, 60, kColAccent, text);

			int maxChars = kColPreviewWidth / (g_res.charW > 0 ? g_res.charW : 7);
			BuildHexPreview(entry, maxChars, text, sizeof(text));
			DrawTextClipped(kColPreview, y, kColPreviewWidth, bodyColour, text);
		}
	}

	void DrawDetailPane(const Snapshot& snapshot)
	{
		char text[512];
		int y = kDetailTop;

		FillRectC(1, y, kPanelW - 2, kDetailH, kColDetail);
		FillRectC(1, y, kPanelW - 2, 1, kColBorder);

		const Entry* entry = nullptr;
		if (g_hasSelection)
		{
			long index = IndexOfSeq(snapshot, g_selectedSeq);
			if (index >= 0) entry = &snapshot.ring[index];
		}

		if (!entry)
		{
			DrawTextClipped(kColSeq + 2, y + 4, kPanelW - 20, kColDim,
				"Click a packet to inspect its bytes.");
			return;
		}

		char clock[32];
		FormatClock(entry->tick, clock, sizeof(clock));

		_snprintf_s(text, _TRUNCATE, "#%06u  %s  len=%u  id=0x%04X  t=%s",
			entry->seq,
			entry->direction == DirectionSend ? "SEND" : "RECV",
			(unsigned)entry->length, (unsigned)entry->messageId, clock);

		DrawTextClipped(kColSeq + 2, y + 4, kPanelW - 20,
			entry->direction == DirectionSend ? kColSend : kColRecv, text);

		int bytes = entry->storedBytes;
		int lineY = y + kDetailTitleH + 2;

		for (int line = 0; line < kDetailLines; ++line)
		{
			int offset = line * 16;
			if (offset >= bytes) break;

			int written = _snprintf_s(text, _TRUNCATE, "%04X  ", offset);

			for (int i = 0; i < 16; ++i)
			{
				if (offset + i < bytes)
					written += _snprintf_s(text + written, sizeof(text) - written, _TRUNCATE, "%02X ", entry->data[offset + i]);
				else
					written += _snprintf_s(text + written, sizeof(text) - written, _TRUNCATE, "   ");
				if (i == 7 && written < (int)sizeof(text) - 2)
				{
					text[written++] = ' ';
					text[written] = '\0';
				}
			}

			written += _snprintf_s(text + written, sizeof(text) - written, _TRUNCATE, " |");
			for (int i = 0; i < 16 && offset + i < bytes; ++i)
			{
				unsigned char c = entry->data[offset + i];
				if (written < (int)sizeof(text) - 2)
				{
					text[written++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
					text[written] = '\0';
				}
			}
			if (written < (int)sizeof(text) - 2)
			{
				text[written++] = '|';
				text[written] = '\0';
			}

			DrawTextClipped(kColSeq + 2, lineY, kPanelW - 20, kColText, text);
			lineY += kRowH;
		}

		if (bytes < (int)entry->length)
		{
			_snprintf_s(text, _TRUNCATE, "  (%d of %u bytes stored)", bytes, (unsigned)entry->length);
			DrawTextClipped(kColPreview, kDetailTop + kDetailTitleH + 2, kColPreviewWidth, kColDim, text);
		}
	}

	void DrawChrome(const Snapshot& snapshot)
	{
		char text[512];

		FillRectC(0, 0, kPanelW, kTitleH, kColTitle);
		FillRectC(0, 0, kPanelW, 1, kColBorder);

		DrawTextClipped(kColSeq, 5, 400, kColText, "Conquer Packet Monitor");

		bool paused = IsPaused();
		_snprintf_s(text, _TRUNCATE, "SEND %ld   RECV %ld   dropped %ld",
			TotalSend(), TotalRecv(), TotalDropped());
		DrawTextClipped(280, 5, 320, kColDim, text);

		if (paused)
			DrawTextClipped(kPanelW - 320, 5, 60, kColWarn, "PAUSED");

		DrawTextClipped(kPanelW - 250, 5, 240, kColDim, "[F8] hide  [F9] pause  [F10] clear");

		// Column header
		FillRectC(0, kTitleH, kPanelW, kHeaderH, kColHeader);
		int headerY = kTitleH + 3;
		DrawTextClipped(kColSeq, headerY, 70, kColDim, "#");
		DrawTextClipped(kColTime, headerY, 80, kColDim, "TIME");
		DrawTextClipped(kColDir, headerY, 50, kColDim, "DIR");
		DrawTextClipped(kColLen, headerY, 46, kColDim, "LEN");
		DrawTextClipped(kColId, headerY, 60, kColDim, "ID");
		DrawTextClipped(kColPreview, headerY, kColPreviewWidth, kColDim, "BYTES");

		// Status bar
		int statusY = kPanelH - kStatusH;
		FillRectC(0, statusY, kPanelW, 1, kColBorder);
		FillRectC(0, statusY + 1, kPanelW, kStatusH - 1, kColTitle);

		long retained = snapshot.count;
		long total = (long)snapshot.total;
		_snprintf_s(text, _TRUNCATE,
			"showing %ld of %ld retained  |  wheel scrolls  |  click a row for the hex dump",
			retained, total);
		DrawTextClipped(kColSeq, statusY + 4, kPanelW - 20, kColDim, text);

		// Outer border
		FillRectC(0, 0, kPanelW, 1, kColBorder);
		FillRectC(0, kPanelH - 1, kPanelW, 1, kColBorder);
		FillRectC(0, 0, 1, kPanelH, kColBorder);
		FillRectC(kPanelW - 1, 0, 1, kPanelH, kColBorder);
	}

	void RedrawPanel()
	{
		Snapshot snapshot = BeginRead();

		// Fill the background (also seeds a valid alpha byte everywhere).
		uint32_t background = (uint32_t)(kPanelAlpha << 24)
			| ((uint32_t)GetRValue(kColPanel) << 16)
			| ((uint32_t)GetGValue(kColPanel) << 8)
			| (uint32_t)GetBValue(kColPanel);

		uint32_t* pixels = (uint32_t*)g_res.bits;
		int count = g_res.width * g_res.height;
		for (int i = 0; i < count; ++i) pixels[i] = background;

		DrawChrome(snapshot);
		DrawListRows(snapshot);
		DrawDetailPane(snapshot);

		EndRead();

		ApplyAlpha(kPanelAlpha);
	}

	// -----------------------------------------------------------------------
	// D3D9 resources
	// -----------------------------------------------------------------------
	void ReleaseResources()
	{
		if (g_res.texture) { g_res.texture->Release(); g_res.texture = nullptr; }

		if (g_res.dc)
		{
			if (g_res.oldFont) { SelectObject(g_res.dc, g_res.oldFont); g_res.oldFont = nullptr; }
			if (g_res.oldBitmap) { SelectObject(g_res.dc, g_res.oldBitmap); g_res.oldBitmap = nullptr; }
			DeleteDC(g_res.dc);
			g_res.dc = nullptr;
		}
		if (g_res.dib) { DeleteObject(g_res.dib); g_res.dib = nullptr; }
		if (g_res.font) { DeleteObject(g_res.font); g_res.font = nullptr; }
		g_res.bits = nullptr;
	}

	bool CreateResources(LPDIRECT3DDEVICE9 device)
	{
		if (!device) return false;

		// Backbuffer size, used to map the cursor and to clamp the window.
		IDirect3DSurface9* backBuffer = nullptr;
		if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
		{
			D3DSURFACE_DESC desc;
			if (SUCCEEDED(backBuffer->GetDesc(&desc)))
			{
				g_backbufferW = (int)desc.Width;
				g_backbufferH = (int)desc.Height;
			}
			backBuffer->Release();
		}

		g_res.width = kPanelW;
		g_res.height = kPanelH;

		// Drawing surface: a top-down 32bpp DIB.
		BITMAPINFO info;
		memset(&info, 0, sizeof(info));
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = g_res.width;
		info.bmiHeader.biHeight = -g_res.height;   // negative = top-down
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;

		g_res.dc = CreateCompatibleDC(NULL);
		if (!g_res.dc) { ReleaseResources(); return false; }

		g_res.dib = CreateDIBSection(g_res.dc, &info, DIB_RGB_COLORS, &g_res.bits, NULL, 0);
		if (!g_res.dib || !g_res.bits) { ReleaseResources(); return false; }
		g_res.oldBitmap = SelectObject(g_res.dc, g_res.dib);

		g_res.font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
		if (!g_res.font)
			g_res.font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
		if (!g_res.font) { ReleaseResources(); return false; }
		g_res.oldFont = SelectObject(g_res.dc, g_res.font);

		TEXTMETRIC tm;
		if (GetTextMetricsA(g_res.dc, &tm))
		{
			g_res.charH = tm.tmHeight;
			g_res.charW = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : tm.tmMaxCharWidth;
		}
		if (g_res.charW <= 0) g_res.charW = 7;
		SetTextAlign(g_res.dc, TA_LEFT | TA_TOP);

		// Managed so it survives device resets.
		HRESULT hr = device->CreateTexture(g_res.width, g_res.height, 1, 0,
			D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_res.texture, NULL);
		if (FAILED(hr) || !g_res.texture)
		{
			HookLog("[Overlay] CreateTexture failed hr=0x%08X", hr);
			ReleaseResources();
			return false;
		}

		HookLog("[Overlay] resources ready (%dx%d, backbuffer %dx%d, char %dx%d)",
			g_res.width, g_res.height, g_backbufferW, g_backbufferH, g_res.charW, g_res.charH);
		return true;
	}

	bool UploadPanel()
	{
		if (!g_res.texture || !g_res.bits) return false;

		D3DLOCKED_RECT locked;
		if (FAILED(g_res.texture->LockRect(0, &locked, NULL, 0))) return false;

		int rowBytes = g_res.width * 4;
		for (int y = 0; y < g_res.height; ++y)
		{
			memcpy((unsigned char*)locked.pBits + (size_t)y * locked.Pitch,
				(unsigned char*)g_res.bits + (size_t)y * rowBytes, rowBytes);
		}
		g_res.texture->UnlockRect(0);
		return true;
	}

	// -----------------------------------------------------------------------
	// D3D9 state save / restore
	// -----------------------------------------------------------------------
	struct SavedState
	{
		DWORD fvf;
		DWORD zEnable, zWrite, alphaBlend, srcBlend, destBlend, cullMode;
		DWORD lighting, fog, stencil, scissor, colorWrite, clipping, textureFactor;
		DWORD tssColorOp, tssColorArg1, tssColorArg2, tssAlphaOp, tssAlphaArg1;
		DWORD tss1ColorOp;
		IDirect3DBaseTexture9* texture0;
		IDirect3DVertexShader9* vertexShader;
		IDirect3DPixelShader9* pixelShader;
	};

	void SaveState(LPDIRECT3DDEVICE9 device, SavedState& state)
	{
		memset(&state, 0, sizeof(state));
		device->GetFVF(&state.fvf);
		device->GetRenderState(D3DRS_ZENABLE, &state.zEnable);
		device->GetRenderState(D3DRS_ZWRITEENABLE, &state.zWrite);
		device->GetRenderState(D3DRS_ALPHABLENDENABLE, &state.alphaBlend);
		device->GetRenderState(D3DRS_SRCBLEND, &state.srcBlend);
		device->GetRenderState(D3DRS_DESTBLEND, &state.destBlend);
		device->GetRenderState(D3DRS_CULLMODE, &state.cullMode);
		device->GetRenderState(D3DRS_LIGHTING, &state.lighting);
		device->GetRenderState(D3DRS_FOGENABLE, &state.fog);
		device->GetRenderState(D3DRS_STENCILENABLE, &state.stencil);
		device->GetRenderState(D3DRS_SCISSORTESTENABLE, &state.scissor);
		device->GetRenderState(D3DRS_COLORWRITEENABLE, &state.colorWrite);
		device->GetRenderState(D3DRS_CLIPPING, &state.clipping);
		device->GetRenderState(D3DRS_TEXTUREFACTOR, &state.textureFactor);
		device->GetTextureStageState(0, D3DTSS_COLOROP, &state.tssColorOp);
		device->GetTextureStageState(0, D3DTSS_COLORARG1, &state.tssColorArg1);
		device->GetTextureStageState(0, D3DTSS_COLORARG2, &state.tssColorArg2);
		device->GetTextureStageState(0, D3DTSS_ALPHAOP, &state.tssAlphaOp);
		device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &state.tssAlphaArg1);
		device->GetTextureStageState(1, D3DTSS_COLOROP, &state.tss1ColorOp);

		state.texture0 = nullptr;
		device->GetTexture(0, &state.texture0);            // addrefs
		state.vertexShader = nullptr;
		device->GetVertexShader(&state.vertexShader);      // addrefs
		state.pixelShader = nullptr;
		device->GetPixelShader(&state.pixelShader);        // addrefs
	}

	void RestoreState(LPDIRECT3DDEVICE9 device, const SavedState& state)
	{
		device->SetFVF(state.fvf);
		device->SetRenderState(D3DRS_ZENABLE, state.zEnable);
		device->SetRenderState(D3DRS_ZWRITEENABLE, state.zWrite);
		device->SetRenderState(D3DRS_ALPHABLENDENABLE, state.alphaBlend);
		device->SetRenderState(D3DRS_SRCBLEND, state.srcBlend);
		device->SetRenderState(D3DRS_DESTBLEND, state.destBlend);
		device->SetRenderState(D3DRS_CULLMODE, state.cullMode);
		device->SetRenderState(D3DRS_LIGHTING, state.lighting);
		device->SetRenderState(D3DRS_FOGENABLE, state.fog);
		device->SetRenderState(D3DRS_STENCILENABLE, state.stencil);
		device->SetRenderState(D3DRS_SCISSORTESTENABLE, state.scissor);
		device->SetRenderState(D3DRS_COLORWRITEENABLE, state.colorWrite);
		device->SetRenderState(D3DRS_CLIPPING, state.clipping);
		device->SetRenderState(D3DRS_TEXTUREFACTOR, state.textureFactor);
		device->SetTextureStageState(0, D3DTSS_COLOROP, state.tssColorOp);
		device->SetTextureStageState(0, D3DTSS_COLORARG1, state.tssColorArg1);
		device->SetTextureStageState(0, D3DTSS_COLORARG2, state.tssColorArg2);
		device->SetTextureStageState(0, D3DTSS_ALPHAOP, state.tssAlphaOp);
		device->SetTextureStageState(0, D3DTSS_ALPHAARG1, state.tssAlphaArg1);
		device->SetTextureStageState(1, D3DTSS_COLOROP, state.tss1ColorOp);

		device->SetTexture(0, state.texture0);
		if (state.texture0) state.texture0->Release();
		device->SetVertexShader(state.vertexShader);
		if (state.vertexShader) state.vertexShader->Release();
		device->SetPixelShader(state.pixelShader);
		if (state.pixelShader) state.pixelShader->Release();
	}

	struct OverlayVertex
	{
		float x, y, z, rhw;
		float u, v;
	};

	void DrawPanelQuad(LPDIRECT3DDEVICE9 device)
	{
		float left = (float)g_panelX;
		float top = (float)g_panelY;
		float right = left + (float)kPanelW;
		float bottom = top + (float)kPanelH;

		OverlayVertex vertices[4] =
		{
			{ left - 0.5f,  top - 0.5f,    0.0f, 1.0f, 0.0f, 0.0f },
			{ right - 0.5f, top - 0.5f,    0.0f, 1.0f, 1.0f, 0.0f },
			{ left - 0.5f,  bottom - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
			{ right - 0.5f, bottom - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
		};

		device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(OverlayVertex));
	}

	// -----------------------------------------------------------------------
	// Cursor mapping
	// -----------------------------------------------------------------------
	bool CursorToOverlay(POINT* out)
	{
		if (!out || !g_renderWindow) return false;

		POINT cursor;
		if (!GetCursorPos(&cursor)) return false;
		if (!ScreenToClient(g_renderWindow, &cursor)) return false;

		RECT client;
		if (!GetClientRect(g_renderWindow, &client)) return false;

		int clientW = client.right - client.left;
		int clientH = client.bottom - client.top;
		if (clientW <= 0 || clientH <= 0) return false;
		if (g_backbufferW <= 0 || g_backbufferH <= 0) return false;

		out->x = (LONG)((float)cursor.x * (float)g_backbufferW / (float)clientW);
		out->y = (LONG)((float)cursor.y * (float)g_backbufferH / (float)clientH);
		return true;
	}

	bool PointInPanel(const POINT& point)
	{
		return point.x >= g_panelX && point.x < g_panelX + kPanelW
			&& point.y >= g_panelY && point.y < g_panelY + kPanelH;
	}

	void ClampPanel()
	{
		int maxX = (g_backbufferW > 0 ? g_backbufferW : 1280) - 60;
		int maxY = (g_backbufferH > 0 ? g_backbufferH : 720) - 30;
		if (g_panelX < -kPanelW + 60) g_panelX = -kPanelW + 60;
		if (g_panelY < 0) g_panelY = 0;
		if (g_panelX > maxX) g_panelX = maxX;
		if (g_panelY > maxY) g_panelY = maxY;
	}

	void ClampScroll(const Snapshot& snapshot)
	{
		long maxScroll = snapshot.count - kVisibleRows;
		if (maxScroll < 0) maxScroll = 0;
		if (g_scrollFromEnd > maxScroll) g_scrollFromEnd = maxScroll;
		if (g_scrollFromEnd < 0) g_scrollFromEnd = 0;
	}

	void SelectRowAt(const POINT& point, const Snapshot& snapshot)
	{
		int localY = point.y - g_panelY - kListTop;
		if (localY < 0 || localY >= kListHeight) return;

		int row = localY / kRowH;
		long newest = (long)snapshot.total - 1;
		long first = newest - g_scrollFromEnd - (kVisibleRows - 1);
		long seq = first + row;
		if (seq < 0) return;

		long index = IndexOfSeq(snapshot, (uint32_t)seq);
		if (index < 0) return;

		g_selectedSeq = snapshot.ring[index].seq;
		g_hasSelection = true;
		g_dirty = true;
	}
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SetRenderWindow(HWND windowHandle)
{
	g_renderWindow = windowHandle;
}

bool IsVisible()
{
	return g_visible;
}

void SetVisible(bool visible)
{
	g_visible = visible;
	g_dirty = true;
}

void OnLostDevice()
{
	// D3DPOOL_MANAGED resources survive a reset on their own. Nothing to do
	// unless the pool is changed; kept so the reset path stays explicit.
}

void OnResetDevice()
{
	g_dirty = true;
}

void OnEndScene(LPDIRECT3DDEVICE9 device)
{
	if (!device) return;

	// First frame: honour the "overlay" setting from packet_log.ini.
	if (!g_visibleInitialised)
	{
		g_visibleInitialised = true;
		g_visible = PacketCapture::OverlayEnabledAtStartup();
		g_dirty = true;
	}

	if (!g_visible) return;

	// Lazily build the panel resources, retrying each frame until they exist
	// (the device can still be coming up, or be mid-reset).
	if (!g_res.texture)
	{
		if (g_startTick == 0) g_startTick = GetTickCount();
		if (!CreateResources(device))
		{
			if (!g_resourceErrorLogged)
			{
				g_resourceErrorLogged = true;
				HookLog("[Overlay] resource creation failed - retrying every frame");
			}
			return;
		}
		g_resourceErrorLogged = false;
	}

	// Detect new traffic so the panel is only redrawn when it changed.
	Snapshot snapshot = BeginRead();
	if (snapshot.total != g_lastTotal)
	{
		g_lastTotal = snapshot.total;
		g_dirty = true;
	}
	ClampScroll(snapshot);
	EndRead();

	if (g_dirty)
	{
		RedrawPanel();
		if (!UploadPanel()) return;
		g_dirty = false;
	}

	SavedState saved;
	SaveState(device, saved);

	device->SetVertexShader(NULL);
	device->SetPixelShader(NULL);
	device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

	device->SetRenderState(D3DRS_ZENABLE, FALSE);
	device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_LIGHTING, FALSE);
	device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	device->SetRenderState(D3DRS_CLIPPING, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

	device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	device->SetTexture(0, g_res.texture);
	DrawPanelQuad(device);

	RestoreState(device, saved);
}

bool OnWindowMessage(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		switch (wParam)
		{
		case VK_F8:
			g_visible = !g_visible;
			g_dirty = true;
			return true;
		case VK_F9:
			SetPaused(!IsPaused());
			g_dirty = true;
			return true;
		case VK_F10:
			Clear();
			g_hasSelection = false;
			g_scrollFromEnd = 0;
			g_dirty = true;
			return true;
		case VK_ESCAPE:
			if (g_visible)
			{
				g_visible = false;
				g_dirty = true;
				return true;
			}
			break;
		default:
			break;
		}
		break;

	case WM_MOUSEWHEEL:
		if (g_visible)
		{
			int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			g_scrollFromEnd += (delta > 0) ? 3 : -3;
			if (g_scrollFromEnd < 0) g_scrollFromEnd = 0;
			g_dirty = true;
			return true;
		}
		break;

	case WM_MBUTTONDOWN:
		if (g_visible)
		{
			g_visible = false;
			g_dirty = true;
			return true;
		}
		break;

	case WM_LBUTTONDOWN:
		if (g_visible)
		{
			POINT point;
			if (CursorToOverlay(&point) && PointInPanel(point))
			{
				if (point.y < g_panelY + kTitleH)
				{
					g_dragging = true;
					g_dragCursor.x = point.x;
					g_dragCursor.y = point.y;
				}
				else if (point.y >= g_panelY + kListTop && point.y < g_panelY + kListTop + kListHeight)
				{
					Snapshot snapshot = BeginRead();
					ClampScroll(snapshot);
					SelectRowAt(point, snapshot);
					EndRead();
				}
				return true;
			}
		}
		break;

	case WM_LBUTTONUP:
		if (g_dragging)
		{
			g_dragging = false;
			return true;
		}
		break;

	case WM_MOUSEMOVE:
		if (g_dragging)
		{
			POINT point;
			if (CursorToOverlay(&point))
			{
				g_panelX += point.x - g_dragCursor.x;
				g_panelY += point.y - g_dragCursor.y;
				g_dragCursor = point;
				ClampPanel();
			}
			return true;
		}
		break;

	default:
		break;
	}

	return false;
}

} // namespace PacketOverlay
