#pragma once

struct PaletteScene {
	bool valid = false;
	bool training = false;
	unsigned char* objMgr = nullptr;
	unsigned char* gameInfo = nullptr;
	unsigned char* fileBuffer = nullptr;
	unsigned char* opSizes = nullptr;
};

extern PaletteScene g_paletteScene;
extern bool g_paletteEditorVisible;

struct IDirect3DDevice9;
void Palette_SetDevice(IDirect3DDevice9* device);
void Palette_Init();
void Palette_Tick();
void Palette_DrawEditor();
void Palette_DrawOnlineSection();

void Palette_SetOnline(bool online, int localSide, unsigned long long peer);
unsigned long long Palette_LocalStamp();
int Palette_BuildPacket(unsigned char* out, int cap);
void Palette_Receive(const unsigned char* msg, int len, unsigned long long from);
void Palette_SetPeerName(unsigned long long id, const char* name);
void Palette_SetPackFunctions(void* entry, void* name);
const unsigned char* __fastcall Palette_CssEntry(void* pack, void* unused, unsigned index);
