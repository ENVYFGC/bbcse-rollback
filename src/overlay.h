#pragma once

struct IDirect3DDevice9;

struct OverlayStats {
	static const int kHistory = 120;
	bool session = false, vsyncOff = false;
	char status[48] = "";
	int side = 0, delay = 0, ping = 0, frame = 0;
	float updatesPerSec = 0;
	int rollbackFramesPerSec = 0, rollbackMaxDepth = 0;
	float aheadFrames = 0, pacedMsPerSec = 0;
	int heldLimit = 0, heldPause = 0, heldSync = 0, heldConnect = 0;
	float tickAvgMs = 0, tickMaxMs = 0, saveMs = 0, loadMs = 0, replayMs = 0, inputDelay = 0;
	int desyncFrame = -1, syncChecks = 0;
	char version[16] = "cse", peerVersion[16] = "";
	bool versionMismatch = false;
	float rollbackHistory[kHistory] = {}, tickMsHistory[kHistory] = {};
	int historyPos = 0;
};
extern OverlayStats g_ovl;

void Overlay_OnPresent(IDirect3DDevice9* dev);

#include <string>
#include <vector>
void Version_List(std::vector<std::string>& out);
const char* Version_Current();
bool Version_Locked();
bool Version_InFight();
const char* Version_ForcedReason();
std::string Version_LoadedCodes();
void Version_Select(const char* name);
void Overlay_BeforeReset();
void Overlay_AfterReset();
