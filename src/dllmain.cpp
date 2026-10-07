// BBCSE rollback - dinput8.dll proxy for BlazBlue: Continuum Shift Extend (Steam).
// Forwards DirectInput8Create to the system dinput8.dll and hooks the game's battle loop:
// GGPO rollback over the game's own Steam P2P connection, per-frame state save/load,
// frame pacing, the Network menu fix and an F1 stats overlay.

#include <winsock2.h>
#include <windows.h>
#include <intrin.h>
#include <dinput.h>

#include "ggponet.h"
#include "overlay.h"

#include <share.h>

#include <cstdio>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

extern "C" int (__cdecl* ggpo_bb_send)(const char* buf, int len);
extern "C" int (__cdecl* ggpo_bb_recv)(char* buf, int cap);
extern "C" sockaddr_in ggpo_bb_peer_addr;

namespace {
using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

HMODULE g_realDinput = nullptr;
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

BYTE* Live(DWORD ghidraVa) { return g_base + (ghidraVa - kImage); }

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
constexpr DWORD kScrollFallback = 0x40;

constexpr DWORD kZoomGuess = 0xB7C40C;

constexpr DWORD kRenderView = 0xB8C9D0;
constexpr DWORD kRenderViewSize = 0x220;

constexpr DWORD kEtcMgr = 0xEEE2D8;
constexpr DWORD kEtcMgrSize = 0x243240;
constexpr DWORD kObjMgr = 0xDF2C00;

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

SIZE_T GuardedHeapSize(HANDLE heap, const void* p) {
	__try {
		return HeapValidate(heap, 0, p) ? HeapSize(heap, 0, p) : static_cast<SIZE_T>(-1);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return static_cast<SIZE_T>(-1);
	}
}

SIZE_T HeapBlockSize(const void* p) {
	HANDLE heaps[64];
	const DWORD n = GetProcessHeaps(64, heaps);
	for (DWORD i = 0; i < n && i < 64; ++i) {
		const SIZE_T s = GuardedHeapSize(heaps[i], p);
		if (s != static_cast<SIZE_T>(-1)) return s;
	}
	return 0;
}

DWORD ScrollSize(const BYTE* scroll) {
	static const BYTE* cachedFor = nullptr;
	static DWORD cached = 0;
	if (scroll != cachedFor) {
		const SIZE_T s = HeapBlockSize(scroll);
		cached = (s >= 0x10 && s <= 0x400) ? static_cast<DWORD>(s) : kScrollFallback;
		cachedFor = scroll;
		Log("scroll object %p: size 0x%lX (%s)", scroll, cached, s ? "from heap" : "fallback");
	}
	return cached;
}

DWORD ListSize(const BYTE* list) {
	static const BYTE* cachedFor[3] = {};
	static DWORD cached[3] = {};
	for (int i = 0; i < 3; ++i)
		if (cachedFor[i] == list) return cached[i];
	const SIZE_T s = HeapBlockSize(list);
	DWORD size = 0;
	if (s >= 0x10 && s <= 0x4000) size = static_cast<DWORD>(s);
	else Log("WARNING: list %p has no readable heap size; not saved", list);
	static int next = 0;
	cachedFor[next] = list;
	cached[next] = size;
	next = (next + 1) % 3;
	return size;
}

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
	out[7] = {"zoom?", Live(kZoomGuess), 0};
	out[8] = {"rview", Live(kRenderView), 0};
	out[9] = {"etcmgr", Live(kEtcMgr), kEtcMgrSize};
	BYTE* const* list = reinterpret_cast<BYTE* const*>(Live(kScrollList));
	const DWORD count = *reinterpret_cast<const DWORD*>(Live(kScrollList) + 8);
	BYTE* scroll = (list[0] && count >= 1) ? *reinterpret_cast<BYTE* const*>(list[0]) : nullptr;
	if (!scroll) return false;
	out[10] = {"scroll", scroll, ScrollSize(scroll)};
	BYTE* pool = *reinterpret_cast<BYTE**>(mgr + kPoolPtrOff);
	if (!pool) return false;
	out[11] = {"objpool", pool, kPoolSize};
	static const char* const kListNames[3] = {"list44", "list48", "list4c"};
	for (int i = 0; i < 3; ++i) {
		BYTE* list = *reinterpret_cast<BYTE**>(mgr + kListOffs[i]);
		if (!list) return false;
		out[12 + i] = {kListNames[i], list, ListSize(list)};
	}
	return true;
}

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
					Log("  DIFF data  ghidra %08X len %4lu  old %08X now %08X", static_cast<DWORD>(at - g_base) + kImage,
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

void ReportHeapBlock(const char* what, const void* p) {
	const SIZE_T s = HeapBlockSize(p);
	if (s) Log("  %s %p: heap block size 0x%X", what, p, static_cast<unsigned>(s));
	else Log("  %s %p: not a heap block start (custom allocator or interior pointer)", what, p);
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

constexpr const char* kRecFile = "bbcse-dev/recording.bin";
constexpr const char* kAutoFlag = "bbcse-dev/autotest.on";
constexpr const char* kDumpDir = "bbcse-dev/dumps";

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

LONG g_flowGen = 0;
DWORD g_flowTarget[2];
#define FLOW_THUNK(N)                                                    	__declspec(naked) void FlowThunk##N() {                                		__asm lock inc dword ptr [g_flowGen]                                 		__asm jmp dword ptr [g_flowTarget + 4 * N]                           	}
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
		Log("  slot %d  this %08X  from ghidra %08X  calls %6ld  lastArg %08X", kFirstSlot + s.slot, s.self,
			s.ret - reinterpret_cast<DWORD>(g_base) + kImage, s.count, s.lastArg);
	}
	g_siteCount = 0;
	g_siteOverflow = 0;
}

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

constexpr int kRing = 8;

constexpr int kPhaseWords = 6;
struct Slot { std::string r[kRegions]; BYTE* addr[kRegions]; DWORD size[kRegions]; std::string ctrl; ParticleSnap fx;
              DWORD phase[kPhaseWords] = {}; LONG flowGen = 0; LONG frame = -1;
              bool hasScene = false; DWORD scene[3] = {};
              BYTE* uiPtr[10] = {}; DWORD ui[10][5] = {}; };

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
int g_syncN = 0;
LONG g_syncFrame = 0;
LONG g_syncTests = 0, g_syncFails = 0, g_syncFailLogs = 0;
double g_syncMs = 0, g_syncMsMax = 0;
const int kSyncSteps[] = {0, 1, 2, 4, 7};

constexpr DWORD kSceneStateOff = 0x2C, kRoundEndFlag = 0xBCE9F0;
constexpr int kSceneFighting = 8;
void* g_sceneForSlots = nullptr;

void SaveSceneFields(Slot& s) {
	s.hasScene = g_sceneForSlots != nullptr;
	if (!s.hasScene) return;
	s.scene[0] = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(g_sceneForSlots) + kSceneStateOff);
	s.scene[1] = *reinterpret_cast<DWORD*>(Live(kRoundEndFlag));
	s.scene[2] = *reinterpret_cast<DWORD*>(Live(0xBC12D8) + 4) & 2;
}
void RestoreSceneFields(const Slot& s) {
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

bool SaveSlot(Slot& s, LONG frame) {
	Region r[kRegions];
	if (!CurrentRegions(r)) return false;
	for (int i = 0; i < kRegions; ++i) {
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

bool RollbackKeepsLive(const char*) { return false; }

bool LoadSlot(const Slot& s) {
	Region r[kRegions];
	if (!CurrentRegions(r)) return false;
	for (int i = 0; i < kRegions; ++i)
		if (r[i].addr != s.addr[i] || r[i].size != s.size[i]) return false;
	if (!RestoreParticles(s.fx)) return false;
	for (int i = 0; i < kRegions; ++i)
		if (!RollbackKeepsLive(r[i].name)) RestoreRegion(r[i], s.r[i]);
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
		if (!CompareRegion(r[i].name) || RollbackKeepsLive(r[i].name) || memcmp(r[i].addr, s.r[i].data(), r[i].size) == 0) continue;
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

constexpr DWORD kD3DCreateIat = 0x9042D0;
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

HRESULT __stdcall HookReset(void* dev, void* pp) {
	if (g_vsyncWanted) ForceImmediate(pp, "Reset");
	Overlay_BeforeReset();
	const HRESULT hr = g_origReset(dev, pp);
	if (hr >= 0) Overlay_AfterReset();
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
			Log("PACING: Present blocks %.1f ms on average: the driver forces VSync (%d Hz). VSync paces alone now",
				g_presentEmaMs, g_refreshHz);
		} else if (!g_vsyncDetected) {
			g_vsyncDetected = true;
			Log("PACING: VSync forced at %d Hz: the mod keeps its 60 fps deadlines (frames show on the next refresh)",
				g_refreshHz);
		}
	}
}
HRESULT __stdcall HookPresent(void* dev, const void* src, const void* dst, HWND wnd, const void* dirty) {
	Overlay_OnPresent(static_cast<IDirect3DDevice9*>(dev));
	const double t0 = QpcNowUs();
	const HRESULT hr = g_origPresent(dev, src, dst, wnd, dirty);
	PaceAfterPresent((QpcNowUs() - t0) / 1000.0);
	if (g_waitAfterPresent) {
		g_waitAfterPresent = false;
		if (OwnPacing()) WaitDeadline();
	}
	return hr;
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
	Log("overlay: device hooks installed (F1 shows / hides the diagnostics panel in online battles)");
}
HRESULT __stdcall HookCreateDevice(void* d3d, UINT adapter, DWORD type, HWND wnd, DWORD flags, void* pp, void** dev) {
	if (g_vsyncWanted) ForceImmediate(pp, "CreateDevice");
	const HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, dev);
	if (hr >= 0 && dev && *dev) InstallDeviceHooks(*dev);
	return hr;
}
void* WINAPI HookD3DCreate(UINT sdk) {
	void* d3d = g_origD3DCreate(sdk);
	if (d3d && !g_origCreateDevice)
		PatchVtableSlot(d3d, 16, reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&g_origCreateDevice));
	return d3d;
}

void InstallVsyncOff(HMODULE self) {
	const std::string ini = ModuleDir(self) + "bbcse-net.ini";
	char mode[16] = {};
	GetPrivateProfileStringA("net", "mode", "", mode, sizeof(mode), ini.c_str());
	if (_stricmp(mode, "steam") != 0 || GetPrivateProfileIntA("net", "vsync", 0, ini.c_str()) != 0) return;
	g_vsyncWanted = true;
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
	Log("VSYNC: display creation hooked; VSync off, the mod paces frames at 60 (vsync=1 in bbcse-net.ini keeps VSync)");
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
bool g_netActive = false;
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
struct SyncMark { int frame = -1; DWORD hash = 0; int hp[2] = {}; };
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

constexpr int kSnapRing = 16;
struct SyncSnap { int frame = -1; BYTE* addr[2] = {}; std::string data[2]; };
SyncSnap g_syncSnaps[kSnapRing];

void MarkSyncFrame() {
	if (g_net.frame % kSyncEvery) return;
	SyncMark& m = g_syncMarks[(g_net.frame / kSyncEvery) % kSyncRing];
	m.frame = g_net.frame;
	m.hash = CrossPcHash();
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
			Log("SYNC frame %d hash %08lX hp %d %d", m.frame, m.hash, m.hp[0], m.hp[1]);
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
bool __cdecl NetLoad(unsigned char* buf, int) {
	struct Add { double t0; ~Add() { g_msLoad += NowMs() - t0; } } add{NowMs()};
	if (!buf) { Log("NET: load of an empty state (save had failed). Desync likely."); return false; }
	const Slot* s = reinterpret_cast<const Slot*>(buf);
	if (!LoadSlot(*s)) { Log("NET: load FAILED (frame %ld): regions moved. Desync likely.", s->frame); return false; }
	g_net.frame = s->frame;
	return true;
}
bool __cdecl NetLogState(char*, unsigned char*, int) { return true; }
void __cdecl NetFree(void* buf) { if (buf) g_slotFree.push_back(static_cast<Slot*>(buf)); }

bool __cdecl NetAdvance(int) {
	int disc = 0;
	ggpo_synchronize_input(g_net.session, g_netIn, sizeof(g_netIn), &disc);
	if (SceneFighting()) {
		struct Add { double t0; ~Add() { g_msResim += NowMs() - t0; } } add{NowMs()};
		g_netInUpdate = true;
		g_origUpdate(g_net.scene, nullptr, 0);
		g_netInUpdate = false;
		EffectStep();
	}
	++g_net.frame;
	MarkSyncFrame();
	++g_netRollbackFrames;
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

struct ISteamNet {
	virtual bool Send(unsigned long long to, const void* data, unsigned len, int type, int channel) = 0;
	virtual bool Available(unsigned* len, int channel) = 0;
	virtual bool Read(void* dst, unsigned cap, unsigned* len, unsigned long long* from, int channel) = 0;
};
ISteamNet* g_steamNet = nullptr;
unsigned long long g_steamPeer = 0;
DWORD g_netStartTick = 0;
bool g_netRealAdvance = false;

int __cdecl SteamSend(const char* buf, int len) {
	return g_steamNet->Send(g_steamPeer, buf, static_cast<unsigned>(len), 0 , kSteamChannel) ? len : 0;
}

DWORD g_fakeLagMs = 0;
struct HeldPacket { DWORD due; std::string data; };
std::vector<HeldPacket> g_heldPackets;

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
	if (!g_fakeLagMs) return SteamRecvNow(buf, cap);
	int got;
	while ((got = SteamRecvNow(buf, cap)) > 0) g_heldPackets.push_back({GetTickCount() + g_fakeLagMs, std::string(buf, got)});
	if (g_heldPackets.empty() || static_cast<LONG>(GetTickCount() - g_heldPackets.front().due) < 0) return 0;
	const std::string& d = g_heldPackets.front().data;
	const int n = static_cast<int>(d.size() < static_cast<size_t>(cap) ? d.size() : cap);
	memcpy(buf, d.data(), n);
	g_heldPackets.erase(g_heldPackets.begin());
	return n;
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
int __fastcall HookNetSend(void* self, void* edx, unsigned side, unsigned input) {
	if (SteamMode() && !g_netRealAdvance) return 1;
	return g_origNetSend(self, edx, side, input);
}

constexpr DWORD kNetIndexSite = 0x56F75E, kNetIndexFn = 0x45F9B0;
using NetIndex_t = void(__fastcall*)(void* netstate, void* edx);
NetIndex_t g_origNetIndex = nullptr;
void __fastcall HookNetIndex(void* netstate, void* edx) {
	if (SteamMode() && !g_netRealAdvance) return;
	g_origNetIndex(netstate, edx);
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
	if (SteamMode()) return 0;
	return g_origNetFetch(self, edx, slot);
}

bool StartSteamSession(GGPOSessionCallbacks& cb, int delay) {
	if (*reinterpret_cast<int*>(Live(kGameMgr) + 8) != 4) { Log("NET: not an online battle; rollback off"); return false; }
	BYTE* self = *reinterpret_cast<BYTE**>(Live(kNetSession) + 0x12040);
	if (!self) return false;
	const WORD flags = *reinterpret_cast<WORD*>(self + 0x5A);
	if (!(flags & 0x10)) { Log("NET: spectating; rollback off"); return false; }
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
	g_heldPackets.clear();

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
	Log("NET: Steam session: I am side %d, opponent %08lX%08lX, delay %d", g_net.side,
		static_cast<DWORD>(g_steamPeer >> 32), static_cast<DWORD>(g_steamPeer), delay);
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
	g_rngDrawSaved = g_netActive && r;
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
	}
}

constexpr int kHashChannel = 2;
struct PeerHash { int frame = -1; DWORD hash = 0; };
PeerHash g_peerHashes[kSyncRing];
bool g_desyncDumped = false;
int g_lastMatched = -1;

void SendSyncHash(int frame, DWORD hash) {
	if (!g_net.steam || !g_steamNet) return;
	const DWORD msg[3] = {0x48535942 , static_cast<DWORD>(frame), hash};
	g_steamNet->Send(g_steamPeer, msg, sizeof(msg), 2 , kHashChannel);
}

void DesyncDump(int frame, DWORD mine, DWORD theirs) {
	const SyncSnap& sn = g_syncSnaps[(frame / kSyncEvery) % kSnapRing];
	Log("NET DESYNC: frame %d, my hash %08lX, peer %08lX", frame, mine, theirs);
	g_ovl.desyncFrame = frame;
	if (sn.frame != frame) { Log("  (no state copy kept for that frame any more)"); return; }
	char path[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA("dinput8.dll"), path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	*(slash ? slash + 1 : path) = 0;
	char name[64];
	sprintf_s(name, "bbcse-desync-%d.bin", frame);
	strcat_s(path, name);
	FILE* f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || !f) { Log("  could not write %s", path); return; }
	const DWORD hdr[3] = {0x53444242 , static_cast<DWORD>(frame), static_cast<DWORD>(g_net.side)};
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
	Log("  state of frame %d written to %s (send this file)", frame, path);
}

void PumpSyncHashes() {
	if (!g_net.steam || !g_steamNet) return;
	unsigned size = 0, got = 0;
	unsigned long long from = 0;
	DWORD msg[3];
	while (g_steamNet->Available(&size, kHashChannel)) {
		if (!g_steamNet->Read(msg, sizeof(msg), &got, &from, kHashChannel)) break;
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
	Log("NET: session closed");
}

void NetBattleStart() {
	NetStop();
	g_steamRearm = false;
	char ini[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA("dinput8.dll"), ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	*(slash ? slash + 1 : ini) = 0;
	strcat_s(ini, "bbcse-net.ini");
	if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
	char mode[16], remote[64];
	GetPrivateProfileStringA("net", "mode", "", mode, sizeof(mode), ini);
	g_net.synctest = _stricmp(mode, "synctest") == 0;
	const bool steam = _stricmp(mode, "steam") == 0;
	if (!g_net.synctest && !steam && _stricmp(mode, "p2p") != 0) { Log("NET: %s has no mode=steam/p2p/synctest; network off", ini); return; }
	g_net.side = GetPrivateProfileIntA("net", "side", 0, ini) ? 1 : 0;
	g_fakeLagMs = static_cast<DWORD>(GetPrivateProfileIntA("net", "fakelag", 0, ini));
	g_heldPackets.clear();
	if (g_fakeLagMs) Log("NET: fakelag %lu ms (test aid: packets from the peer are held back)", g_fakeLagMs);
	g_net.seed = static_cast<DWORD>(GetPrivateProfileIntA("net", "seed", 12345, ini));
	const int port = GetPrivateProfileIntA("net", "localport", 7000, ini);
	const int delay = GetPrivateProfileIntA("net", "delay", 2, ini);
	GetPrivateProfileStringA("net", "remote", "", remote, sizeof(remote), ini);

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
	} else if (g_net.synctest) {
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
			g_ovl.rollbackMaxDepth = g_ovlMaxDepth;
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
	g_origUpdate(self, edx, arg);
	g_realFrame = -1;
	g_holdSim = false;
	++g_ticks;
	if (g_mode == 2) { g_origUpdate(self, edx, arg); ++g_doubled; }
	if (g_mode == 3) g_mode = 1;
	if (g_inMode != InMode::Idle) ++g_frameIdx;

	const DWORD now = GetTickCount();
	if (now - g_lastReport >= 1000) {
		Log("1s: calls=%ld ticks=%ld skipped=%ld doubled=%ld  P1 frame %d -> %d  arg=%d mode=%d",
			g_calls, g_ticks, g_skipped, g_doubled, before, P1FrameCounter(), arg, g_mode);
		if (*reinterpret_cast<int*>(Live(kGameMgr) + 8) == 4) LogNetCounters("no rollback (spectating?)");
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
	}
}

void WriteMiniDump(EXCEPTION_POINTERS* ep) {
	using Dump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
	HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
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

LONG CALLBACK CrashCapture(EXCEPTION_POINTERS* ep) {
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	const bool cxx = code == 0xE06D7363;
	const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
	                   code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
	                   code == 0xC0000409 ;
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
	AddVectoredExceptionHandler(1, CrashCapture);
	Log("crash capture on (C++ exceptions and faults -> log, bbcse-crash.txt, bbcse-crash.dmp)");
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
		{kNetFetchSite, kNetFetchFn, reinterpret_cast<void*>(&HookNetFetch)},
		{kNetIndexSite, kNetIndexFn, reinterpret_cast<void*>(&HookNetIndex)},
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
	Log("tick hook installed (%u call sites). F5 freeze, F6 double, F7 step one frame while frozen.",
		static_cast<unsigned>(sizeof(patches) / sizeof(patches[0])));
	InstallInputProbe();
	InstallFlowHooks();

	InstallDeviceHooks(*reinterpret_cast<void**>(Live(0xB8C800 + 0x2C)));
}

}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid,
                                             LPVOID* out, LPUNKNOWN outer) {
	InstallTickHook();
	if (!LoadRealDinput()) {
		MessageBoxA(nullptr,
			"bbcse-probe: could not load the real dinput8.dll.",
			"bbcse-probe", MB_OK | MB_ICONERROR);
		return E_FAIL;
	}
	return g_realCreate(inst, version, iid, out, outer);
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

	const std::string logPath = ModuleDir(self) + "bbcse-probe.log";

	MoveFileExA(logPath.c_str(), (ModuleDir(self) + "bbcse-probe.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
	g_log = _fsopen(logPath.c_str(), "w", _SH_DENYNO);

	Log("bbcse-probe attached");
	InstallCrashCapture(self);
	ReportProcess();
	LoadRealDinput();
	InstallVsyncOff(self);

	return TRUE;
}
