// F1 stats overlay (Dear ImGui, DX9 + Win32 backends), drawn in online battles.

#include <d3d9.h>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "overlay.h"

OverlayStats g_ovl;
static bool s_ready = false, s_visible = true, s_f1Down = false;

static void Init(IDirect3DDevice9* dev) {
	D3DDEVICE_CREATION_PARAMETERS cp{};
	if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
	ImGui::StyleColorsDark();
	ImGui::GetStyle().Alpha = 0.92f;
	s_ready = ImGui_ImplWin32_Init(cp.hFocusWindow) && ImGui_ImplDX9_Init(dev);
}

static void Row(const char* label, const char* fmt, ...) {
	ImGui::TextUnformatted(label);
	ImGui::SameLine(170);
	va_list a;
	va_start(a, fmt);
	ImGui::TextV(fmt, a);
	va_end(a);
}

static void Draw() {
	const OverlayStats& s = g_ovl;
	if (!s.session) return;
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowBgAlpha(0.75f);
	const ImGuiWindowFlags f = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
	                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	ImGui::Begin("BBCSE rollback  (F1 hides)", nullptr, f);
	Row("Status", "%s   side %d   delay %d", s.status, s.side, s.delay);
	Row("Ping", "%d ms", s.ping);
	Row("Frame", "%d   (%.1f updates/s)", s.frame, s.updatesPerSec);
	ImGui::Separator();
	Row("Rollback", "%d frames/s   deepest %d", s.rollbackFramesPerSec, s.rollbackMaxDepth);
	ImGui::PlotHistogram("##rb", s.rollbackHistory, OverlayStats::kHistory, s.historyPos, "rollback depth per frame",
	                     0.0f, 8.0f, ImVec2(300, 50));
	Row("Frame advantage", "%+.2f frames   (slowed %.1f ms/s)", s.aheadFrames, s.pacedMsPerSec);
	Row("Held frames/s", "limit %d  pause %d  sync %d  connect %d", s.heldLimit, s.heldPause, s.heldSync, s.heldConnect);
	ImGui::Separator();
	Row("Frame work", "avg %.1f ms   max %.1f ms", s.tickAvgMs, s.tickMaxMs);
	ImGui::PlotLines("##ms", s.tickMsHistory, OverlayStats::kHistory, s.historyPos, "mod work per frame (ms)", 0.0f, 16.6f,
	                 ImVec2(300, 50));
	Row("Save / load / replay", "%.0f / %.0f / %.0f ms/s", s.saveMs, s.loadMs, s.replayMs);
	Row("Input delay", "%.1f frames (measured)", s.inputDelay);
	Row("VSync", "%s", s.vsyncOff ? "off (game limiter, 60 fps)" : "on");
	ImGui::Separator();
	if (s.desyncFrame >= 0)
		ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "DESYNC at frame %d (state saved to bbcse-desync.bin)", s.desyncFrame);
	else
		ImGui::TextColored(ImVec4(0.45f, 1, 0.45f, 1), "In sync (%d checks matched)", s.syncChecks);
	ImGui::End();
}

void Overlay_OnPresent(IDirect3DDevice9* dev) {
	const bool f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
	if (f1 && !s_f1Down) s_visible = !s_visible;
	s_f1Down = f1;
	if (!s_visible) return;
	if (!s_ready) {
		Init(dev);
		if (!s_ready) return;
	}
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	Draw();
	ImGui::EndFrame();
	if (SUCCEEDED(dev->BeginScene())) {
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		dev->EndScene();
	}
}

void Overlay_BeforeReset() {
	if (s_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
}
void Overlay_AfterReset() {
	if (s_ready) ImGui_ImplDX9_CreateDeviceObjects();
}
