// ImGui overlay: F1 panel, F2 menu, F3 frame advantage, F4 hitboxes.

#include <d3d9.h>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "overlay.h"
#include "framedata.h"
#include "hitboxes.h"
#include "palette.h"
#include "imfont.h"

OverlayStats g_ovl;
static bool s_ready = false, s_visible = true, s_f1Down = false, s_f3Down = false, s_f4Down = false;
static bool s_menuOpen = false, s_f2Down = false;
static bool s_netBar = true;
static WNDPROC s_origWndProc = nullptr;
static HWND s_wnd = nullptr;
static std::vector<std::string> s_versions;
static int s_menuSize = 1;
static int s_page = 0;
static const float kMenuScales[] = {0.8f, 1.0f, 1.3f, 1.6f};
static const char* const kMenuSizeNames[] = {"Small", "Normal", "Large", "Extra large"};
enum { Page_Game, Page_Training, Page_Online, Page_Interface, Page_Count };
static const char* const kPageNames[] = {"Game", "Training", "Online", "Interface"};
static const char* const kPageBlurbs[] = {
	"Pick which version of the game's characters and rules you play: CSE, CS2 or CS1.",
	"Training tools. They only work in Training mode.",
	"Online play and the rollback panel.",
	"Size and look of this menu.",
};

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool FrameAdvantageShown() { return g_frameAdvVisible && g_frameAdv.valid; }

static bool PaletteEditorShown() { return g_paletteEditorVisible && g_paletteScene.valid && g_paletteScene.training; }

static bool WantsMouse() { return s_menuOpen || FrameAdvantageShown() || PaletteEditorShown(); }

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (s_ready && WantsMouse() && ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
	return CallWindowProcA(s_origWndProc, h, m, w, l);
}

static void Init(IDirect3DDevice9* dev) {
	D3DDEVICE_CREATION_PARAMETERS cp{};
	if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowBorderSize = 1;
	style.FrameBorderSize = 1;
	style.ScrollbarSize = 14;
	style.Alpha = 0.95f;
	io.Fonts->AddFontFromMemoryCompressedTTF(DroidSans_compressed_data, static_cast<int>(DroidSans_compressed_size), 18.0f);
	s_wnd = cp.hFocusWindow;
	s_ready = ImGui_ImplWin32_Init(cp.hFocusWindow) && ImGui_ImplDX9_Init(dev);
	if (s_ready && !s_origWndProc)
		s_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(cp.hFocusWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
}

static void Row(const char* label, const char* fmt, ...) {
	ImGui::TextUnformatted(label);
	ImGui::SameLine(170);
	va_list a;
	va_start(a, fmt);
	ImGui::TextV(fmt, a);
	va_end(a);
}

static std::string InvulText(const MoveFrameData& m) {
	if (m.invulFirst < 0) return "";
	std::string s = std::to_string(m.invulFirst);
	if (m.invulLast != m.invulFirst) s += "-" + std::to_string(m.invulLast);
	static const struct { unsigned int bit; const char* name; } kKinds[] = {{0x8, "H"}, {0x10, "B"}, {0x20, "F"}, {0x40, "T"}};
	std::string kinds;
	for (const auto& k : kKinds)
		if (m.invulFlags & k.bit) kinds += kinds.empty() ? k.name : std::string(" ") + k.name;
	return kinds.empty() ? s : s + " (" + kinds + ")";
}

static void DrawFrameAdvantage() {
	const FrameAdvantageView& fa = g_frameAdv;
	const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f), red(1.0f, 0.0f, 0.0f, 1.0f), green(0.0f, 1.0f, 0.0f, 1.0f);
	ImGui::SetNextWindowSize(ImVec2(300, 140), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(350, 250), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Frame Advantage", &g_frameAdvVisible, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	ImGui::Columns(2, "columns_layout", true);
	for (int side = 0; side < 2; ++side) {
		const int adv = side == 0 ? fa.advantage : -fa.advantage;
		const int gap = side == 0 ? fa.p1Gap : fa.p2Gap;
		ImGui::Text("Player %d", side + 1);
		ImGui::TextUnformatted("Gap");
		ImGui::SameLine();
		ImGui::TextUnformatted(gap != -1 ? std::to_string(gap).c_str() : "");
		ImGui::TextUnformatted("Advantage");
		ImGui::SameLine();
		ImGui::TextColored(adv > 0 ? green : adv < 0 ? red : white, "%s", ((adv > 0 ? "+" : "") + std::to_string(adv)).c_str());
		if (fa.hitSide >= 0) {
			ImGui::SameLine();
			ImGui::TextDisabled(fa.onBlock ? "on block" : "on hit");
		}
		const MoveFrameData& m = fa.move[side];
		ImGui::TextUnformatted("Startup");
		ImGui::SameLine();
		ImGui::TextUnformatted(m.startup > 0 ? std::to_string(m.startup).c_str() : "");
		ImGui::TextUnformatted("Invul");
		ImGui::SameLine();
		ImGui::TextUnformatted(InvulText(m).c_str());
		if (side == 0) ImGui::NextColumn();
	}
	ImGui::Columns(1);
	ImGui::End();
}

std::string CharacterNames(const std::string& codes) {
	static const char* const kNames[][2] = {
		{"ar", "Arakune"}, {"bn", "Bang"}, {"ca", "Carl"}, {"ha", "Hakumen"}, {"hz", "Hazama"}, {"jn", "Jin"},
		{"lc", "Litchi"}, {"mk", "Makoto"}, {"mu", "Mu"}, {"no", "Noel"}, {"ny", "Lambda"}, {"pt", "Platinum"},
		{"rc", "Rachel"}, {"rg", "Ragna"}, {"rl", "Relius"}, {"tb", "Tsubaki"}, {"tg", "Tager"}, {"tk", "Taokaka"},
		{"vh", "Valkenhayn"},
	};
	std::string out;
	for (size_t i = 0; i < codes.size(); i += 3) {
		const std::string code = codes.substr(i, 2);
		const char* name = code.c_str();
		for (const auto& n : kNames)
			if (code == n[0]) name = n[1];
		if (!out.empty()) out += ", ";
		out += name;
	}
	return out;
}

static std::string UpperVersion() {
	std::string v = Version_Current();
	for (char& c : v) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
	return v;
}

static void DrawHudBox(const char* text, float centerY, float maxWidth, float pixelsAt1080, ImU32 color) {
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImFont* font = ImGui::GetFont();
	float fontSize = pixelsAt1080 * display.y / 1080.0f;
	ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
	float pad = fontSize * 0.4f;
	if (size.x + pad * 2 > maxWidth) {
		fontSize *= maxWidth / (size.x + pad * 2);
		pad = fontSize * 0.4f;
		size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
	}
	const float top = floorf(centerY - size.y * 0.5f);
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	dl->AddRectFilled(ImVec2(floorf((display.x - size.x) * 0.5f) - pad, top - pad * 0.5f),
	                  ImVec2(floorf((display.x + size.x) * 0.5f) + pad, top + size.y + pad * 0.5f), IM_COL32(0, 0, 0, 160), pad * 0.5f);
	float y = top;
	for (const char* line = text; *line;) {
		const char* end = strchr(line, '\n');
		if (!end) end = line + strlen(line);
		const ImVec2 lineSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, line, end);
		dl->AddText(font, fontSize, ImVec2(floorf((display.x - lineSize.x) * 0.5f), y), color, line, end);
		y += lineSize.y;
		line = *end ? end + 1 : end;
	}
}

static void DrawVersionBadge() {
	if (_stricmp(Version_Current(), "cse") == 0 || !Version_InFight()) return;
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	DrawHudBox(UpperVersion().c_str(), display.y * 0.03f, display.x * 0.11f, 34.0f, IM_COL32(255, 220, 90, 255));
}

static const char* VersionDescription(const std::string& v) {
	if (_stricmp(v.c_str(), "cse") == 0) return "Continuum Shift Extend, the PC game as it is.";
	if (_stricmp(v.c_str(), "cs2") == 0) return "Continuum Shift 2: version 2.0 characters.";
	if (_stricmp(v.c_str(), "cs1") == 0) return "Continuum Shift: version 1.00 characters and system rules.";
	return "Files from the bbcse-versions folder of the same name.";
}

static void DrawGamePage() {
	ImGui::SeparatorText("Game version");
	if (ImGui::IsWindowAppearing() || s_versions.empty()) Version_List(s_versions);
	const bool locked = Version_Locked();
	if (locked) ImGui::BeginDisabled();
	for (const std::string& v : s_versions) {
		std::string label = v;
		for (char& c : label) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
		if (ImGui::RadioButton(label.c_str(), _stricmp(Version_Current(), v.c_str()) == 0)) Version_Select(v.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", VersionDescription(v));
	}
	if (locked) ImGui::EndDisabled();
	ImGui::Spacing();
	const std::string codes = Version_LoadedCodes();
	if (_stricmp(Version_Current(), "cse") != 0)
		ImGui::Text("Last fight, loaded from %s: %s", UpperVersion().c_str(), codes.empty() ? "nothing yet" : CharacterNames(codes).c_str());
	ImGui::Spacing();
	ImGui::SeparatorText("Save");
	static int s_unlocked = -2;
	if (Version_InFight()) ImGui::BeginDisabled();
	if (ImGui::Button("Unlock all gallery items")) ImGui::OpenPopup("unlock");
	if (Version_InFight()) ImGui::EndDisabled();
	if (ImGui::BeginPopupModal("unlock", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Marks every gallery item as bought: Unlimited characters, colors, art, movies, voices.");
		ImGui::TextUnformatted("The current save is copied to Save\\bbsave.before-unlock.dat first.");
		if (ImGui::Button("Unlock", ImVec2(120, 0))) {
			s_unlocked = Save_UnlockGallery();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (s_unlocked >= 0) ImGui::TextDisabled("%d items unlocked. Restart the game to see them.", s_unlocked);
	else if (s_unlocked == -1) ImGui::TextDisabled("Could not read or write Save\\bbsave.dat.");
	ImGui::PushTextWrapPos(0.0f);
	if (Version_ForcedReason()[0])
		ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Online: %s %s. Your own choice comes back when you leave the room.", Version_ForcedReason(), UpperVersion().c_str());
	else if (locked)
		ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Leave the fight to change the version.");
	else
		ImGui::TextDisabled("Applies from the next character load. Online, both players and spectators need the same version.");
	ImGui::PopTextWrapPos();
}

static void DrawTrainingPage() {
	ImGui::SeparatorText("Overlays");
	ImGui::Checkbox("Frame advantage", &g_frameAdvVisible);
	ImGui::SameLine();
	ImGui::TextDisabled("F3");
	ImGui::Checkbox("Hitboxes", &g_hitboxVisible);
	ImGui::SameLine();
	ImGui::TextDisabled("F4");
	ImGui::SeparatorText("Tools");
	ImGui::Checkbox("Palette editor", &g_paletteEditorVisible);
	ImGui::Spacing();
	ImGui::TextDisabled("Shown in Training mode only.");
}

static void DrawOnlinePage() {
	ImGui::SeparatorText("Rollback");
	ImGui::Checkbox("Rollback panel", &s_visible);
	ImGui::SameLine();
	ImGui::TextDisabled("F1, during online fights");
	ImGui::Checkbox("Ping and rollback bar", &s_netBar);
	ImGui::SameLine();
	ImGui::TextDisabled("during online fights");
	ImGui::Spacing();
	ImGui::SeparatorText("Version");
	ImGui::Text("This PC: %s", Version_Current());
	if (g_ovl.peerVersion[0]) {
		if (g_ovl.versionMismatch)
			ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Opponent: %s  (mismatch, the match will desync)", g_ovl.peerVersion);
		else
			ImGui::Text("Opponent: %s", g_ovl.peerVersion);
	}
	ImGui::Spacing();
	Palette_DrawOnlineSection();
}

static void DrawInterfacePage() {
	ImGui::SeparatorText("Menu");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	ImGui::Combo("Menu size", &s_menuSize, kMenuSizeNames, IM_ARRAYSIZE(kMenuSizeNames));
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	ImGui::SliderFloat("Opacity", &ImGui::GetStyle().Alpha, 0.5f, 1.0f, "%.2f");
}

static void DrawMenu() {
	ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(420, 280), ImVec2(4000, 3000));
	if (!ImGui::Begin("IM BBCS:EX###immenu", &s_menuOpen, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	ImGui::BeginChild("##nav", ImVec2(ImGui::GetFontSize() * 9.0f, 0), ImGuiChildFlags_Borders);
	ImGui::SeparatorText("Pages");
	for (int i = 0; i < Page_Count; ++i)
		if (ImGui::Selectable(kPageNames[i], s_page == i)) s_page = i;
	ImGui::Spacing();
	ImGui::SeparatorText("Keys");
	ImGui::TextDisabled("F2  this menu");
	ImGui::TextDisabled("F1  online panel");
	ImGui::TextDisabled("F3  frame advantage");
	ImGui::TextDisabled("F4  hitboxes");
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##page", ImVec2(0, 0));
	ImGui::TextUnformatted(kPageNames[s_page]);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", kPageBlurbs[s_page]);
	ImGui::PopTextWrapPos();
	ImGui::Separator();
	ImGui::Spacing();
	switch (s_page) {
	case Page_Game: DrawGamePage(); break;
	case Page_Training: DrawTrainingPage(); break;
	case Page_Online: DrawOnlinePage(); break;
	default: DrawInterfacePage(); break;
	}
	ImGui::EndChild();
	ImGui::End();
}

static void DrawNetBar() {
	const OverlayStats& s = g_ovl;
	if (!s_netBar || !s.session || !Version_InFight()) return;
	char text[64];
	sprintf_s(text, "Ping %d ms\nRollback %df", s.ping, s.rollbackMaxDepth);
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	DrawHudBox(text, display.y * 0.925f, display.x * 0.12f, 26.0f, IM_COL32(255, 255, 255, 255));
}

static void Draw() {
	DrawVersionBadge();
	DrawNetBar();
	if (s_menuOpen) DrawMenu();
	if (g_hitboxVisible && g_hitboxScene.valid)
		Hitboxes_Draw(ImGui::GetBackgroundDrawList(), ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
	if (FrameAdvantageShown()) DrawFrameAdvantage();
	if (PaletteEditorShown()) Palette_DrawEditor();
	const OverlayStats& s = g_ovl;
	if (!s_visible || !s.session || !Version_InFight()) return;
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
	if (s.versionMismatch)
		ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "VERSION MISMATCH: you %s, opponent %s", s.version, s.peerVersion);
	else
		Row("Version", "%s", s.version);
	ImGui::Separator();
	if (s.desyncFrame >= 0)
		ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "DESYNC at frame %d (state saved to bbcse-desync.bin)", s.desyncFrame);
	else
		ImGui::TextColored(ImVec4(0.45f, 1, 0.45f, 1), "In sync (%d checks matched)", s.syncChecks);
	ImGui::End();
}

static void FitDisplayToBackBuffer(IDirect3DDevice9* dev) {
	IDirect3DSurface9* bb = nullptr;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
	D3DSURFACE_DESC d{};
	const bool ok = SUCCEEDED(bb->GetDesc(&d));
	bb->Release();
	RECT rc{};
	if (!ok || !GetClientRect(s_wnd, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) return;
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(d.Width), static_cast<float>(d.Height));
	ImGui_ImplWin32_SetMousePosScale(static_cast<float>(d.Width) / (rc.right - rc.left), static_cast<float>(d.Height) / (rc.bottom - rc.top));
}

static bool KeyDown(int vk) {
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void Overlay_OnPresent(IDirect3DDevice9* dev) {
	Palette_SetDevice(dev);
	const bool f1 = KeyDown(VK_F1);
	if (f1 && !s_f1Down) s_visible = !s_visible;
	s_f1Down = f1;
	const bool f3 = KeyDown(VK_F3);
	if (f3 && !s_f3Down) g_frameAdvVisible = !g_frameAdvVisible;
	s_f3Down = f3;
	const bool f4 = KeyDown(VK_F4);
	if (f4 && !s_f4Down) g_hitboxVisible = !g_hitboxVisible;
	s_f4Down = f4;
	const bool f2 = KeyDown(VK_F2);
	if (f2 && !s_f2Down) s_menuOpen = !s_menuOpen;
	s_f2Down = f2;
	if (s_ready) {
		ImGuiIO& io = ImGui::GetIO();
		if (WantsMouse()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		else io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		if (PaletteEditorShown()) io.ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;
		else io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
		io.MouseDrawCursor = WantsMouse();
	}
	const bool badge = _stricmp(Version_Current(), "cse") != 0 && Version_InFight();
	if (!badge && !s_menuOpen && !(s_visible && g_ovl.session && Version_InFight()) && !(g_frameAdvVisible && g_frameAdv.valid) && !(g_hitboxVisible && g_hitboxScene.valid) && !PaletteEditorShown() && !(s_netBar && g_ovl.session && Version_InFight())) return;
	if (!s_ready) {
		Init(dev);
		if (!s_ready) return;
	}
	ImGui::GetStyle().FontScaleMain = kMenuScales[s_menuSize];
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	FitDisplayToBackBuffer(dev);
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
