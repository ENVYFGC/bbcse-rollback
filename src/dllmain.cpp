// BBCSE rollback: dinput8.dll / winmm.dll proxy for BlazBlue: Continuum Shift Extend (Steam).
// GGPO rollback over the game's Steam P2P connection, frame pacing, display options and the F2 menu.

#include <winsock2.h>
#include <windows.h>
#include <intrin.h>
#include <dinput.h>
#include <d3d9.h>

#include "ggponet.h"
#include "overlay.h"
#include "framedata.h"
#include "hitboxes.h"
#include "palette.h"

#include <share.h>
#include <io.h>

#include <cstdio>
#include <algorithm>
#include <map>
#include <set>
#include <array>
#include <string>
#include <vector>

extern "C" int (__cdecl* ggpo_bb_send)(const char* buf, int len);
extern "C" int (__cdecl* ggpo_bb_recv)(char* buf, int cap);
extern "C" sockaddr_in ggpo_bb_peer_addr;

#ifdef BBCSE_PROXY_WINMM
#include <delayimp.h>
#include "proxy_winmm.inc"
HMODULE g_realWinmm = nullptr;
bool LoadRealWinmm(HMODULE self) {
	char sys[MAX_PATH]{};
	GetSystemDirectoryA(sys, MAX_PATH);
	const std::string path = std::string(sys) + "\\winmm.dll";
	HMODULE m = LoadLibraryA(path.c_str());
	if (!m || m == self) return false;
	g_realWinmm = m;
	for (int i = 0; i < kWinmmExports; ++i)
		g_winmm[i] = g_winmmName[i] ? GetProcAddress(m, g_winmmName[i])
		                            : GetProcAddress(m, MAKEINTRESOURCEA(g_winmmOrd[i]));
	return true;
}
FARPROC WINAPI WinmmDelayHook(unsigned note, PDelayLoadInfo info) {
	if (note == dliNotePreLoadLibrary && info && _stricmp(info->szDll, "winmm.dll") == 0 && g_realWinmm)
		return reinterpret_cast<FARPROC>(g_realWinmm);
	return nullptr;
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = WinmmDelayHook;
#endif

namespace {
using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

HMODULE g_realDinput = nullptr;
HMODULE g_self = nullptr;
DirectInput8Create_t g_realCreate = nullptr;
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
	if (!g_log) {
		return;
	}
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

	va_list args;
	va_start(args, fmt);
	vfprintf(g_log, fmt, args);
	va_end(args);

	fputc('\n', g_log);
	fflush(g_log);
}

std::string ModuleDir(HMODULE m) {
	char buf[MAX_PATH]{};
	GetModuleFileNameA(m, buf, MAX_PATH);
	std::string s(buf);
	const size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
}

bool LoadRealDinput() {
	if (g_realCreate) {
		return true;
	}

	char sys[MAX_PATH]{};
	GetSystemDirectoryA(sys, MAX_PATH);
	std::string path = std::string(sys) + "\\dinput8.dll";

	g_realDinput = LoadLibraryA(path.c_str());
	if (!g_realDinput) {
		Log("FATAL: could not load %s (err %lu)", path.c_str(), GetLastError());
		return false;
	}

	g_realCreate = reinterpret_cast<DirectInput8Create_t>(
		GetProcAddress(g_realDinput, "DirectInput8Create"));
	if (!g_realCreate) {
		Log("FATAL: no DirectInput8Create in real dinput8");
		return false;
	}

	Log("real dinput8 loaded at %p, DirectInput8Create at %p", g_realDinput, g_realCreate);
	return true;
}

#ifdef BBCSE_DEV_TOOLS
void ReportProcess() {
	char exe[MAX_PATH]{};
	GetModuleFileNameA(nullptr, exe, MAX_PATH);

	const HMODULE base = GetModuleHandleA(nullptr);
	const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(
		reinterpret_cast<BYTE*>(base) + dos->e_lfanew);

	Log("host   : %s", exe);
	Log("base   : %p  (preferred 0x%08X)", base, nt->OptionalHeader.ImageBase);
	Log("entry  : 0x%08X", nt->OptionalHeader.AddressOfEntryPoint);
	Log("sections:");

	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
		char nm[9]{};
		memcpy(nm, sec->Name, 8);
		Log("   %-8s va=%p size=0x%08X raw=0x%08X",
			nm,
			reinterpret_cast<BYTE*>(base) + sec->VirtualAddress,
			sec->Misc.VirtualSize,
			sec->SizeOfRawData);
	}
}
#endif

constexpr DWORD kImage = 0x400000;
constexpr DWORD kUpdateBattle = 0x497E30;
constexpr DWORD kCallSites[] = {0x49DC71, 0x49DD0E, 0x49DD2E};
constexpr DWORD kP1Ptr = 0x7C6C28;

using UpdateBattle_t = void(__fastcall*)(void* self, void* edx, int arg);
UpdateBattle_t g_origUpdate = nullptr;
BYTE* g_base = nullptr;

LONG g_calls = 0, g_ticks = 0, g_skipped = 0, g_doubled = 0;
DWORD g_lastReport = 0;
int g_mode = 0;
bool g_keyDown[10]{};

std::string CaptureCtrls();
void RestoreCtrls(const std::string& s);
extern std::string g_ctrlSnap;
extern int g_ctrlCount;

BYTE* Live(DWORD va) { return g_base + (va - kImage); }

int P1FrameCounter() {
	auto p1 = *reinterpret_cast<BYTE**>(g_base + kP1Ptr);
	return p1 ? *reinterpret_cast<int*>(p1 + 4) : -1;
}

bool Pressed(int vk, int slot) {
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000);
	const bool edge = down && !g_keyDown[slot];
	g_keyDown[slot] = down;
	return edge;
}

constexpr DWORD kIsBattlePaused = 0x488390;
constexpr DWORD kIsPausedSite = 0x498032;
using IsBattlePaused_t = int(__cdecl*)();
IsBattlePaused_t g_origIsPaused = nullptr;
bool g_holdSim = false;

constexpr int kPauseRing = 8;
int g_framePaused[kPauseRing];
LONG g_framePausedTag[kPauseRing] = {-1, -1, -1, -1, -1, -1, -1, -1};
extern LONG g_resimFrame;
extern LONG g_realFrame;
extern bool g_netActive;
extern DWORD g_netIn[2];
extern bool g_netInUpdate;
int NetSideOf(const int* ctrl);
void NetLatencyPoll(int side);

int __cdecl HookIsBattlePaused() {
	if (g_netActive) return g_holdSim ? 1 : 0;
	if (g_resimFrame >= 0) {
		const int slot = g_resimFrame % kPauseRing;
		if (g_framePausedTag[slot] == g_resimFrame) return g_framePaused[slot];
	}
	const int real = g_origIsPaused();
	int result = real;
	if (g_holdSim && real == 0) { ++g_skipped; result = 1; }
	if (g_realFrame >= 0) {
		const int slot = g_realFrame % kPauseRing;
		g_framePaused[slot] = result;
		g_framePausedTag[slot] = g_realFrame;
	}
	return result;
}

constexpr int kRegions = 16;
constexpr DWORD kPoolPtrOff = 0x62650;
constexpr DWORD kPoolSize = 400 * 0x20E8;
constexpr DWORD kListOffs[3] = {0x62644, 0x62648, 0x6264C};
constexpr DWORD kScrollList = 0xB7B6C4;
constexpr DWORD kCameraZoom = 0xB7C40C;
constexpr DWORD kRenderView = 0xB8C9D0;
constexpr DWORD kRenderViewSize = 0x220;
constexpr DWORD kEtcMgr = 0xEEE2D8;
constexpr DWORD kEtcMgrSize = 0x243240;
constexpr DWORD kObjMgr = 0xDF2C00;
constexpr DWORD kGameModeVa = 0xBC12D8 + 8;
constexpr int kModeTraining = 10;
constexpr DWORD kObjMgrSize = 0x8375C;
constexpr DWORD kCharsPtrOff = 0x62654;
constexpr DWORD kCharsSize = 2 * 0x20140;
constexpr DWORD kRngArrayPtr = 0xB7C394;
constexpr DWORD kRngSize = 0x9CC;
constexpr DWORD kCamera = 0xE78400;
constexpr DWORD kCameraSize = 0x13C;
constexpr DWORD kClocks[] = {0xBC12D8, 0xCE3DB0, 0xCE3E9C};

struct Region { const char* name; BYTE* addr; DWORD size; };
struct Snapshot { Region regions[kRegions]; std::string data[kRegions]; bool valid = false; };
Snapshot g_snap;

__declspec(thread) int t_probing = 0;
struct ProbeScope { ProbeScope() { ++t_probing; } ~ProbeScope() { --t_probing; } };
constexpr DWORD kScrollBytes = 0x11C;

constexpr DWORD kListBytes = 0x648;

DWORD Fnv(const BYTE* p, DWORD n) {
	DWORD h = 2166136261u;
	for (DWORD i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
	return h;
}

bool CurrentRegions(Region out[kRegions]) {
	BYTE* mgr = Live(kObjMgr);
	BYTE* chars = *reinterpret_cast<BYTE**>(mgr + kCharsPtrOff);
	BYTE* rng = *reinterpret_cast<BYTE**>(Live(kRngArrayPtr));
	if (!chars || !rng) return false;
	const int rngCount = *reinterpret_cast<int*>(rng - 4);
	if (rngCount <= 0 || rngCount > 64) return false;
	out[0] = {"objmgr", mgr, kObjMgrSize};
	out[1] = {"chars", chars, kCharsSize};
	out[2] = {"rng0", rng, kRngSize};
	out[15] = {"rng", rng + kRngSize, static_cast<DWORD>(rngCount - 1) * kRngSize};
	out[3] = {"camera", Live(kCamera), kCameraSize};
	for (int i = 0; i < 3; ++i) out[4 + i] = {"clock", Live(kClocks[i]), 0};
	out[7] = {"zoom", Live(kCameraZoom), 0};
	out[8] = {"rview", Live(kRenderView), 0};
	out[9] = {"etcmgr", Live(kEtcMgr), kEtcMgrSize};
	BYTE* const* list = reinterpret_cast<BYTE* const*>(Live(kScrollList));
	const DWORD count = *reinterpret_cast<const DWORD*>(Live(kScrollList) + 8);
	BYTE* scroll = (list[0] && count >= 1) ? *reinterpret_cast<BYTE* const*>(list[0]) : nullptr;
	if (!scroll) return false;
	out[10] = {"scroll", scroll, kScrollBytes};
	BYTE* pool = *reinterpret_cast<BYTE**>(mgr + kPoolPtrOff);
	if (!pool) return false;
	out[11] = {"objpool", pool, kPoolSize};
	static const char* const kListNames[3] = {"list44", "list48", "list4c"};
	for (int i = 0; i < 3; ++i) {
		BYTE* list = *reinterpret_cast<BYTE**>(mgr + kListOffs[i]);
		if (!list) return false;
		out[12 + i] = {kListNames[i], list, kListBytes};
	}
	return true;
}

#ifdef BBCSE_DEV_TOOLS
constexpr DWORD kDataRva = 0x636000, kDataSize = 0xD63118;
constexpr DWORD kMaxHeapScan = 64u << 20;

struct ScanArea { const char* name; BYTE* addr; DWORD size; std::string copy; };
ScanArea g_scan[2];

void AllocationOf(BYTE* p, BYTE*& start, DWORD& size) {
	MEMORY_BASIC_INFORMATION mbi{};
	VirtualQuery(p, &mbi, sizeof(mbi));
	start = static_cast<BYTE*>(mbi.AllocationBase);
	BYTE* cur = start;
	constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
	                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	while (VirtualQuery(cur, &mbi, sizeof(mbi)) && mbi.AllocationBase == start &&
	       static_cast<DWORD>(cur - start) < kMaxHeapScan) {
		if (mbi.State != MEM_COMMIT || !(mbi.Protect & kReadable) || (mbi.Protect & PAGE_GUARD)) break;
		cur += mbi.RegionSize;
	}
	if (cur == start) {
		size = 0;
		return;
	}
	size = static_cast<DWORD>(cur - start);
}

void ScanSave(const Region r[kRegions]) {
	g_scan[0] = {"data", g_base + kDataRva, kDataSize, {}};
	BYTE* hs; DWORD hn;
	AllocationOf(r[1].addr, hs, hn);
	g_scan[1] = {"heap", hs, hn, {}};
	for (auto& a : g_scan) {
		a.copy.assign(reinterpret_cast<char*>(a.addr), a.size);
		Log("  scan %-4s %p size 0x%X", a.name, a.addr, a.size);
	}
}

void ScanDiff() {
	const Region* r = g_snap.regions;
	auto restored = [&](BYTE* p) {
		for (int i = 0; i < kRegions; ++i)
			if (p >= r[i].addr && p < r[i].addr + r[i].size) return true;
		return false;
	};
	BYTE* p1 = *reinterpret_cast<BYTE**>(g_base + kP1Ptr);
	for (auto& a : g_scan) {
		if (a.copy.empty()) continue;
		const BYTE* old = reinterpret_cast<const BYTE*>(a.copy.data());
		DWORD runs = 0, bytes = 0, runStart = 0, runEnd = 0;
		bool open = false;
		auto flush = [&] {
			if (!open) return;
			++runs; bytes += runEnd - runStart;
			if (runs <= 80) {
				BYTE* at = a.addr + runStart;
				if (a.name[0] == 'd')
					Log("  DIFF data  va %08X len %4lu  old %08X now %08X", static_cast<DWORD>(at - g_base) + kImage,
						runEnd - runStart, *reinterpret_cast<const DWORD*>(old + runStart), *reinterpret_cast<DWORD*>(at));
				else
					Log("  DIFF heap  %p (P1%+ld) len %4lu  old %08X now %08X", at, static_cast<long>(at - p1),
						runEnd - runStart, *reinterpret_cast<const DWORD*>(old + runStart), *reinterpret_cast<DWORD*>(at));
			}
			open = false;
		};
		for (DWORD off = 0; off + 4 <= a.size; off += 4) {
			BYTE* at = a.addr + off;
			const bool diff = !restored(at) && memcmp(at, old + off, 4) != 0;
			if (diff) {
				if (open && off - runEnd <= 16) runEnd = off + 4;
				else { flush(); open = true; runStart = off; runEnd = off + 4; }
			}
		}
		flush();
		Log("SCAN %s: %lu changed runs, %lu bytes, outside restored regions", a.name, runs, bytes);
	}
}

constexpr DWORD kBgMgr = 0xDECB70;
constexpr DWORD kStageProbe = 0x4000;
struct StageCopy { BYTE* obj = nullptr; std::string copy; };
StageCopy g_stage[2];
#endif

bool ReadableRange(const BYTE* p, DWORD n) {
	MEMORY_BASIC_INFORMATION mbi{};
	const BYTE* end = p + n;
	while (p < end) {
		if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
		    !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
		                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
		    (mbi.Protect & PAGE_GUARD))
			return false;
		p = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
	}
	return true;
}

#ifdef BBCSE_DEV_TOOLS
void ReportHeapBlock(const char* what, const void* p) {
	Log("  %s %p", what, p);
}

const char* ClassOf(const BYTE* obj) {
	static char name[64];
	__try {
		const BYTE* vt = *reinterpret_cast<BYTE* const*>(obj);
		const BYTE* col = *reinterpret_cast<BYTE* const*>(vt - 4);
		const char* td = *reinterpret_cast<const char* const*>(col + 12) + 8;
		strncpy_s(name, td, _TRUNCATE);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		strcpy_s(name, "?");
	}
	return name;
}

void StageSave() {
	BYTE* mgr = Live(kBgMgr);
	BYTE* objs[2] = {*reinterpret_cast<BYTE**>(mgr + 0x08), *reinterpret_cast<BYTE**>(mgr + 0x0C)};
	for (int i = 0; i < 2; ++i) {
		g_stage[i] = {};
		if (!objs[i] || !ReadableRange(objs[i], kStageProbe)) { Log("  stage[%d] %p not readable", i, objs[i]); continue; }
		g_stage[i].obj = objs[i];
		g_stage[i].copy.assign(reinterpret_cast<char*>(objs[i]), kStageProbe);
		Log("  stage[%d] %p class %s", i, objs[i], ClassOf(objs[i]));
		ReportHeapBlock("stage", objs[i]);
	}
}

void StageDiff() {
	for (int i = 0; i < 2; ++i) {
		const StageCopy& s = g_stage[i];
		if (!s.obj || !ReadableRange(s.obj, kStageProbe)) continue;
		const BYTE* old = reinterpret_cast<const BYTE*>(s.copy.data());
		unsigned runs = 0;
		for (DWORD off = 0; off < kStageProbe; off += 4) {
			if (memcmp(s.obj + off, old + off, 4) == 0) continue;
			DWORD end = off + 4;
			while (end < kStageProbe && memcmp(s.obj + end, old + end, 4) != 0) end += 4;
			if (++runs <= 40)
				Log("  STAGE[%d] +0x%04lX len %3lu old %08X (%g) now %08X (%g)", i, off, end - off,
					*reinterpret_cast<const DWORD*>(old + off), *reinterpret_cast<const float*>(old + off),
					*reinterpret_cast<const DWORD*>(s.obj + off), *reinterpret_cast<const float*>(s.obj + off));
			off = end - 4;
		}
		Log("STAGE[%d] %s: %u changed runs in first 0x%X bytes", i, ClassOf(s.obj), runs, kStageProbe);
	}
}
#endif

constexpr DWORD kParticleReg = 0xB7B7E8, kParticleRegSize = 0x54;
struct Pool { DWORD listOff, slotSize, count; };
constexpr Pool kPools[3] = {{0x04, 0x260, 5000}, {0x10, 0x84, 1024}, {0x1C, 0xE8, 512}};
constexpr DWORD kFxArraySize = 64 * 0x50;

std::vector<BYTE*> g_poolNodes[3];
BYTE* g_fxArray = nullptr;
bool g_poolsKnown = false;

struct ParticleSnap { BYTE hdr[kParticleRegSize]; std::string data; bool valid = false; };

bool WalkPool(BYTE* head, DWORD expect, std::vector<BYTE*>& out) {
	out.clear();
	for (BYTE* n = head; n && out.size() <= expect; n = *reinterpret_cast<BYTE**>(n + 4)) out.push_back(n);
	std::sort(out.begin(), out.end());
	return out.size() == expect && std::adjacent_find(out.begin(), out.end()) == out.end();
}

constexpr DWORD kFxSetup = 0x47C540, kFxSetupSite = 0x497B2E;
using FxSetup_t = int(__cdecl*)();
FxSetup_t g_origFxSetup = nullptr;
extern bool g_ringReset;
int g_autoCountdown = -1;
#ifdef BBCSE_DEV_TOOLS
constexpr const char* kRecFile = "bbcse-dev/recording.bin";
constexpr const char* kAutoFlag = "bbcse-dev/autotest.on";
constexpr const char* kDumpDir = "bbcse-dev/dumps";
#endif

void NetBattleStart();

std::string g_particlePristine;
constexpr DWORD kParticleNodeHead = 0x10, kParticleNodeSize = 0x260;

void CapturePoolsAfterSetup() {
	BYTE* reg = Live(kParticleReg);
	std::vector<BYTE*> found[3];
	for (int i = 0; i < 3; ++i)
		if (!WalkPool(*reinterpret_cast<BYTE**>(reg + kPools[i].listOff + 4), kPools[i].count, found[i])) {
			Log("effect pools NOT captured: pool %d walked %u slots, expected %lu", i,
				static_cast<unsigned>(found[i].size()), kPools[i].count);
			g_poolsKnown = false;
			return;
		}
	BYTE* arr = *reinterpret_cast<BYTE**>(reg + 0x34);
	if (!arr || *reinterpret_cast<DWORD*>(reg + 0x38) != 64) { g_poolsKnown = false; return; }
	for (int i = 0; i < 3; ++i) g_poolNodes[i] = std::move(found[i]);
	g_fxArray = arr;
	g_poolsKnown = true;
	g_particlePristine.clear();
	g_particlePristine.reserve(g_poolNodes[0].size() * (kParticleNodeSize - kParticleNodeHead));
	for (BYTE* n : g_poolNodes[0])
		g_particlePristine.append(reinterpret_cast<const char*>(n + kParticleNodeHead), kParticleNodeSize - kParticleNodeHead);
	g_ringReset = true;
	Log("effect pools captured at battle start: 5000 particles, 1024 groups, 512 effects, array %p", arr);
	NetBattleStart();
#ifdef BBCSE_DEV_TOOLS
	if (GetFileAttributesA(kAutoFlag) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(kRecFile) != INVALID_FILE_ATTRIBUTES) {
		g_autoCountdown = 120;
		Log("AUTOTEST armed: starts in 2 s with %s", kRecFile);
	}
#endif
}

int __cdecl HookFxSetup() {
	const int r = g_origFxSetup();
	CapturePoolsAfterSetup();
	return r;
}

using ThisFn_t = void(__fastcall*)(void* self, void* edx);
using ListFn_t = void(__fastcall*)(void* list, void* edx, void* node);
constexpr DWORD kWorkStep = 0x40BB10, kWorkFinish = 0x40B970, kListUnlink = 0x4012F0, kListPush = 0x4012B0;

void EffectStep() {
	BYTE* reg = Live(kParticleReg);
	const auto step = reinterpret_cast<ThisFn_t>(Live(kWorkStep));
	const auto finish = reinterpret_cast<ThisFn_t>(Live(kWorkFinish));
	const auto unlink = reinterpret_cast<ListFn_t>(Live(kListUnlink));
	const auto push = reinterpret_cast<ListFn_t>(Live(kListPush));
	for (BYTE* w = *reinterpret_cast<BYTE**>(reg + 0x2C); w; w = *reinterpret_cast<BYTE**>(w + 4)) {
		if (*reinterpret_cast<int*>(reg + 0x48) == 0) *reinterpret_cast<int*>(w + 0x3C) = 0;
		else step(w, nullptr);
	}
	for (BYTE* w = *reinterpret_cast<BYTE**>(reg + 0x2C); w;) {
		BYTE* next = *reinterpret_cast<BYTE**>(w + 4);
		if (*reinterpret_cast<int*>(w + 0x34) == 0 || *reinterpret_cast<int*>(w + 0x38) != 0) {
			finish(w, nullptr);
			unlink(reg + 0x28, nullptr, w);
			push(reg + 0x1C, nullptr, w);
		}
		w = next;
	}
}

constexpr DWORD kNodeHead = 0x10;
std::vector<BYTE> g_nodeFree;

void CaptureParticles(ParticleSnap& p) {
	p.valid = g_poolsKnown;
	if (!p.valid) return;
	BYTE* reg = Live(kParticleReg);
	memcpy(p.hdr, reg, kParticleRegSize);
	const std::vector<BYTE*>& nodes = g_poolNodes[0];
	const DWORD body = kPools[0].slotSize - kNodeHead;
	g_nodeFree.assign(nodes.size(), 0);
	DWORD walked = 0;
	for (BYTE* n = *reinterpret_cast<BYTE**>(reg + kPools[0].listOff + 4); n && walked <= nodes.size();
	     n = *reinterpret_cast<BYTE**>(n + 4), ++walked) {
		auto it = std::lower_bound(nodes.begin(), nodes.end(), n);
		if (it == nodes.end() || *it != n) { p.valid = false; return; }
		g_nodeFree[it - nodes.begin()] = 1;
	}
	size_t used = 0;
	for (BYTE f : g_nodeFree) used += !f;
	size_t total = kFxArraySize + nodes.size() * (kNodeHead + 1) + used * body;
	for (int i = 1; i < 3; ++i) total += g_poolNodes[i].size() * kPools[i].slotSize;
	p.data.resize(total);
	char* out = &p.data[0];
	memcpy(out, g_fxArray, kFxArraySize);
	out += kFxArraySize;
	for (int i = 1; i < 3; ++i)
		for (BYTE* n : g_poolNodes[i]) { memcpy(out, n, kPools[i].slotSize); out += kPools[i].slotSize; }
	for (size_t k = 0; k < nodes.size(); ++k) {
		memcpy(out, nodes[k], kNodeHead);
		out += kNodeHead;
		*out++ = static_cast<char>(g_nodeFree[k]);
		if (!g_nodeFree[k]) { memcpy(out, nodes[k] + kNodeHead, body); out += body; }
	}
}
bool RestoreParticles(const ParticleSnap& p) {
	if (!p.valid || !g_poolsKnown) return false;
	memcpy(Live(kParticleReg), p.hdr, kParticleRegSize);
	const char* in = p.data.data();
	memcpy(g_fxArray, in, kFxArraySize);
	in += kFxArraySize;
	for (int i = 1; i < 3; ++i)
		for (BYTE* n : g_poolNodes[i]) { memcpy(n, in, kPools[i].slotSize); in += kPools[i].slotSize; }
	const DWORD body = kPools[0].slotSize - kNodeHead;
	const bool pristine = g_particlePristine.size() == g_poolNodes[0].size() * body;
	for (size_t k = 0; k < g_poolNodes[0].size(); ++k) {
		BYTE* n = g_poolNodes[0][k];
		memcpy(n, in, kNodeHead);
		in += kNodeHead;
		const bool wasFree = *in++ != 0;
		if (!wasFree) { memcpy(n + kNodeHead, in, body); in += body; }
		else if (pristine) memcpy(n + kNodeHead, g_particlePristine.data() + k * body, body);
	}
	return true;
}

ParticleSnap g_partSnap;

#ifdef BBCSE_DEV_TOOLS
void SaveState() {
	Region r[kRegions];
	if (!CurrentRegions(r)) { Log("SAVE failed: battle not set up"); return; }
	StageSave();
	Log("P1 ptr %p, chars region %p", *reinterpret_cast<BYTE**>(g_base + kP1Ptr), r[1].addr);
	ScanSave(r);
	DWORD total = 0;
	for (int i = 0; i < kRegions; ++i) {
		g_snap.regions[i] = r[i];
		g_snap.data[i].assign(reinterpret_cast<char*>(r[i].addr), r[i].size);
		total += r[i].size;
		Log("  save %-6s %p size 0x%06X hash %08X", r[i].name, r[i].addr, r[i].size, Fnv(r[i].addr, r[i].size));
	}
	g_snap.valid = true;
	g_ctrlSnap = CaptureCtrls();
	CaptureParticles(g_partSnap);
	Log("  save fx     %s", g_partSnap.valid ? "effect pools saved" : "NOT saved: pools not captured yet");
	Log("  save ctrl   %d controllers, %u bytes", g_ctrlCount, static_cast<unsigned>(g_ctrlSnap.size()));
	Log("SAVE ok: %lu bytes, P1 frame %d", total, P1FrameCounter());
}

void LoadState() {
	if (!g_snap.valid) { Log("LOAD skipped: nothing saved (press F8 first)"); return; }
	Region r[kRegions];
	if (!CurrentRegions(r)) { Log("LOAD failed: battle not set up"); return; }
	for (int i = 0; i < kRegions; ++i) {
		if (r[i].addr != g_snap.regions[i].addr || r[i].size != g_snap.regions[i].size) {
			Log("LOAD refused: %s moved (%p/%X -> %p/%X). New round or new match?", r[i].name,
				g_snap.regions[i].addr, g_snap.regions[i].size, r[i].addr, r[i].size);
			return;
		}
	}
	for (int i = 0; i < kRegions; ++i) {
		memcpy(r[i].addr, g_snap.data[i].data(), r[i].size);
		const bool same = memcmp(r[i].addr, g_snap.data[i].data(), r[i].size) == 0;
		Log("  load %-6s hash %08X %s", r[i].name, Fnv(r[i].addr, r[i].size), same ? "byte-exact" : "MISMATCH");
	}
	RestoreCtrls(g_ctrlSnap);
	if (!RestoreParticles(g_partSnap)) Log("  load fx     skipped: effect pools not captured at save time");
	Log("LOAD ok: P1 frame %d", P1FrameCounter());
	ScanDiff();
	StageDiff();
}

constexpr DWORD kBattleKeyVtbl = 0x960604;
constexpr int kFirstSlot = 2, kSlotCount = 6;
constexpr DWORD kSlotOrig[kSlotCount] = {0x43BF40, 0x43B890, 0x43C030, 0x43F690, 0x43F720, 0x43BEE0};

struct CallSite { int slot; DWORD self, ret, lastArg; LONG count; };
CallSite g_sites[96];
LONG g_siteCount = 0, g_siteOverflow = 0;
DWORD g_slotTarget[kSlotCount];

extern "C" void __cdecl RecordCall(int slot, DWORD self, DWORD ret, DWORD arg0) {
	for (LONG i = 0; i < g_siteCount; ++i) {
		CallSite& s = g_sites[i];
		if (s.slot == slot && s.self == self && s.ret == ret) { ++s.count; s.lastArg = arg0; return; }
	}
	if (g_siteCount < 96) g_sites[g_siteCount++] = {slot, self, ret, arg0, 1};
	else ++g_siteOverflow;
}

#define INPUT_THUNK(N)                                                   \
	__declspec(naked) void Thunk##N() {                                    \
		__asm pushad                                                         \
		__asm push dword ptr [esp + 36]                                      \
		__asm push dword ptr [esp + 36]                                      \
		__asm push ecx                                                       \
		__asm push N                                                         \
		__asm call RecordCall                                                \
		__asm add esp, 16                                                    \
		__asm popad                                                          \
		__asm jmp dword ptr [g_slotTarget + 4 * N]                           \
	}
INPUT_THUNK(0) INPUT_THUNK(1) INPUT_THUNK(2) INPUT_THUNK(3) INPUT_THUNK(4) INPUT_THUNK(5)
void (*const kThunks[kSlotCount])() = {Thunk0, Thunk1, Thunk2, Thunk3, Thunk4, Thunk5};
#endif

LONG g_flowGen = 0;
DWORD g_flowTarget[2];
#define FLOW_THUNK(N)                                \
	__declspec(naked) void FlowThunk##N() {          \
		__asm lock inc dword ptr [g_flowGen]             \
		__asm jmp dword ptr [g_flowTarget + 4 * N]       \
	}
void NetBarrier();
__declspec(naked) void FlowThunk0() {
	__asm pushad
	__asm call NetBarrier
	__asm popad
	__asm lock inc dword ptr [g_flowGen]
	__asm jmp dword ptr [g_flowTarget]
}
FLOW_THUNK(1)
struct FlowHook { DWORD slot, orig; void (*thunk)(); };
const FlowHook kFlowHooks[2] = {{0x9E8EDC + 10 * 4, 0x496CA0, FlowThunk0},
                                {0x9FE3BC + 2 * 4, 0x56DB60, FlowThunk1}};

void InstallFlowHooks() {
	for (const FlowHook& h : kFlowHooks)
		if (*reinterpret_cast<DWORD*>(Live(h.slot)) != reinterpret_cast<DWORD>(Live(h.orig))) {
			Log("flow hooks ABORT: slot %08X is %08X, expected %p", h.slot, *reinterpret_cast<DWORD*>(Live(h.slot)), Live(h.orig));
			return;
		}
	for (int i = 0; i < 2; ++i) {
		DWORD* slot = reinterpret_cast<DWORD*>(Live(kFlowHooks[i].slot));
		DWORD old;
		VirtualProtect(slot, 4, PAGE_READWRITE, &old);
		g_flowTarget[i] = *slot;
		*slot = reinterpret_cast<DWORD>(kFlowHooks[i].thunk);
		VirtualProtect(slot, 4, old, &old);
	}
	Log("round-flow hooks installed (scene round setup, pause menu)");
}

#ifdef BBCSE_DEV_TOOLS
void InstallInputProbe() {
	DWORD* vt = reinterpret_cast<DWORD*>(Live(kBattleKeyVtbl)) + kFirstSlot;
	for (int i = 0; i < kSlotCount; ++i) {
		if (vt[i] != reinterpret_cast<DWORD>(Live(kSlotOrig[i]))) {
			Log("input probe ABORT: slot %d is %08X, expected %p. Nothing changed.", kFirstSlot + i, vt[i], Live(kSlotOrig[i]));
			return;
		}
	}
	DWORD old;
	VirtualProtect(vt, kSlotCount * 4, PAGE_READWRITE, &old);
	for (int i = 0; i < kSlotCount; ++i) {
		g_slotTarget[i] = vt[i];
		vt[i] = reinterpret_cast<DWORD>(kThunks[i]);
	}
	VirtualProtect(vt, kSlotCount * 4, old, &old);
	Log("input probe installed on BattleKeyControler slots %d..%d. F10 dumps.", kFirstSlot, kFirstSlot + kSlotCount - 1);
}

void DumpInputProbe() {
	Log("INPUT PROBE: %ld call sites (overflow %ld) since last dump", g_siteCount, g_siteOverflow);
	for (LONG i = 0; i < g_siteCount; ++i) {
		const CallSite& s = g_sites[i];
		Log("  slot %d  this %08X  from %08X  calls %6ld  lastArg %08X", kFirstSlot + s.slot, s.self,
			s.ret - reinterpret_cast<DWORD>(g_base) + kImage, s.count, s.lastArg);
	}
	g_siteCount = 0;
	g_siteOverflow = 0;
}
#endif

constexpr DWORD kPollFn = 0x45BB90, kPollSite = 0x45C370;
constexpr int kMaxCtrls = 8, kRingNodes = 50;
constexpr DWORD kCtrlHeader = 0x58;
using Poll_t = DWORD(__fastcall*)(int* obj);
Poll_t g_origPoll = nullptr;

int* g_ctrls[kMaxCtrls]{};
int g_ctrlCount = 0;

enum class InMode { Idle, Recording, Replaying, FileRecording, AutoReplay };
InMode g_inMode = InMode::Idle;
DWORD g_frameIdx = 0, g_endFrame = 0;
std::vector<std::array<DWORD, kMaxCtrls>> g_rec;

int CtrlIndex(int* obj) {
	for (int i = 0; i < g_ctrlCount; ++i)
		if (g_ctrls[i] == obj) return i;
	if (g_ctrlCount < kMaxCtrls) { g_ctrls[g_ctrlCount] = obj; return g_ctrlCount++; }
	return -1;
}

constexpr int kMaskRing = 8;
DWORD g_frameMasks[kMaskRing][kMaxCtrls];
LONG g_frameMaskTag[kMaskRing] = {-1, -1, -1, -1, -1, -1, -1, -1};
LONG g_resimFrame = -1;
LONG g_realFrame = -1;
LONG g_resimPolls = 0;

DWORD __fastcall HookPoll(int* obj) {
	if (g_netActive && g_netInUpdate) {
		const int side = NetSideOf(obj);
		if (side >= 0) {
			obj[0x13] = static_cast<int>(g_netIn[side]);
			obj[0x14] = 1;
			NetLatencyPoll(side);
		}
		return g_origPoll(obj);
	}
	const int ci = CtrlIndex(obj);
	if (g_resimFrame >= 0) {
		const int slot = g_resimFrame % kMaskRing;
		if (ci >= 0 && g_frameMaskTag[slot] == g_resimFrame) {
			obj[0x13] = static_cast<int>(g_frameMasks[slot][ci]);
			obj[0x14] = 1;
		}
		++g_resimPolls;
		return g_origPoll(obj);
	}
	if ((g_inMode == InMode::Replaying || g_inMode == InMode::AutoReplay) && ci >= 0 && g_frameIdx < g_rec.size()) {
		obj[0x13] = static_cast<int>(g_rec[g_frameIdx][ci]);
		obj[0x14] = 1;
	}
	const DWORD mask = g_origPoll(obj);
	if (g_realFrame >= 0 && ci >= 0) {
		const int slot = g_realFrame % kMaskRing;
		if (g_frameMaskTag[slot] != g_realFrame) {
			memset(g_frameMasks[slot], 0, sizeof(g_frameMasks[slot]));
			g_frameMaskTag[slot] = g_realFrame;
		}
		g_frameMasks[slot][ci] = mask;
	}
	if ((g_inMode == InMode::Recording || g_inMode == InMode::FileRecording) && ci >= 0) {
		if (g_rec.size() <= g_frameIdx) g_rec.resize(g_frameIdx + 1, {});
		g_rec[g_frameIdx][ci] = mask;
	}
	return mask;
}

std::string CaptureCtrls() {
	std::string out;
	for (int i = 0; i < g_ctrlCount; ++i) {
		const BYTE* obj = reinterpret_cast<const BYTE*>(g_ctrls[i]);
		out.append(reinterpret_cast<const char*>(obj), kCtrlHeader);
		const BYTE* node = *reinterpret_cast<BYTE* const*>(obj + 0x20);
		for (int n = 0; n < kRingNodes && node; ++n) {
			out.append(reinterpret_cast<const char*>(node + 0xC), 8);
			node = *reinterpret_cast<BYTE* const*>(node + 4);
		}
	}
	return out;
}

void RestoreCtrls(const std::string& s) {
	const char* p = s.data();
	for (int i = 0; i < g_ctrlCount && p < s.data() + s.size(); ++i) {
		BYTE* obj = reinterpret_cast<BYTE*>(g_ctrls[i]);
		memcpy(obj, p, kCtrlHeader);
		p += kCtrlHeader;
		BYTE* node = *reinterpret_cast<BYTE**>(obj + 0x20);
		for (int n = 0; n < kRingNodes && node; ++n) {
			memcpy(node + 0xC, p, 8);
			p += 8;
			node = *reinterpret_cast<BYTE**>(node + 4);
		}
	}
}

std::string g_ctrlSnap;

struct EndState { std::string r[kRegions]; std::string ctrl; };
EndState g_endA;
EndState g_endPrev;
int g_replayNo = 0;

bool CompareRegion(const char* name) { return strcmp(name, "rview") != 0 && strcmp(name, "rng0") != 0; }

std::string CtrlsForCompare() {
	std::string c = CaptureCtrls();
	constexpr size_t kStride = kCtrlHeader + kRingNodes * 8;
	for (size_t base = 0; base + kStride <= c.size(); base += kStride) {
		memset(&c[base + 0x4C], 0, 8);
		memset(&c[base + 0x30], 0, 4);
	}
	return c;
}

using FrameHash = std::array<DWORD, kRegions + 1>;
std::vector<FrameHash> g_hashRec;
bool g_divergenceLogged = false;

constexpr DWORD kPoolStride = 0x20E8, kPoolHandleOff = 0x1260, kPoolHandleEnd = 0x1270;
bool IsPoolHandle(size_t off) {
	const size_t o = off % kPoolStride;
	return o >= kPoolHandleOff && o < kPoolHandleEnd;
}
struct LiveField { const char* region; DWORD off; DWORD len = 4; };
constexpr LiveField kLiveFields[] = {
	{"etcmgr", 0x242A30}, {"etcmgr", 0x242A3C}, {"etcmgr", 0x242A44}, {"etcmgr", 0x242A4C},
	{"etcmgr", 0x242A54}, {"etcmgr", 0x242A5C}, {"etcmgr", 0x242A64}, {"etcmgr", 0x242A6C},
	{"etcmgr", 0x242A74}, {"etcmgr", 0x242A8C},
	{"scroll", 0x4C, 0x40}};
bool IsLiveField(const char* region, size_t off) {
	for (const LiveField& f : kLiveFields)
		if (off >= f.off && off < f.off + f.len && strcmp(region, f.region) == 0) return true;
	return false;
}

bool SkipDword(const char* region, size_t off) {
	if (IsLiveField(region, off)) return true;
	return strcmp(region, "objpool") == 0 && IsPoolHandle(off);
}

void RestoreRegion(const Region& r, const std::string& saved) {
	constexpr size_t kN = sizeof(kLiveFields) / sizeof(kLiveFields[0]);
	BYTE keep[kN][0x40];
	for (size_t i = 0; i < kN; ++i)
		if (strcmp(r.name, kLiveFields[i].region) == 0) memcpy(keep[i], r.addr + kLiveFields[i].off, kLiveFields[i].len);
	memcpy(r.addr, saved.data(), r.size);
	for (size_t i = 0; i < kN; ++i)
		if (strcmp(r.name, kLiveFields[i].region) == 0) memcpy(r.addr + kLiveFields[i].off, keep[i], kLiveFields[i].len);
}

DWORD FastHash(const void* p, size_t n) {
	const DWORD* w = static_cast<const DWORD*>(p);
	DWORD h = 2166136261u;
	for (size_t i = 0; i < n / 4; ++i) h = (h ^ w[i]) * 16777619u;
	return h;
}

DWORD HashRegion(const char* name, const BYTE* p, size_t n) {
	if (strcmp(name, "objpool") != 0) return FastHash(p, n);
	const DWORD* w = reinterpret_cast<const DWORD*>(p);
	DWORD h = 2166136261u;
	for (size_t i = 0; i < n / 4; ++i)
		if (!IsPoolHandle(i * 4)) h = (h ^ w[i]) * 16777619u;
	return h;
}

FrameHash HashFrame() {
	FrameHash fh{};
	Region r[kRegions];
	if (!CurrentRegions(r)) return fh;
	for (int i = 0; i < kRegions; ++i)
		fh[i] = CompareRegion(r[i].name) ? HashRegion(r[i].name, r[i].addr, r[i].size) : 0;
	const std::string c = CtrlsForCompare();
	fh[kRegions] = FastHash(c.data(), c.size());
	return fh;
}

constexpr int kPoolSlots = 400;
std::vector<DWORD> g_objRec;
std::string g_splitCopy;
DWORD g_splitCopyFrame = 0;
int g_splitCopyReplay = 0;

#ifdef BBCSE_DEV_TOOLS
void PoolHashes(DWORD out[kPoolSlots]) {
	Region r[kRegions];
	if (!CurrentRegions(r)) { memset(out, 0, kPoolSlots * 4); return; }
	for (int i = 0; i < kPoolSlots; ++i) out[i] = HashRegion("objpool", r[11].addr + i * kPoolStride, kPoolStride);
}

void DiffPoolAgainstCopy() {
	Region r[kRegions];
	if (!CurrentRegions(r) || g_splitCopy.size() != r[11].size) return;
	const BYTE* now = r[11].addr;
	const BYTE* old = reinterpret_cast<const BYTE*>(g_splitCopy.data());
	unsigned shown = 0, total = 0;
	for (DWORD off = 0; off + 4 <= r[11].size; off += 4) {
		if (SkipDword("objpool", off) || memcmp(now + off, old + off, 4) == 0) continue;
		++total;
		if (++shown <= 40)
			Log("  POOLDIFF slot %3lu +0x%04lX  replay#%d %08X (%g)  replay#%d %08X (%g)", off / kPoolStride,
				off % kPoolStride, g_splitCopyReplay, *reinterpret_cast<const DWORD*>(old + off),
				*reinterpret_cast<const float*>(old + off), g_replayNo, *reinterpret_cast<const DWORD*>(now + off),
				*reinterpret_cast<const float*>(now + off));
	}
	Log("POOL replay#%d vs replay#%d at frame %lu: %u differing dwords", g_replayNo, g_splitCopyReplay,
		g_splitCopyFrame, total);
}

void CheckFrame() {
	const FrameHash now = HashFrame();
	if (g_inMode == InMode::Recording) {
		if (g_hashRec.size() <= g_frameIdx) g_hashRec.resize(g_frameIdx + 1);
		g_hashRec[g_frameIdx] = now;
		if (g_objRec.size() < (g_frameIdx + 1) * kPoolSlots) g_objRec.resize((g_frameIdx + 1) * kPoolSlots);
		PoolHashes(&g_objRec[g_frameIdx * kPoolSlots]);
		return;
	}
	if (!g_splitCopy.empty() && g_splitCopyReplay == g_replayNo - 1 && g_frameIdx == g_splitCopyFrame)
		DiffPoolAgainstCopy();
	if (g_divergenceLogged || g_frameIdx >= g_hashRec.size() || now == g_hashRec[g_frameIdx]) return;
	if ((g_frameIdx + 1) * kPoolSlots <= g_objRec.size()) {
		DWORD cur[kPoolSlots];
		PoolHashes(cur);
		char slots[256] = "";
		int n = 0;
		for (int i = 0; i < kPoolSlots; ++i)
			if (cur[i] != g_objRec[g_frameIdx * kPoolSlots + i] && n++ < 12) {
				char one[16];
				sprintf_s(one, "%d ", i);
				strcat_s(slots, one);
			}
		if (n) Log("  pool slots differing from the recording: %d (%s)", n, slots);
	}
	{
		Region r[kRegions];
		if (CurrentRegions(r)) {
			g_splitCopy.assign(reinterpret_cast<char*>(r[11].addr), r[11].size);
			g_splitCopyFrame = g_frameIdx;
			g_splitCopyReplay = g_replayNo;
		}
	}
	Region r[kRegions];
	CurrentRegions(r);
	char which[256] = "";
	for (int i = 0; i <= kRegions; ++i)
		if (now[i] != g_hashRec[g_frameIdx][i]) {
			strcat_s(which, i < kRegions ? r[i].name : "ctrl");
			strcat_s(which, " ");
		}
	Log("FIRST SPLIT at replay frame %lu of %lu: %s", g_frameIdx, g_endFrame, which);
	g_divergenceLogged = true;
}

void CaptureEnd(EndState& e) {
	Region r[kRegions];
	if (!CurrentRegions(r)) return;
	for (int i = 0; i < kRegions; ++i) e.r[i].assign(reinterpret_cast<char*>(r[i].addr), r[i].size);
	e.ctrl = CtrlsForCompare();
}

void CompareEnd(const EndState& a, const EndState& b) {
	Region r[kRegions];
	CurrentRegions(r);
	unsigned totalRuns = 0;
	auto diff = [&](const char* name, const std::string& x, const std::string& y) {
		unsigned runs = 0;
		const size_t n = x.size() < y.size() ? x.size() : y.size();
		for (size_t off = 0; off + 4 <= n; off += 4) {
			if (SkipDword(name, off) || memcmp(x.data() + off, y.data() + off, 4) == 0) continue;
			size_t end = off + 4;
			while (end + 4 <= n && !SkipDword(name, end) && memcmp(x.data() + end, y.data() + end, 4) != 0) end += 4;
			if (++runs <= 12)
				Log("  DESYNC %-6s +0x%06X len %4u  run1 %08X  run2 %08X", name, static_cast<unsigned>(off),
					static_cast<unsigned>(end - off), *reinterpret_cast<const DWORD*>(x.data() + off),
					*reinterpret_cast<const DWORD*>(y.data() + off));
			off = end - 4;
		}
		if (runs) Log("  %s: %u differing runs", name, runs);
		totalRuns += runs;
	};
	for (int i = 0; i < kRegions; ++i)
		if (CompareRegion(r[i].name)) diff(r[i].name, a.r[i], b.r[i]);
	diff("ctrl", a.ctrl, b.ctrl);
	Log("  => %s: %u differing runs after %lu frames", totalRuns ? "DIFFERENT" : "IDENTICAL", totalRuns, g_endFrame);
}

void StartReplay() {
	LoadState();
	g_frameIdx = 0;
	g_divergenceLogged = false;
	g_inMode = InMode::Replaying;
	++g_replayNo;
	Log("F11: replay #%d of %lu frames for %d controllers", g_replayNo, g_endFrame, g_ctrlCount);
}

void FinishReplay() {
	EndState b;
	CaptureEnd(b);
	if (!g_divergenceLogged) Log("no split found in per-frame fingerprints");
	Log("COMPARE replay #%d vs recorded run:", g_replayNo);
	CompareEnd(g_endA, b);
	if (g_replayNo > 1) {
		Log("COMPARE replay #%d vs replay #%d:", g_replayNo, g_replayNo - 1);
		CompareEnd(g_endPrev, b);
	}
	g_endPrev = std::move(b);
	g_inMode = InMode::Idle;
}
#endif

constexpr int kRing = 8;
constexpr int kPhaseWords = 6;
struct Slot { std::string r[kRegions]; BYTE* addr[kRegions]; DWORD size[kRegions]; std::string ctrl; ParticleSnap fx;
              DWORD phase[kPhaseWords] = {}; LONG flowGen = 0; LONG frame = -1;
              bool hasScene = false; DWORD scene[3] = {};
              BYTE* uiPtr[10] = {}; DWORD ui[10][5] = {};
              DWORD rec[3] = {};
              LONG poolSeq = -1; };

void PhaseSignature(const Region r[kRegions], DWORD out[kPhaseWords]) {
	const BYTE* mgr = r[0].addr;
	const BYTE* etc = r[9].addr;
	out[0] = *reinterpret_cast<const DWORD*>(mgr + 0x0);
	out[1] = *reinterpret_cast<const DWORD*>(mgr + 0x62664);
	out[2] = *reinterpret_cast<const DWORD*>(mgr + 0x62680);
	out[3] = *reinterpret_cast<const DWORD*>(mgr + 0x62684);
	out[4] = *reinterpret_cast<const DWORD*>(etc + 0x242868);
	out[5] = *reinterpret_cast<const DWORD*>(mgr + 0x62658) & 0xFFFE0000;
}
Slot g_ring[kRing];
static_assert(kRing == kMaskRing, "mask ring must match the sync ring");
static_assert(kRing == kPauseRing, "pause ring must match the sync ring");
bool g_ringReset = false;
int g_resimArg = 0;
int g_stepLogs = 0;
LONG g_syncBarriers = 0;
LONG g_autoTests = 0, g_autoFails = 0;
int g_dumpCount = 0;

#ifdef BBCSE_DEV_TOOLS
void DumpSplit(const Slot& real, LONG frame, int step) {
	CreateDirectoryA(kDumpDir, nullptr);
	char path[MAX_PATH];
	sprintf_s(path, "%s/split_%d_frame%ld_step%d.bin", kDumpDir, g_dumpCount, frame, step);
	FILE* f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || !f) { Log("dump FAILED: %s", path); return; }
	Region r[kRegions];
	CurrentRegions(r);
	const DWORD blocks = kRegions + 1;
	fwrite("BBSD", 1, 4, f);
	fwrite(&blocks, 4, 1, f);
	auto block = [f](const char* name, DWORD addr, const std::string& a, const void* b, DWORD size) {
		char nm[16] = {};
		strncpy_s(nm, name, _TRUNCATE);
		fwrite(nm, 1, 16, f); fwrite(&addr, 4, 1, f); fwrite(&size, 4, 1, f);
		fwrite(a.data(), 1, size, f); fwrite(b, 1, size, f);
	};
	for (int i = 0; i < kRegions; ++i)
		block(r[i].name, static_cast<DWORD>(reinterpret_cast<UINT_PTR>(r[i].addr)), real.r[i], r[i].addr, r[i].size);
	const std::string ctrlNow = CaptureCtrls();
	block("ctrl", 0, real.ctrl, ctrlNow.data(), static_cast<DWORD>(real.ctrl.size() < ctrlNow.size() ? real.ctrl.size() : ctrlNow.size()));
	fclose(f);
	++g_dumpCount;
	Log("  dumped real + replayed state to %s", path);
}
#endif
int g_syncN = 0;
LONG g_syncFrame = 0;
LONG g_syncTests = 0, g_syncFails = 0, g_syncFailLogs = 0;
double g_syncMs = 0, g_syncMsMax = 0;
const int kSyncSteps[] = {0, 1, 2, 4, 7};

constexpr DWORD kSceneStateOff = 0x2C, kRoundEndFlag = 0xBCE9F0;
constexpr int kSceneFighting = 8;
void* g_sceneForSlots = nullptr;

constexpr DWORD kRecorder = 0x1395370, kRecTotalOff = 0x585E8, kRecRoundOff = 0x585EC, kRecRoundBlock = 0x8D10,
                kRecRoundCountOff = 0x300;
void SaveSceneFields(Slot& s) {
	{
		BYTE* rec = Live(kRecorder);
		s.rec[0] = *reinterpret_cast<DWORD*>(rec + kRecTotalOff);
		s.rec[1] = *reinterpret_cast<DWORD*>(rec + kRecRoundOff);
		s.rec[2] = s.rec[1] < 8 ? *reinterpret_cast<DWORD*>(rec + s.rec[1] * kRecRoundBlock + kRecRoundCountOff) : 0;
	}
	s.hasScene = g_sceneForSlots != nullptr;
	if (!s.hasScene) return;
	s.scene[0] = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(g_sceneForSlots) + kSceneStateOff);
	s.scene[1] = *reinterpret_cast<DWORD*>(Live(kRoundEndFlag));
	s.scene[2] = *reinterpret_cast<DWORD*>(Live(0xBC12D8) + 4) & 2;
}
void RestoreSceneFields(const Slot& s) {
	{
		BYTE* rec = Live(kRecorder);
		*reinterpret_cast<DWORD*>(rec + kRecTotalOff) = s.rec[0];
		*reinterpret_cast<DWORD*>(rec + kRecRoundOff) = s.rec[1];
		if (s.rec[1] < 8) *reinterpret_cast<DWORD*>(rec + s.rec[1] * kRecRoundBlock + kRecRoundCountOff) = s.rec[2];
	}
	if (!s.hasScene || !g_sceneForSlots) return;
	*reinterpret_cast<DWORD*>(static_cast<BYTE*>(g_sceneForSlots) + kSceneStateOff) = s.scene[0];
	*reinterpret_cast<DWORD*>(Live(kRoundEndFlag)) = s.scene[1];
	DWORD& gf = *reinterpret_cast<DWORD*>(Live(0xBC12D8) + 4);
	gf = (gf & ~2u) | s.scene[2];
}
bool SceneFighting() {
	return !g_sceneForSlots ||
	       *reinterpret_cast<int*>(static_cast<BYTE*>(g_sceneForSlots) + kSceneStateOff) == kSceneFighting;
}

constexpr DWORD kUiObjList = 0xBCE9C8;
constexpr DWORD kFadeFieldOff = 4, kFadeFieldBytes = 5 * 4;
void SaveUiObjs(Slot& s) {
	BYTE* const* list = reinterpret_cast<BYTE* const*>(Live(kUiObjList));
	for (int i = 0; i < 10; ++i) {
		s.uiPtr[i] = list[i];
		if (list[i]) memcpy(s.ui[i], list[i] + kFadeFieldOff, kFadeFieldBytes);
	}
}
void RestoreUiObjs(const Slot& s) {
	BYTE* const* list = reinterpret_cast<BYTE* const*>(Live(kUiObjList));
	for (int i = 0; i < 10; ++i)
		if (s.uiPtr[i] && list[i] == s.uiPtr[i]) memcpy(list[i] + kFadeFieldOff, s.ui[i], kFadeFieldBytes);
}

constexpr DWORD kPoolNewSite = 0x48C761, kPoolNewFn = 0x5DF7DF, kPoolObjOff = 4;
constexpr int kPoolHist = 64;
using OpNew_t = void*(__cdecl*)(size_t);
BYTE* g_poolMem = nullptr;
DWORD g_poolPages = 0;
LONG g_poolSeq = 0;
std::vector<BYTE> g_poolDirty[kPoolHist];
bool g_poolCheck = false;
LONG g_ppSaves = 0, g_ppFull = 0, g_ppPages = 0, g_ppLoads = 0, g_ppLoadPages = 0, g_ppMismatch = 0;

void TrainingPositionEnd();
void* __cdecl HookPoolNew(size_t n) {
	g_sceneForSlots = nullptr;
	TrainingPositionEnd();
	void* p = VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
	if (!p) {
		Log("POOL: write-watch allocation failed (%lu); the pool is saved whole", GetLastError());
		return reinterpret_cast<OpNew_t>(Live(kPoolNewFn))(n);
	}
	g_poolMem = static_cast<BYTE*>(p);
	g_poolPages = static_cast<DWORD>((n + 4095) / 4096);
	for (auto& d : g_poolDirty) d.assign(g_poolPages, 0);
	g_poolSeq += kPoolHist + 1;
	Log("POOL: object pool %p, %lu pages, write-watched", p, g_poolPages);
	return p;
}

constexpr DWORD kPoolDelSiteA = 0x48B7ED, kPoolDelSiteB = 0x48CE6F, kOpDeleteFn = 0x5F3A44;
using OpDelete_t = void(__cdecl*)(void*);
void __cdecl HookPoolDelete(void* p) {
	if (p && p == g_poolMem) {
		VirtualFree(p, 0, MEM_RELEASE);
		g_poolMem = nullptr;
		g_sceneForSlots = nullptr;
		TrainingPositionEnd();
		Log("POOL: object pool %p freed (battle over)", p);
		return;
	}
	reinterpret_cast<OpDelete_t>(Live(kOpDeleteFn))(p);
}

void PoolCollect(std::vector<BYTE>& out) {
	static std::vector<PVOID> addrs;
	addrs.resize(g_poolPages);
	ULONG_PTR count = addrs.size();
	ULONG gran = 0;
	if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_poolMem, static_cast<SIZE_T>(g_poolPages) * 4096, addrs.data(), &count,
	                  &gran) != 0) {
		std::fill(out.begin(), out.end(), BYTE(1));
		return;
	}
	for (ULONG_PTR i = 0; i < count; ++i) out[(static_cast<BYTE*>(addrs[i]) - g_poolMem) / 4096] = 1;
}
bool PoolWatched(const Region& r) { return g_poolMem && r.addr == g_poolMem + kPoolObjOff && r.size == kPoolSize; }
bool PoolSince(LONG since, std::vector<BYTE>& out) {
	if (since < 0 || since > g_poolSeq || g_poolSeq - since >= kPoolHist) return false;
	for (LONG q = since + 1; q <= g_poolSeq; ++q) {
		const std::vector<BYTE>& d = g_poolDirty[q % kPoolHist];
		for (DWORD i = 0; i < g_poolPages; ++i) out[i] |= d[i];
	}
	return true;
}
LONG PoolCopyPages(const std::vector<BYTE>& pages, std::string& buf, bool toSave) {
	LONG n = 0;
	for (DWORD p = 0; p < g_poolPages; ++p) {
		if (!pages[p]) continue;
		const DWORD lo = (std::max)(p * 4096, kPoolObjOff) - kPoolObjOff;
		const DWORD hi = (std::min)((p + 1) * 4096, kPoolObjOff + kPoolSize) - kPoolObjOff;
		if (lo >= hi) continue;
		BYTE* live = g_poolMem + kPoolObjOff + lo;
		if (toSave) memcpy(&buf[lo], live, hi - lo);
		else memcpy(live, buf.data() + lo, hi - lo);
		++n;
	}
	return n;
}
bool PoolSave(Slot& s, const Region& r) {
	if (!PoolWatched(r)) { s.poolSeq = -1; return false; }
	++g_poolSeq;
	std::vector<BYTE>& now = g_poolDirty[g_poolSeq % kPoolHist];
	std::fill(now.begin(), now.end(), BYTE(0));
	PoolCollect(now);
	static std::vector<BYTE> pages;
	pages.assign(g_poolPages, 0);
	++g_ppSaves;
	const bool incremental = s.r[11].size() == r.size && s.addr[11] == r.addr && PoolSince(s.poolSeq, pages);
	if (incremental) {
		g_ppPages += PoolCopyPages(pages, s.r[11], true);
		if (g_poolCheck && memcmp(s.r[11].data(), r.addr, r.size) != 0) {
			const BYTE* a = reinterpret_cast<const BYTE*>(s.r[11].data());
			DWORD off = 0;
			while (off < r.size && a[off] == r.addr[off]) ++off;
			if (++g_ppMismatch <= 20)
				Log("POOL: SAVE MISMATCH at pool +0x%lX (page %lu), copying since save %ld; copied whole", off,
				    (off + kPoolObjOff) / 4096, s.poolSeq);
			s.r[11].assign(reinterpret_cast<char*>(r.addr), r.size);
		}
	} else {
		++g_ppFull;
		s.r[11].assign(reinterpret_cast<char*>(r.addr), r.size);
	}
	s.addr[11] = r.addr;
	s.size[11] = r.size;
	s.poolSeq = g_poolSeq;
	return true;
}
bool PoolLoad(const Slot& s, const Region& r) {
	if (!PoolWatched(r) || s.poolSeq < 0) return false;
	static std::vector<BYTE> pages;
	pages.assign(g_poolPages, 0);
	PoolCollect(pages);
	if (!PoolSince(s.poolSeq, pages)) return false;
	++g_ppLoads;
	g_ppLoadPages += PoolCopyPages(pages, const_cast<std::string&>(s.r[11]), false);
	if (g_poolCheck && memcmp(s.r[11].data(), r.addr, r.size) != 0) {
		if (++g_ppMismatch <= 20) Log("POOL: LOAD MISMATCH restoring save %ld; restored whole", s.poolSeq);
		return false;
	}
	return true;
}

bool SaveSlot(Slot& s, LONG frame) {
	Region r[kRegions];
	if (!CurrentRegions(r)) return false;
	for (int i = 0; i < kRegions; ++i) {
		if (i == 11 && PoolSave(s, r[i])) continue;
		s.r[i].assign(reinterpret_cast<char*>(r[i].addr), r[i].size);
		s.addr[i] = r[i].addr;
		s.size[i] = r[i].size;
	}
	s.ctrl = CaptureCtrls();
	CaptureParticles(s.fx);
	PhaseSignature(r, s.phase);
	s.flowGen = g_flowGen;
	s.frame = frame;
	SaveSceneFields(s);
	SaveUiObjs(s);
	return true;
}

bool LoadSlot(const Slot& s) {
	Region r[kRegions];
	if (!CurrentRegions(r)) return false;
	for (int i = 0; i < kRegions; ++i)
		if (r[i].addr != s.addr[i] || r[i].size != s.size[i]) return false;
	if (!RestoreParticles(s.fx)) return false;
	for (int i = 0; i < kRegions; ++i) {
		if (i == 11 && PoolLoad(s, r[i])) continue;
		RestoreRegion(r[i], s.r[i]);
	}
	RestoreCtrls(s.ctrl);
	RestoreSceneFields(s);
	RestoreUiObjs(s);
	return true;
}

bool MatchesSlot(const Slot& s, bool log) {
	Region r[kRegions];
	if (!CurrentRegions(r)) return false;
	bool same = true;
	for (int i = 0; i < kRegions; ++i) {
		if (!CompareRegion(r[i].name) || memcmp(r[i].addr, s.r[i].data(), r[i].size) == 0) continue;
		const bool pool = strcmp(r[i].name, "objpool") == 0;
		unsigned shown = 0;
		for (DWORD off = 0; off + 4 <= r[i].size; off += 4) {
			if ((pool && IsPoolHandle(off)) || IsLiveField(r[i].name, off)) continue;
			if (memcmp(r[i].addr + off, s.r[i].data() + off, 4) == 0) continue;
			same = false;
			if (!log) break;
			if (++shown <= 8)
				Log("  SYNC DIFF %-7s +0x%06lX  rollback %08X  real %08X", r[i].name, off,
					*reinterpret_cast<DWORD*>(r[i].addr + off), *reinterpret_cast<const DWORD*>(s.r[i].data() + off));
		}
		if (!same && !log) return false;
	}
	return same;
}

#ifdef BBCSE_DEV_TOOLS
void SyncTestFrame(void* self, void* edx) {
	if (g_ringReset) {
		for (auto& s : g_ring) s.frame = -1;
		g_ringReset = false;
	}
	if (!g_poolsKnown) {
		static DWORD lastWait = 0;
		if (GetTickCount() - lastWait > 2000) {
			Log("sync test idle: effect pools unknown. Start a new battle (e.g. leave and re-enter Training).");
			lastWait = GetTickCount();
		}
		return;
	}
	LARGE_INTEGER t0, t1, f;
	QueryPerformanceCounter(&t0);
	const LONG k = g_syncFrame++;
	Slot& now = g_ring[k % kRing];
	if (!SaveSlot(now, k)) return;
	const LONG back = k - g_syncN;
	if (back < 0 || g_ring[back % kRing].frame != back) return;
	for (LONG j = back; j < k; ++j) {
		const Slot& sj = g_ring[j % kRing];
		if (sj.frame != j || sj.flowGen != now.flowGen || memcmp(sj.phase, now.phase, sizeof(now.phase)) != 0) {
			++g_syncBarriers;
			return;
		}
	}
	if (!LoadSlot(g_ring[back % kRing])) {
		for (auto& s : g_ring) s.frame = -1;
		return;
	}
	auto replay = [&](bool stepCheck) {
		for (LONG j = back; j < k; ++j) {
			RestoreCtrls(g_ring[j % kRing].ctrl);
			g_resimFrame = j;
			g_origUpdate(self, edx, g_resimArg);
			g_resimFrame = -1;
			EffectStep();
			if (!stepCheck || j + 1 >= k) continue;
			Slot& real = g_ring[(j + 1) % kRing];
			if (real.frame == j + 1 && !MatchesSlot(real, false)) {
				Log("FIRST SPLIT in replay: step %ld of %d (frame %ld). Differences there:", j + 1 - back, g_syncN, j + 1);
				MatchesSlot(real, true);
				if (g_dumpCount < 3) DumpSplit(real, j + 1, static_cast<int>(j + 1 - back));
				return;
			}
		}
	};
	replay(false);
	RestoreCtrls(now.ctrl);
	++g_syncTests;
	++g_autoTests;
	if (!MatchesSlot(now, false)) {
		++g_syncFails;
		++g_autoFails;
		if (++g_syncFailLogs <= 3) {
			Log("SYNC FAIL at sync frame %ld (rollback of %d frames):", k, g_syncN);
			MatchesSlot(now, true);
		}
		if (g_stepLogs < 6 && LoadSlot(g_ring[back % kRing])) {
			++g_stepLogs;
			replay(true);
		}
		LoadSlot(now);
	}
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&f);
	const double ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart;
	g_syncMs += ms;
	if (ms > g_syncMsMax) g_syncMsMax = ms;
}
#endif

constexpr DWORD kD3DCreateIat = 0x9042D0;
constexpr DWORD kDinputCreateIat = 0x904000;
constexpr DWORD kLimiterTimer = 0xB8EBA8;
constexpr DWORD kPresentImmediate = 0x80000000;
constexpr DWORD kPresentIntervalOff = 0x34;

using D3DCreate_t = void*(WINAPI*)(UINT sdk);
using CreateDevice_t = HRESULT(__stdcall*)(void* d3d, UINT adapter, DWORD type, HWND wnd, DWORD flags, void* pp, void** dev);
using Reset_t = HRESULT(__stdcall*)(void* dev, void* pp);
D3DCreate_t g_origD3DCreate = nullptr;
CreateDevice_t g_origCreateDevice = nullptr;
Reset_t g_origReset = nullptr;
bool g_vsyncForcedOff = false;
bool g_vsyncWanted = false;

void ForceImmediate(void* pp, const char* where) {
	if (!pp) return;
	DWORD& interval = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pp) + kPresentIntervalOff);
	if (interval != kPresentImmediate) {
		Log("VSYNC: %s present interval %08lX -> immediate (game limiter paces at 60)", where, interval);
		interval = kPresentImmediate;
	}
	g_vsyncForcedOff = true;
}

bool PatchVtableSlot(void* obj, int slot, void* hook, void** orig) {
	void** vt = *reinterpret_cast<void***>(obj);
	if (vt[slot] == hook) return true;
	DWORD old;
	if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
	*orig = vt[slot];
	vt[slot] = hook;
	VirtualProtect(&vt[slot], sizeof(void*), old, &old);
	return true;
}

bool g_borderless = false;
HWND g_borderlessWnd = nullptr;
RECT g_borderlessRect{};
constexpr DWORD kIatSetWindowPos = 0x90425C, kIatSetWindowLongA = 0x904298;
constexpr DWORD kIatCreateFileA = 0x904098, kIatCreateFileW = 0x90406C;
constexpr LONG kFrameStyles = WS_OVERLAPPEDWINDOW;
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SetWindowLongA_t = LONG(WINAPI*)(HWND, int, LONG);
SetWindowPos_t g_origSetWindowPos = nullptr;
SetWindowLongA_t g_origSetWindowLongA = nullptr;
LONG WINAPI HookSetWindowLongA(HWND w, int i, LONG v) {
	if (w && w == g_borderlessWnd && i == GWL_STYLE) v = (v & ~kFrameStyles) | WS_POPUP;
	return g_origSetWindowLongA(w, i, v);
}
BOOL WINAPI HookSetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags) {
	if (w && w == g_borderlessWnd) {
		const RECT& r = g_borderlessRect;
		x = r.left; y = r.top; cx = r.right - r.left; cy = r.bottom - r.top;
		flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
	}
	return g_origSetWindowPos(w, after, x, y, cx, cy, flags);
}
void PatchIat(DWORD slotVa, void* hook, void** orig) {
	void** slot = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr)) + (slotVa - kImage));
	if (*slot == hook) return;
	DWORD old;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
	*orig = *slot;
	*slot = hook;
	VirtualProtect(slot, sizeof(void*), old, &old);
}
struct VersionFile { const char* path; DWORD size; DWORD crc; };
#include "versions_manifest.inc"
std::string g_versionName = "cse";
std::string g_versionDir;
bool VersionFileListed(const std::string& version, const std::string& rel) {
	static const std::set<std::string> listed = [] {
		std::set<std::string> s;
		for (const VersionFile& f : kVersionFiles) s.insert(f.path);
		return s;
	}();
	return listed.count(version + "\\" + rel) != 0;
}
CRITICAL_SECTION g_versionLock;
std::string g_versionLoaded;
void NoteVersionLoad(const std::string& path) {
	const size_t slash = path.find_last_of("\\/");
	std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
	for (char& c : file) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	if (file.compare(0, 5, "char_") != 0 || file.size() < 7) return;
	const std::string code = file.compare(0, 10, "char_boss_") == 0 ? std::string() : file.substr(5, 2);
	if (code.empty() || code == "e0") return;
	EnterCriticalSection(&g_versionLock);
	if (g_versionLoaded.find(code) == std::string::npos) {
		if (!g_versionLoaded.empty()) g_versionLoaded += ' ';
		g_versionLoaded += code;
	}
	LeaveCriticalSection(&g_versionLock);
}
LONG g_versionRedirects = 0;
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileA_t g_origCreateFileA = nullptr;
CreateFileW_t g_origCreateFileW = nullptr;
std::string VersionPath(const std::string& path) {
	EnterCriticalSection(&g_versionLock);
	const std::string root = g_versionDir;
	const std::string version = g_versionName;
	LeaveCriticalSection(&g_versionLock);
	if (root.empty() || path.empty() || version == "cse") return std::string();
	std::string p = path;
	for (char& c : p)
		if (c == '/') c = '\\';
	std::string low = p;
	for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	size_t at = std::string::npos;
	if (low.compare(0, 5, "data\\") == 0) {
		at = 5;
	} else {
		const size_t k = low.rfind("\\data\\");
		if (k != std::string::npos) at = k + 6;
	}
	if (at == std::string::npos) return std::string();
	if (!VersionFileListed(version, low.substr(at))) return std::string();
	const std::string alt = root + p.substr(at);
	return GetFileAttributesA(alt.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : alt;
}
std::string RelPath(const std::string& p) {
	const std::string dir = ModuleDir(g_self);
	return _strnicmp(p.c_str(), dir.c_str(), dir.size()) == 0 ? p.substr(dir.size()) : p;
}
void NoteMediaOpen(const char* name) {
#ifdef BBCSE_DEV_TOOLS
	static LONG s_logged = 0;
	std::string low = name;
	for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	if ((low.find("bgm") != std::string::npos || low.find("movie") != std::string::npos) && InterlockedIncrement(&s_logged) <= 300)
		Log("MEDIA: %s", name);
#else
	(void)name;
#endif
}
HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
	if (name && !(access & GENERIC_WRITE)) {
		NoteMediaOpen(name);
		const std::string alt = VersionPath(name);
		if (!alt.empty()) {
			if (g_versionRedirects++ < 200) Log("VERSION: %s -> %s", RelPath(name).c_str(), RelPath(alt).c_str());
			NoteVersionLoad(name);
			return g_origCreateFileA(alt.c_str(), access, share, sa, disp, flags, tmpl);
		}
	}
	return g_origCreateFileA(name, access, share, sa, disp, flags, tmpl);
}
HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
	if (name && !(access & GENERIC_WRITE)) {
		char narrow[MAX_PATH * 2] = {};
		if (WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr) > 0) {
			NoteMediaOpen(narrow);
			const std::string alt = VersionPath(narrow);
			if (!alt.empty()) {
				wchar_t wide[MAX_PATH * 2] = {};
				if (MultiByteToWideChar(CP_ACP, 0, alt.c_str(), -1, wide, MAX_PATH * 2) > 0) {
					if (g_versionRedirects++ < 200) Log("VERSION: %s -> %s", RelPath(narrow).c_str(), RelPath(alt).c_str());
					NoteVersionLoad(narrow);
					return g_origCreateFileW(wide, access, share, sa, disp, flags, tmpl);
				}
			}
		}
	}
	return g_origCreateFileW(name, access, share, sa, disp, flags, tmpl);
}
void LoadSavedVersion();
void InstallVersionSelect() {
	static bool done = false;
	if (done) return;
	done = true;
	InitializeCriticalSection(&g_versionLock);
	PatchIat(kIatCreateFileA, reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&g_origCreateFileA));
	PatchIat(kIatCreateFileW, reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_origCreateFileW));
}
void CheckVersionFiles();
void LoadVersionChoice() {
	static bool done = false;
	if (done) return;
	done = true;
	CheckVersionFiles();
	LoadSavedVersion();
}
std::string VersionsRoot() { return ModuleDir(g_self) + "bbcse-versions\\"; }
constexpr DWORD kCatalogBlockFn = 0x453C10;
constexpr DWORD kRuleOffsets[] = {0xB28, 0xB2C, 0xB30, 0xB34, 0xB44, 0xB38, 0xB3C};
const char* const kRuleNames[] = {"SafeGuardPrimer", "BarrierRecoverEx", "SafeRendaBarrier", "EtcBBCS2Function", "VoiceUpdateCS2", "JustGuardFrame", "GuardCrushDamageRate"};
constexpr int kCs1Rules[] = {0, 0, 0, 0, 0, 5, 70};
constexpr int kCseRules[] = {1, 1, 1, 1, 1, 3, 90};
constexpr int kRuleCount = static_cast<int>(sizeof(kRuleOffsets) / sizeof(kRuleOffsets[0]));
bool g_gameRulesSaved = false;
void ApplyVersionRules() {
	using CatalogBlock_t = BYTE*(__cdecl*)(int);
	BYTE* block = reinterpret_cast<CatalogBlock_t>(Live(kCatalogBlockFn))(-1);
	if (!block) return;
	g_gameRulesSaved = true;
	const bool cs1 = g_versionName == "cs1";
	char line[256] = "";
	for (int i = 0; i < kRuleCount; ++i) {
		const int v = cs1 ? kCs1Rules[i] : kCseRules[i];
		*reinterpret_cast<int*>(block + kRuleOffsets[i]) = v;
		char part[40];
		sprintf_s(part, "%s%s %d", i ? ", " : "", kRuleNames[i], v);
		strcat_s(line, part);
	}
	Log("VERSION rules (%s): %s", g_versionName.c_str(), line);
}
void MakeBorderless(void* pp) {
	if (!g_borderless || !pp) return;
	const auto* p = static_cast<const D3DPRESENT_PARAMETERS*>(pp);
	if (!p->Windowed || !p->hDeviceWindow) return;
	const HWND w = p->hDeviceWindow;
	MONITORINFO mi{sizeof(mi)};
	if (!GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
	const RECT& r = mi.rcMonitor;
	g_borderlessWnd = w;
	g_borderlessRect = r;
	if (!g_origSetWindowPos) {
		PatchIat(kIatSetWindowPos, reinterpret_cast<void*>(&HookSetWindowPos), reinterpret_cast<void**>(&g_origSetWindowPos));
		PatchIat(kIatSetWindowLongA, reinterpret_cast<void*>(&HookSetWindowLongA), reinterpret_cast<void**>(&g_origSetWindowLongA));
	}
	SetWindowLongA(w, GWL_STYLE, (GetWindowLongA(w, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
	SetWindowPos(w, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	static bool logged = false;
	if (!logged) { logged = true; Log("DISPLAY: borderless window %ldx%ld (back buffer %ux%u)", r.right - r.left, r.bottom - r.top, p->BackBufferWidth, p->BackBufferHeight); }
}
bool NetSessionActive();
HRESULT __stdcall HookReset(void* dev, void* pp) {
	if (g_vsyncWanted) ForceImmediate(pp, "Reset");
	Overlay_BeforeReset();
	const HRESULT hr = g_origReset(dev, pp);
	if (hr >= 0) { Overlay_AfterReset(); MakeBorderless(pp); }
	static HRESULT s_lastReset = 1;
	if (hr != s_lastReset) {
		s_lastReset = hr;
		Log("DISPLAY: device reset %08lX%s", hr, NetSessionActive() ? ", online match" : "");
	}
	return hr;
}
using Present_t = HRESULT(__stdcall*)(void* dev, const void* src, const void* dst, HWND wnd, const void* dirty);
Present_t g_origPresent = nullptr;
constexpr double kFramePeriodUs = 1000000.0 / 60.0;
double g_paceNextUs = 0, g_presentEmaMs = 0;
int g_presentSamples = 0;
bool g_vsyncDetected = false;
double QpcNowUs() {
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return static_cast<double>(t.QuadPart) * 1000000.0 / static_cast<double>(f.QuadPart);
}
enum class Pace { Own, VsyncOnly };
Pace g_pace = Pace::Own;
int g_refreshHz = 0;
double g_loopLastUs = 0, g_loopEmaMs = 0;
int g_loopSamples = 0;
int g_paceHoldRequests = 0;
bool g_vsyncOnlyRefuted = false;
int RefreshHz() {
	DEVMODEA dm{};
	dm.dmSize = sizeof(dm);
	return EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? static_cast<int>(dm.dmDisplayFrequency) : 60;
}
bool OwnPacing() { return g_vsyncWanted && g_pace == Pace::Own; }
bool g_waitAfterPresent = false;
void WaitDeadline() {
	double now = QpcNowUs();
	if (g_paceNextUs == 0 || now - g_paceNextUs > kFramePeriodUs) {
		g_paceNextUs = now;
	} else {
		for (;;) {
			now = QpcNowUs();
			const double left = g_paceNextUs - now;
			if (left <= 0) break;
			if (left > 2000) Sleep(1); else YieldProcessor();
		}
	}
	g_paceNextUs += kFramePeriodUs;
}
constexpr DWORD kLimiterFn = 0x427E50, kLimiterSiteA = 0x412526, kLimiterSiteB = 0x412665;
using Limiter_t = int(__fastcall*)(void* timer, void* edx);
Limiter_t g_origLimiter = nullptr;
int __fastcall HookLimiter(void* timer, void* edx) {
	if (!g_vsyncWanted) return g_origLimiter(timer, edx);
	double now = QpcNowUs();
	if (g_pace == Pace::Own && g_waitAfterPresent) {
		g_waitAfterPresent = false;
		WaitDeadline();
		now = QpcNowUs();
	}
	if (g_pace == Pace::Own && _ReturnAddress() == static_cast<void*>(Live(kLimiterSiteB) + 5)) {
		g_waitAfterPresent = true;
		return 0;
	}
	if (g_pace == Pace::VsyncOnly) {
		if (g_loopLastUs != 0) {
			const double ms = (now - g_loopLastUs) / 1000.0;
			g_loopEmaMs = g_loopSamples ? g_loopEmaMs + (ms - g_loopEmaMs) / 30.0 : ms;
			if (++g_loopSamples > 60 && g_loopEmaMs < 12.0) {
				g_pace = Pace::Own;
				g_paceNextUs = 0;
				g_presentSamples = 0;
				g_vsyncOnlyRefuted = true;
				Log("PACING: frames came every %.1f ms with VSync pacing alone: the mod paces again", g_loopEmaMs);
			}
		}
		g_loopLastUs = now;
		return 0;
	}
	WaitDeadline();
	return 0;
}

void PaceAfterPresent(double presentMs) {
	if (!g_vsyncWanted || g_pace != Pace::Own) return;
	g_presentEmaMs = g_presentSamples ? g_presentEmaMs + (presentMs - g_presentEmaMs) / 30.0 : presentMs;
	if (++g_presentSamples > 180 && g_presentEmaMs > 6.0) {
		g_refreshHz = RefreshHz();
		g_presentSamples = 0;
		if (g_refreshHz <= 61 && !g_vsyncOnlyRefuted) {
			g_pace = Pace::VsyncOnly;
			g_loopLastUs = 0;
			g_loopSamples = 0;
			Log("PACING: Present blocks %.1f ms on average: the driver forces VSync (%d Hz). VSync paces alone",
				g_presentEmaMs, g_refreshHz);
		} else if (!g_vsyncDetected) {
			g_vsyncDetected = true;
			Log("PACING: VSync forced at %d Hz: the mod keeps its 60 fps deadlines (frames show on the next refresh)",
				g_refreshHz);
		}
	}
}
RECT g_canvasDst{};
UINT g_backBufferW = 0, g_backBufferH = 0;
constexpr DWORD kIatPerspectiveFov = 0x9042F4;
using PerspectiveFov_t = float*(WINAPI*)(float* out, float fovy, float aspect, float zn, float zf);
PerspectiveFov_t g_origPerspectiveFov = nullptr;
float* g_battleProj = nullptr;
float* WINAPI HookPerspectiveFov(float* out, float fovy, float aspect, float zn, float zf) {
	if (fabsf(fovy - 0.785f) < 0.01f && fabsf(aspect - 1.667f) < 0.01f) g_battleProj = out;
	return g_origPerspectiveFov(out, fovy, aspect, zn, zf);
}
bool BattleCamera(const float*& view, const float*& proj) {
	if (!g_battleProj) return false;
	MEMORY_BASIC_INFORMATION mbi{};
	const float* v = g_battleProj - 20;
	if (!VirtualQuery(v, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0)
		return false;
	if (static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize < reinterpret_cast<const BYTE*>(g_battleProj + 16)) return false;
	const float* p = g_battleProj;
	if (p[11] != 1.0f || p[15] != 0.0f || v[15] != 1.0f || v[3] != 0.0f || v[7] != 0.0f || v[11] != 0.0f) return false;
	view = v;
	proj = p;
	return true;
}
void CollectHitboxScene() {
	HitboxScene& h = g_hitboxScene;
	h.valid = false;
	h.objects.clear();
	if (!g_hitboxVisible || !BattleCamera(h.view, h.proj) || !g_backBufferW) return;
	const BYTE* chars = *reinterpret_cast<BYTE**>(Live(kObjMgr) + kCharsPtrOff);
	if (!chars) return;
	h.objects.push_back(chars);
	h.objects.push_back(chars + 0x20140);
	const BYTE* pool = g_poolMem + kPoolObjOff;
	h.chars = chars;
	h.poolBegin = pool;
	h.poolStride = kPoolStride;
	h.poolCount = 400;
	for (DWORD i = 0; i < 400; ++i) {
		const BYTE* o = pool + i * kPoolStride;
		if (*reinterpret_cast<const int*>(o + 0x10) == 1 && *reinterpret_cast<void* const*>(o + 0x4C)) h.objects.push_back(o);
	}
	h.dstLeft = static_cast<float>(g_canvasDst.left);
	h.dstTop = static_cast<float>(g_canvasDst.top);
	h.dstWidth = static_cast<float>(g_canvasDst.right - g_canvasDst.left);
	h.dstHeight = static_cast<float>(g_canvasDst.bottom - g_canvasDst.top);
	h.backBufferWidth = static_cast<float>(g_backBufferW);
	h.backBufferHeight = static_cast<float>(g_backBufferH);
	h.valid = true;
}
void VersionSyncTick();
void PaletteSyncTick();
HRESULT __stdcall HookPresent(void* dev, const void* src, const void* dst, HWND wnd, const void* dirty) {
	LoadVersionChoice();
	VersionSyncTick();
	PaletteSyncTick();
	const bool training = *reinterpret_cast<int*>(Live(kGameModeVa)) == kModeTraining;
	if (g_poolMem && training) {
		CollectHitboxScene();
	} else {
		g_frameAdv.valid = false;
		g_hitboxScene.valid = false;
	}
	g_paletteScene.objMgr = Live(kObjMgr);
	g_paletteScene.gameInfo = Live(kGameModeVa - 8);
	g_paletteScene.fileBuffer = Live(0xBBEA50);
	g_paletteScene.opSizes = Live(0x9EB920);
	g_paletteScene.training = training;
	g_paletteScene.valid = g_poolMem && SceneFighting() && *reinterpret_cast<BYTE**>(Live(kObjMgr) + kCharsPtrOff);
	Palette_Tick();
	Overlay_OnPresent(static_cast<IDirect3DDevice9*>(dev));
	const double t0 = QpcNowUs();
	const HRESULT hr = g_origPresent(dev, src, dst, wnd, dirty);
	static bool s_presentFailed = false;
	if ((hr < 0) != s_presentFailed) {
		s_presentFailed = hr < 0;
		if (s_presentFailed) Log("DISPLAY: Present failed %08lX%s", hr, NetSessionActive() ? ", online match" : "");
		else Log("DISPLAY: Present ok");
	}
	PaceAfterPresent((QpcNowUs() - t0) / 1000.0);
	if (g_waitAfterPresent) {
		g_waitAfterPresent = false;
		if (OwnPacing()) WaitDeadline();
	}
	return hr;
}
int g_renderW = 0, g_renderH = 0;
using CreateRT_t = HRESULT(__stdcall*)(void* dev, UINT w, UINT h, DWORD fmt, DWORD ms, DWORD q, BOOL flag, void** surf, HANDLE* share);
using SetViewport_t = HRESULT(__stdcall*)(void* dev, const D3DVIEWPORT9* vp);
using StretchRect_t = HRESULT(__stdcall*)(void* dev, void* src, const RECT* srcRect, void* dst, const RECT* dstRect, DWORD filter);
CreateRT_t g_origCreateRT = nullptr, g_origCreateDS = nullptr;
SetViewport_t g_origSetViewport = nullptr;
StretchRect_t g_origStretchRect = nullptr;
bool IsCanvasSize(UINT w, UINT h) { return w == 1280 && h == 768; }
HRESULT __stdcall HookCreateRT(void* dev, UINT w, UINT h, DWORD fmt, DWORD ms, DWORD q, BOOL lockable, void** surf, HANDLE* share) {
	if (IsCanvasSize(w, h)) { w = g_renderW; h = g_renderH; Log("DISPLAY: game canvas drawn at %ux%u", w, h); }
	return g_origCreateRT(dev, w, h, fmt, ms, q, lockable, surf, share);
}
HRESULT __stdcall HookCreateDS(void* dev, UINT w, UINT h, DWORD fmt, DWORD ms, DWORD q, BOOL discard, void** surf, HANDLE* share) {
	if (IsCanvasSize(w, h)) { w = g_renderW; h = g_renderH; }
	return g_origCreateDS(dev, w, h, fmt, ms, q, discard, surf, share);
}
bool IsRenderSized(IDirect3DSurface9* surf) {
	D3DSURFACE_DESC d{};
	return surf && SUCCEEDED(surf->GetDesc(&d)) && d.Width == static_cast<UINT>(g_renderW) && d.Height == static_cast<UINT>(g_renderH);
}
bool TargetIsRenderSized(void* dev) {
	IDirect3DSurface9* rt = nullptr;
	if (FAILED(static_cast<IDirect3DDevice9*>(dev)->GetRenderTarget(0, &rt)) || !rt) return false;
	const bool yes = IsRenderSized(rt);
	rt->Release();
	return yes;
}
HRESULT __stdcall HookSetViewport(void* dev, const D3DVIEWPORT9* vp) {
	if (vp && vp->X == 0 && vp->Y == 0 && IsCanvasSize(vp->Width, vp->Height) && TargetIsRenderSized(dev)) {
		D3DVIEWPORT9 v = *vp;
		v.Width = g_renderW;
		v.Height = g_renderH;
		return g_origSetViewport(dev, &v);
	}
	return g_origSetViewport(dev, vp);
}
bool IsCanvasSurface(void* surf) {
	D3DSURFACE_DESC d{};
	if (!surf || FAILED(static_cast<IDirect3DSurface9*>(surf)->GetDesc(&d))) return false;
	return g_renderW > 0 ? d.Width == static_cast<UINT>(g_renderW) && d.Height == static_cast<UINT>(g_renderH)
	                     : IsCanvasSize(d.Width, d.Height);
}
void NoteCanvasCopy(void* dev, void* src, void* dst, const RECT* dstRect) {
	if (!IsCanvasSurface(src)) return;
	IDirect3DSurface9* bb = nullptr;
	if (FAILED(static_cast<IDirect3DDevice9*>(dev)->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
	if (bb == dst) {
		D3DSURFACE_DESC d{};
		if (SUCCEEDED(bb->GetDesc(&d))) {
			g_backBufferW = d.Width;
			g_backBufferH = d.Height;
			g_canvasDst = dstRect ? *dstRect : RECT{0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
		}
	}
	bb->Release();
}
HRESULT __stdcall HookStretchRect(void* dev, void* src, const RECT* srcRect, void* dst, const RECT* dstRect, DWORD filter) {
	NoteCanvasCopy(dev, src, dst, dstRect);
	if (g_renderW > 0 && srcRect && srcRect->left == 0 && srcRect->top == 0 && IsCanvasSize(srcRect->right, srcRect->bottom) &&
	    IsRenderSized(static_cast<IDirect3DSurface9*>(src))) {
		const RECT r = {0, 0, g_renderW, g_renderH};
		return g_origStretchRect(dev, src, &r, dst, dstRect, D3DTEXF_LINEAR);
	}
	return g_origStretchRect(dev, src, srcRect, dst, dstRect, filter);
}
void InstallDeviceHooks(void* dev) {
	if (!dev || g_origPresent) return;
	MEMORY_BASIC_INFORMATION mbi{};
	void* present = (*reinterpret_cast<void***>(dev))[17];
	if (!VirtualQuery(present, &mbi, sizeof(mbi)) || mbi.AllocationBase != GetModuleHandleA("d3d9.dll")) {
		Log("overlay: NOT installed (device %p, Present %p is not in d3d9.dll)", dev, present);
		return;
	}
	PatchVtableSlot(dev, 16, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_origReset));
	PatchVtableSlot(dev, 17, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent));
	if (g_renderW > 0 && g_renderH > 0) {
		PatchVtableSlot(dev, 28, reinterpret_cast<void*>(&HookCreateRT), reinterpret_cast<void**>(&g_origCreateRT));
		PatchVtableSlot(dev, 29, reinterpret_cast<void*>(&HookCreateDS), reinterpret_cast<void**>(&g_origCreateDS));
		PatchVtableSlot(dev, 47, reinterpret_cast<void*>(&HookSetViewport), reinterpret_cast<void**>(&g_origSetViewport));
		Log("DISPLAY: render size %dx%d (game default 1280x768)", g_renderW, g_renderH);
	}
	PatchVtableSlot(dev, 34, reinterpret_cast<void*>(&HookStretchRect), reinterpret_cast<void**>(&g_origStretchRect));
	Log("overlay: device hooks installed (F1 shows / hides the diagnostics panel in online battles)");
}
HRESULT __stdcall HookCreateDevice(void* d3d, UINT adapter, DWORD type, HWND wnd, DWORD flags, void* pp, void** dev) {
	if (g_vsyncWanted) ForceImmediate(pp, "CreateDevice");
	const HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, dev);
	if (hr >= 0 && dev && *dev) InstallDeviceHooks(*dev);
	if (hr >= 0) MakeBorderless(pp);
	return hr;
}
void* WINAPI HookD3DCreate(UINT sdk) {
	void* d3d = g_origD3DCreate(sdk);
	if (d3d && !g_origCreateDevice)
		PatchVtableSlot(d3d, 16, reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&g_origCreateDevice));
	return d3d;
}

constexpr DWORD kIatSteamRemoteStorage = 0x90439C;
constexpr DWORD kAppId = 294810;
std::string g_cloudDir;
using SteamRemoteStorage_t = void*(__cdecl*)();
SteamRemoteStorage_t g_origRemoteStorage = nullptr;
bool g_cloudHooked = false;

bool CloudName(const char* name, std::string& path) {
	if (!name || !*name || strpbrk(name, "\\/:") || strstr(name, "..")) return false;
	path = g_cloudDir + name;
	return true;
}
bool __fastcall CloudFileWrite(void*, void*, const char* name, const void* data, int size) {
	std::string path;
	if (!CloudName(name, path) || size < 0) return false;
	const std::string tmp = path + ".tmp";
	FILE* f = nullptr;
	if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) { Log("CLOUD: write %s FAILED", name); return false; }
	bool ok = fwrite(data, 1, static_cast<size_t>(size), f) == static_cast<size_t>(size);
	ok = (fflush(f) == 0) && ok;
	ok = (_commit(_fileno(f)) == 0) && ok;
	ok = (fclose(f) == 0) && ok;
	ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	if (!ok) DeleteFileA(tmp.c_str());
	Log("CLOUD: write %s, %d bytes%s", name, size, ok ? "" : " FAILED (old profile kept)");
	return ok;
}
int __fastcall CloudFileRead(void*, void*, const char* name, void* data, int cap) {
	std::string path;
	if (!CloudName(name, path) || cap <= 0) return 0;
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) { Log("CLOUD: read %s: not found", name); return 0; }
	const int got = static_cast<int>(fread(data, 1, static_cast<size_t>(cap), f));
	fclose(f);
	Log("CLOUD: read %s, %d bytes", name, got);
	return got;
}
bool __fastcall CloudFileDelete(void*, void*, const char* name) {
	std::string path;
	if (!CloudName(name, path)) return false;
	Log("CLOUD: delete %s", name);
	return DeleteFileA(path.c_str()) != FALSE;
}
bool __fastcall CloudFileExists(void*, void*, const char* name) {
	std::string path;
	return CloudName(name, path) && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
int __fastcall CloudGetFileSize(void*, void*, const char* name) {
	std::string path;
	WIN32_FILE_ATTRIBUTE_DATA fa{};
	if (!CloudName(name, path) || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
	return static_cast<int>(fa.nFileSizeLow);
}
long long __fastcall CloudGetFileTimestamp(void*, void*, const char* name) {
	std::string path;
	WIN32_FILE_ATTRIBUTE_DATA fa{};
	if (!CloudName(name, path) || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
	const unsigned long long t = (static_cast<unsigned long long>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
	                             fa.ftLastWriteTime.dwLowDateTime;
	return static_cast<long long>(t / 10000000ULL - 11644473600ULL);
}
bool __fastcall CloudGetQuota(void*, void*, int* total, int* available) {
	if (total) *total = 100 << 20;
	if (available) *available = 99 << 20;
	return true;
}
bool __fastcall CloudEnabled(void*, void*) { return true; }

void ImportSteamCloudCopy(void*) {
	std::string local;
	if (!CloudName("DCARD.UMS", local) || GetFileAttributesA(local.c_str()) != INVALID_FILE_ATTRIBUTES) return;
	char steamPath[MAX_PATH] = {};
	DWORD len = sizeof(steamPath);
	if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, steamPath, &len) != ERROR_SUCCESS)
		return;
	using SteamUser_t = void*(__cdecl*)();
	const auto steamUser = reinterpret_cast<SteamUser_t>(GetProcAddress(GetModuleHandleA("steam_api.dll"), "SteamUser"));
	void* user = steamUser ? steamUser() : nullptr;
	if (!user) return;
	unsigned long long id = 0;
	using GetSteamID_t = unsigned long long*(__fastcall*)(void* self, void* edx, unsigned long long* out);
	(*reinterpret_cast<GetSteamID_t*>(*reinterpret_cast<BYTE**>(user) + 8))(user, nullptr, &id);
	char src[MAX_PATH];
	sprintf_s(src, "%s\\userdata\\%lu\\%lu\\remote\\DCARD.UMS", steamPath, static_cast<unsigned long>(id & 0xFFFFFFFF), kAppId);
	if (CopyFileA(src, local.c_str(), TRUE)) Log("CLOUD: imported the old online profile from Steam userdata");
	else Log("CLOUD: no old online profile in Steam userdata (a new one will be made)");
}

void PatchCloudSlot(void** vt, int slot, void* fn) {
	DWORD old;
	VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
	vt[slot] = fn;
	VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}
void* __cdecl HookSteamRemoteStorage() {
	void* storage = g_origRemoteStorage();
	if (storage && !g_cloudHooked) {
		g_cloudHooked = true;
		void** vt = *reinterpret_cast<void***>(storage);
		PatchCloudSlot(vt, 0, reinterpret_cast<void*>(&CloudFileWrite));
		PatchCloudSlot(vt, 1, reinterpret_cast<void*>(&CloudFileRead));
		PatchCloudSlot(vt, 3, reinterpret_cast<void*>(&CloudFileDelete));
		PatchCloudSlot(vt, 10, reinterpret_cast<void*>(&CloudFileExists));
		PatchCloudSlot(vt, 11, reinterpret_cast<void*>(&CloudFileExists));
		PatchCloudSlot(vt, 12, reinterpret_cast<void*>(&CloudGetFileSize));
		PatchCloudSlot(vt, 13, reinterpret_cast<void*>(&CloudGetFileTimestamp));
		PatchCloudSlot(vt, 17, reinterpret_cast<void*>(&CloudGetQuota));
		PatchCloudSlot(vt, 18, reinterpret_cast<void*>(&CloudEnabled));
		PatchCloudSlot(vt, 19, reinterpret_cast<void*>(&CloudEnabled));
		CreateDirectoryA(g_cloudDir.c_str(), nullptr);
		ImportSteamCloudCopy(storage);
		Log("CLOUD: online profile kept in bbcse-cloud (Steam Cloud is off for this game)");
	}
	return storage;
}

void InstallCloudFix(HMODULE self) {
	g_cloudDir = ModuleDir(self) + "bbcse-cloud\\";
	BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
	void** slot = reinterpret_cast<void**>(base + (kIatSteamRemoteStorage - kImage));
	HMODULE api = GetModuleHandleA("steam_api.dll");
	void* real = api ? reinterpret_cast<void*>(GetProcAddress(api, "SteamRemoteStorage")) : nullptr;
	if (!real || *slot != real) { Log("CLOUD: not installed (import slot %p holds %p, export %p)", slot, *slot, real); return; }
	DWORD old;
	VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
	g_origRemoteStorage = reinterpret_cast<SteamRemoteStorage_t>(real);
	*slot = reinterpret_cast<void*>(&HookSteamRemoteStorage);
	VirtualProtect(slot, sizeof(void*), old, &old);
}

void InstallVsyncOff(HMODULE self) {
	const std::string ini = ModuleDir(self) + "bbcse-net.ini";
	char mode[16] = {};
	GetPrivateProfileStringA("net", "mode", "", mode, sizeof(mode), ini.c_str());
	if (_stricmp(mode, "steam") == 0) {
		g_vsyncWanted = GetPrivateProfileIntA("net", "vsync", 0, ini.c_str()) == 0;
		g_renderW = GetPrivateProfileIntA("display", "renderwidth", 0, ini.c_str());
		g_renderH = GetPrivateProfileIntA("display", "renderheight", 0, ini.c_str());
		g_borderless = GetPrivateProfileIntA("display", "borderless", 0, ini.c_str()) != 0;
		if (g_renderW < 320 || g_renderH < 192 || g_renderW > 8192 || g_renderH > 8192) g_renderW = g_renderH = 0;
	}
	BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
	void** slot = reinterpret_cast<void**>(base + (kD3DCreateIat - kImage));
	HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
	void* real = d3d9 ? reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
	if (!real || *slot != real) {
		Log("VSYNC: not changed (import slot %p holds %p, d3d9 export %p)", slot, *slot, real);
		return;
	}
	DWORD old;
	VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
	g_origD3DCreate = reinterpret_cast<D3DCreate_t>(real);
	*slot = reinterpret_cast<void*>(&HookD3DCreate);
	VirtualProtect(slot, sizeof(void*), old, &old);
	Log("VSYNC: display creation hooked; %s", g_vsyncWanted
		? "VSync off, the mod paces frames at 60 (vsync=1 in bbcse-net.ini keeps VSync)"
		: "VSync on, the game paces (G-Sync / the display syncs); [display] settings still apply");
}

double g_paceOutstandingMs = 0, g_paceRiftEma = 0, g_paceAppliedMs = 0;
bool g_paceHasRift = false;
int g_paceHold = 0;
constexpr double kPaceMaxStepMs = 3.0, kPaceMinShiftMs = 1.0, kPaceDeadZone = 0.75, kPaceGain = 1.0 / 120.0;

void PaceReset() {
	g_paceOutstandingMs = g_paceRiftEma = 0;
	g_paceHasRift = false;
	g_paceHold = 0;
}
void PaceOnRecommendation(int framesAhead) {
	if (framesAhead > 0) g_paceOutstandingMs = (framesAhead > 9 ? 9 : framesAhead) * (1000.0 / 60.0);
}
void PaceOnPredictionStall() { g_paceHold = 45; }
void PaceOnRiftSample(int localBehind, int remoteBehind) {
	if (g_paceHold > 0) { --g_paceHold; return; }
	if (localBehind < -30 || localBehind > 30 || remoteBehind < -30 || remoteBehind > 30) return;
	const double rift = (remoteBehind - localBehind) * 0.5;
	g_paceRiftEma = g_paceHasRift ? g_paceRiftEma + (rift - g_paceRiftEma) / 15.0 : rift;
	g_paceHasRift = true;
	if (g_paceRiftEma > kPaceDeadZone) {
		g_paceOutstandingMs += (g_paceRiftEma - kPaceDeadZone) * (1000.0 / 60.0) * kPaceGain;
		if (g_paceOutstandingMs > 2 * kPaceMaxStepMs * 3) g_paceOutstandingMs = 2 * kPaceMaxStepMs * 3;
	}
}
void PaceApply() {
	if (g_paceOutstandingMs < kPaceMinShiftMs) return;
	if (g_vsyncWanted && g_pace == Pace::VsyncOnly) {
		if (g_paceOutstandingMs >= 1000.0 / 60.0) {
			++g_paceHoldRequests;
			g_paceOutstandingMs -= 1000.0 / 60.0;
			g_paceAppliedMs += 1000.0 / 60.0;
		}
		return;
	}
	if (OwnPacing()) {
		if (g_paceNextUs == 0) return;
		const double ms = g_paceOutstandingMs < kPaceMaxStepMs ? g_paceOutstandingMs : kPaceMaxStepMs;
		g_paceNextUs += ms * 1000.0;
		g_paceOutstandingMs -= ms;
		g_paceAppliedMs += ms;
		return;
	}
	BYTE* clock = *reinterpret_cast<BYTE**>(Live(kLimiterTimer));
	if (!clock) return;
	const double ms = g_paceOutstandingMs < kPaceMaxStepMs ? g_paceOutstandingMs : kPaceMaxStepMs;
	using Elapsed_t = unsigned long long(__fastcall*)(BYTE* clock);
	const unsigned long long elapsedUs = reinterpret_cast<Elapsed_t>(Live(0x407CC0))(clock);
	if (elapsedUs < static_cast<unsigned long long>((ms + 1.0) * 1000.0)) return;
	*reinterpret_cast<unsigned long long*>(clock + 0x10) += static_cast<unsigned long long>(ms * 1000.0);
	g_paceOutstandingMs -= ms;
	g_paceAppliedMs += ms;
}

constexpr DWORD kGameMgr = 0xBC12D8, kSideSlotOff = 0x595C, kCtrlListPtr = 0xBC8B68;
constexpr DWORD kRngSeed = 0x428F80;
using Seed_t = void(__fastcall*)(void* rng, void* edx, DWORD seed);
using Button_t = int(__fastcall*)(int* ctrl, void* edx, int id);

struct NetState {
	GGPOSession* session = nullptr;
	GGPOPlayerHandle local = GGPO_INVALID_HANDLE, remote = GGPO_INVALID_HANDLE;
	bool synctest = false, steam = false, running = false, seeded = false;
	int side = 0, stall = 0, frame = 0;
	DWORD seed = 0;
	void* scene = nullptr;
} g_net;
bool NetSessionActive() { return g_net.session != nullptr; }
bool g_netActive = false;
bool g_spectating = false;
LONG g_specRngFixes = 0;
int g_specBase = 0, g_specRound = -1, g_specIdx = -1;
void SpectatorSync();
void ReplaySync();
int g_replayFrame = 0;
DWORD g_netIn[2];
bool g_netInUpdate = false;
DWORD g_latPrevPad = 0, g_latPendingMask = 0;
int g_latPendingFrame = -1;
LONG g_latSum = 0, g_latCount = 0, g_latMax = 0;
bool g_latPending = false;
bool g_steamRearm = false;
LONG g_netRollbackFrames = 0, g_netStalls = 0, g_netSyncErrors = 0, g_netSlowTicks = 0;
double g_netMsSum = 0, g_netMsMax = 0, g_lastTickMs = 0;
LONG g_ovlMaxDepth = 0;
int g_netDelay = 0;
double g_msSave = 0, g_msLoad = 0, g_msResim = 0;
LONG g_holdPause = 0, g_holdLimit = 0, g_holdSync = 0, g_holdConnect = 0;
double NowMs() {
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return 1000.0 * t.QuadPart / f.QuadPart;
}

int* CtrlForSide(int side) {
	const int idx = *reinterpret_cast<int*>(Live(kGameMgr) + kSideSlotOff + 4 * side);
	BYTE* list = *reinterpret_cast<BYTE**>(Live(kCtrlListPtr));
	return (list && idx >= 0 && idx < 2) ? *reinterpret_cast<int**>(list + 0x20 + 4 * idx) : nullptr;
}

int NetSideOf(const int* ctrl) {
	for (int s = 0; s < 2; ++s)
		if (CtrlForSide(s) == ctrl) return s;
	return -1;
}

constexpr DWORD kLocalSideOff = 0x318;
DWORD ReadLocalMask() {
	int* c = nullptr;
	if (*reinterpret_cast<int*>(Live(kGameMgr) + 8) == 4) {
		const int slot = *reinterpret_cast<int*>(Live(kGameMgr) + 0x5958);
		BYTE* list = *reinterpret_cast<BYTE**>(Live(kCtrlListPtr));
		c = (list && slot >= 0 && slot < 2) ? *reinterpret_cast<int**>(list + 0x20 + 4 * slot) : nullptr;
	} else {
		const int side = *reinterpret_cast<int*>(Live(kGameMgr) + kLocalSideOff);
		c = CtrlForSide(side == 1 ? 1 : 0);
	}
	if (!c) return 0;
	DWORD m = 0;
	const auto query = *reinterpret_cast<Button_t*>(*reinterpret_cast<BYTE**>(c) + 8);
	const int* ids = reinterpret_cast<const int*>(c[0xD]);
	for (int i = 0; i < c[0xE] && i < 32; ++i)
		if (query(c, nullptr, ids[i])) m |= 1u << i;
	if ((m & 1) && (m & 4)) m &= ~5u;
	if ((m & 2) && (m & 8)) m &= ~0xAu;
	return m;
}

std::vector<Slot*> g_slotFree;
Slot* TakeSlot() {
	if (g_slotFree.empty()) return new Slot();
	Slot* s = g_slotFree.back();
	g_slotFree.pop_back();
	return s;
}

int NetChecksum() {
	Region r[kRegions];
	if (!CurrentRegions(r)) return 0;
	return static_cast<int>(FastHash(r[1].addr, r[1].size) ^ FastHash(r[15].addr, r[15].size));
}

constexpr int kSyncEvery = 30, kSyncRing = 64;
constexpr DWORD kHpOff = 0x95C;
struct HashParts { DWORD v[7] = {}; };
struct SyncMark { int frame = -1; DWORD hash = 0; int hp[2] = {}; HashParts parts; };
SyncMark g_syncMarks[kSyncRing];
int g_nextSyncLog = kSyncEvery;

DWORD CrossPcHash() {
	Region r[kRegions];
	if (!CurrentRegions(r)) return 0;
	DWORD h = FastHash(r[15].addr, 0x9C4);
	const BYTE* chars = r[1].addr;
	h ^= (*reinterpret_cast<const DWORD*>(chars + kHpOff) * 2654435761u) ^
	     (*reinterpret_cast<const DWORD*>(chars + 0x20140 + kHpOff) * 40503u);
	const BYTE* mgr = r[0].addr;
	const DWORD extra[] = {*reinterpret_cast<const DWORD*>(chars + 0x248), *reinterpret_cast<const DWORD*>(chars + 0x24C),
	                       *reinterpret_cast<const DWORD*>(chars + 0x20140 + 0x248), *reinterpret_cast<const DWORD*>(chars + 0x20140 + 0x24C),
	                       *reinterpret_cast<const DWORD*>(mgr + 0x63698), *reinterpret_cast<const DWORD*>(mgr + 0x62680)};
	return h ^ FastHash(extra, sizeof(extra)) * 31u;
}
HashParts CrossPcParts() {
	HashParts p;
	Region r[kRegions];
	if (!CurrentRegions(r)) return p;
	const BYTE* chars = r[1].addr;
	const BYTE* mgr = r[0].addr;
	p.v[0] = FastHash(r[15].addr, 0x9C4);
	for (int i = 0; i < 4; ++i)
		p.v[1 + i] = *reinterpret_cast<const DWORD*>(chars + (i / 2) * 0x20140 + 0x248 + (i % 2) * 4);
	p.v[5] = *reinterpret_cast<const DWORD*>(mgr + 0x63698);
	p.v[6] = *reinterpret_cast<const DWORD*>(mgr + 0x62680);
	return p;
}
std::string PartsText(const HashParts& p) {
	char b[128];
	sprintf_s(b, " | rng %08lX pos %ld,%ld %ld,%ld mgr %lX %lX", p.v[0], static_cast<long>(p.v[1]), static_cast<long>(p.v[2]),
	          static_cast<long>(p.v[3]), static_cast<long>(p.v[4]), p.v[5], p.v[6]);
	return b;
}

constexpr int kSnapRing = 16;
struct SyncSnap { int frame = -1; BYTE* addr[2] = {}; std::string data[2]; };
SyncSnap g_syncSnaps[kSnapRing];

void MarkSyncFrame() {
	if (g_net.frame % kSyncEvery) return;
	SyncMark& m = g_syncMarks[(g_net.frame / kSyncEvery) % kSyncRing];
	m.frame = g_net.frame;
	m.hash = CrossPcHash();
	m.parts = CrossPcParts();
	{
		Region r[kRegions];
		SyncSnap& sn = g_syncSnaps[(g_net.frame / kSyncEvery) % kSnapRing];
		sn.frame = -1;
		if (CurrentRegions(r)) {
			for (int i = 0; i < 2; ++i) {
				sn.addr[i] = r[i].addr;
				sn.data[i].assign(reinterpret_cast<char*>(r[i].addr), r[i].size);
			}
			sn.frame = g_net.frame;
		}
	}
	const BYTE* chars = *reinterpret_cast<BYTE**>(Live(kObjMgr) + kCharsPtrOff);
	m.hp[0] = chars ? *reinterpret_cast<const int*>(chars + kHpOff) : -1;
	m.hp[1] = chars ? *reinterpret_cast<const int*>(chars + 0x20140 + kHpOff) : -1;
}

void SendSyncHash(int frame, DWORD hash);

void LogConfirmedSync() {
	const int confirmed = ggpo_get_confirmed_frame(g_net.session);
	while (g_nextSyncLog <= confirmed && g_nextSyncLog <= g_net.frame) {
		const SyncMark& m = g_syncMarks[(g_nextSyncLog / kSyncEvery) % kSyncRing];
		if (m.frame == g_nextSyncLog) {
			Log("SYNC frame %d hash %08lX hp %d %d%s", m.frame, m.hash, m.hp[0], m.hp[1], PartsText(m.parts).c_str());
			SendSyncHash(m.frame, m.hash);
		}
		g_nextSyncLog += kSyncEvery;
	}
}

bool __cdecl NetBeginGame(const char*) { return true; }
bool __cdecl NetSave(unsigned char** buf, int* len, int* checksum, int frame) {
	const double t0 = NowMs();
	struct Add { double t0; ~Add() { g_msSave += NowMs() - t0; } } add{t0};
	Slot* s = TakeSlot();
	if (!SaveSlot(*s, frame)) { g_slotFree.push_back(s); Log("NET: save FAILED at frame %d", frame); return false; }
	*buf = reinterpret_cast<unsigned char*>(s);
	*len = sizeof(Slot);
	*checksum = NetChecksum();
	return true;
}
int g_rbFromFrame = -1, g_rbShownHash = 0, g_rbDepth = 0;
LONG g_rbCount = 0, g_rbChanged = 0, g_rbChangedDepthMax = 0, g_rbChangedDepthSum = 0;
LONG g_rbDepthMaxSec = 0;
constexpr DWORD kPosXOff = 0x248, kPosYOff = 0x24C, kCharStrideOff = 0x20140;
int g_rbShownPos[2][2] = {};
LONG g_jumps = 0, g_jumpMaxPx = 0, g_jumpsLogged = 0;
constexpr DWORD kFlowFlagsOff = 0x62658, kFlowStateOff = 0x62680, kFlowSubOff = 0x62684, kFadeStart = 0x4397A0;
constexpr DWORD kTutMgr = 0x1704A68, kTutCornerArmOff = 0x1F58, kScreenMgr = 0xE78400, kCamPrevXOff = 0xDC;
constexpr DWORD kCornerRight = 0x40000, kCornerLeft = 0x20000;
constexpr int kCornerNear = 1465000, kCornerFar = 1865000, kCenterX = 260000;
static int s_posPending = 0, s_posTicks = 0, s_posTarget[2] = {};
static bool s_posFlags = false;
static bool TrainingFight() {
	return g_poolMem && SceneFighting() && *reinterpret_cast<int*>(Live(kGameModeVa)) == kModeTraining;
}
static int HumanSide() {
	const BYTE* gm = Live(kGameMgr);
	return *reinterpret_cast<const int*>(gm + 0x10) != 1 && *reinterpret_cast<const int*>(gm + 0x168 + 0x10) == 1 ? 1 : 0;
}
static bool CornerRight() { return (g_posPreset == 1 ? HumanSide() : 1 - HumanSide()) == 1; }
static void ClearCornerFlags() {
	if (!s_posFlags) return;
	*reinterpret_cast<DWORD*>(Live(kTutMgr)) &= ~(kCornerRight | kCornerLeft);
	*reinterpret_cast<int*>(Live(kTutMgr) + kTutCornerArmOff) = 0;
	s_posFlags = false;
}
static void ArmCornerFlags() {
	ClearCornerFlags();
	if (g_posPreset == 0) return;
	*reinterpret_cast<DWORD*>(Live(kTutMgr)) |= CornerRight() ? kCornerRight : kCornerLeft;
	*reinterpret_cast<int*>(Live(kTutMgr) + kTutCornerArmOff) = 1;
	s_posFlags = true;
}
void TrainingPositionEnd() {
	s_posPending = 0;
	ClearCornerFlags();
}
constexpr DWORD kTrainingMenu = 0x16E7B58, kMenuOpenCountOff = 0x1ADF4;
static void TrainingMenuUnstick() {
	BYTE* menu = Live(kTrainingMenu);
	int& count = *reinterpret_cast<int*>(menu + kMenuOpenCountOff);
	if ((*reinterpret_cast<DWORD*>(menu) & 0xD) == 5 && count > 0) {
		count = 0;
		Log("TRAINING: hidden menu released");
	}
}
static void TrainingPositionTick() {
	if (!TrainingFight()) { s_posPending = 0; ClearCornerFlags(); return; }
	TrainingMenuUnstick();
	if (g_origIsPaused()) return;
	BYTE* mgr = Live(kObjMgr);
	if (s_posPending != 1 && *reinterpret_cast<int*>(mgr + kFlowStateOff) == 3 && *reinterpret_cast<int*>(mgr + kFlowSubOff) == 1) {
		if (g_posPreset == 0 && !g_posRight) {
			s_posPending = 0;
			ClearCornerFlags();
			return;
		}
		ArmCornerFlags();
		s_posPending = 1;
		s_posTicks = 0;
		Log("POS: reset (preset %d, swap %d, human side %d)", g_posPreset, g_posRight, HumanSide());
	}
	if (!s_posPending) return;
	if (s_posPending == 1) {
		if (*reinterpret_cast<int*>(mgr + kFlowSubOff) == 0) { s_posPending = 2; s_posTicks = 0; }
		else if (++s_posTicks > 300) { s_posPending = 0; ClearCornerFlags(); Log("POS: no restart after 300 frames"); }
		return;
	}
	BYTE* chars = *reinterpret_cast<BYTE**>(mgr + kCharsPtrOff);
	if (!chars) { s_posPending = 0; ClearCornerFlags(); return; }
	int* x[2] = {reinterpret_cast<int*>(chars + kPosXOff), reinterpret_cast<int*>(chars + kCharStrideOff + kPosXOff)};
	if (s_posPending == 2) {
		if (++s_posTicks < 2) return;
		if (g_posPreset == 0) {
			s_posTarget[0] = -kCenterX;
			s_posTarget[1] = kCenterX;
		} else if (CornerRight()) {
			s_posTarget[0] = kCornerNear;
			s_posTarget[1] = kCornerFar;
		} else {
			s_posTarget[0] = -kCornerFar;
			s_posTarget[1] = -kCornerNear;
		}
		int cam = g_posPreset == 0 ? 0 : CornerRight() ? 0x500 : -0x500;
		if (g_posRight) {
			s_posTarget[0] = -s_posTarget[0];
			s_posTarget[1] = -s_posTarget[1];
			cam = -cam;
		}
		*reinterpret_cast<int*>(Live(kScreenMgr) + kCamPrevXOff) = cam;
		Log("POS: P1 %d P2 %d -> P1 %d P2 %d, camera %d", *x[0], *x[1], s_posTarget[0], s_posTarget[1], cam);
		ClearCornerFlags();
		s_posPending = 3;
		s_posTicks = 0;
	}
	++s_posTicks;
	const bool moved0 = *x[0] - s_posTarget[0] > 100000 || s_posTarget[0] - *x[0] > 100000;
	const bool moved1 = *x[1] - s_posTarget[1] > 100000 || s_posTarget[1] - *x[1] > 100000;
	if (s_posTicks <= 4 || (s_posTicks <= 20 && moved0 && moved1)) {
		if (s_posTicks > 4) Log("POS: moved back at frame %d (P1 %d P2 %d), placing again", s_posTicks, *x[0], *x[1]);
		*x[0] = s_posTarget[0];
		*x[1] = s_posTarget[1];
	}
	if (s_posTicks >= 20) { s_posPending = 0; Log("POS: done, P1 %d P2 %d", *x[0], *x[1]); }
}
bool ReadFighterPos(int out[2][2]) {
	const BYTE* chars = *reinterpret_cast<BYTE**>(Live(kObjMgr) + kCharsPtrOff);
	if (!chars) return false;
	for (int c = 0; c < 2; ++c) {
		out[c][0] = *reinterpret_cast<const int*>(chars + c * kCharStrideOff + kPosXOff);
		out[c][1] = *reinterpret_cast<const int*>(chars + c * kCharStrideOff + kPosYOff);
	}
	return true;
}
bool __cdecl NetLoad(unsigned char* buf, int) {
	struct Add { double t0; ~Add() { g_msLoad += NowMs() - t0; } } add{NowMs()};
	if (!buf) { Log("NET: load of an empty state (save had failed). Desync likely."); return false; }
	const Slot* s = reinterpret_cast<const Slot*>(buf);
	if (g_rbFromFrame < 0 && s->frame < g_net.frame) {
		g_rbFromFrame = g_net.frame;
		g_rbDepth = g_net.frame - s->frame;
		if (g_rbDepth > g_rbDepthMaxSec) g_rbDepthMaxSec = g_rbDepth;
		g_rbShownHash = NetChecksum();
		ReadFighterPos(g_rbShownPos);
	}
	if (!LoadSlot(*s)) { Log("NET: load FAILED (frame %ld): regions moved. Desync likely.", s->frame); return false; }
	g_net.frame = s->frame;
	return true;
}
bool __cdecl NetLogState(char*, unsigned char*, int) { return true; }
void __cdecl NetFree(void* buf) { if (buf) g_slotFree.push_back(static_cast<Slot*>(buf)); }

constexpr DWORD kMatchMenuSite = 0x49801C, kMatchMenuFn = 0x56E400;
using MatchMenu_t = int(__cdecl*)();
MatchMenu_t g_origMatchMenu = nullptr;
bool g_inReplayUpdate = false;
int __cdecl HookMatchMenu() {
	if (g_net.session && g_inReplayUpdate) return 0;
	return g_origMatchMenu();
}

bool __cdecl NetAdvance(int) {
	int disc = 0;
	ggpo_synchronize_input(g_net.session, g_netIn, sizeof(g_netIn), &disc);
	if (SceneFighting()) {
		struct Add { double t0; ~Add() { g_msResim += NowMs() - t0; } } add{NowMs()};
		g_netInUpdate = true;
		g_inReplayUpdate = true;
		g_origUpdate(g_net.scene, nullptr, 0);
		g_inReplayUpdate = false;
		g_netInUpdate = false;
		EffectStep();
	}
	++g_net.frame;
	MarkSyncFrame();
	++g_netRollbackFrames;
	if (g_rbFromFrame >= 0 && g_net.frame >= g_rbFromFrame) {
		++g_rbCount;
		if (NetChecksum() != g_rbShownHash) {
			++g_rbChanged;
			g_rbChangedDepthSum += g_rbDepth;
			if (g_rbDepth > g_rbChangedDepthMax) g_rbChangedDepthMax = g_rbDepth;
		}
		int now[2][2];
		if (ReadFighterPos(now)) {
			int px[2][2], worst = 0;
			for (int c = 0; c < 2; ++c)
				for (int k = 0; k < 2; ++k) {
					px[c][k] = (now[c][k] - g_rbShownPos[c][k]) / 1000;
					worst = (std::max)(worst, std::abs(px[c][k]));
				}
			if (worst > 0) {
				++g_jumps;
				if (worst > g_jumpMaxPx) g_jumpMaxPx = worst;
				if (worst >= 8 && g_jumpsLogged < 300) {
					++g_jumpsLogged;
					Log("NET JUMP frame %d, %d frames deep: P1 moved %+d,%+d px, P2 moved %+d,%+d px", g_net.frame,
					    g_rbDepth, px[0][0], px[0][1], px[1][0], px[1][1]);
				}
			}
		}
		g_rbFromFrame = -1;
	}
	ggpo_advance_frame(g_net.session);
	return true;
}

bool __cdecl NetEvent(GGPOEvent* e) {
	switch (e->code) {
	case GGPO_EVENTCODE_CONNECTED_TO_PEER: Log("NET: connected to peer"); break;
	case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER: Log("NET: synchronized with peer"); break;
	case GGPO_EVENTCODE_RUNNING: Log("NET: running"); g_net.running = true; break;
	case GGPO_EVENTCODE_TIMESYNC: PaceOnRecommendation(e->u.timesync.frames_ahead); break;
	case GGPO_EVENTCODE_CONNECTION_INTERRUPTED: Log("NET: connection interrupted"); break;
	case GGPO_EVENTCODE_CONNECTION_RESUMED: Log("NET: connection resumed"); break;
	case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER: Log("NET: peer disconnected"); break;
	default: break;
	}
	return true;
}

constexpr DWORD kNetState = 0xBD0AA8, kNetSession = 0xBD2AC0;
constexpr DWORD kNetWaitSite = 0x56E89F, kNetWaitFn = 0x45F9D0;
constexpr DWORD kNetSendSite = 0x56F1AA, kNetSendFn = 0x45F790;
constexpr DWORD kNetFetchSite = 0x56F1CC, kNetFetchFn = 0x56E970;
constexpr DWORD kLocalSlotOff = 0x5958;
constexpr int kSteamChannel = 1;

struct P2PState {
	BYTE active, connecting, error, relay;
	int bytesQueued, packetsQueued;
	DWORD remoteIp;
	WORD remotePort;
};
struct ISteamNet {
	virtual bool Send(unsigned long long to, const void* data, unsigned len, int type, int channel) = 0;
	virtual bool Available(unsigned* len, int channel) = 0;
	virtual bool Read(void* dst, unsigned cap, unsigned* len, unsigned long long* from, int channel) = 0;
	virtual bool AcceptSession(unsigned long long who) = 0;
	virtual bool CloseSession(unsigned long long who) = 0;
	virtual bool CloseChannel(unsigned long long who, int channel) = 0;
	virtual bool SessionState(unsigned long long who, P2PState* state) = 0;
};
ISteamNet* g_steamNet = nullptr;
unsigned long long g_steamPeer = 0;
DWORD g_netStartTick = 0;
bool g_netRealAdvance = false;

int __cdecl SteamSend(const char* buf, int len) {
	return g_steamNet->Send(g_steamPeer, buf, static_cast<unsigned>(len), 0, kSteamChannel) ? len : 0;
}
#ifdef BBCSE_DEV_TOOLS
DWORD g_fakeLagMs = 0;
struct HeldPacket { DWORD due; std::string data; };
std::vector<HeldPacket> g_heldPackets;
#endif

int SteamRecvNow(char* buf, int cap) {
	unsigned size = 0, got = 0;
	unsigned long long from = 0;
	while (g_steamNet->Available(&size, kSteamChannel)) {
		if (!g_steamNet->Read(buf, static_cast<unsigned>(cap), &got, &from, kSteamChannel)) return 0;
		if (from == g_steamPeer && got > 0) return static_cast<int>(got);
	}
	return 0;
}
int __cdecl SteamRecv(char* buf, int cap) {
#ifndef BBCSE_DEV_TOOLS
	return SteamRecvNow(buf, cap);
#else
	if (!g_fakeLagMs) return SteamRecvNow(buf, cap);
	int got;
	while ((got = SteamRecvNow(buf, cap)) > 0) g_heldPackets.push_back({GetTickCount() + g_fakeLagMs, std::string(buf, got)});
	if (g_heldPackets.empty() || static_cast<LONG>(GetTickCount() - g_heldPackets.front().due) < 0) return 0;
	const std::string& d = g_heldPackets.front().data;
	const int n = static_cast<int>(d.size() < static_cast<size_t>(cap) ? d.size() : cap);
	memcpy(buf, d.data(), n);
	g_heldPackets.erase(g_heldPackets.begin());
	return n;
#endif
}

bool SteamMode() { return g_net.session && g_net.steam; }

BYTE* NetPeer(int side) {
	return *reinterpret_cast<BYTE**>(Live(kNetSession) + 0x12048 + 4 * side);
}

using NetWait_t = void*(__fastcall*)(void* self, void* edx, int arg);
using NetSend_t = int(__fastcall*)(void* self, void* edx, unsigned side, unsigned input);
using NetFetch_t = unsigned(__fastcall*)(void* self, void* edx, int slot);
NetWait_t g_origNetWait = nullptr;
NetSend_t g_origNetSend = nullptr;
NetFetch_t g_origNetFetch = nullptr;

void* __fastcall HookNetWait(void* self, void* edx, int arg) {
	if (!SteamMode()) return g_origNetWait(self, edx, arg);
	BYTE* ns = static_cast<BYTE*>(self);
	*reinterpret_cast<int*>(ns + 0x14) = 0;
	*reinterpret_cast<int*>(ns + 0x18) = 0;
	return self;
}
void AskForMissingInputs() {
	static WORD lastCount[2] = {0xFFFF, 0xFFFF};
	static int stale[2] = {};
	static unsigned lastRound = 0xFF;
	static int sinceRoundChange = 0;
	BYTE* ses = Live(kNetSession);
	const unsigned cur = ses[0x11AAA] & 1;
	if (cur != lastRound) { lastRound = cur; sinceRoundChange = 0; }
	else ++sinceRoundChange;
	const int opp = 1 - g_net.side;
	for (const unsigned r : {cur, cur ^ 1u}) {
		WORD* counts = reinterpret_cast<WORD*>(ses + 0x160 + (36000 + r * 2) * 2);
		const WORD count = counts[opp];
		if (r != cur && (sinceRoundChange > 600 || count >= counts[g_net.side])) continue;
		if (count != lastCount[r]) { lastCount[r] = count; stale[r] = 0; continue; }
		if (++stale[r] % 30) continue;
		const BYTE direct = ses[0x11D05];
		BYTE* to = *reinterpret_cast<BYTE**>(ses + ((direct & 0x20) && (direct & 0x40) ? 0x12048 + 4 * opp : 0x12044));
		if (!to) return;
		WORD msg[5] = {10, 0x2B, count, static_cast<WORD>(opp), ses[0x11AA8 + r]};
		using Queue_t = int(__fastcall*)(void* net, void* edx, WORD* msg, void* addr);
		using GetNet_t = void*(__cdecl*)();
		reinterpret_cast<Queue_t>(Live(0x404140))(reinterpret_cast<GetNet_t>(Live(0x4044F0))(), nullptr, msg, to + 0x70);
	}
}

int AppliedInputWord(int side) {
	BYTE* gm = Live(kGameMgr);
	const int slot = *reinterpret_cast<int*>(gm + kSideSlotOff + 4 * side);
	if (slot < 0) return -1;
	using ToWord_t = unsigned short(__fastcall*)(void* gm, void* edx, int slot, unsigned mask, int flip);
	return reinterpret_cast<ToWord_t>(Live(0x456050))(gm, nullptr, slot, g_netIn[side], 0);
}
void* g_netSendSelf = nullptr;
int __fastcall HookNetSend(void* self, void* edx, unsigned side, unsigned input) {
	g_netSendSelf = self;
	if (SteamMode() && !g_netRealAdvance) return 1;
	if (SteamMode() && side < 2) {
		const int word = AppliedInputWord(static_cast<int>(side));
		if (word >= 0) input = static_cast<unsigned>(word);
		BYTE* ses = Live(kNetSession);
		WORD* count = reinterpret_cast<WORD*>(ses + 0x160 + (side + 36000 + ses[0x11AAA] * 2u) * 2);
		const WORD index = *reinterpret_cast<WORD*>(ses + 0x11AB2);
		if (static_cast<int>(side) == g_net.side && *count != index) {
			static int logged = 0;
			if (logged++ < 4) Log("NET: input slot %u realigned to frame index %u (side %u)", *count, index, side);
			*count = index;
		}
	}
	const int sent = g_origNetSend(self, edx, side, input);
	if (SteamMode() && static_cast<int>(side) == g_net.side) AskForMissingInputs();
	return sent;
}
constexpr DWORD kNetIndexSite = 0x56F75E, kNetIndexFn = 0x45F9B0;
using NetIndex_t = void(__fastcall*)(void* netstate, void* edx);
NetIndex_t g_origNetIndex = nullptr;
void __fastcall HookNetIndex(void* netstate, void* edx) {
	if (SteamMode() && !g_netRealAdvance) return;
	g_origNetIndex(netstate, edx);
}
constexpr DWORD kNetInputMsgSite = 0x4691A3, kNetInputMsgFn = 0x45FF50;
constexpr DWORD kNetRelayMsgSite = 0x4691E0, kNetRelayMsgFn = 0x4603E0;
constexpr DWORD kNetRelayReplySite = 0x4691BE, kNetRelayReplyFn = 0x4602A0;
constexpr DWORD kNetResendSite = 0x4691CF, kNetResendFn = 0x460030;
constexpr WORD kNetBufFrames = 9000;
using NetMsg_t = void(__fastcall*)(void* self, void* edx, void* msg, void* ctx);
NetMsg_t g_origNetInputMsg = nullptr;
NetMsg_t g_origNetRelayMsg = nullptr;
NetMsg_t g_origNetRelayReply = nullptr;
NetMsg_t g_origNetResend = nullptr;
LONG g_netMsgDropped = 0;
bool NetMsgFramesOk(void* msg, BYTE maxCount) {
	BYTE* m = static_cast<BYTE*>(msg);
	const WORD frame = *reinterpret_cast<const WORD*>(m + 4);
	if (frame >= kNetBufFrames || m[8] > maxCount) {
		if (g_netMsgDropped++ < 4) Log("NET: dropped message %02X (frame %u, count %u)", m[2], frame, m[8]);
		return false;
	}
	if (m[8] > frame) m[8] = static_cast<BYTE>(frame);
	return true;
}
void __fastcall HookNetRelayReply(void* self, void* edx, void* msg, void* ctx) {
	if (NetMsgFramesOk(msg, 40)) g_origNetRelayReply(self, edx, msg, ctx);
}
void __fastcall HookNetResend(void* self, void* edx, void* msg, void* ctx) {
	const BYTE* m = static_cast<const BYTE*>(msg);
	const WORD frame = *reinterpret_cast<const WORD*>(m + 4);
	const WORD side = *reinterpret_cast<const WORD*>(m + 6);
	if (side > 1 || frame >= kNetBufFrames) {
		if (g_netMsgDropped++ < 4) Log("NET: dropped message 2B (frame %u, side %u)", frame, side);
		return;
	}
	g_origNetResend(self, edx, msg, ctx);
}
DWORD g_netInputGraceUntil = 0;
void __fastcall HookNetInputMsg(void* self, void* edx, void* msg, void* ctx) {
	if (static_cast<const BYTE*>(msg)[7] > 1 || !NetMsgFramesOk(msg, 20)) return;
	if (!SteamMode() && static_cast<LONG>(GetTickCount() - g_netInputGraceUntil) > 0)
		return g_origNetInputMsg(self, edx, msg, ctx);
	BYTE* ses = Live(kNetSession);
	const BYTE tag = static_cast<const BYTE*>(msg)[6];
	const BYTE round = ses[0x11AAA];
	const unsigned cur = round & 1;
	const bool previous = tag != ses[0x11AA8 + cur] && tag == ses[0x11AA8 + (cur ^ 1)];
	WORD* idx = reinterpret_cast<WORD*>(ses + 0x11AB2);
	const WORD keep = *idx;
	*idx = 0;
	if (previous) ses[0x11AAA] = static_cast<BYTE>(cur ^ 1);
	g_origNetInputMsg(self, edx, msg, ctx);
	ses[0x11AAA] = round;
	*idx = keep;
}
void __fastcall HookNetRelayMsg(void* self, void* edx, void* msg, void* ctx) {
	if (!SteamMode()) return g_origNetRelayMsg(self, edx, msg, ctx);
	WORD* count = reinterpret_cast<WORD*>(Live(kNetSession) + 0x160 + 36000 * 2);
	WORD keep[4];
	for (int r = 0; r < 2; ++r) {
		keep[r * 2] = count[r * 2];
		keep[r * 2 + 1] = count[r * 2 + 1];
		const WORD both = (std::min)(count[r * 2], count[r * 2 + 1]);
		count[r * 2] = count[r * 2 + 1] = both;
	}
	g_origNetRelayMsg(self, edx, msg, ctx);
	for (int i = 0; i < 4; ++i) count[i] = keep[i];
}
void LogNetCounters(const char* who) {
	BYTE* ses = Live(kNetSession);
	BYTE* self = *reinterpret_cast<BYTE**>(ses + 0x12040);
	const unsigned round = ses[0x11AAA];
	const unsigned idx = *reinterpret_cast<WORD*>(ses + 0x11AB2);
	const unsigned c0 = *reinterpret_cast<WORD*>(ses + 0x160 + (0 + 36000 + round * 2) * 2);
	const unsigned c1 = *reinterpret_cast<WORD*>(ses + 0x160 + (1 + 36000 + round * 2) * 2);
	Log("NETSTATE %s: flags %04X, round %u, frame index %u, inputs P1 %u / P2 %u, resend requests %u, waits %u", who,
		self ? *reinterpret_cast<WORD*>(self + 0x5A) : 0, round, idx, c0, c1,
		*reinterpret_cast<WORD*>(ses + 0x11AAC), *reinterpret_cast<WORD*>(ses + 0x11AB0));
}
unsigned __fastcall HookNetFetch(void* self, void* edx, int slot) {
	if (SteamMode()) {
		BYTE* gm = Live(kGameMgr);
		int side = -1;
		if (*reinterpret_cast<int*>(gm + kSideSlotOff) == slot) side = 0;
		else if (*reinterpret_cast<int*>(gm + kSideSlotOff + 4) == slot) side = 1;
		if (side < 0) return 0;
		const int word = AppliedInputWord(side);
		return word < 0 ? 0 : static_cast<unsigned>(word);
	}
	return g_origNetFetch(self, edx, slot);
}

void SendVersionTag();
bool StartSteamSession(GGPOSessionCallbacks& cb, int delay) {
	if (*reinterpret_cast<int*>(Live(kGameMgr) + 8) != 4) { Log("NET: not an online battle; rollback off"); return false; }
	BYTE* self = *reinterpret_cast<BYTE**>(Live(kNetSession) + 0x12040);
	if (!self) return false;
	const WORD flags = *reinterpret_cast<WORD*>(self + 0x5A);
	if (!(flags & 0x10)) { g_spectating = true; Log("NET: spectating; rollback off"); return false; }
	g_net.side = flags & 1;
	BYTE* peer = NetPeer(1 - g_net.side);
	if (!peer) { Log("NET: no opponent object; rollback off"); return false; }
	g_steamPeer = *reinterpret_cast<unsigned long long*>(peer + 0x70);
	using GetNet_t = ISteamNet*(__cdecl*)();
	const auto getNet = reinterpret_cast<GetNet_t>(GetProcAddress(GetModuleHandleA("steam_api.dll"), "SteamNetworking"));
	g_steamNet = getNet ? getNet() : nullptr;
	if (!g_steamNet || !g_steamPeer) { Log("NET: Steam networking unavailable; rollback off"); return false; }
	char drain[1500];
	while (SteamRecv(drain, sizeof(drain)) > 0) {}
#ifdef BBCSE_DEV_TOOLS
	g_heldPackets.clear();
#endif

	srand(GetTickCount() ^ GetCurrentProcessId());
	ggpo_bb_send = SteamSend;
	ggpo_bb_recv = SteamRecv;
	ggpo_bb_peer_addr = {};
	ggpo_bb_peer_addr.sin_family = AF_INET;
	ggpo_bb_peer_addr.sin_addr.s_addr = htonl(0x0A000001);
	ggpo_bb_peer_addr.sin_port = htons(7000);
	char game[] = "bbcse";
	ggpo_start_session(&g_net.session, &cb, game, 2, sizeof(DWORD), 7000);
	ggpo_set_disconnect_timeout(g_net.session, 5000);
	ggpo_set_disconnect_notify_start(g_net.session, 1000);
	GGPOPlayer me_p{};
	me_p.size = sizeof(me_p);
	me_p.type = GGPO_PLAYERTYPE_LOCAL;
	me_p.player_num = g_net.side + 1;
	GGPOPlayer them = me_p;
	them.type = GGPO_PLAYERTYPE_REMOTE;
	them.player_num = 2 - g_net.side;
	strcpy_s(them.u.remote.ip_address, "10.0.0.1");
	them.u.remote.port = 7000;
	if (!GGPO_SUCCEEDED(ggpo_add_player(g_net.session, &me_p, &g_net.local)) ||
	    !GGPO_SUCCEEDED(ggpo_add_player(g_net.session, &them, &g_net.remote))) {
		Log("NET: add_player failed; rollback off");
		ggpo_close_session(g_net.session);
		g_net.session = nullptr;
		return false;
	}
	ggpo_set_frame_delay(g_net.session, g_net.local, delay);
	g_netDelay = delay;
	g_net.steam = true;
	g_netStartTick = GetTickCount();
	Log("NET: Steam session: I am side %d, delay %d", g_net.side, delay);
	SendVersionTag();
	return true;
}

constexpr DWORD kDrawBeginSite = 0x4980EE, kDrawBeginFn = 0x5092C0;
constexpr DWORD kRoundCheckSite = 0x49812E, kRoundCheckFn = 0x490BB0;
using DrawBegin_t = void(__cdecl*)();
using RoundCheck_t = bool(__fastcall*)(void* flags);
DrawBegin_t g_origDrawBegin = nullptr;
RoundCheck_t g_origRoundCheck = nullptr;
BYTE g_rngDrawCopy[kRngSize];
bool g_rngDrawSaved = false;
BYTE g_rngAfterTick[kRngSize];
bool g_rngAfterValid = false;
LONG g_rngDrawFixes = 0, g_rngOutsideFixes = 0;

BYTE* Rng1() {
	BYTE* rng = *reinterpret_cast<BYTE**>(Live(kRngArrayPtr));
	return rng ? rng + kRngSize : nullptr;
}
void __cdecl HookDrawBegin() {
	BYTE* r = Rng1();
	g_rngDrawSaved = (g_netActive || (g_spectating && SceneFighting())) && r;
	if (g_rngDrawSaved) memcpy(g_rngDrawCopy, r, kRngSize);
	g_origDrawBegin();
}
bool __fastcall HookRoundCheck(void* flags) {
	if (g_rngDrawSaved) {
		g_rngDrawSaved = false;
		BYTE* r = Rng1();
		if (r && memcmp(r, g_rngDrawCopy, kRngSize) != 0) {
			memcpy(r, g_rngDrawCopy, kRngSize);
			++g_rngDrawFixes;
			if (g_spectating) ++g_specRngFixes;
		}
	}
	return g_origRoundCheck(flags);
}
void RngMarkTick() {
	BYTE* r = Rng1();
	g_rngAfterValid = r != nullptr;
	if (r) memcpy(g_rngAfterTick, r, kRngSize);
}
void RngCheckBetweenTicks() {
	BYTE* r = Rng1();
	if (r && g_rngAfterValid && memcmp(r, g_rngAfterTick, kRngSize) != 0) {
		memcpy(r, g_rngAfterTick, kRngSize);
		++g_rngOutsideFixes;
		if (g_spectating) ++g_specRngFixes;
	}
}

constexpr int kHashChannel = 2;
struct PeerHash { int frame = -1; DWORD hash = 0; };
PeerHash g_peerHashes[kSyncRing];
bool g_desyncDumped = false;
int g_lastMatched = -1;

void SendSyncHash(int frame, DWORD hash) {
	if (!g_net.steam || !g_steamNet) return;
	const DWORD msg[3] = {0x48535942, static_cast<DWORD>(frame), hash};
	g_steamNet->Send(g_steamPeer, msg, sizeof(msg), 2, kHashChannel);
}

void DesyncDump(int frame, DWORD mine, DWORD theirs) {
	const SyncSnap& sn = g_syncSnaps[(frame / kSyncEvery) % kSnapRing];
	Log("NET DESYNC: frame %d, my hash %08lX, peer %08lX", frame, mine, theirs);
	g_ovl.desyncFrame = frame;
	if (sn.frame != frame) { Log("  (no state copy kept for that frame any more)"); return; }
	char path[MAX_PATH];
	GetModuleFileNameA(g_self, path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	*(slash ? slash + 1 : path) = 0;
	strcat_s(path, "bbcse-desync.bin");
	FILE* f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || !f) { Log("  could not write bbcse-desync.bin"); return; }
	const DWORD hdr[3] = {0x53444242, static_cast<DWORD>(frame), static_cast<DWORD>(g_net.side)};
	fwrite(hdr, 4, 3, f);
	const char* names[2] = {"objmgr", "chars"};
	for (int i = 0; i < 2; ++i) {
		char nm[16] = {};
		strncpy_s(nm, names[i], _TRUNCATE);
		const DWORD addr = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(sn.addr[i]));
		const DWORD size = static_cast<DWORD>(sn.data[i].size());
		fwrite(nm, 1, 16, f);
		fwrite(&addr, 4, 1, f);
		fwrite(&size, 4, 1, f);
		fwrite(sn.data[i].data(), 1, size, f);
	}
	fclose(f);
	Log("  state of frame %d written to bbcse-desync.bin (send this file)", frame);
}

constexpr DWORD kVersionTag = 0x52565942;
void SendVersionTag() {
	if (!g_steamNet || !g_steamPeer) return;
	DWORD msg[3] = {kVersionTag, 0, 0};
	memcpy(&msg[1], g_versionName.c_str(), (std::min)(g_versionName.size(), static_cast<size_t>(8)));
	g_steamNet->Send(g_steamPeer, msg, sizeof(msg), 2, kHashChannel);
	strcpy_s(g_ovl.version, g_versionName.c_str());
	g_ovl.peerVersion[0] = 0;
	g_ovl.versionMismatch = false;
}
void NotePeerVersion(const DWORD* msg) {
	char peer[9] = {};
	memcpy(peer, &msg[1], 8);
	for (char& c : peer)
		if (c && (static_cast<unsigned char>(c) < 32 || c == 127)) c = '?';
	if (strcmp(g_ovl.peerVersion, peer) == 0) return;
	strcpy_s(g_ovl.peerVersion, peer);
	g_ovl.versionMismatch = g_versionName != peer;
	Log("VERSION: opponent plays %s, this PC %s%s", peer, g_versionName.c_str(), g_ovl.versionMismatch ? "  MISMATCH: the match will desync" : "");
}
void PumpSyncHashes() {
	if (!g_net.steam || !g_steamNet) return;
	unsigned size = 0, got = 0;
	unsigned long long from = 0;
	DWORD msg[3];
	while (g_steamNet->Available(&size, kHashChannel)) {
		if (!g_steamNet->Read(msg, sizeof(msg), &got, &from, kHashChannel)) break;
		if (from == g_steamPeer && got == sizeof(msg) && msg[0] == kVersionTag) {
			NotePeerVersion(msg);
			continue;
		}
		if (from != g_steamPeer || got != sizeof(msg) || msg[0] != 0x48535942) continue;
		PeerHash& ph = g_peerHashes[(msg[1] / kSyncEvery) % kSyncRing];
		ph.frame = static_cast<int>(msg[1]);
		ph.hash = msg[2];
	}
	if (g_desyncDumped) return;
	for (const PeerHash& ph : g_peerHashes) {
		if (ph.frame < 0 || ph.frame >= g_nextSyncLog) continue;
		const SyncMark& m = g_syncMarks[(ph.frame / kSyncEvery) % kSyncRing];
		if (m.frame == ph.frame && m.hash == ph.hash && ph.frame > g_lastMatched) {
			g_lastMatched = ph.frame;
			++g_ovl.syncChecks;
		}
		if (m.frame == ph.frame && m.hash != ph.hash) {
			g_desyncDumped = true;
			DesyncDump(ph.frame, m.hash, ph.hash);
			return;
		}
	}
}

void NetLatencyPoll(int side) {
	if (g_latPending && g_netRealAdvance && side == g_net.side && g_netIn[side] == g_latPendingMask) {
		const LONG lat = g_net.frame - g_latPendingFrame;
		g_latSum += lat;
		++g_latCount;
		if (lat > g_latMax) g_latMax = lat;
		g_latPending = false;
	}
}

void NetStop() {
	if (!g_net.session) return;
	ggpo_close_session(g_net.session);
	g_net = NetState();
	g_ovl.session = false;
	g_rngAfterValid = false;
	PaceReset();
	ggpo_bb_send = nullptr;
	ggpo_bb_recv = nullptr;
	g_netActive = false;
	g_netInputGraceUntil = GetTickCount() + 10000;
	Log("NET: session closed");
}

void ApplyVersionRules();
void NetBattleStart() {
	NetStop();
	EnterCriticalSection(&g_versionLock);
	g_versionLoaded.clear();
	LeaveCriticalSection(&g_versionLock);
	if (g_versionName != "cse" || g_gameRulesSaved) ApplyVersionRules();
	g_spectating = false;
	g_replayFrame = 0;
	g_specBase = 0;
	g_specRound = g_specIdx = -1;
	g_steamRearm = false;
	char ini[MAX_PATH];
	GetModuleFileNameA(g_self, ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	*(slash ? slash + 1 : ini) = 0;
	strcat_s(ini, "bbcse-net.ini");
	if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
	char mode[16];
	GetPrivateProfileStringA("net", "mode", "", mode, sizeof(mode), ini);
	const bool steam = _stricmp(mode, "steam") == 0;
	const int delay = GetPrivateProfileIntA("net", "delay", 2, ini);
#ifdef BBCSE_DEV_TOOLS
	char remote[64];
	g_heldPackets.clear();
	g_net.synctest = _stricmp(mode, "synctest") == 0;
	if (!g_net.synctest && !steam && _stricmp(mode, "p2p") != 0) { Log("NET: bbcse-net.ini has no mode=steam/p2p/synctest; network off"); return; }
	g_net.side = GetPrivateProfileIntA("net", "side", 0, ini) ? 1 : 0;
	g_fakeLagMs = static_cast<DWORD>(GetPrivateProfileIntA("net", "fakelag", 0, ini));
	if (g_fakeLagMs) Log("NET: fakelag %lu ms", g_fakeLagMs);
	g_net.seed = static_cast<DWORD>(GetPrivateProfileIntA("net", "seed", 12345, ini));
	const int port = GetPrivateProfileIntA("net", "localport", 7000, ini);
	g_poolCheck = GetPrivateProfileIntA("net", "savecheck", 0, ini) != 0;
	GetPrivateProfileStringA("net", "remote", "", remote, sizeof(remote), ini);
#else
	if (!steam) { Log("NET: bbcse-net.ini has no mode=steam; network off"); return; }
#endif

	GGPOSessionCallbacks cb{};
	cb.begin_game = NetBeginGame;
	cb.save_game_state = NetSave;
	cb.load_game_state = NetLoad;
	cb.log_game_state = NetLogState;
	cb.free_buffer = NetFree;
	cb.advance_frame = NetAdvance;
	cb.on_event = NetEvent;
	char game[] = "bbcse";
	GGPOPlayer me{};
	me.size = sizeof(me);
	me.type = GGPO_PLAYERTYPE_LOCAL;
	me.player_num = g_net.side + 1;
	GGPOPlayer them = me;
	them.player_num = 2 - g_net.side;
	if (steam) {
		if (!StartSteamSession(cb, delay)) { g_net = NetState(); return; }
	}
#ifdef BBCSE_DEV_TOOLS
	else if (g_net.synctest) {
		ggpo_start_synctest(&g_net.session, &cb, game, 2, sizeof(DWORD), 7);
		ggpo_add_player(g_net.session, &me, &g_net.local);
		ggpo_add_player(g_net.session, &them, &g_net.remote);
		Log("NET: synctest session started (rolls back 7 frames, compares)");
	} else {
		char* colon = strrchr(remote, ':');
		if (!colon) { Log("NET: remote must be ip:port, got '%s'; network off", remote); return; }
		*colon = 0;
		srand(GetTickCount() ^ GetCurrentProcessId());
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		ggpo_start_session(&g_net.session, &cb, game, 2, sizeof(DWORD), static_cast<unsigned short>(port));
		ggpo_set_disconnect_timeout(g_net.session, 5000);
		ggpo_set_disconnect_notify_start(g_net.session, 1000);
		them.type = GGPO_PLAYERTYPE_REMOTE;
		strcpy_s(them.u.remote.ip_address, remote);
		them.u.remote.port = static_cast<unsigned short>(atoi(colon + 1));
		if (!GGPO_SUCCEEDED(ggpo_add_player(g_net.session, &me, &g_net.local)) ||
		    !GGPO_SUCCEEDED(ggpo_add_player(g_net.session, &them, &g_net.remote))) {
			Log("NET: add_player failed; network off");
			NetStop();
			return;
		}
		ggpo_set_frame_delay(g_net.session, g_net.local, delay);
		Log("NET: p2p session: I am side %d on port %d, peer %s:%s, delay %d", g_net.side, port, remote, colon + 1, delay);
	}
#endif
	for (auto& m : g_syncMarks) m = SyncMark();
	for (auto& ph : g_peerHashes) ph = PeerHash();
	g_desyncDumped = false;
	g_lastMatched = -1;
	g_ovl.desyncFrame = -1;
	g_ovl.syncChecks = 0;
	g_nextSyncLog = kSyncEvery;
	g_syncN = 0;
	g_inMode = InMode::Idle;
	g_netActive = true;
}

void NetBarrier() {
	if (!g_net.session || g_net.synctest || g_net.frame == 0) return;
	const DWORD start = GetTickCount();
	while (ggpo_get_confirmed_frame(g_net.session) < g_net.frame - 1) {
		ggpo_idle(g_net.session, 1);
		if (GetTickCount() - start > 5000) { Log("NET: round barrier timed out at frame %d", g_net.frame); break; }
	}
	ggpo_idle(g_net.session, 0);
	RngMarkTick();
}

void PadMatchEndInputs() {
	if (!g_netSendSelf || g_net.side < 0) return;
	BYTE* ses = Live(kNetSession);
	const WORD count = *reinterpret_cast<WORD*>(ses + 0x160 + (g_net.side + 36000 + ses[0x11AAA] * 2u) * 2);
	if (count + 30 >= 9000) return;
	const int word = AppliedInputWord(g_net.side);
	for (int i = 0; i < 30; ++i) g_origNetSend(g_netSendSelf, nullptr, g_net.side, word < 0 ? 0 : word);
	Log("NET: match end: sent 30 spare input frames after %u", count);
}

void NetRoundEnd() {
	const DWORD start = GetTickCount();
	while (ggpo_get_confirmed_frame(g_net.session) < g_net.frame - 1 && GetTickCount() - start < 5000)
		ggpo_idle(g_net.session, 1);
	ggpo_idle(g_net.session, 0);
	Log("NET: round end at frame %d, confirmed %d after %lu ms; scene state %d", g_net.frame,
		ggpo_get_confirmed_frame(g_net.session), GetTickCount() - start,
		*reinterpret_cast<int*>(static_cast<BYTE*>(g_sceneForSlots) + kSceneStateOff));
	if (SceneFighting()) {
		Log("NET: that round end was a misprediction; the fight continues");
		return;
	}
	const bool steam = g_net.steam;
	if (steam) PadMatchEndInputs();
	NetStop();
	g_steamRearm = steam;
}

void NetTick(void* self, void* edx, int arg) {
	if (g_net.steam && arg == 0) return;
	if (g_net.steam && !g_net.running && GetTickCount() - g_netStartTick > 15000) {
		Log("NET: opponent did not answer in 15 s (no mod?). Rollback off, the game's netcode continues.");
		NetStop();
		g_origUpdate(self, edx, arg);
		return;
	}
	g_net.scene = self;
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	RngCheckBetweenTicks();
	ggpo_idle(g_net.session, 0);
	if (g_net.steam && !SceneFighting()) {
		NetRoundEnd();
		g_holdSim = true;
		g_origUpdate(self, edx, arg);
		g_holdSim = false;
		return;
	}
	if (!g_net.synctest) LogConfirmedSync();
	PumpSyncHashes();
	if (g_net.steam && g_net.running) {
		GGPONetworkStats st{};
		if (GGPO_SUCCEEDED(ggpo_get_network_stats(g_net.session, g_net.remote, &st)))
			PaceOnRiftSample(st.timesync.local_frames_behind, st.timesync.remote_frames_behind);
	}
	bool advance = false;
	static int s_stallGap = 0;
	if (s_stallGap > 0) --s_stallGap;
	if ((g_net.stall > 0 || g_paceHoldRequests > 0) && s_stallGap == 0) {
		if (g_net.stall > 0) --g_net.stall;
		else --g_paceHoldRequests;
		++g_netStalls;
		s_stallGap = 20;
	} else if (g_origIsPaused()) {
		++g_holdPause;
	} else {
		if (!g_net.seeded && g_net.running && g_net.steam) g_net.seeded = true;
		if (!g_net.seeded && g_net.running) {
			BYTE* rng = *reinterpret_cast<BYTE**>(Live(kRngArrayPtr));
			const auto seed = reinterpret_cast<Seed_t>(Live(kRngSeed));
			seed(rng, nullptr, g_net.seed);
			seed(rng + kRngSize, nullptr, g_net.seed);
			g_net.seeded = true;
			Log("NET: RNG seeded with %lu", g_net.seed);
		}
		DWORD local = ReadLocalMask();
		const bool connected = g_net.seeded;
		if (local != g_latPrevPad) {
			g_latPrevPad = local;
			if (!g_latPending) {
				g_latPending = true;
				g_latPendingMask = local;
				g_latPendingFrame = g_net.frame;
			}
		}
		if (!connected) {
			++g_holdConnect;
		} else if (GGPO_SUCCEEDED(ggpo_add_local_input(g_net.session, g_net.local, &local, sizeof(local)))) {
			int disc = 0;
			advance = GGPO_SUCCEEDED(ggpo_synchronize_input(g_net.session, g_netIn, sizeof(g_netIn), &disc));
			if (!advance) ++g_holdSync;
		} else {
			++g_holdLimit;
			PaceOnPredictionStall();
		}
	}
	g_holdSim = !advance;
	g_netInUpdate = true;
	g_netRealAdvance = advance;
	g_origUpdate(self, edx, arg);
	g_netRealAdvance = false;
	g_netInUpdate = false;
	g_holdSim = false;
	++g_ticks;
	if (advance) {
		++g_net.frame;
		MarkSyncFrame();
		ggpo_advance_frame(g_net.session);
	} else {
		++g_skipped;
	}
	RngMarkTick();
	PaceApply();
	{
		LARGE_INTEGER t1, f;
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&f);
		const double ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart;
		g_netMsSum += ms;
		if (ms > g_netMsMax) g_netMsMax = ms;
		if (ms > 16.6) ++g_netSlowTicks;
		g_lastTickMs = ms;
	}
	if (g_net.steam && advance && !SceneFighting()) {
		NetRoundEnd();
		return;
	}
	{
		static LONG s_rbBefore = 0;
		LONG depth = g_netRollbackFrames - s_rbBefore;
		if (depth < 0) depth = g_netRollbackFrames;
		s_rbBefore = g_netRollbackFrames;
		if (depth > g_ovlMaxDepth) g_ovlMaxDepth = depth;
		g_ovl.rollbackHistory[g_ovl.historyPos] = static_cast<float>(depth);
		g_ovl.tickMsHistory[g_ovl.historyPos] = static_cast<float>(g_lastTickMs);
		g_ovl.historyPos = (g_ovl.historyPos + 1) % OverlayStats::kHistory;
	}
	const DWORD now = GetTickCount();
	if (now - g_lastReport >= 1000) {
		GGPONetworkStats st{};
		if (!g_net.synctest) ggpo_get_network_stats(g_net.session, g_net.remote, &st);
		{
			static int s_frameBefore = 0;
			g_ovl.session = true;
			strcpy_s(g_ovl.status, g_net.running ? "running" : "connecting");
			g_ovl.side = g_net.side;
			g_ovl.delay = g_netDelay;
			g_ovl.ping = st.network.ping;
			g_ovl.frame = g_net.frame;
			g_ovl.updatesPerSec = static_cast<float>(g_net.frame - s_frameBefore) * 1000.0f / static_cast<float>(now - g_lastReport);
			s_frameBefore = g_net.frame;
			g_ovl.rollbackFramesPerSec = g_netRollbackFrames;
			g_ovl.rollbackMaxDepth = g_rbDepthMaxSec;
			g_rbDepthMaxSec = 0;
			g_ovlMaxDepth = 0;
			g_ovl.aheadFrames = static_cast<float>(g_paceRiftEma);
			g_ovl.pacedMsPerSec = static_cast<float>(g_paceAppliedMs);
			g_ovl.heldLimit = g_holdLimit;
			g_ovl.heldPause = g_holdPause;
			g_ovl.heldSync = g_holdSync;
			g_ovl.heldConnect = g_holdConnect;
			g_ovl.tickAvgMs = static_cast<float>(g_ticks ? g_netMsSum / g_ticks : 0.0);
			g_ovl.tickMaxMs = static_cast<float>(g_netMsMax);
			g_ovl.saveMs = static_cast<float>(g_msSave);
			g_ovl.loadMs = static_cast<float>(g_msLoad);
			g_ovl.replayMs = static_cast<float>(g_msResim);
			if (g_latCount) g_ovl.inputDelay = static_cast<float>(static_cast<double>(g_latSum) / g_latCount);
			g_ovl.vsyncOff = g_vsyncForcedOff;
		}
		Log("NET 1s: frame %d, held %ld, rollback frames %ld, stalls %ld, ping %d ms, synctest errors %ld, "
			"tick avg %.1f ms max %.1f ms, %ld ticks over 16.6 ms, rng fixes draw-half %ld outside %ld, "
			"input delay avg %.1f max %ld frames (%ld presses), ms/s save %.0f load %.0f replay %.0f, "
			"held because: pause %ld, rollback limit %ld, sync %ld, connecting %ld, speed-correction %ld, "
			"pacing: ahead %.2f frames, slowed %.1f ms, vsync %s",
			g_net.frame, g_skipped, g_netRollbackFrames, g_netStalls, st.network.ping, g_netSyncErrors,
			g_ticks ? g_netMsSum / g_ticks : 0.0, g_netMsMax, g_netSlowTicks, g_rngDrawFixes, g_rngOutsideFixes,
			g_latCount ? static_cast<double>(g_latSum) / g_latCount : 0.0, g_latMax, g_latCount,
			g_msSave, g_msLoad, g_msResim, g_holdPause, g_holdLimit, g_holdSync, g_holdConnect, g_netStalls,
			g_paceRiftEma, g_paceAppliedMs, g_vsyncForcedOff ? "off" : "on");
		Log("NET 1s: %ld rollbacks, %ld changed what was on screen (avg %.1f, max %ld frames deep)", g_rbCount, g_rbChanged,
			g_rbChanged ? static_cast<double>(g_rbChangedDepthSum) / g_rbChanged : 0.0, g_rbChangedDepthMax);
		Log("NET 1s: %ld rollbacks moved a fighter on screen (largest jump %ld px)", g_jumps, g_jumpMaxPx);
		if (g_ppSaves || g_ppLoads)
			Log("POOL 1s: %ld saves (%ld whole), %.1f of %lu pages per save; %ld loads, %.1f pages per load; %ld mismatches%s",
			    g_ppSaves, g_ppFull, g_ppSaves > g_ppFull ? static_cast<double>(g_ppPages) / (g_ppSaves - g_ppFull) : 0.0,
			    g_poolPages, g_ppLoads, g_ppLoads ? static_cast<double>(g_ppLoadPages) / g_ppLoads : 0.0, g_ppMismatch,
			    g_poolCheck ? " (savecheck on)" : "");
		g_ppSaves = g_ppFull = g_ppPages = g_ppLoads = g_ppLoadPages = 0;
		g_jumps = g_jumpMaxPx = 0;
		g_rbCount = g_rbChanged = g_rbChangedDepthSum = g_rbChangedDepthMax = 0;
		if (g_net.steam) LogNetCounters("player");
		g_paceAppliedMs = 0;
		g_msSave = g_msLoad = g_msResim = 0;
		g_holdPause = g_holdLimit = g_holdSync = g_holdConnect = 0;
		g_latSum = g_latCount = g_latMax = 0;
		g_rngDrawFixes = g_rngOutsideFixes = 0;
		g_skipped = g_netRollbackFrames = g_netStalls = 0;
		g_ticks = 0;
		g_netMsSum = g_netMsMax = 0;
		g_netSlowTicks = 0;
		g_lastReport = now;
	}
}

void __fastcall HookUpdateBattle(void* self, void* edx, int arg) {
	++g_calls;
	g_sceneForSlots = self;
	if (!g_net.session && g_steamRearm && arg == 1 && SceneFighting()) {
		g_steamRearm = false;
		NetBattleStart();
	}
	if (g_net.session) { NetTick(self, edx, arg); return; }
#ifdef BBCSE_DEV_TOOLS
	if (Pressed(VK_F10, 5)) DumpInputProbe();
	if (Pressed(VK_F4, 7)) {
		int i = 0;
		while (kSyncSteps[i] != g_syncN) ++i;
		g_syncN = kSyncSteps[(i + 1) % 5];
		for (auto& s : g_ring) s.frame = -1;
		g_syncFrame = 0;
		g_syncFailLogs = 0;
		g_stepLogs = 0;
		Log("F4: sync test %s%d", g_syncN ? "ON, rollback frames = " : "OFF", g_syncN);
	}
	if (Pressed(VK_F2, 9)) {
		if (g_inMode == InMode::Idle) {
			g_rec.clear();
			g_frameIdx = 0;
			g_inMode = InMode::FileRecording;
			Log("F2: recording inputs to file. Press F2 again to stop.");
		} else if (g_inMode == InMode::AutoReplay) {
			g_syncN = 0;
			g_rec.clear();
			g_frameIdx = 0;
			g_inMode = InMode::FileRecording;
			Log("F2: autotest stopped; recording inputs to file. Press F2 again to stop.");
		} else if (g_inMode == InMode::FileRecording) {
			g_inMode = InMode::Idle;
			FILE* f = nullptr;
			if (fopen_s(&f, kRecFile, "wb") == 0 && f) {
				const DWORD frames = static_cast<DWORD>(g_rec.size()), ctrls = kMaxCtrls;
				fwrite("BBRC", 1, 4, f); fwrite(&frames, 4, 1, f); fwrite(&ctrls, 4, 1, f);
				for (const auto& fr : g_rec) fwrite(fr.data(), 4, kMaxCtrls, f);
				fclose(f);
				Log("F2: saved %lu frames to %s", frames, kRecFile);
			} else {
				Log("F2: could NOT write %s", kRecFile);
			}
		}
	}
	if (g_autoCountdown > 0) --g_autoCountdown;
	if (g_autoCountdown == 0 && g_inMode != InMode::Idle) {
		g_autoCountdown = -1;
		Log("AUTOTEST skipped: another recording or test is running");
	}
	if (g_autoCountdown == 0) {
		g_autoCountdown = -1;
		FILE* f = nullptr;
		DWORD frames = 0, ctrls = 0;
		char magic[4] = {};
		if (fopen_s(&f, kRecFile, "rb") == 0 && f && fread(magic, 1, 4, f) == 4 && memcmp(magic, "BBRC", 4) == 0 &&
		    fread(&frames, 4, 1, f) == 1 && fread(&ctrls, 4, 1, f) == 1 && ctrls == kMaxCtrls) {
			g_rec.assign(frames, {});
			for (auto& fr : g_rec) fread(fr.data(), 4, kMaxCtrls, f);
			g_frameIdx = 0;
			g_inMode = InMode::AutoReplay;
			g_syncN = 7;
			g_ringReset = true;
			g_autoTests = g_autoFails = 0;
			g_stepLogs = 0;
			g_syncFailLogs = 0;
			Log("AUTOTEST START: replaying %lu frames with sync test N=7", frames);
		} else {
			Log("AUTOTEST: could not read %s", kRecFile);
		}
		if (f) fclose(f);
	}
	if (g_inMode == InMode::AutoReplay && g_frameIdx == 300 &&
	    GetFileAttributesA("bbcse-dev/autoko.on") != INVALID_FILE_ATTRIBUTES) {
		BYTE* chars = *reinterpret_cast<BYTE**>(Live(kObjMgr) + kCharsPtrOff);
		if (chars) {
			*reinterpret_cast<int*>(chars + 0x95C) = 1;
			*reinterpret_cast<int*>(chars + 0x20140 + 0x95C) = 1;
			Log("AUTOTEST: both characters set to 1 HP to force a round change");
		}
	}
	if (g_inMode == InMode::AutoReplay && g_frameIdx >= g_rec.size()) {
		g_inMode = InMode::Idle;
		g_syncN = 0;
		Log("AUTOTEST DONE: %ld tests, %ld fails, %d dumps in %s", g_autoTests, g_autoFails, g_dumpCount, kDumpDir);
	}
	if (Pressed(VK_F3, 8)) {
		g_resimArg ^= 1;
		Log("F3: fast-forward uses UpdateBattle(%d)", g_resimArg);
	}
	if (g_syncN && g_mode == 0 && (g_inMode == InMode::Idle || g_inMode == InMode::AutoReplay)) SyncTestFrame(self, edx);
	if (Pressed(VK_F11, 6)) {
		if (g_inMode == InMode::Recording) {
			g_endFrame = g_frameIdx;
			CaptureEnd(g_endA);
			g_replayNo = 0;
			g_endPrev = {};
			StartReplay();
		} else if (g_inMode == InMode::Idle && g_endFrame > 0) {
			StartReplay();
		}
	}
	if (g_inMode == InMode::Replaying && g_frameIdx >= g_endFrame) FinishReplay();
	if (Pressed(VK_F8, 3)) {
		SaveState();
		g_rec.clear();
		g_hashRec.clear();
		g_objRec.clear();
		g_splitCopy.clear();
		g_frameIdx = 0;
		g_endFrame = 0;
		g_inMode = InMode::Recording;
		Log("F8: recording inputs. Play, then press F11 to replay and compare.");
	}
	if (g_inMode == InMode::Recording || g_inMode == InMode::Replaying) CheckFrame();
	if (Pressed(VK_F9, 4)) LoadState();
	if (Pressed(VK_F5, 0)) { g_mode = g_mode == 1 ? 0 : 1; Log("F5 -> mode %d (%s)", g_mode, g_mode ? "FROZEN" : "normal"); }
	if (Pressed(VK_F6, 1)) { g_mode = g_mode == 2 ? 0 : 2; Log("F6 -> mode %d (%s)", g_mode, g_mode ? "DOUBLE" : "normal"); }
	if (Pressed(VK_F7, 2) && g_mode == 1) { g_mode = 3; Log("F7 -> step one frame"); }
#endif

	const int before = P1FrameCounter();
	g_holdSim = g_mode == 1;
	g_realFrame = g_syncN ? g_syncFrame - 1 : -1;
	const bool walled = g_spectating && SceneFighting();
	if (walled) RngCheckBetweenTicks();
	else g_rngAfterValid = false;
	g_origUpdate(self, edx, arg);
	if (SceneFighting() && *reinterpret_cast<int*>(Live(kGameModeVa)) == kModeTraining && !g_origIsPaused()) {
		static LONG s_trainingFrame = 0;
		const BYTE* mgr = Live(kObjMgr);
		const BYTE* chars = *reinterpret_cast<BYTE* const*>(mgr + kCharsPtrOff);
		const int objectCount = *reinterpret_cast<const int*>(mgr + 0x62634);
		const unsigned char* const* objects = *reinterpret_cast<const unsigned char* const* const*>(mgr + 0x62648);
		FrameData_Update(chars, chars ? chars + 0x20140 : nullptr, ++s_trainingFrame, objects, objects ? objectCount : 0);
	}
	TrainingPositionTick();
	if (walled && SceneFighting()) RngMarkTick();
	else g_rngAfterValid = false;
	if (walled) SpectatorSync();
	if (*reinterpret_cast<int*>(Live(kGameMgr) + 8) == 8 && SceneFighting()) ReplaySync();
	g_realFrame = -1;
	g_holdSim = false;
	++g_ticks;
	if (g_mode == 2) { g_origUpdate(self, edx, arg); ++g_doubled; }
	if (g_mode == 3) g_mode = 1;
	if (g_inMode != InMode::Idle) ++g_frameIdx;

	const DWORD now = GetTickCount();
	if (now - g_lastReport >= 1000) {
#ifdef BBCSE_DEV_TOOLS
		Log("1s: calls=%ld ticks=%ld skipped=%ld doubled=%ld  P1 frame %d -> %d  arg=%d mode=%d",
			g_calls, g_ticks, g_skipped, g_doubled, before, P1FrameCounter(), arg, g_mode);
#endif
		if (*reinterpret_cast<int*>(Live(kGameMgr) + 8) == 4) LogNetCounters("no rollback (spectating?)");
		if (g_spectating && g_specRngFixes) Log("SPECTATE: RNG wall stepped in %ld times", g_specRngFixes);
		g_specRngFixes = 0;
		if (g_syncN) {
			Log("   sync N=%d arg=%d: %ld tests, %ld fails, %ld barrier skips, avg %.2f ms, max %.2f ms per frame, %ld polls during replays",
				g_syncN, g_resimArg, g_syncTests, g_syncFails, g_syncBarriers, g_syncTests ? g_syncMs / g_syncTests : 0.0,
				g_syncMsMax, g_resimPolls);
			g_resimPolls = 0;
			g_syncBarriers = 0;
			g_syncTests = g_syncFails = 0;
			g_syncMs = g_syncMsMax = 0;
		}
		g_calls = g_ticks = g_skipped = g_doubled = 0;
		g_lastReport = now;
	}
}

LONG g_crashEvents = 0;
std::string g_crashDir;

void DescribeAddress(DWORD a, char* out, size_t cap) {
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCSTR>(static_cast<UINT_PTR>(a)), &mod) && mod) {
		char path[MAX_PATH] = {};
		GetModuleFileNameA(mod, path, MAX_PATH);
		const char* name = strrchr(path, '\\');
		name = name ? name + 1 : path;
		if (mod == GetModuleHandleA(nullptr))
			sprintf_s(out, cap, "BBCSE 0x%08lX", a - static_cast<DWORD>(reinterpret_cast<UINT_PTR>(mod)) + kImage);
		else
			sprintf_s(out, cap, "%s+0x%lX", name, a - static_cast<DWORD>(reinterpret_cast<UINT_PTR>(mod)));
	} else {
		sprintf_s(out, cap, "0x%08lX", a);
	}
}

void CxxTypeName(const EXCEPTION_RECORD* er, char* out, size_t cap) {
	strcpy_s(out, cap, "?");
	__try {
		if (er->NumberParameters < 3) return;
		const DWORD* throwInfo = reinterpret_cast<const DWORD*>(er->ExceptionInformation[2]);
		const DWORD* cta = reinterpret_cast<const DWORD*>(throwInfo[3]);
		const DWORD* ct = reinterpret_cast<const DWORD*>(cta[1]);
		const char* name = reinterpret_cast<const char*>(ct[1] + 8);
		strncpy_s(out, cap, name, _TRUNCATE);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		strcpy_s(out, cap, "?");
	}
}

void WriteMiniDump(EXCEPTION_POINTERS* ep) {
	using Dump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
	char sys[MAX_PATH]{};
	GetSystemDirectoryA(sys, MAX_PATH);
	HMODULE dbghelp = LoadLibraryA((std::string(sys) + "\\dbghelp.dll").c_str());
	const auto dump = dbghelp ? reinterpret_cast<Dump_t>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
	if (!dump) return;
	const std::string path = g_crashDir + "bbcse-crash.dmp";
	HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	struct { DWORD tid; EXCEPTION_POINTERS* ep; BOOL client; } info{GetCurrentThreadId(), ep, FALSE};
	dump(GetCurrentProcess(), GetCurrentProcessId(), f, 0x40, &info, nullptr, nullptr);
	CloseHandle(f);
}

bool SafeReadDword(const BYTE* p, DWORD* v) {
	__try { *v = *reinterpret_cast<const DWORD*>(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FollowsCall(DWORD v) {
	__try {
		const BYTE* p = reinterpret_cast<const BYTE*>(static_cast<UINT_PTR>(v));
		return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) ||
		       (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

LONG CALLBACK CrashCaptureInner(EXCEPTION_POINTERS* ep);
LONG CALLBACK CrashCapture(EXCEPTION_POINTERS* ep) {
	if (t_probing) return EXCEPTION_CONTINUE_SEARCH;
	ProbeScope probing;
	return CrashCaptureInner(ep);
}
LONG CALLBACK CrashCaptureInner(EXCEPTION_POINTERS* ep) {
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	const bool cxx = code == 0xE06D7363;
	const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
	                   code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
	                   code == 0xC0000409;
	if (!cxx && !fatal) return EXCEPTION_CONTINUE_SEARCH;
	const LONG n = InterlockedIncrement(&g_crashEvents);
	if (n > 8) return EXCEPTION_CONTINUE_SEARCH;
	char where[96], type[96] = "", line[160];
	DescribeAddress(static_cast<DWORD>(reinterpret_cast<UINT_PTR>(ep->ExceptionRecord->ExceptionAddress)), where, sizeof(where));
	if (cxx) CxxTypeName(ep->ExceptionRecord, type, sizeof(type));
	std::string report;
	sprintf_s(line, "EXCEPTION #%ld code %08lX %s at %s, thread %lu\n", n, code, type, where, GetCurrentThreadId());
	report += line;
	const BYTE* sp = reinterpret_cast<const BYTE*>(static_cast<UINT_PTR>(ep->ContextRecord->Esp));
	int shown = 0;
	for (int i = 0; i < 0x2000 && shown < 24; i += 4) {
		DWORD v = 0;
		if (!SafeReadDword(sp + i, &v)) break;
		if (v < 0x10000 || !FollowsCall(v)) continue;
		DescribeAddress(v, where, sizeof(where));
		if (strncmp(where, "0x", 2) == 0) continue;
		sprintf_s(line, "    [esp+0x%04X] %s\n", i, where);
		report += line;
		++shown;
	}
	Log("%s", report.c_str());
	FILE* f = nullptr;
	if (fopen_s(&f, (g_crashDir + "bbcse-crash.txt").c_str(), "a") == 0 && f) {
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, report.c_str());
		fclose(f);
	}
	if (fatal) WriteMiniDump(ep);
	return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashCapture(HMODULE self) {
	g_crashDir = ModuleDir(self);
	AddVectoredExceptionHandler(0, CrashCapture);
	Log("crash capture on (C++ exceptions and faults -> log, bbcse-crash.txt, bbcse-crash.dmp)");
}

constexpr DWORD kRoomCreateSlot = 0x9609F0;
using RoomCreate_t = int(__fastcall*)(void* self, void* edx, BYTE* cfg);
RoomCreate_t g_origRoomCreate = nullptr;
int __fastcall HookRoomCreate(void* self, void* edx, BYTE* cfg) {
#ifdef BBCSE_DEV_TOOLS
	char hex[3 * 0x20 + 1] = {};
	for (int i = 0; i < 0x20; ++i) sprintf_s(hex + 3 * i, 4, "%02X ", cfg[0x48 + i]);
	Log("ROOM: create, Steam member limit %u, settings +0x48: %s", cfg[0x52], hex);
#else
	Log("ROOM: create, Steam member limit %u", cfg[0x52]);
#endif
	return g_origRoomCreate(self, edx, cfg);
}

void ReplaySync() {
	const int f = g_replayFrame++;
	if (f % 30) return;
	const BYTE* chars = *reinterpret_cast<BYTE**>(Live(kObjMgr) + kCharsPtrOff);
	Log("REPLAYSYNC frame %d hash %08lX hp %d %d", f, CrossPcHash(),
		chars ? *reinterpret_cast<const int*>(chars + kHpOff) : -1,
		chars ? *reinterpret_cast<const int*>(chars + 0x20140 + kHpOff) : -1);
}

void SpectatorSync() {
	BYTE* ses = Live(kNetSession);
	const int round = ses[0x11AAA];
	const int idx = *reinterpret_cast<WORD*>(ses + 0x11AB2);
	if (round != g_specRound) {
		if (g_specRound >= 0 && g_specIdx > 0) g_specBase += g_specIdx + 1;
		g_specRound = round;
		g_specIdx = -1;
	}
	if (idx == g_specIdx) return;
	g_specIdx = idx;
	const int f = g_specBase + idx;
	if ((f + 1) % kSyncEvery > 2) return;
	const BYTE* chars = *reinterpret_cast<BYTE**>(Live(kObjMgr) + kCharsPtrOff);
	Log("SPECSYNC frame %d (round index %d) hash %08lX hp %d %d%s", f, idx, CrossPcHash(),
		chars ? *reinterpret_cast<const int*>(chars + kHpOff) : -1,
		chars ? *reinterpret_cast<const int*>(chars + 0x20140 + kHpOff) : -1, PartsText(CrossPcParts()).c_str());
}

const char* SteamPersonaName(unsigned long long id);

using RoomInfo_t = void*(__fastcall*)(void* self, void* edx, int index, BYTE* out);
RoomInfo_t g_origRoomInfo = nullptr;

void RoomOwnerName(int index, BYTE* out) {
	__try {
		using Instance_t = void**(__cdecl*)();
		using Mgr_t = BYTE*(__fastcall*)(void* self, void* edx);
		void* obj = *reinterpret_cast<Instance_t>(Live(0x4598B0))();
		BYTE* mgr = reinterpret_cast<Mgr_t>((*reinterpret_cast<void***>(obj))[7])(obj, nullptr);
		if (!mgr || index < 0 || index >= *reinterpret_cast<int*>(mgr + 0x444)) return;
		const BYTE* room = *reinterpret_cast<BYTE**>(mgr + 0x448);
		if (!room) return;
		const unsigned long long owner = *reinterpret_cast<const unsigned long long*>(room + 0x50);
		if ((owner >> 32) != 0x01100001) return;
		const char* name = SteamPersonaName(owner);
		if (!name || !*name || !strcmp(name, "[unknown]")) return;
		strncpy_s(reinterpret_cast<char*>(out + 0x78), 32, name, _TRUNCATE);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void* __fastcall HookRoomInfo(void* self, void* edx, int index, BYTE* out) {
	void* r = g_origRoomInfo(self, edx, index, out);
	if (out && *reinterpret_cast<int*>(out + 0x54)) {
		++t_probing;
		RoomOwnerName(index, out);
		--t_probing;
	}
	return r;
}

void InstallTickHook() {
	static bool done = false;
	if (done) return;
	done = true;

	g_base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
	g_origUpdate = reinterpret_cast<UpdateBattle_t>(Live(kUpdateBattle));

	g_origIsPaused = reinterpret_cast<IsBattlePaused_t>(Live(kIsBattlePaused));
	g_origPoll = reinterpret_cast<Poll_t>(Live(kPollFn));
	g_origFxSetup = reinterpret_cast<FxSetup_t>(Live(kFxSetup));
	g_origNetWait = reinterpret_cast<NetWait_t>(Live(kNetWaitFn));
	g_origLimiter = reinterpret_cast<Limiter_t>(Live(kLimiterFn));
	g_origDrawBegin = reinterpret_cast<DrawBegin_t>(Live(kDrawBeginFn));
	g_origRoundCheck = reinterpret_cast<RoundCheck_t>(Live(kRoundCheckFn));
	g_origNetSend = reinterpret_cast<NetSend_t>(Live(kNetSendFn));
	g_origNetFetch = reinterpret_cast<NetFetch_t>(Live(kNetFetchFn));
	g_origNetIndex = reinterpret_cast<NetIndex_t>(Live(kNetIndexFn));
	g_origMatchMenu = reinterpret_cast<MatchMenu_t>(Live(kMatchMenuFn));
	g_origNetInputMsg = reinterpret_cast<NetMsg_t>(Live(kNetInputMsgFn));
	g_origNetRelayMsg = reinterpret_cast<NetMsg_t>(Live(kNetRelayMsgFn));
	g_origNetRelayReply = reinterpret_cast<NetMsg_t>(Live(kNetRelayReplyFn));
	g_origNetResend = reinterpret_cast<NetMsg_t>(Live(kNetResendFn));
	{
		void** slot = reinterpret_cast<void**>(Live(kRoomCreateSlot));
		if (*slot == Live(0x442550)) {
			g_origRoomCreate = reinterpret_cast<RoomCreate_t>(*slot);
			PatchCloudSlot(slot, 0, reinterpret_cast<void*>(&HookRoomCreate));
		}
	}

	struct Patch { DWORD site, target; void* hook; };
	const Patch patches[] = {
		{kCallSites[0], kUpdateBattle, reinterpret_cast<void*>(&HookUpdateBattle)},
		{kCallSites[1], kUpdateBattle, reinterpret_cast<void*>(&HookUpdateBattle)},
		{kCallSites[2], kUpdateBattle, reinterpret_cast<void*>(&HookUpdateBattle)},
		{kIsPausedSite, kIsBattlePaused, reinterpret_cast<void*>(&HookIsBattlePaused)},
		{kPollSite, kPollFn, reinterpret_cast<void*>(&HookPoll)},
		{kFxSetupSite, kFxSetup, reinterpret_cast<void*>(&HookFxSetup)},
		{kNetWaitSite, kNetWaitFn, reinterpret_cast<void*>(&HookNetWait)},
		{kDrawBeginSite, kDrawBeginFn, reinterpret_cast<void*>(&HookDrawBegin)},
		{kLimiterSiteA, kLimiterFn, reinterpret_cast<void*>(&HookLimiter)},
		{kLimiterSiteB, kLimiterFn, reinterpret_cast<void*>(&HookLimiter)},
		{kRoundCheckSite, kRoundCheckFn, reinterpret_cast<void*>(&HookRoundCheck)},
		{kNetSendSite, kNetSendFn, reinterpret_cast<void*>(&HookNetSend)},
		{kNetInputMsgSite, kNetInputMsgFn, reinterpret_cast<void*>(&HookNetInputMsg)},
		{kNetRelayMsgSite, kNetRelayMsgFn, reinterpret_cast<void*>(&HookNetRelayMsg)},
		{kNetRelayReplySite, kNetRelayReplyFn, reinterpret_cast<void*>(&HookNetRelayReply)},
		{kNetResendSite, kNetResendFn, reinterpret_cast<void*>(&HookNetResend)},
		{kNetFetchSite, kNetFetchFn, reinterpret_cast<void*>(&HookNetFetch)},
		{kNetIndexSite, kNetIndexFn, reinterpret_cast<void*>(&HookNetIndex)},
		{kMatchMenuSite, kMatchMenuFn, reinterpret_cast<void*>(&HookMatchMenu)},
		{kPoolNewSite, kPoolNewFn, reinterpret_cast<void*>(&HookPoolNew)},
		{kPoolDelSiteA, kOpDeleteFn, reinterpret_cast<void*>(&HookPoolDelete)},
		{kPoolDelSiteB, kOpDeleteFn, reinterpret_cast<void*>(&HookPoolDelete)},
	};

	for (const Patch& pt : patches) {
		BYTE* p = Live(pt.site);
		const LONG rel = *reinterpret_cast<LONG*>(p + 1);
		if (p[0] != 0xE8 || p + 5 + rel != Live(pt.target)) {
			Log("ABORT: call site 0x%08X does not match (byte %02X, target %p). No patch applied.",
				pt.site, p[0], p + 5 + rel);
			return;
		}
	}
	for (const Patch& pt : patches) {
		BYTE* p = Live(pt.site);
		DWORD old;
		VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
		*reinterpret_cast<LONG*>(p + 1) = static_cast<LONG>(static_cast<BYTE*>(pt.hook) - (p + 5));
		VirtualProtect(p, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
	}
	Palette_Init();
	{
		BYTE* p = Live(0x51291D);
		const LONG rel = *reinterpret_cast<LONG*>(p + 1);
		if (p[0] == 0xE8 && p + 5 + rel == Live(0x42D860)) {
			Palette_SetPackFunctions(Live(0x42D860), Live(0x42D8E0));
			DWORD old;
			VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
			*reinterpret_cast<LONG*>(p + 1) = static_cast<LONG>(reinterpret_cast<BYTE*>(&Palette_CssEntry) - (p + 5));
			VirtualProtect(p, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p, 5);
			Log("palette: character select colors hooked");
		} else {
			Log("palette: character select colors NOT hooked (call site differs)");
		}
	}
	{
		BYTE* p = Live(0x59B86A);
		const LONG rel = *reinterpret_cast<LONG*>(p + 1);
		if (p[0] == 0xE8 && p + 5 + rel == Live(0x46D030)) {
			g_origRoomInfo = reinterpret_cast<RoomInfo_t>(Live(0x46D030));
			DWORD old;
			VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
			*reinterpret_cast<LONG*>(p + 1) = static_cast<LONG>(reinterpret_cast<BYTE*>(&HookRoomInfo) - (p + 5));
			VirtualProtect(p, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p, 5);
			Log("room owner name hooked");
		} else {
			Log("room owner name NOT hooked (call site differs)");
		}
	}
	{
		static const BYTE expect[] = {0x83, 0xF8, 0x0C, 0x75};
		BYTE* p = Live(0x47AAB7);
		if (memcmp(p, expect, sizeof(expect)) == 0) {
			DWORD old;
			VirtualProtect(p + 3, 1, PAGE_EXECUTE_READWRITE, &old);
			p[3] = 0x70;
			VirtualProtect(p + 3, 1, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p + 3, 1);
			Log("network menu fix applied (D-Code check skipped)");
		} else if (p[3] == 0x70) {
			Log("network menu fix already in the exe (Geo's patcher)");
		} else {
			Log("network menu fix NOT applied: bytes at 0x47AAB7 differ");
		}
	}
	{
		static const BYTE expect[] = {0xB9, 0x68, 0x01, 0x00, 0x00};
		int done = 0;
		for (const DWORD site : {0x45EB19u, 0x46588Eu}) {
			BYTE* p = Live(site);
			if (memcmp(p, expect, sizeof(expect)) != 0) continue;
			DWORD old;
			VirtualProtect(p + 1, 2, PAGE_EXECUTE_READWRITE, &old);
			*reinterpret_cast<WORD*>(p + 1) = 1200;
			VirtualProtect(p + 1, 2, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p + 1, 2);
			++done;
		}
		Log("spectator timeout %s (%d of 2 sites)", done == 2 ? "6 s -> 20 s" : "NOT fully raised", done);
	}
	Log("tick hook installed (%u call sites)", static_cast<unsigned>(sizeof(patches) / sizeof(patches[0])));
	PatchIat(kIatPerspectiveFov, reinterpret_cast<void*>(&HookPerspectiveFov), reinterpret_cast<void**>(&g_origPerspectiveFov));
	InstallVersionSelect();
	LoadVersionChoice();
#ifdef BBCSE_DEV_TOOLS
	InstallInputProbe();
#endif
	InstallFlowHooks();
	InstallDeviceHooks(*reinterpret_cast<void**>(Live(0xB8C800 + 0x2C)));
}

struct KeyName { const char* name; BYTE dik; };
const KeyName kKeyNames[] = {
	{"A", DIK_A}, {"B", DIK_B}, {"C", DIK_C}, {"D", DIK_D}, {"E", DIK_E}, {"F", DIK_F}, {"G", DIK_G},
	{"H", DIK_H}, {"I", DIK_I}, {"J", DIK_J}, {"K", DIK_K}, {"L", DIK_L}, {"M", DIK_M}, {"N", DIK_N},
	{"O", DIK_O}, {"P", DIK_P}, {"Q", DIK_Q}, {"R", DIK_R}, {"S", DIK_S}, {"T", DIK_T}, {"U", DIK_U},
	{"V", DIK_V}, {"W", DIK_W}, {"X", DIK_X}, {"Y", DIK_Y}, {"Z", DIK_Z},
	{"0", DIK_0}, {"1", DIK_1}, {"2", DIK_2}, {"3", DIK_3}, {"4", DIK_4}, {"5", DIK_5}, {"6", DIK_6},
	{"7", DIK_7}, {"8", DIK_8}, {"9", DIK_9},
	{"apostrophe", DIK_APOSTROPHE}, {"semicolon", DIK_SEMICOLON}, {"comma", DIK_COMMA},
	{"period", DIK_PERIOD}, {"slash", DIK_SLASH}, {"backslash", DIK_BACKSLASH}, {"grave", DIK_GRAVE},
	{"minus", DIK_MINUS}, {"equals", DIK_EQUALS}, {"lbracket", DIK_LBRACKET}, {"rbracket", DIK_RBRACKET},
	{"space", DIK_SPACE}, {"tab", DIK_TAB}, {"capslock", DIK_CAPITAL}, {"enter", DIK_RETURN},
	{"backspace", DIK_BACK}, {"lshift", DIK_LSHIFT}, {"rshift", DIK_RSHIFT}, {"lctrl", DIK_LCONTROL},
	{"rctrl", DIK_RCONTROL}, {"lalt", DIK_LMENU}, {"ralt", DIK_RMENU}, {"up", DIK_UP}, {"down", DIK_DOWN},
	{"left", DIK_LEFT}, {"right", DIK_RIGHT}, {"insert", DIK_INSERT}, {"delete", DIK_DELETE},
	{"home", DIK_HOME}, {"end", DIK_END}, {"pageup", DIK_PRIOR}, {"pagedown", DIK_NEXT},
	{"num0", DIK_NUMPAD0}, {"num1", DIK_NUMPAD1}, {"num2", DIK_NUMPAD2}, {"num3", DIK_NUMPAD3},
	{"num4", DIK_NUMPAD4}, {"num5", DIK_NUMPAD5}, {"num6", DIK_NUMPAD6}, {"num7", DIK_NUMPAD7},
	{"num8", DIK_NUMPAD8}, {"num9", DIK_NUMPAD9},
};
int KeyCode(const char* s) {
	for (const auto& k : kKeyNames)
		if (_stricmp(s, k.name) == 0) return k.dik;
	char* end = nullptr;
	const long v = strtol(s, &end, 0);
	return *s && end && !*end && v > 0 && v < 256 ? static_cast<int>(v) : -1;
}

BYTE g_keyMap[256];
bool g_keyRemap = false;
void* g_keyboard = nullptr;
using DiCreateDevice_t = HRESULT(__stdcall*)(void* di, REFGUID guid, void** dev, void* outer);
using GetDeviceState_t = HRESULT(__stdcall*)(void* dev, DWORD size, void* data);
using GetDeviceData_t = HRESULT(__stdcall*)(void* dev, DWORD objSize, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags);
DiCreateDevice_t g_origCreateInputDevice = nullptr;
GetDeviceState_t g_origGetDeviceState = nullptr;
GetDeviceData_t g_origGetDeviceData = nullptr;

HRESULT __stdcall HookGetDeviceState(void* dev, DWORD size, void* data) {
	const HRESULT hr = g_origGetDeviceState(dev, size, data);
	if (SUCCEEDED(hr) && dev == g_keyboard && size == 256 && data) {
		BYTE in[256], * out = static_cast<BYTE*>(data);
		memcpy(in, out, 256);
		memset(out, 0, 256);
		for (int k = 0; k < 256; ++k) out[g_keyMap[k]] |= in[k];
	}
	return hr;
}
HRESULT __stdcall HookGetDeviceData(void* dev, DWORD objSize, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) {
	const HRESULT hr = g_origGetDeviceData(dev, objSize, data, count, flags);
	if (SUCCEEDED(hr) && dev == g_keyboard && data && count && objSize >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
		for (DWORD i = 0; i < *count; ++i) {
			auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data) + i * objSize);
			if (e->dwOfs < 256) e->dwOfs = g_keyMap[e->dwOfs];
		}
	}
	return hr;
}
HRESULT __stdcall HookCreateInputDevice(void* di, REFGUID guid, void** dev, void* outer) {
	const HRESULT hr = g_origCreateInputDevice(di, guid, dev, outer);
	if (SUCCEEDED(hr) && dev && *dev && IsEqualGUID(guid, GUID_SysKeyboard)) {
		g_keyboard = *dev;
		PatchVtableSlot(*dev, 9, reinterpret_cast<void*>(&HookGetDeviceState), reinterpret_cast<void**>(&g_origGetDeviceState));
		PatchVtableSlot(*dev, 10, reinterpret_cast<void*>(&HookGetDeviceData), reinterpret_cast<void**>(&g_origGetDeviceData));
		Log("KEYS: keyboard remap active");
	}
	return hr;
}
void LoadKeyRemap() {
	for (int k = 0; k < 256; ++k) g_keyMap[k] = static_cast<BYTE>(k);
	char ini[MAX_PATH];
	GetModuleFileNameA(g_self, ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	*(slash ? slash + 1 : ini) = 0;
	strcat_s(ini, "bbcse-net.ini");
	char sec[4096] = {};
	GetPrivateProfileSectionA("keys", sec, sizeof(sec), ini);
	for (char* p = sec; *p; p += strlen(p) + 1) {
		char line[128];
		strncpy_s(line, p, _TRUNCATE);
		char* eq = strchr(line, '=');
		if (!eq || line[0] == ';') continue;
		*eq = 0;
		auto trim = [](char* s) { while (*s == ' ' || *s == '\t') ++s; for (char* e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0; return s; };
		char* from = trim(line);
		char* to = trim(eq + 1);
		if (char* c = strchr(to, ';')) { *c = 0; to = trim(to); }
		const int a = KeyCode(from), b = KeyCode(to);
		if (a < 0 || b < 0) { Log("KEYS: ignored '%s=%s' (unknown key name)", from, to); continue; }
		g_keyMap[a] = static_cast<BYTE>(b);
		g_keyRemap = true;
		Log("KEYS: %s reads as %s", from, to);
	}
}

bool g_passive = false;

std::string g_userVersion = "cse";
std::string g_forcedVersion;
std::string g_forcedReason;

std::map<std::string, int> g_versionStates;
std::string g_versionProblem;
DWORD Crc32(DWORD crc, const BYTE* p, size_t n) {
	static DWORD table[256];
	static bool ready = false;
	if (!ready) {
		for (DWORD i = 0; i < 256; ++i) {
			DWORD c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		ready = true;
	}
	crc = ~crc;
	while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}
bool VersionFileOk(const VersionFile& f) {
	FILE* fp = nullptr;
	if (fopen_s(&fp, (VersionsRoot() + f.path).c_str(), "rb") || !fp) return false;
	static BYTE buf[0x10000];
	DWORD crc = 0, total = 0;
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
		crc = Crc32(crc, buf, got);
		total += static_cast<DWORD>(got);
	}
	fclose(fp);
	return total == f.size && crc == f.crc;
}
int VersionState(const std::string& name) {
	const auto it = g_versionStates.find(name);
	if (it != g_versionStates.end()) return it->second;
	if (GetFileAttributesA((VersionsRoot() + name).c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
	const std::string prefix = name + "\\";
	int files = 0, bad = 0;
	std::string first;
	for (const VersionFile& f : kVersionFiles) {
		if (strncmp(f.path, prefix.c_str(), prefix.size()) != 0) continue;
		++files;
		if (VersionFileOk(f)) continue;
		if (first.empty()) first = f.path;
		++bad;
	}
	if (files == 0) return 0;
	const int state = bad ? 2 : 1;
	if (bad) {
		Log("VERSION: %s off, %d of %d files missing or different (first: %s)", name.c_str(), bad, files, first.c_str());
		g_versionProblem += (g_versionProblem.empty() ? "" : ", ") + name;
	} else {
		Log("VERSION: %s files ok", name.c_str());
	}
	g_versionStates[name] = state;
	return state;
}
void CheckVersionFiles() {
	std::set<std::string> names;
	for (const VersionFile& f : kVersionFiles) {
		const char* slash = strchr(f.path, '\\');
		if (slash) names.insert(std::string(f.path, slash));
	}
	for (const std::string& n : names)
		if (GetFileAttributesA((VersionsRoot() + n).c_str()) != INVALID_FILE_ATTRIBUTES) VersionState(n);
}

bool VersionNameOk(const std::string& name) {
	if (name.empty() || name.size() > 8) return false;
	for (char c : name)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
	return true;
}

DWORD VersionHash(const std::string& name) {
	if (name == "cse") return 0;
	if (!VersionNameOk(name)) return 1;
	if (VersionState(name) != 1) return 1;
	const std::string prefix = name + "\\";
	DWORD h = 2166136261u;
	for (const VersionFile& f : kVersionFiles) {
		if (strncmp(f.path, prefix.c_str(), prefix.size()) != 0) continue;
		for (const char* c = f.path; *c; ++c) h = (h ^ static_cast<BYTE>(*c)) * 16777619u;
		for (DWORD v : {f.size, f.crc})
			for (int i = 0; i < 4; ++i) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 16777619u;
	}
	return h | 2;
}

bool ApplyVersion(const std::string& name) {
	if (!VersionNameOk(name)) return false;
	std::string dir = VersionsRoot() + name + "\\";
	if (name == "cse") dir.clear();
	else if (VersionState(name) != 1) return false;
	if (name == g_versionName) return true;
	EnterCriticalSection(&g_versionLock);
	g_versionName = name;
	g_versionDir = dir;
	LeaveCriticalSection(&g_versionLock);
	g_versionRedirects = 0;
	Log("VERSION: %s selected", g_versionName.c_str());
	ApplyVersionRules();
	return true;
}

void LoadSavedVersion() {
	char saved[16] = {};
	GetPrivateProfileStringA("menu", "version", "cse", saved, sizeof(saved), (ModuleDir(g_self) + "bbcse-im.ini").c_str());
	std::string want = saved;
	for (char& c : want) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	const std::string dir = VersionsRoot() + want + "\\";
	if (want == "cse" || !VersionNameOk(want) || VersionState(want) != 1) {
		Log("VERSION: cse (choose another in the IM BBCS:EX menu, F2)");
		return;
	}
	EnterCriticalSection(&g_versionLock);
	g_versionName = want;
	g_versionDir = dir;
	LeaveCriticalSection(&g_versionLock);
	g_userVersion = want;
	Log("VERSION: %s (saved choice; change it in the IM BBCS:EX menu, F2)", want.c_str());
}

constexpr int kVersionChannel = 3;
constexpr DWORD kVerAsk = 0x51565942, kVerAnswer = 0x41565942, kVerCannot = 0x4E565942;
constexpr DWORD kVerAskEvery = 1000, kVerTimeout = 4000;

void SetForced(const std::string& name, const char* reason) {
	if (g_poolMem) return;
	if (g_forcedVersion != name || g_forcedReason != reason) {
		g_forcedVersion = name;
		g_forcedReason = reason;
		Log("VERSION: lobby %s -> %s", reason, name.empty() ? g_userVersion.c_str() : name.c_str());
	}
	ApplyVersion(name.empty() ? g_userVersion : name);
}

bool Readable(const void* p, size_t n) {
	MEMORY_BASIC_INFORMATION mbi{};
	if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
	return static_cast<const BYTE*>(p) + n <= static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
}

unsigned long long MemberId(const BYTE* member) {
	return Readable(member, 0x78) ? *reinterpret_cast<const unsigned long long*>(member + 0x70) : 0;
}

bool Connected(unsigned long long who) {
	P2PState st{};
	return who && g_steamNet && g_steamNet->SessionState(who, &st) && (st.active || st.connecting);
}

void VersionSyncTick() {
	static DWORD s_last = 0, s_lastAsk = 0, s_lastAnswer = 0, s_sessionStart = 0;
	static std::map<unsigned long long, DWORD> s_asked;
	static std::map<unsigned long long, DWORD> s_cannot;
	const DWORD now = GetTickCount();
	if (now - s_last < 100) return;
	s_last = now;
	if (!g_steamNet) {
		using GetNet_t = ISteamNet*(__cdecl*)();
		const HMODULE steam = GetModuleHandleA("steam_api.dll");
		const auto getNet = steam ? reinterpret_cast<GetNet_t>(GetProcAddress(steam, "SteamNetworking")) : nullptr;
		g_steamNet = getNet ? getNet() : nullptr;
		if (!g_steamNet) return;
	}
	BYTE* ses = Live(kNetSession);
	BYTE* self = *reinterpret_cast<BYTE**>(ses + 0x12040);
	BYTE* host = *reinterpret_cast<BYTE**>(ses + 0x12044);
	const unsigned long long hostId = MemberId(host);
	const bool amHost = Readable(self, 0x5C) && (self == host || (*reinterpret_cast<const WORD*>(self + 0x5A) & 2));
	bool inSession = false;
	if (amHost) {
		for (int side = 0; side < 2; ++side) {
			BYTE* peer = NetPeer(side);
			const unsigned long long id = peer != self ? MemberId(peer) : 0;
			if (id && Connected(id)) inSession = true;
		}
	} else {
		inSession = Readable(self, 0x5C) && hostId && Connected(hostId);
	}
	if (!inSession) {
		static bool s_seenLogged = false;
		if (!s_seenLogged && Readable(self, 0x5C) && host) {
			s_seenLogged = true;
			Log("VERSION: room members seen, no P2P link yet (host %s)", amHost ? "is this PC" : "is remote");
		}
		if (s_sessionStart) Log("VERSION: online room ended");
		s_sessionStart = 0;
		s_asked.clear();
		s_cannot.clear();
		if (!g_forcedVersion.empty() || !g_forcedReason.empty()) SetForced("", "left");
		if (!g_forcedReason.empty() && !g_poolMem) g_forcedReason.clear();
		return;
	}
	if (!s_sessionStart) {
		s_sessionStart = now;
		s_lastAnswer = now;
		Log("VERSION: online room, this PC is %s (own choice %s)", amHost ? "the host" : "a guest", g_userVersion.c_str());
	}
	unsigned size = 0, got = 0;
	unsigned long long from = 0;
	DWORD msg[4];
	for (int n = 0; n < 64 && g_steamNet->Available(&size, kVersionChannel); ++n) {
		if (size > sizeof(msg)) {
			std::vector<unsigned char> drop(size);
			g_steamNet->Read(drop.data(), size, &got, &from, kVersionChannel);
			continue;
		}
		if (!g_steamNet->Read(msg, sizeof(msg), &got, &from, kVersionChannel)) break;
		if (got != sizeof(msg)) continue;
		if (amHost && msg[0] == kVerAsk) {
			s_asked[from] = now;
			DWORD out[4] = {kVerAnswer, 0, 0, VersionHash(g_versionName)};
			memcpy(&out[1], g_versionName.c_str(), (std::min)(g_versionName.size(), static_cast<size_t>(8)));
			g_steamNet->Send(from, out, sizeof(out), 2, kVersionChannel);
		} else if (amHost && msg[0] == kVerCannot) {
			s_cannot[from] = now;
		} else if (!amHost && from == hostId && msg[0] == kVerAnswer) {
			s_lastAnswer = now;
			char name[9] = {};
			memcpy(name, &msg[1], 8);
			std::string want = name;
			for (char& c : want) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			if (VersionNameOk(want) && VersionHash(want) == msg[3] && (want == "cse" || VersionState(want) == 1)) {
				SetForced(want, "host picked");
			} else {
				DWORD out[4] = {kVerCannot, 0, 0, 0};
				g_steamNet->Send(hostId, out, sizeof(out), 2, kVersionChannel);
				SetForced("cse", "has different files from the host, playing");
			}
		}
	}
	if (amHost) {
		const char* reason = nullptr;
		for (int side = 0; side < 2 && !reason; ++side) {
			BYTE* peer = NetPeer(side);
			const unsigned long long id = peer != self ? MemberId(peer) : 0;
			if (!id) continue;
			const auto c = s_cannot.find(id);
			if (c != s_cannot.end() && now - c->second < kVerTimeout * 3) reason = "opponent has different files, playing";
			const auto a = s_asked.find(id);
			if (!reason && now - s_sessionStart > kVerTimeout && (a == s_asked.end() || now - a->second > kVerTimeout))
				reason = "opponent has no version select, playing";
		}
		if (reason) SetForced("cse", reason);
		else if (!g_forcedVersion.empty()) SetForced("", "back to own choice");
		return;
	}
	if (now - s_lastAsk >= kVerAskEvery) {
		s_lastAsk = now;
		const DWORD out[4] = {kVerAsk, 0, 0, 0};
		g_steamNet->Send(hostId, out, sizeof(out), 2, kVersionChannel);
	}
	if (now - s_lastAnswer > kVerTimeout && g_forcedVersion.empty()) SetForced("cse", "host has no version select, playing");
}

constexpr int kPaletteChannel = 4;

const char* SteamPersonaName(unsigned long long id) {
	using Friends_t = void*(__cdecl*)();
	using Name_t = const char*(__thiscall*)(void*, unsigned long long);
	const HMODULE steam = GetModuleHandleA("steam_api.dll");
	const auto get = steam ? reinterpret_cast<Friends_t>(GetProcAddress(steam, "SteamFriends")) : nullptr;
	void* friends = get ? get() : nullptr;
	if (!friends) return nullptr;
	return reinterpret_cast<Name_t>((*reinterpret_cast<void***>(friends))[7])(friends, id);
}

void PaletteSyncTick() {
	static unsigned long long s_sentStamp = 0, s_sentPeer = 0, s_namedPeer = 0;
	static DWORD s_last = 0;
	static unsigned char s_buf[16384];
	BYTE* self = *reinterpret_cast<BYTE**>(Live(kNetSession) + 0x12040);
	const WORD flags = Readable(self, 0x5C) ? *reinterpret_cast<const WORD*>(self + 0x5A) : 0;
	const bool player = (flags & 0x10) != 0;
	const int side = flags & 1;
	BYTE* peer = player ? NetPeer(1 - side) : nullptr;
	const unsigned long long peerId = peer && peer != self ? MemberId(peer) : 0;
	const bool online = g_steamNet && g_poolMem && player && peerId && *reinterpret_cast<int*>(Live(kGameModeVa)) == 4;
	Palette_SetOnline(online, side, peerId);
	if (!online) return;
	if (peerId != s_namedPeer) {
		s_namedPeer = peerId;
		Palette_SetPeerName(peerId, SteamPersonaName(peerId));
	}
	unsigned size = 0, got = 0;
	unsigned long long from = 0;
	static unsigned char s_lastMsg[16384];
	int lastLen = 0;
	unsigned long long lastFrom = 0;
	static int s_pendingLen = 0;
	static unsigned long long s_pendingFrom = 0;
	static DWORD s_lastApply = 0;
	for (int n = 0; n < 64 && g_steamNet->Available(&size, kPaletteChannel); ++n) {
		if (size > sizeof(s_buf)) {
			std::vector<unsigned char> drop(size);
			g_steamNet->Read(drop.data(), size, &got, &from, kPaletteChannel);
			continue;
		}
		if (!g_steamNet->Read(s_buf, sizeof(s_buf), &got, &from, kPaletteChannel)) break;
		if (from != peerId || got > sizeof(s_buf)) continue;
		memcpy(s_lastMsg, s_buf, got);
		lastLen = static_cast<int>(got);
		lastFrom = from;
	}
	if (lastLen) {
		s_pendingLen = lastLen;
		s_pendingFrom = lastFrom;
	}
	if (s_pendingLen && GetTickCount() - s_lastApply >= 250) {
		Palette_Receive(s_lastMsg, s_pendingLen, s_pendingFrom);
		s_pendingLen = 0;
		s_lastApply = GetTickCount();
	}
	const DWORD now = GetTickCount();
	if (now - s_last < 250) return;
	s_last = now;
	const unsigned long long stamp = Palette_LocalStamp();
	if (!stamp || (stamp == s_sentStamp && peerId == s_sentPeer)) return;
	const int len = Palette_BuildPacket(s_buf, sizeof(s_buf));
	if (len > 0 && g_steamNet->Send(peerId, s_buf, static_cast<unsigned>(len), 2, kPaletteChannel)) {
		s_sentStamp = stamp;
		s_sentPeer = peerId;
		Log("PALETTE: sent own palette");
	}
}

}
int g_posPreset = 0;
bool g_posRight = false;
void Training_ResetPosition() {
	if (!TrainingFight() || g_origIsPaused() || NetSessionActive()) return;
	BYTE* mgr = Live(kObjMgr);
	if (*reinterpret_cast<int*>(mgr + kFlowStateOff) != 3 || *reinterpret_cast<int*>(mgr + kFlowSubOff) != 0) return;
	*reinterpret_cast<int*>(mgr + kFlowSubOff) = 1;
	DWORD& flags = *reinterpret_cast<DWORD*>(mgr + kFlowFlagsOff);
	flags = (flags & ~0x80u) | 0x20000u;
	reinterpret_cast<void(__cdecl*)(int)>(Live(kFadeStart))(0x1e);
}

void Palette_Log(const char* text) { Log("%s", text); }

void Version_List(std::vector<std::string>& out) {
	out.assign(1, "cse");
	WIN32_FIND_DATAA fd{};
	const HANDLE h = FindFirstFileA((VersionsRoot() + "*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
		std::string name = fd.cFileName;
		for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (name != "cse" && VersionState(name) == 1) out.push_back(name);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}
const char* Version_Problem() { return g_versionProblem.c_str(); }
int Save_UnlockGallery() {
	if (g_poolMem) return -1;
	const std::string dir = ModuleDir(g_self) + "Save\\";
	const std::string path = dir + "bbsave.dat";
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") || !f) return -1;
	std::vector<BYTE> file(0xE8B28);
	const size_t got = fread(file.data(), 1, file.size(), f);
	fclose(f);
	if (got != file.size() || memcmp(file.data() + 8, "Save", 4) != 0) return -1;
	BYTE* mem = Live(0xC013B0);
	if (!Readable(mem, 0xE8B28) || memcmp(mem + 8, "Save", 4) != 0) return -1;
	if (!CopyFileA(path.c_str(), (dir + "bbsave.before-unlock.dat").c_str(), TRUE) && GetLastError() != ERROR_FILE_EXISTS) return -1;
	int n = 0;
	for (int i = 0x160; i < 0x160 + 0x1F2; ++i) {
		if (mem[i] == 0) mem[i] = 2;
		if (file[i] == 0) { file[i] = 2; ++n; }
	}
	const std::string tmp = path + ".tmp";
	if (fopen_s(&f, tmp.c_str(), "wb") || !f) return -1;
	bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
	ok = (fclose(f) == 0) && ok;
	ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	if (!ok) DeleteFileA(tmp.c_str());
	Log("SAVE: unlocked %d gallery items%s", n, ok ? "" : " (file write failed)");
	return ok ? n : -1;
}
const char* Version_Current() { return g_versionName.c_str(); }
bool Version_Locked() { return g_poolMem != nullptr || !g_forcedVersion.empty(); }
bool Version_InFight() { return g_poolMem != nullptr; }
const char* Version_ForcedReason() { return g_forcedVersion.empty() ? "" : g_forcedReason.c_str(); }
std::string Version_LoadedCodes() {
	EnterCriticalSection(&g_versionLock);
	const std::string out = g_versionLoaded;
	LeaveCriticalSection(&g_versionLock);
	return out;
}
void Version_Select(const char* name) {
	if (Version_Locked() || !name) return;
	std::string want = name;
	for (char& c : want) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	if (!ApplyVersion(want)) return;
	g_userVersion = want;
	WritePrivateProfileStringA("menu", "version", want.c_str(), (ModuleDir(g_self) + "bbcse-im.ini").c_str());
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid,
                                             LPVOID* out, LPUNKNOWN outer) {
	if (g_passive) return LoadRealDinput() ? g_realCreate(inst, version, iid, out, outer) : E_FAIL;
	InstallTickHook();
	LoadVersionChoice();
	if (!LoadRealDinput()) {
		MessageBoxA(nullptr,
			"Could not load the real dinput8.dll.",
			"BBCSE rollback", MB_OK | MB_ICONERROR);
		return E_FAIL;
	}
	const HRESULT hr = g_realCreate(inst, version, iid, out, outer);
	static bool keysLoaded = false;
	if (!keysLoaded) { keysLoaded = true; LoadKeyRemap(); }
	if (SUCCEEDED(hr) && out && *out && g_keyRemap)
		PatchVtableSlot(*out, 3, reinterpret_cast<void*>(&HookCreateInputDevice), reinterpret_cast<void**>(&g_origCreateInputDevice));
	return hr;
}

extern "C" void __cdecl ggpo_bb_sync_error(const char* msg) {
	++g_netSyncErrors;
	if (g_netSyncErrors <= 5) Log("NET SYNCTEST ERROR: %s", msg);
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
	if (reason != DLL_PROCESS_ATTACH) {
		return TRUE;
	}

	DisableThreadLibraryCalls(self);
	g_self = self;
#ifdef BBCSE_PROXY_WINMM
	const bool winmmOk = LoadRealWinmm(self);
#endif
	{
		char name[64];
		sprintf_s(name, "Local\\bbcse-probe-%lu", GetCurrentProcessId());
		CreateMutexA(nullptr, FALSE, name);
		if (GetLastError() == ERROR_ALREADY_EXISTS) {
			g_passive = true;
			return TRUE;
		}
	}

	const std::string logPath = ModuleDir(self) + "bbcse-probe.log";
	WIN32_FILE_ATTRIBUTE_DATA fa{};
	if (GetFileAttributesExA(logPath.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (20u << 20)))
		MoveFileExA(logPath.c_str(), (ModuleDir(self) + "bbcse-probe.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
	g_log = _fsopen(logPath.c_str(), "a", _SH_DENYNO);
	if (g_log) {
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(g_log, "\n===== session %04d-%02d-%02d %02d:%02d:%02d =====\n", t.wYear, t.wMonth, t.wDay, t.wHour,
		        t.wMinute, t.wSecond);
	}

	Log("bbcse-probe attached");
	InstallCrashCapture(self);
	InstallVersionSelect();
#ifdef BBCSE_DEV_TOOLS
	ReportProcess();
#endif
#ifdef BBCSE_PROXY_WINMM
	Log("loaded as winmm.dll; real winmm %s", winmmOk ? "loaded" : "NOT FOUND (timers will fail)");
	if (!winmmOk)
		MessageBoxA(nullptr, "Could not load the real winmm.dll.", "BBCSE rollback", MB_OK | MB_ICONERROR);
	{
		void** slot = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr)) + (kDinputCreateIat - kImage));
		HMODULE di = GetModuleHandleA("dinput8.dll");
		void* real = di ? reinterpret_cast<void*>(GetProcAddress(di, "DirectInput8Create")) : nullptr;
		if (real && *slot == real) {
			g_realCreate = reinterpret_cast<DirectInput8Create_t>(real);
			DWORD old;
			VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
			*slot = reinterpret_cast<void*>(&DirectInput8Create);
			VirtualProtect(slot, sizeof(void*), old, &old);
			Log("DirectInput8Create hooked through the exe import slot (dinput8 at %p untouched)", di);
		} else {
			Log("DirectInput8Create NOT hooked (slot %p holds %p, dinput8 export %p): no rollback this session",
			    slot, *slot, real);
		}
	}
#endif
	LoadRealDinput();
	InstallVsyncOff(self);
	InstallCloudFix(self);

	return TRUE;
}
