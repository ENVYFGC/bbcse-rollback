#include "palette.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

PaletteScene g_paletteScene;
bool g_paletteEditorVisible = false;

std::string CharacterNames(const std::string& codes);
void Palette_Log(const char* text);

namespace {
constexpr int kCharsPtrOff = 0x62654;
constexpr int kCharStride = 0x20140;
constexpr int kLoadBase = 0x2AC;
constexpr int kLoadStride = 0x20BD8;
constexpr int kPalListOff = 0x82C;
constexpr int kPalIndexOff = 0x2E8;
constexpr int kPalIndexStride = 0x1C;
constexpr int kCharCodeOff = 0x20E8;
constexpr int kEntryDirtyOff = 0x8;
constexpr int kEntryColorsOff = 0x14;
constexpr int kEntryCopyOff = 0x814;
constexpr int kColors = 256;
constexpr int kFileBytes = kColors * 4;
constexpr int kFiles = 8;
constexpr int kGameColors = 24;
constexpr int kColumns = 16;
constexpr size_t kMaxPalettes = 256;
constexpr DWORD kMaxPngBytes = 1 << 20;
constexpr double kUndoMergeSeconds = 0.2;
const char* const kFileNames[kFiles] = {"Character", "Effect01", "Effect02", "Effect03", "Effect04", "Effect05", "Effect06", "Effect07"};
const char kSignature[8] = "IMPLCF";
const char* const kExtension = ".cfpl";
constexpr short kCseMark = 0x5343;
constexpr size_t kHplHeader = 0x20;
const char* const kCharacterKeyword = "BBCSEIM_Character";

#pragma pack(push, 1)
struct PalHeader {
	char signature[8];
	int headerLen;
	int dataLen;
	short charIndex;
	short reserved;
};
struct PalInfo {
	char name[32];
	char creator[32];
	char desc[64];
	bool hasBloom;
};
struct PalData {
	PalInfo info;
	unsigned char files[kFiles][kFileBytes];
};
#pragma pack(pop)

struct SideState {
	const unsigned char* list = nullptr;
	int palIndex = -1;
	std::string code;
	bool backedUp = false;
	unsigned char original[kFiles][kFileBytes] = {};
	std::string selected;
	bool editorChoice = false;
	std::vector<PalData> custom;
};

struct Change {
	int side;
	unsigned char before[kFiles][kFileBytes];
	unsigned char after[kFiles][kFileBytes];
	double time;
};

SideState g_side[2];
int s_side = 0;
int s_file = 0;
unsigned char s_edit[kFileBytes] = {};
unsigned char s_highlightArray[kFileBytes] = {};
bool s_highlight = false;
bool s_showAlpha = false;
int s_highlighted = -1;
char s_name[32] = "", s_creator[32] = "", s_desc[64] = "";
std::string s_message;
std::vector<Change> s_undo, s_redo;
bool s_iniChecked = false;

unsigned char* Char(int side) {
	unsigned char* chars = *reinterpret_cast<unsigned char**>(g_paletteScene.objMgr + kCharsPtrOff);
	return chars ? chars + side * kCharStride : nullptr;
}

const unsigned char* List(int side) {
	return *reinterpret_cast<const unsigned char* const*>(g_paletteScene.objMgr + kLoadBase + side * kLoadStride + kPalListOff);
}

unsigned char* Entry(int side, int palIndex, int file) {
	const unsigned char* list = List(side);
	if (!list || palIndex < 0) return nullptr;
	unsigned char* const* entries = *reinterpret_cast<unsigned char* const* const*>(list + 4);
	const int count = *reinterpret_cast<const int*>(list + 0xC);
	const int i = palIndex * kFiles + file;
	return entries && i < count ? entries[i] : nullptr;
}

const unsigned char* GameFile(int side, int file) {
	const unsigned char* e = Entry(side, g_side[side].palIndex, file);
	return e ? e + kEntryColorsOff : nullptr;
}

void WriteGameFile(int side, int file, const unsigned char* colors) {
	unsigned char* e = Entry(side, g_side[side].palIndex, file);
	if (!e) return;
	memcpy(e + kEntryColorsOff, colors, kFileBytes);
	memcpy(e + kEntryCopyOff, colors, kFileBytes);
	*reinterpret_cast<int*>(e + kEntryDirtyOff) = 1;
}

void Backup(int side) {
	SideState& s = g_side[side];
	if (s.backedUp) return;
	for (int f = 0; f < kFiles; ++f) {
		const unsigned char* src = GameFile(side, f);
		if (src) memcpy(s.original[f], src, kFileBytes);
	}
	s.backedUp = true;
}

void ReadAll(int side, unsigned char (&out)[kFiles][kFileBytes]) {
	for (int f = 0; f < kFiles; ++f) {
		const unsigned char* src = GameFile(side, f);
		if (src) memcpy(out[f], src, kFileBytes);
		else memset(out[f], 0, kFileBytes);
	}
}

void ApplyFile(int side, int file, const unsigned char* colors) {
	Backup(side);
	WriteGameFile(side, file, colors);
}

bool IsEmpty(const unsigned char* colors) {
	for (int i = 0; i < kFileBytes; ++i)
		if (colors[i]) return false;
	return true;
}

void ApplyFiles(int side, const unsigned char (*files)[kFileBytes]) {
	Backup(side);
	const SideState& s = g_side[side];
	for (int f = 0; f < kFiles; ++f)
		WriteGameFile(side, f, files && !IsEmpty(files[f]) ? files[f] : s.original[f]);
}

std::string GameDir() {
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string dir = path;
	return dir.substr(0, dir.find_last_of("\\/"));
}

std::string Root() { return GameDir() + "\\bbcse-palettes"; }
std::string Folder(const std::string& code) { return Root() + "\\" + CharacterNames(code); }
std::string IniPath() { return Root() + "\\palettes.ini"; }

unsigned int Crc32(const unsigned char* data, size_t len, unsigned int crc = 0) {
	static unsigned int table[256];
	if (!table[1])
		for (unsigned int n = 0; n < 256; ++n) {
			unsigned int c = n;
			for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[n] = c;
		}
	crc = ~crc;
	for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

void PutBE32(std::vector<unsigned char>& out, unsigned int v) {
	for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<unsigned char>(v >> s));
}

unsigned int GetBE32(const unsigned char* p) { return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3]; }

void PutChunk(std::vector<unsigned char>& out, const char* type, const unsigned char* data, size_t len) {
	PutBE32(out, static_cast<unsigned int>(len));
	const size_t start = out.size();
	out.insert(out.end(), type, type + 4);
	if (len) out.insert(out.end(), data, data + len);
	PutBE32(out, Crc32(out.data() + start, out.size() - start));
}

constexpr DWORD kHipRle8 = 0x20010101;

struct Sprite {
	std::string name;
	int w = 0, h = 0;
	std::vector<unsigned char> pixels;
};

struct Texture {
	IDirect3DTexture9* tex = nullptr;
	int w = 0, h = 0;
	unsigned long long key = 0;
};

IDirect3DDevice9* s_device = nullptr;
Sprite s_sprite[2];
Texture s_preview;
std::map<std::string, Texture> s_thumbs;

struct PackFile {
	std::string name;
	const unsigned char* data;
	unsigned int size;
};

bool ReadableRange(const void* p, size_t n) {
	const unsigned char* at = static_cast<const unsigned char*>(p);
	const unsigned char* end = at + n;
	if (!p || end < at) return false;
	while (at < end) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
		at = static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
	}
	return true;
}

std::vector<PackFile> PackFiles(const unsigned char* pack) {
	std::vector<PackFile> out;
	if (!ReadableRange(pack, 0x20) || memcmp(pack, "FPAC", 4) != 0) return out;
	const unsigned int dataStart = *reinterpret_cast<const unsigned int*>(pack + 4);
	const unsigned int total = *reinterpret_cast<const unsigned int*>(pack + 8);
	const unsigned int count = *reinterpret_cast<const unsigned int*>(pack + 0xC);
	const unsigned int nameLen = *reinterpret_cast<const unsigned int*>(pack + 0x14);
	if (!count || dataStart < 0x20 || dataStart > total || !nameLen || nameLen > 0x100 || !ReadableRange(pack, total)) return out;
	const unsigned int rec = (dataStart - 0x20) / count;
	if (rec < nameLen + 12) return out;
	const unsigned int room = total - dataStart;
	for (unsigned int i = 0; i < count; ++i) {
		const unsigned char* r = pack + 0x20 + i * rec;
		const unsigned int off = *reinterpret_cast<const unsigned int*>(r + nameLen + 4);
		const unsigned int size = *reinterpret_cast<const unsigned int*>(r + nameLen + 8);
		if (off > room || size > room - off) continue;
		out.push_back({std::string(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), nameLen)), pack + dataStart + off, size});
	}
	return out;
}

PackFile FindLoadedImage(int side, const std::string& name) {
	if (g_paletteScene.fileBuffer) {
		for (int id = 0x50 + side * 8; id < 0x58 + side * 8; ++id) {
			const unsigned char* pack = *reinterpret_cast<const unsigned char* const*>(g_paletteScene.fileBuffer + id * 8);
			for (const PackFile& f : PackFiles(pack))
				if (_stricmp(f.name.c_str(), name.c_str()) == 0) return f;
		}
	}
	return {std::string(), nullptr, 0};
}

bool DecodeHip(const PackFile& file, Sprite& out) {
	const unsigned char* hip = file.data;
	if (!hip || file.size < 0x30 || memcmp(hip, "HIP", 4) != 0) return false;
	const unsigned int* h = reinterpret_cast<const unsigned int*>(hip);
	const unsigned int colors = h[3], encoding = h[6], headerSize = h[7];
	const unsigned int size = (std::min)(h[2], file.size);
	if (encoding != kHipRle8 || colors > 256 || headerSize < 0x10 || headerSize > size || 0x20 + headerSize + colors * 4 > size) return false;
	const unsigned int w = h[8], ht = h[9];
	if (!w || !ht || w > 4096 || ht > 4096) return false;
	out.w = static_cast<int>(w);
	out.h = static_cast<int>(ht);
	out.pixels.assign(static_cast<size_t>(w) * ht, 0);
	const unsigned char* p = hip + 0x20 + headerSize + colors * 4;
	const unsigned char* end = hip + size;
	size_t at = 0;
	while (at < out.pixels.size() && p + 1 < end) {
		const size_t run = (std::min)(static_cast<size_t>(p[1]), out.pixels.size() - at);
		memset(out.pixels.data() + at, p[0], run);
		at += run;
		p += 2;
	}
	return true;
}

void LoadSprite(int side) {
	Sprite& sp = s_sprite[side];
	sp = Sprite();
	const std::string code = g_side[side].code;
	for (const char* suffix : {"000_00.hip", "000_01.hip", "000_02.hip"}) {
		const std::string name = code + suffix;
		if (DecodeHip(FindLoadedImage(side, name), sp)) {
			sp.name = name;
			return;
		}
	}
	sp = Sprite();
}

void ReleaseTexture(Texture& t) {
	if (t.tex) t.tex->Release();
	t = Texture();
}

void ReleaseThumbs() {
	for (auto& kv : s_thumbs) ReleaseTexture(kv.second);
	s_thumbs.clear();
}

unsigned long long HashColors(const unsigned char* colors) {
	unsigned long long h = 1469598103934665603ull;
	for (int i = 0; i < kFileBytes; ++i) h = (h ^ colors[i]) * 1099511628211ull;
	return h;
}

bool Render(Texture& t, const Sprite& sp, const unsigned char* colors) {
	if (!s_device || sp.pixels.empty() || !colors) return false;
	const unsigned long long key = HashColors(colors) ^ reinterpret_cast<uintptr_t>(sp.pixels.data());
	if (t.tex && t.key == key) return true;
	if (!t.tex || t.w != sp.w || t.h != sp.h) {
		ReleaseTexture(t);
		if (FAILED(s_device->CreateTexture(sp.w, sp.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex, nullptr))) return false;
		t.w = sp.w;
		t.h = sp.h;
	}
	D3DLOCKED_RECT lr;
	if (FAILED(t.tex->LockRect(0, &lr, nullptr, 0))) return false;
	for (int y = 0; y < sp.h; ++y) {
		unsigned char* row = static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch;
		const unsigned char* src = sp.pixels.data() + static_cast<size_t>(y) * sp.w;
		for (int x = 0; x < sp.w; ++x) {
			const unsigned char i = src[x];
			memcpy(row + x * 4, colors + i * 4, 4);
			if (i == 0) row[x * 4 + 3] = 0;
		}
	}
	t.tex->UnlockRect(0);
	t.key = key;
	return true;
}

void DrawSprite(Texture& t, int side, const unsigned char* colors, float height) {
	const Sprite& sp = s_sprite[side];
	if (!Render(t, sp, colors)) return;
	const float scale = height / sp.h;
	ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t.tex)), ImVec2(sp.w * scale, height));
}

void DrawThumb(int side, const PalData* p) {
	const std::string key = std::to_string(side) + ":" + (p ? p->info.name : "");
	const unsigned char* colors = p && !IsEmpty(p->files[0]) ? p->files[0] : g_side[side].original[0];
	if (!g_side[side].backedUp && !p) colors = GameFile(side, 0);
	DrawSprite(s_thumbs[key], side, colors, 48.0f);
}

constexpr unsigned int kOpState = 0, kOpSprite = 2, kOpIf = 4, kOpEndIf = 5, kOpSub = 8, kOpEndSub = 9, kOpCall = 10;
constexpr unsigned int kOpUpon = 15, kOpEndUpon = 16, kOpIfNot = 54, kOpEndIfNot = 55, kOpElse = 56, kOpEndElse = 57;
constexpr unsigned int kOpSetPalette = 4061;
constexpr int kEffectThumbHeight = 96;

struct Command {
	unsigned int op;
	const unsigned char* args;
};

struct EffectSet {
	bool built = false;
	std::vector<std::string> images[kFiles];
	std::map<std::string, Sprite> sprites;
	std::map<std::string, Texture> textures;
};

EffectSet s_effects[2];
std::map<unsigned int, unsigned int> s_opSizes;

void LoadOpSizes() {
	if (!s_opSizes.empty() || !g_paletteScene.opSizes) return;
	const unsigned int* pre = reinterpret_cast<const unsigned int*>(g_paletteScene.opSizes - 0x30);
	for (int i = 0; i < 6; ++i) s_opSizes[pre[i * 2]] = pre[i * 2 + 1];
	const unsigned int* table = reinterpret_cast<const unsigned int*>(g_paletteScene.opSizes);
	for (int i = 0; i < 963; ++i) s_opSizes.emplace(table[i * 2], table[i * 2 + 1]);
}

bool EndsWith(const std::string& s, const char* tail) {
	const size_t n = strlen(tail);
	return s.size() >= n && _stricmp(s.c_str() + s.size() - n, tail) == 0;
}

std::vector<Command> ParseScript(const PackFile& f) {
	std::vector<Command> out;
	if (f.size < 4) return out;
	const unsigned int entries = *reinterpret_cast<const unsigned int*>(f.data);
	if (entries > f.size / 0x24) return out;
	size_t pos = 4 + static_cast<size_t>(entries) * 0x24;
	while (pos + 4 <= f.size) {
		const unsigned int op = *reinterpret_cast<const unsigned int*>(f.data + pos);
		const auto it = s_opSizes.find(op);
		if (it == s_opSizes.end() || it->second < 4 || it->second > f.size - pos) break;
		out.push_back({op, f.data + pos + 4});
		pos += it->second;
	}
	return out;
}

std::string CmdString(const Command& c) { return std::string(reinterpret_cast<const char*>(c.args), strnlen(reinterpret_cast<const char*>(c.args), 32)); }

int CmdInt(const Command& c) { return *reinterpret_cast<const int*>(c.args); }

void Walk(const std::vector<Command>& cmds, const std::map<std::string, std::vector<Command>>& subs, unsigned int& files,
          const std::map<std::string, PackFile>& jonbins, std::map<std::string, unsigned int>& usage, int depth, long& budget) {
	std::vector<bool> stack;
	for (const Command& c : cmds) {
		if (--budget <= 0) return;
		if (c.op == kOpIf || c.op == kOpIfNot || c.op == kOpElse || c.op == kOpUpon) {
			stack.push_back(c.op == kOpUpon && CmdInt(c) == 0);
		} else if (c.op == kOpEndIf || c.op == kOpEndIfNot || c.op == kOpEndElse || c.op == kOpEndUpon) {
			if (!stack.empty()) stack.pop_back();
		} else if (c.op == kOpSetPalette) {
			bool always = true;
			for (bool b : stack) always &= b;
			if (always) files = 0;
			const int f = CmdInt(c);
			if (f >= 0 && f < kFiles) files |= 1u << f;
		} else if (c.op == kOpCall && depth < 8) {
			const auto it = subs.find(CmdString(c));
			if (it == subs.end()) continue;
			bool always = true;
			for (bool b : stack) always &= b;
			unsigned int branch = files;
			Walk(it->second, subs, always ? files : branch, jonbins, usage, depth + 1, budget);
			if (!always) files |= branch;
		} else if (c.op == kOpSprite) {
			const auto jt = jonbins.find(CmdString(c));
			if (jt == jonbins.end()) continue;
			const unsigned char* j = jt->second.data;
			if (jt->second.size < 6 || memcmp(j, "JONB", 4) != 0) continue;
			const unsigned int count = *reinterpret_cast<const unsigned short*>(j + 4);
			size_t pos = 6 + count * 32;
			if (!count || pos + 7 > jt->second.size) continue;
			const unsigned int chunks = *reinterpret_cast<const unsigned int*>(j + pos + 3);
			pos += 93;
			for (unsigned int i = 0; i < chunks && pos + (i + 1) * 0x50 <= jt->second.size; ++i) {
				const unsigned int* f = reinterpret_cast<const unsigned int*>(j + pos + i * 0x50);
				const unsigned int index = f[15] < count ? f[15] : 0;
				std::string image(reinterpret_cast<const char*>(j + 6 + index * 32), strnlen(reinterpret_cast<const char*>(j + 6 + index * 32), 32));
				image = image.substr(0, image.find_last_of('.'));
				usage[image] |= f[18] ? (f[18] - 1 < static_cast<unsigned int>(kFiles) ? 1u << (f[18] - 1) : 0u) : files;
			}
		}
	}
}

void BuildEffects(int side) {
	EffectSet& e = s_effects[side];
	if (e.built) return;
	e.built = true;
	LoadOpSizes();
	if (!g_paletteScene.fileBuffer || s_opSizes.empty()) return;
	std::vector<PackFile> scripts;
	std::map<std::string, PackFile> jonbins, images;
	for (int id = 0x50 + side * 8; id < 0x58 + side * 8; ++id) {
		const unsigned char* pack = *reinterpret_cast<const unsigned char* const*>(g_paletteScene.fileBuffer + id * 8);
		for (const PackFile& f : PackFiles(pack)) {
			if (EndsWith(f.name, ".jonbin")) jonbins[f.name.substr(0, f.name.size() - 7)] = f;
			else if (EndsWith(f.name, ".hip")) images[f.name.substr(0, f.name.size() - 4)] = f;
			else if (EndsWith(f.name, ".bin") && !_strnicmp(f.name.c_str(), "scr_", 4)) scripts.push_back(f);
		}
	}
	std::vector<std::vector<Command>> parsed;
	for (const PackFile& f : scripts) parsed.push_back(ParseScript(f));
	std::map<std::string, std::vector<Command>> subs;
	for (const auto& cmds : parsed) {
		std::vector<Command>* cur = nullptr;
		for (const Command& c : cmds) {
			if (c.op == kOpSub) cur = &subs[CmdString(c)];
			else if (c.op == kOpEndSub) cur = nullptr;
			else if (cur) cur->push_back(c);
		}
	}
	std::map<std::string, unsigned int> usage;
	long budget = 4000000;
	for (const auto& cmds : parsed) {
		std::vector<Command> body;
		bool inState = false;
		for (const Command& c : cmds) {
			if (c.op == kOpState || c.op == kOpSub) {
				if (inState) {
					unsigned int files = 1;
					Walk(body, subs, files, jonbins, usage, 0, budget);
				}
				body.clear();
				inState = c.op == kOpState;
			} else if (inState) {
				body.push_back(c);
			}
		}
		if (inState) {
			unsigned int files = 1;
			Walk(body, subs, files, jonbins, usage, 0, budget);
		}
	}
	for (const auto& kv : usage) {
		if (!images.count(kv.first)) continue;
		for (int f = 1; f < kFiles; ++f)
			if (kv.second & (1u << f)) e.images[f].push_back(kv.first);
	}
	for (int f = 1; f < kFiles; ++f) {
		auto& list = e.images[f];
		if (list.size() > 48) list.resize(48);
		for (const std::string& name : list) {
			Sprite sp;
			if (!DecodeHip(images[name], sp)) continue;
			const int scale = (std::max)(1, (sp.h + kEffectThumbHeight * 2 - 1) / (kEffectThumbHeight * 2));
			if (scale > 1) {
				Sprite reduced;
				reduced.w = (std::max)(1, sp.w / scale);
				reduced.h = (std::max)(1, sp.h / scale);
				reduced.pixels.resize(static_cast<size_t>(reduced.w) * reduced.h);
				for (int y = 0; y < reduced.h; ++y)
					for (int x = 0; x < reduced.w; ++x) reduced.pixels[static_cast<size_t>(y) * reduced.w + x] = sp.pixels[static_cast<size_t>(y * scale) * sp.w + x * scale];
				sp = reduced;
			}
			sp.name = name;
			e.sprites[name] = sp;
		}
	}
}

void ReleaseEffects(int side) {
	for (auto& kv : s_effects[side].textures) ReleaseTexture(kv.second);
	s_effects[side] = EffectSet();
}

void DrawEffectSheet(int side, int file, const unsigned char* colors) {
	BuildEffects(side);
	EffectSet& e = s_effects[side];
	if (e.images[file].empty()) {
		ImGui::TextDisabled("No effects drawn with this file were found in the character's scripts.");
		return;
	}
	ImGui::BeginChild("##effects", ImVec2(0, kEffectThumbHeight + 24.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	bool first = true;
	for (const std::string& name : e.images[file]) {
		const auto it = e.sprites.find(name);
		if (it == e.sprites.end() || !Render(e.textures[name], it->second, colors)) continue;
		if (!first) ImGui::SameLine();
		first = false;
		const float scale = static_cast<float>(kEffectThumbHeight) / it->second.h;
		ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(e.textures[name].tex)), ImVec2(it->second.w * scale, static_cast<float>(kEffectThumbHeight)));
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
	}
	ImGui::EndChild();
}

void PutStored(std::vector<unsigned char>& idat, const std::vector<unsigned char>& raw) {
	idat.push_back(0x78);
	idat.push_back(0x01);
	unsigned int a = 1, b = 0;
	for (unsigned char v : raw) {
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	size_t at = 0;
	do {
		const size_t len = (std::min)(raw.size() - at, static_cast<size_t>(65535));
		idat.push_back(at + len == raw.size() ? 1 : 0);
		idat.push_back(len & 0xFF);
		idat.push_back((len >> 8) & 0xFF);
		idat.push_back(~len & 0xFF);
		idat.push_back((~len >> 8) & 0xFF);
		idat.insert(idat.end(), raw.begin() + at, raw.begin() + at + len);
		at += len;
	} while (at < raw.size());
	PutBE32(idat, b << 16 | a);
}

bool WritePng(const std::string& path, const PalData& pal, const std::string& code, const Sprite* sprite) {
	std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	const int w = sprite ? sprite->w : 16, h = sprite ? sprite->h : 16;
	const unsigned char ihdr[13] = {static_cast<unsigned char>(w >> 24), static_cast<unsigned char>(w >> 16), static_cast<unsigned char>(w >> 8), static_cast<unsigned char>(w),
	                                static_cast<unsigned char>(h >> 24), static_cast<unsigned char>(h >> 16), static_cast<unsigned char>(h >> 8), static_cast<unsigned char>(h), 8, 3, 0, 0, 0};
	PutChunk(png, "IHDR", ihdr, sizeof(ihdr));
	unsigned char plte[kColors * 3], trns[kColors];
	for (int i = 0; i < kColors; ++i) {
		const unsigned char* c = pal.files[0] + i * 4;
		plte[i * 3] = c[2];
		plte[i * 3 + 1] = c[1];
		plte[i * 3 + 2] = c[0];
		trns[i] = i == 0 ? 0 : c[3];
	}
	PutChunk(png, "PLTE", plte, sizeof(plte));
	PutChunk(png, "tRNS", trns, sizeof(trns));
	std::vector<unsigned char> text(kCharacterKeyword, kCharacterKeyword + strlen(kCharacterKeyword));
	text.push_back(0);
	text.insert(text.end(), code.begin(), code.end());
	PutChunk(png, "tEXt", text.data(), text.size());
	std::vector<unsigned char> extras = {'B', 'B', 'I', 'M'};
	PutBE32(extras, 1);
	const unsigned char* info = reinterpret_cast<const unsigned char*>(&pal.info);
	extras.insert(extras.end(), info, info + sizeof(PalInfo));
	for (int f = 1; f < kFiles; ++f) extras.insert(extras.end(), pal.files[f], pal.files[f] + kFileBytes);
	PutChunk(png, "bbIm", extras.data(), extras.size());
	std::vector<unsigned char> raw;
	for (int y = 0; y < h; ++y) {
		raw.push_back(0);
		for (int x = 0; x < w; ++x) raw.push_back(sprite ? sprite->pixels[static_cast<size_t>(y) * w + x] : static_cast<unsigned char>(y * 16 + x));
	}
	std::vector<unsigned char> idat;
	PutStored(idat, raw);
	PutChunk(png, "IDAT", idat.data(), idat.size());
	PutChunk(png, "IEND", nullptr, 0);
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
	const bool ok = fwrite(png.data(), png.size(), 1, f) == 1;
	fclose(f);
	return ok;
}

bool ReadPng(const std::string& path, PalData& out) {
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
	std::vector<unsigned char> bytes;
	unsigned char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < (16u << 20)) bytes.insert(bytes.end(), buf, buf + n);
	fclose(f);
	if (bytes.size() < 8 || memcmp(bytes.data(), "\x89PNG\r\n\x1A\n", 8) != 0) return false;
	const unsigned char *plte = nullptr, *trns = nullptr;
	size_t plteLen = 0, trnsLen = 0;
	int colorType = -1;
	memset(&out, 0, sizeof(out));
	for (size_t off = 8; off + 12 <= bytes.size();) {
		const size_t len = GetBE32(&bytes[off]);
		if (len > bytes.size() - off - 12) return false;
		const char* type = reinterpret_cast<const char*>(&bytes[off + 4]);
		const unsigned char* data = &bytes[off + 8];
		if (!memcmp(type, "IHDR", 4) && len >= 13) colorType = data[9];
		else if (!memcmp(type, "PLTE", 4) && !plte) plte = data, plteLen = len;
		else if (!memcmp(type, "tRNS", 4) && !trns) trns = data, trnsLen = len;
		else if (!memcmp(type, "bbIm", 4) && len >= 8 + sizeof(PalInfo) + (kFiles - 1) * kFileBytes && !memcmp(data, "BBIM", 4) && GetBE32(data + 4) == 1) {
			memcpy(&out.info, data + 8, sizeof(PalInfo));
			for (int i = 1; i < kFiles; ++i) memcpy(out.files[i], data + 8 + sizeof(PalInfo) + (i - 1) * kFileBytes, kFileBytes);
		} else if (!memcmp(type, "IEND", 4)) break;
		off += 12 + len;
	}
	if (colorType != 3 || !plte || !plteLen || plteLen % 3) return false;
	const size_t count = (std::min)(plteLen / 3, static_cast<size_t>(kColors));
	for (size_t i = 1; i < count; ++i) {
		unsigned char* c = out.files[0] + i * 4;
		c[0] = plte[i * 3 + 2];
		c[1] = plte[i * 3 + 1];
		c[2] = plte[i * 3];
		c[3] = i < trnsLen ? trns[i] : 0xFF;
	}
	out.info.name[sizeof(out.info.name) - 1] = 0;
	out.info.creator[sizeof(out.info.creator) - 1] = 0;
	out.info.desc[sizeof(out.info.desc) - 1] = 0;
	return true;
}

bool KnownCode(const std::string& code) {
	for (const char* c : {"ar", "bn", "ca", "ha", "hz", "jn", "lc", "mk", "mu", "no", "ny", "pt", "rc", "rg", "rl", "tb", "tg", "tk", "vh"})
		if (code == c) return true;
	return false;
}

void CleanText(char* text, size_t size) {
	for (size_t i = 0; i < size && text[i]; ++i)
		if (static_cast<unsigned char>(text[i]) < 32 || text[i] == 127) text[i] = ' ';
}

std::string CleanText(std::string text) {
	CleanText(&text[0], text.size());
	return text;
}

bool ReservedName(std::string name) {
	while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
	const std::string base = name.substr(0, name.find('.'));
	for (const char* r : {"CON", "PRN", "AUX", "NUL"})
		if (_stricmp(base.c_str(), r) == 0) return true;
	return base.size() == 4 && (!_strnicmp(base.c_str(), "COM", 3) || !_strnicmp(base.c_str(), "LPT", 3)) && base[3] >= '0' && base[3] <= '9';
}

void SafeName(char* name, size_t size) {
	name[size - 1] = 0;
	for (size_t i = 0; i < size && name[i]; ++i)
		if (!strchr(" abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.()[]!@&+-'^,;{}$=", name[i])) name[i] = '_';
	size_t n = strnlen(name, size);
	while (n && (name[n - 1] == ' ' || name[n - 1] == '.')) name[--n] = 0;
	size_t lead = 0;
	while (name[lead] == ' ' || name[lead] == '.') ++lead;
	if (lead) memmove(name, name + lead, n - lead + 1);
	if (ReservedName(name) && strnlen(name, size) + 1 < size) {
		memmove(name + 1, name, strnlen(name, size) + 1);
		name[0] = '_';
	}
}

void CleanInfo(PalInfo& info) {
	SafeName(info.name, sizeof(info.name));
	info.creator[sizeof(info.creator) - 1] = 0;
	info.desc[sizeof(info.desc) - 1] = 0;
	CleanText(info.creator, sizeof(info.creator));
	CleanText(info.desc, sizeof(info.desc));
}

struct CfCharacter {
	const char* code;
	unsigned int mask;
	short index;
};

const CfCharacter* FindCf(const std::string& code) {
	static const CfCharacter kCf[] = {
		{"ar", 0xE1, 7}, {"bn", 0xEF, 8}, {"ca", 0xFB, 9}, {"ha", 0x99, 10}, {"hz", 0xFF, 13}, {"jn", 0xBB, 1}, {"lc", 0xDF, 6},
		{"mk", 0xF9, 15}, {"mu", 0x3F, 14}, {"no", 0xFF, 2}, {"ny", 0x7F, 11}, {"pt", 0xE3, 17}, {"rc", 0xCF, 3}, {"rg", 0x7F, 0},
		{"rl", 0xFF, 18}, {"tb", 0xBD, 12}, {"tg", 0xFF, 5}, {"tk", 0xA5, 4}, {"vh", 0x7F, 16},
	};
	for (const CfCharacter& c : kCf)
		if (_stricmp(code.c_str(), c.code) == 0) return &c;
	return nullptr;
}

bool ReadCfpl(const std::string& path, const std::string& code, PalData& out) {
	const CfCharacter* cf = FindCf(code);
	if (!cf) return false;
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
	PalHeader hdr{};
	const bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1 && memcmp(hdr.signature, kSignature, sizeof(kSignature)) == 0 &&
	                hdr.headerLen >= static_cast<int>(sizeof(hdr)) && hdr.dataLen == sizeof(out) && fseek(f, hdr.headerLen, SEEK_SET) == 0 &&
	                fread(&out, sizeof(out), 1, f) == 1;
	fclose(f);
	if (!ok) return false;
	if (hdr.reserved != kCseMark) {
		for (int i = 1; i < kFiles; ++i)
			if (!(cf->mask & (1u << i))) memset(out.files[i], 0, kFileBytes);
		out.info.hasBloom = false;
	}
	return true;
}

bool ReadHpl(const std::string& path, unsigned char* file) {
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
	unsigned char head[kHplHeader];
	const bool ok = fread(head, sizeof(head), 1, f) == 1 && memcmp(head, "HPAL", 4) == 0 && fread(file, kFileBytes, 1, f) == 1;
	fclose(f);
	return ok;
}

bool ReadAct(const std::string& path, unsigned char* file) {
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
	unsigned char rgb[kColors * 3 + 5] = {};
	const size_t n = fread(rgb, 1, sizeof(rgb), f);
	fclose(f);
	if (n != kColors * 3 && n != kColors * 3 + 4) return false;
	size_t count = kColors;
	if (n == kColors * 3 + 4) {
		const size_t stated = static_cast<size_t>(rgb[kColors * 3]) << 8 | rgb[kColors * 3 + 1];
		if (stated && stated < count) count = stated;
	}
	memset(file, 0, kFileBytes);
	for (size_t i = 0; i < count; ++i) {
		file[i * 4] = rgb[i * 3 + 2];
		file[i * 4 + 1] = rgb[i * 3 + 1];
		file[i * 4 + 2] = rgb[i * 3];
		file[i * 4 + 3] = 0xFF;
	}
	return true;
}

int EffectSuffix(std::string& stem) {
	const size_t at = stem.size() >= 9 ? stem.size() - 9 : std::string::npos;
	if (at == std::string::npos || _strnicmp(stem.c_str() + at, "_effect0", 8) != 0) return 0;
	const int index = stem.back() - '0';
	if (index < 1 || index >= kFiles) return -1;
	stem.resize(at);
	return index;
}

bool WritePal(const std::string& path, const std::string& code, const PalData& data) {
	const CfCharacter* cf = FindCf(code);
	if (!cf) return false;
	PalHeader hdr{};
	memcpy(hdr.signature, kSignature, sizeof(kSignature));
	hdr.headerLen = sizeof(PalHeader);
	hdr.dataLen = sizeof(PalData);
	hdr.charIndex = cf->index;
	hdr.reserved = kCseMark;
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
	const bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 && fwrite(&data, sizeof(data), 1, f) == 1;
	fclose(f);
	if (!ok) DeleteFileA(path.c_str());
	return ok;
}

std::vector<PalData> LoadFolder(const std::string& code) {
	std::vector<PalData> out;
	if (code.empty()) return out;
	std::vector<std::pair<std::string, std::string>> effects;
	for (const char* pattern : {"\\*.cfpl", "\\*.hpl", "\\*.act", "\\*.png"}) {
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA((Folder(code) + pattern).c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			const std::string file = fd.cFileName;
			const std::string path = Folder(code) + "\\" + file;
			const std::string ext = file.substr(file.find_last_of('.'));
			std::string stem = file.substr(0, file.find_last_of('.'));
			PalData data{};
			bool ok = false;
			if (!_stricmp(ext.c_str(), ".cfpl")) {
				ok = ReadCfpl(path, code, data);
			} else if (!_stricmp(ext.c_str(), ".png")) {
				ok = !fd.nFileSizeHigh && fd.nFileSizeLow <= kMaxPngBytes && ReadPng(path, data);
			} else if (!_stricmp(ext.c_str(), ".hpl") || !_stricmp(ext.c_str(), ".act")) {
				std::string base = stem;
				if (base.size() >= 12 && !_stricmp(base.c_str() + base.size() - 12, "_effectbloom")) continue;
				if (EffectSuffix(base)) {
					if (effects.size() < kMaxPalettes * kFiles) effects.push_back({file, base});
					continue;
				}
				ok = !_stricmp(ext.c_str(), ".hpl") ? ReadHpl(path, data.files[0]) : ReadAct(path, data.files[0]);
			}
			if (!ok) continue;
			strncpy_s(data.info.name, stem.c_str(), _TRUNCATE);
			CleanInfo(data.info);
			if (!data.info.name[0]) continue;
			bool duplicate = false;
			for (const PalData& p : out) duplicate |= _stricmp(p.info.name, data.info.name) == 0;
			if (!duplicate) out.push_back(data);
		} while (out.size() < kMaxPalettes && FindNextFileA(h, &fd));
		FindClose(h);
	}
	for (auto& e : effects) {
		std::string base = e.first.substr(0, e.first.find_last_of('.'));
		const int index = EffectSuffix(base);
		if (index < 1) continue;
		char name[sizeof(PalInfo::name)] = {};
		strncpy_s(name, base.c_str(), _TRUNCATE);
		SafeName(name, sizeof(name));
		for (PalData& p : out) {
			if (_stricmp(p.info.name, name) != 0) continue;
			const std::string path = Folder(code) + "\\" + e.first;
			const bool hpl = !_stricmp(e.first.c_str() + e.first.size() - 4, ".hpl");
			if (!(hpl ? ReadHpl(path, p.files[index]) : ReadAct(path, p.files[index]))) memset(p.files[index], 0, kFileBytes);
			break;
		}
	}
	return out;
}

void LoadCustom(int side) { g_side[side].custom = LoadFolder(g_side[side].code); }

const PalData* FindIn(const std::vector<PalData>& list, const std::string& name) {
	if (name.empty()) return nullptr;
	for (const PalData& p : list)
		if (_stricmp(name.c_str(), p.info.name) == 0) return &p;
	return nullptr;
}

const PalData* FindCustom(int side, const std::string& name) { return FindIn(g_side[side].custom, name); }

std::string Trim(std::string v) {
	const char* ws = " \t\"";
	v.erase(0, v.find_first_not_of(ws));
	const size_t end = v.find_last_not_of(ws);
	v.erase(end == std::string::npos ? 0 : end + 1);
	return v;
}

void EnsureIni() {
	if (s_iniChecked) return;
	s_iniChecked = true;
	if (GetFileAttributesA(IniPath().c_str()) != INVALID_FILE_ATTRIBUTES) return;
	CreateDirectoryA(Root().c_str(), nullptr);
	FILE* f = nullptr;
	if (fopen_s(&f, IniPath().c_str(), "w") || !f) return;
	fputs("; Filled in by the palette editor (F2 > Training > Palette editor > Use for this color).\n"
	      "; Lines starting with ; are ignored.\n", f);
	fclose(f);
}

std::vector<std::string> IniPool(const std::string& code, int color, const std::vector<PalData>& custom, bool namedOnly) {
	char buf[1024] = {};
	const std::string key = std::to_string(color + 1);
	GetPrivateProfileStringA(CharacterNames(code).c_str(), key.c_str(), "", buf, sizeof(buf), IniPath().c_str());
	std::vector<std::string> pool;
	const std::string value = buf;
	for (size_t start = 0; start <= value.size();) {
		size_t comma = value.find(',', start);
		if (comma == std::string::npos) comma = value.size();
		const std::string name = Trim(value.substr(start, comma - start));
		const bool random = _stricmp(name.c_str(), "Random") == 0, randomNamed = _stricmp(name.c_str(), "Random_Exclude_Default") == 0;
		if (_stricmp(name.c_str(), "Default") == 0 || (random && !namedOnly)) pool.push_back("");
		if ((random || randomNamed) && !namedOnly)
			for (const PalData& p : custom) pool.push_back(p.info.name);
		if (!random && !randomNamed && FindIn(custom, name)) pool.push_back(name);
		start = comma + 1;
	}
	return pool;
}

std::string PickFromIni(int side) {
	const SideState& s = g_side[side];
	const std::vector<std::string> pool = IniPool(s.code, s.palIndex, s.custom, false);
	return pool.empty() ? std::string() : pool[rand() % pool.size()];
}

void SyncEditor() {
	const unsigned char* src = GameFile(s_side, s_file);
	if (src) memcpy(s_edit, src, kFileBytes);
	else memset(s_edit, 0, kFileBytes);
}

void FillSaveFields() {
	const PalData* p = FindCustom(s_side, g_side[s_side].selected);
	strcpy_s(s_name, p ? p->info.name : "");
	strcpy_s(s_creator, p ? p->info.creator : "");
	strcpy_s(s_desc, p ? p->info.desc : "");
}

void ClearHistory() {
	s_undo.clear();
	s_redo.clear();
}

void Commit(const unsigned char (&before)[kFiles][kFileBytes]) {
	Change c;
	c.side = s_side;
	memcpy(c.before, before, sizeof(c.before));
	ReadAll(s_side, c.after);
	c.time = ImGui::GetTime();
	if (!s_undo.empty() && s_undo.back().side == s_side && c.time - s_undo.back().time < kUndoMergeSeconds) {
		memcpy(s_undo.back().after, c.after, sizeof(c.after));
		s_undo.back().time = c.time;
	} else {
		s_undo.push_back(c);
		if (s_undo.size() > 200) s_undo.erase(s_undo.begin());
	}
	s_redo.clear();
}

void EditFile(int file, const unsigned char* colors) {
	unsigned char before[kFiles][kFileBytes];
	ReadAll(s_side, before);
	ApplyFile(s_side, file, colors);
	Commit(before);
}

void Step(std::vector<Change>& from, std::vector<Change>& to, bool undo) {
	if (from.empty()) return;
	Change c = from.back();
	from.pop_back();
	if (c.side != s_side) return;
	ApplyFiles(s_side, undo ? c.before : c.after);
	c.time = 0;
	to.push_back(c);
	SyncEditor();
}

void StopHighlight() {
	if (!s_highlight) return;
	s_highlight = false;
	ApplyFile(s_side, s_file, s_edit);
}

void ShowHighlight(int index) {
	if (index == s_highlighted) return;
	s_highlighted = index;
	for (int i = 0; i < kColors; ++i) {
		const unsigned char v = i == index ? 0xFF : 0x00;
		s_highlightArray[i * 4 + 0] = v;
		s_highlightArray[i * 4 + 1] = v;
		s_highlightArray[i * 4 + 2] = v;
		s_highlightArray[i * 4 + 3] = 0xFF;
	}
	ApplyFile(s_side, s_file, s_highlightArray);
}

bool ColorBox(unsigned char* bgra, ImGuiColorEditFlags flags, bool button) {
	float c[4] = {bgra[2] / 255.0f, bgra[1] / 255.0f, bgra[0] / 255.0f, bgra[3] / 255.0f};
	if (button) return ImGui::ColorButton("##c", ImVec4(c[0], c[1], c[2], c[3]), flags);
	if (!ImGui::ColorEdit4("##c", c, flags)) return false;
	bgra[2] = static_cast<unsigned char>(c[0] * 255.0f + 0.5f);
	bgra[1] = static_cast<unsigned char>(c[1] * 255.0f + 0.5f);
	bgra[0] = static_cast<unsigned char>(c[2] * 255.0f + 0.5f);
	bgra[3] = static_cast<unsigned char>(c[3] * 255.0f + 0.5f);
	return true;
}

ImGuiColorEditFlags EditFlags() {
	ImGuiColorEditFlags f = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
	return s_showAlpha ? f | ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_AlphaBar : f | ImGuiColorEditFlags_NoAlpha;
}

void DrawGradient() {
	if (!ImGui::BeginPopup("gradient")) return;
	static int first = 1, last = 2;
	static float from[4] = {1, 1, 1, 1}, to[4] = {1, 1, 1, 1};
	ImGui::TextUnformatted("Gradient generator");
	ImGui::SliderInt("Start index", &first, 1, kColors - 1);
	if (last <= first) last = first + 1;
	ImGui::SliderInt("End index", &last, first + 1, kColors);
	ImGui::ColorEdit3("Start color", from);
	ImGui::ColorEdit3("End color", to);
	if (ImGui::Button("Swap colors"))
		for (int i = 0; i < 3; ++i) std::swap(from[i], to[i]);
	if (ImGui::Button("Generate gradient")) {
		StopHighlight();
		const int steps = last - first;
		for (int i = 0; i <= steps; ++i) {
			const float t = static_cast<float>(i) / steps;
			unsigned char* c = s_edit + (first - 1 + i) * 4;
			c[2] = static_cast<unsigned char>((from[0] + (to[0] - from[0]) * t) * 255.0f + 0.5f);
			c[1] = static_cast<unsigned char>((from[1] + (to[1] - from[1]) * t) * 255.0f + 0.5f);
			c[0] = static_cast<unsigned char>((from[2] + (to[2] - from[2]) * t) * 255.0f + 0.5f);
		}
		EditFile(s_file, s_edit);
	}
	ImGui::EndPopup();
}

void DrawBoxes() {
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
	int hovered = -1;
	for (int i = 0; i < kColors; ++i) {
		if (i % kColumns == 0) {
			ImGui::Text("%.3d ", i + 1);
			ImGui::SameLine();
		}
		ImGui::PushID(i);
		if (s_highlight) {
			ColorBox(s_edit + i * 4, EditFlags(), true);
			if (ImGui::IsItemHovered()) hovered = i;
		} else if (ColorBox(s_edit + i * 4, EditFlags(), false)) {
			EditFile(s_file, s_edit);
		}
		ImGui::PopID();
		if (i % kColumns != kColumns - 1) ImGui::SameLine();
	}
	ImGui::PopStyleVar();
	if (s_highlight && hovered >= 0) ShowHighlight(hovered);
}

int FilterName(ImGuiInputTextCallbackData* data) {
	return data->EventChar < 256 && strchr(" abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.()[]!@&+-'^,;{}$=", static_cast<char>(data->EventChar)) ? 0 : 1;
}

PalData CurrentPalette() {
	PalData data{};
	strcpy_s(data.info.name, s_name);
	strcpy_s(data.info.creator, s_creator);
	strcpy_s(data.info.desc, s_desc);
	ReadAll(s_side, data.files);
	return data;
}

bool WriteCurrent(const std::string& path) {
	CreateDirectoryA(Root().c_str(), nullptr);
	CreateDirectoryA(Folder(g_side[s_side].code).c_str(), nullptr);
	return WritePal(path, g_side[s_side].code, CurrentPalette());
}

void DrawSave() {
	ImGui::SeparatorText("Save");
	if (s_highlight) {
		ImGui::TextDisabled("Turn highlight mode off to save.");
		return;
	}
	ImGui::SetNextItemWidth(250);
	ImGui::InputText("Palette name", s_name, sizeof(s_name), ImGuiInputTextFlags_CallbackCharFilter, FilterName);
	ImGui::SetNextItemWidth(250);
	ImGui::InputText("Creator (optional)", s_creator, sizeof(s_creator), ImGuiInputTextFlags_CallbackCharFilter, FilterName);
	ImGui::SetNextItemWidth(250);
	ImGui::InputText("Description (optional)", s_desc, sizeof(s_desc), ImGuiInputTextFlags_CallbackCharFilter, FilterName);
	const std::string code = g_side[s_side].code;
	SafeName(s_name, sizeof(s_name));
	const std::string path = Folder(code) + "\\" + s_name + kExtension;
	bool save = false;
	if (ImGui::Button("Save palette", ImVec2(125, 0))) {
		if (!s_name[0] || _stricmp(s_name, "Default") == 0 || _stricmp(s_name, "Random") == 0) s_message = "Give the palette a name.";
		else if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) ImGui::OpenPopup("Overwrite?");
		else save = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Export PNG", ImVec2(125, 0))) {
		const std::string dir = Root() + "\\exports";
		CreateDirectoryA(Root().c_str(), nullptr);
		CreateDirectoryA(dir.c_str(), nullptr);
		const std::string png = dir + "\\" + code + "_" + (s_name[0] ? s_name : "palette") + ".png";
		s_message = WritePng(png, CurrentPalette(), code, s_sprite[s_side].pixels.empty() ? nullptr : &s_sprite[s_side]) ? "Exported to bbcse-palettes\\exports\\" + png.substr(dir.size() + 1) : "Could not export the palette.";
	}
	if (ImGui::BeginPopupModal("Overwrite?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("'%s%s' already exists. Overwrite it?", s_name, kExtension);
		if (ImGui::Button("OK", ImVec2(120, 0))) {
			save = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (save) {
		if (WriteCurrent(path)) {
			s_message = "Saved to bbcse-palettes\\" + code + "\\" + s_name + kExtension;
			LoadCustom(s_side);
			g_side[s_side].selected = s_name;
			g_side[s_side].editorChoice = true;
		} else {
			s_message = "Could not save the palette.";
		}
	}
	if (!s_message.empty()) ImGui::TextDisabled("%s", s_message.c_str());
}

void SelectSide(int side) {
	StopHighlight();
	s_side = side;
	ClearHistory();
	SyncEditor();
	FillSaveFields();
}

void SelectPalette(const PalData* p) {
	StopHighlight();
	SideState& s = g_side[s_side];
	s.selected = p ? p->info.name : "";
	s.editorChoice = true;
	ApplyFiles(s_side, p ? p->files : nullptr);
	ClearHistory();
	SyncEditor();
	FillSaveFields();
}

void StartFromGameColor(int color) {
	SideState& s = g_side[s_side];
	unsigned char before[kFiles][kFileBytes];
	ReadAll(s_side, before);
	static unsigned char files[kFiles][kFileBytes];
	for (int f = 0; f < kFiles; ++f) {
		const unsigned char* e = Entry(s_side, color, f);
		if (color == s.palIndex) memcpy(files[f], s.original[f], kFileBytes);
		else if (e) memcpy(files[f], e + kEntryColorsOff, kFileBytes);
		else memset(files[f], 0, kFileBytes);
	}
	StopHighlight();
	ApplyFiles(s_side, files);
	Commit(before);
	SyncEditor();
}

#pragma pack(push, 1)
struct PalPacket {
	unsigned int magic;
	unsigned int version;
	unsigned char custom;
	unsigned char allowSave;
	char code[4];
	unsigned char reserved[2];
	PalData data;
};
#pragma pack(pop)

constexpr unsigned int kPacketMagic = 0x4C505942;

struct OnlineState {
	bool active = false;
	int localSide = 0;
	unsigned long long peer = 0;
	bool have = false;
	bool custom = false;
	bool allowSave = false;
	std::string code;
	PalData data{};
	unsigned long long hash = 0;
	bool applied = false;
};

OnlineState s_online;
bool s_settingsLoaded = false;
bool s_showOpponent = true;
bool s_allowSave = true;
std::map<unsigned long long, std::string> s_blockedPalettes, s_blockedPlayers, s_names;
std::string s_onlineMessage;

std::string SettingsPath() { return GameDir() + "\\bbcse-im.ini"; }
std::string BlockPath() { return Root() + "\\blocked.ini"; }

unsigned long long HashFiles(const unsigned char (&files)[kFiles][kFileBytes]) {
	unsigned long long h = 1469598103934665603ull;
	for (int f = 0; f < kFiles; ++f)
		for (int i = 0; i < kFileBytes; ++i) h = (h ^ files[f][i]) * 1099511628211ull;
	return h;
}

void LoadBlockSection(const char* section, std::map<unsigned long long, std::string>& out) {
	std::vector<char> buf(65536);
	const DWORD n = GetPrivateProfileSectionA(section, buf.data(), static_cast<DWORD>(buf.size()), BlockPath().c_str());
	for (const char* p = buf.data(); p < buf.data() + n && *p; p += strlen(p) + 1) {
		const char* eq = strchr(p, '=');
		if (!eq) continue;
		out[_strtoui64(std::string(p, eq).c_str(), nullptr, 16)] = eq + 1;
	}
}

void SaveBlockSection(const char* section, const std::map<unsigned long long, std::string>& in) {
	CreateDirectoryA(Root().c_str(), nullptr);
	std::string body;
	for (const auto& kv : in) {
		char key[24];
		sprintf_s(key, "%016llX", kv.first);
		body += std::string(key) + "=" + kv.second;
		body.push_back('\0');
	}
	body.push_back('\0');
	WritePrivateProfileSectionA(section, body.c_str(), BlockPath().c_str());
}

void LoadSettings() {
	if (s_settingsLoaded) return;
	s_settingsLoaded = true;
	s_showOpponent = GetPrivateProfileIntA("palette", "showopponent", 1, SettingsPath().c_str()) != 0;
	s_allowSave = GetPrivateProfileIntA("palette", "allowsave", 1, SettingsPath().c_str()) != 0;
	LoadBlockSection("palettes", s_blockedPalettes);
	LoadBlockSection("players", s_blockedPlayers);
}

void SaveSettings() {
	WritePrivateProfileStringA("palette", "showopponent", s_showOpponent ? "1" : "0", SettingsPath().c_str());
	WritePrivateProfileStringA("palette", "allowsave", s_allowSave ? "1" : "0", SettingsPath().c_str());
}

bool LocalIsCustom(int side) {
	const SideState& s = g_side[side];
	if (!s.backedUp) return false;
	for (int f = 0; f < kFiles; ++f) {
		const unsigned char* cur = GameFile(side, f);
		if (cur && memcmp(cur, s.original[f], kFileBytes) != 0) return true;
	}
	return false;
}

bool ReceivedBlocked() {
	return s_blockedPalettes.count(s_online.hash) || s_blockedPlayers.count(s_online.peer);
}

int RemoteSide() { return 1 - s_online.localSide; }

void RestoreRemote() {
	const int side = RemoteSide();
	if (!g_paletteScene.valid || !g_side[side].backedUp) return;
	const PalData* p = FindCustom(side, g_side[side].selected);
	ApplyFiles(side, p ? p->files : nullptr);
	s_online.applied = false;
}

void ApplyReceived() {
	const int side = RemoteSide();
	if (!s_online.active || !s_online.have || !s_online.custom || !s_showOpponent || ReceivedBlocked()) return;
	if (!g_paletteScene.valid || g_side[side].code != s_online.code || !GameFile(side, 0)) return;
	ApplyFiles(side, s_online.data.files);
	s_online.applied = true;
}

std::string PeerName(unsigned long long id) {
	const auto it = s_names.find(id);
	if (it != s_names.end() && !it->second.empty()) return it->second;
	char buf[24];
	sprintf_s(buf, "%016llX", id);
	return buf;
}

std::string FreePath(const std::string& code, std::string name) {
	if (name.empty()) name = "online";
	for (char& c : name)
		if (strchr("\\/:*?<>|", c) || c == '"' || static_cast<unsigned char>(c) < 32) c = '_';
	if (ReservedName(name)) name = "_" + name;
	std::string path = Folder(code) + "\\" + name + kExtension;
	for (int i = 2; GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
		path = Folder(code) + "\\" + name + " (" + std::to_string(i) + ")" + kExtension;
	return path;
}

bool SaveReceived() {
	if (!KnownCode(s_online.code)) return false;
	CreateDirectoryA(Root().c_str(), nullptr);
	CreateDirectoryA(Folder(s_online.code).c_str(), nullptr);
	const std::string path = FreePath(s_online.code, s_online.data.info.name);
	const size_t slash = path.find_last_of('\\') + 1;
	const std::string name = path.substr(slash, path.size() - slash - strlen(kExtension));
	PalData data = s_online.data;
	strncpy_s(data.info.name, name.c_str(), _TRUNCATE);
	const bool ok = WritePal(path, s_online.code, data);
	for (int side = 0; side < 2; ++side)
		if (g_side[side].code == s_online.code) LoadCustom(side);
	s_onlineMessage = ok ? "Saved to bbcse-palettes\\" + s_online.code + "\\" + name + kExtension : "Could not save the palette.";
	Palette_Log(("PALETTE: " + s_onlineMessage).c_str());
	return ok;
}

constexpr int kObjCountOff = 0x62634;
constexpr int kObjListOff = 0x62648;
constexpr int kSlotPalettesOff = 0xCC;
constexpr int kSlotInUseOff = 0x20C;
constexpr int kSlotCount = 40;
constexpr int kObjSideOff = 0x30;
constexpr int kObjColorOff = 0x330;
constexpr int kObjFileOff = 0x334;
constexpr int kObjSlotOff = 0x338;

void SyncControlSlots() {
	unsigned char* mgr = g_paletteScene.objMgr;
	const int count = *reinterpret_cast<const int*>(mgr + kObjCountOff);
	unsigned char* const* objects = *reinterpret_cast<unsigned char* const* const*>(mgr + kObjListOff);
	if (!objects) return;
	for (int i = 0; i < count; ++i) {
		const unsigned char* o = objects[i];
		if (!o) continue;
		const int slot = *reinterpret_cast<const int*>(o + kObjSlotOff);
		if (slot < 0 || slot >= kSlotCount || !*reinterpret_cast<const int*>(mgr + kSlotInUseOff + slot * 4)) continue;
		const int side = *reinterpret_cast<const int*>(o + kObjSideOff);
		if (side < 0 || side > 1 || !g_side[side].backedUp) continue;
		const int color = *reinterpret_cast<const int*>(o + kObjColorOff);
		const int file = *reinterpret_cast<const int*>(o + kObjFileOff);
		if ((color != -1 && color != g_side[side].palIndex) || file < 0 || file >= kFiles) continue;
		const unsigned char* src = GameFile(side, file);
		const unsigned char* list = *reinterpret_cast<const unsigned char* const*>(mgr + kSlotPalettesOff + slot * 4);
		unsigned char* const* entries = list ? *reinterpret_cast<unsigned char* const* const*>(list + 4) : nullptr;
		unsigned char* dst = entries && *reinterpret_cast<const int*>(list + 0xC) > 0 ? entries[0] : nullptr;
		if (!src || !dst || memcmp(dst + kEntryColorsOff, src, kFileBytes) == 0) continue;
		memcpy(dst + kEntryColorsOff, src, kFileBytes);
		memcpy(dst + kEntryCopyOff, src, kFileBytes);
		*reinterpret_cast<int*>(dst + kEntryDirtyOff) = 1;
	}
}

void DrawPaletteRow(SideState& s) {
	ImGui::SetNextItemWidth(250);
	if (ImGui::BeginCombo("Palette", s.selected.empty() ? "Default" : s.selected.c_str())) {
		const bool thumbs = !s_sprite[s_side].pixels.empty();
		if (thumbs) {
			DrawThumb(s_side, nullptr);
			ImGui::SameLine();
		}
		if (ImGui::Selectable("Default", s.selected.empty(), 0, ImVec2(0, thumbs ? 48.0f : 0.0f))) SelectPalette(nullptr);
		for (const PalData& p : s.custom) {
			if (thumbs) {
				DrawThumb(s_side, &p);
				ImGui::SameLine();
			}
			if (ImGui::Selectable(p.info.name, _stricmp(s.selected.c_str(), p.info.name) == 0, 0, ImVec2(0, thumbs ? 48.0f : 0.0f))) SelectPalette(&p);
			if (ImGui::IsItemHovered() && (p.info.creator[0] || p.info.desc[0])) {
				ImGui::BeginTooltip();
				if (p.info.creator[0]) ImGui::Text("Creator: %s", p.info.creator);
				if (p.info.desc[0]) ImGui::Text("Description: %s", p.info.desc);
				ImGui::EndTooltip();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("Reload")) {
		LoadCustom(s_side);
		FillSaveFields();
	}
	const std::string section = CharacterNames(s.code), key = std::to_string(s.palIndex + 1);
	char assigned[256] = {};
	GetPrivateProfileStringA(section.c_str(), key.c_str(), "", assigned, sizeof(assigned), IniPath().c_str());
	const std::string current = Trim(assigned);
	ImGui::Text("Color %d: %s", s.palIndex + 1, current.empty() ? "game color" : current.c_str());
	ImGui::SameLine();
	ImGui::BeginDisabled(s.selected.empty() || _stricmp(current.c_str(), s.selected.c_str()) == 0);
	if (ImGui::Button("Use for this color")) {
		WritePrivateProfileStringA(section.c_str(), key.c_str(), ("\"" + s.selected + "\"").c_str(), IniPath().c_str());
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(current.empty());
	if (ImGui::Button("Clear")) {
		WritePrivateProfileStringA(section.c_str(), key.c_str(), nullptr, IniPath().c_str());
	}
	ImGui::EndDisabled();
}

}

void Palette_Tick() {
	LoadSettings();
	if (!g_paletteScene.valid) return;
	EnsureIni();
	SyncControlSlots();
	for (int side = 0; side < 2; ++side) {
		SideState& s = g_side[side];
		const unsigned char* c = Char(side);
		const unsigned char* list = List(side);
		if (!c || !list) continue;
		const int palIndex = *reinterpret_cast<const int*>(g_paletteScene.gameInfo + kPalIndexOff + side * kPalIndexStride);
		const std::string code(reinterpret_cast<const char*>(c + kCharCodeOff), strnlen(reinterpret_cast<const char*>(c + kCharCodeOff), 4));
		if (list == s.list && palIndex == s.palIndex && code == s.code) continue;
		const bool newCharacter = code != s.code;
		const bool sameMatch = list == s.list && !newCharacter;
		s.list = list;
		s.palIndex = palIndex;
		s.code = code;
		s.backedUp = false;
		if (newCharacter) {
			s.selected.clear();
			s.editorChoice = false;
		}
		if (!sameMatch) {
			LoadCustom(side);
			LoadSprite(side);
			ReleaseThumbs();
			ReleaseEffects(side);
		}
		if (!(g_paletteScene.training && s.editorChoice)) {
			s.selected = PickFromIni(side);
			s.editorChoice = false;
		}
		if (const PalData* p = FindCustom(side, s.selected)) ApplyFiles(side, p->files);
		if (s_online.active && side == RemoteSide()) {
			s_online.applied = false;
			ApplyReceived();
		}
		if (side == s_side) {
			s_highlight = false;
			ClearHistory();
			SyncEditor();
			FillSaveFields();
		}
	}
}

void Palette_DrawEditor() {
	if (!g_paletteScene.valid || !g_paletteScene.training || !Char(0) || !Char(1)) return;
	ImGui::SetNextWindowSize(ImVec2(560, 760), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Palette Editor", &g_paletteEditorVisible, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	for (int side = 0; side < 2; ++side) {
		const std::string label = "P" + std::to_string(side + 1) + " " + CharacterNames(g_side[side].code) + "  (color " + std::to_string(g_side[side].palIndex + 1) + ")";
		if (ImGui::RadioButton(label.c_str(), s_side == side)) SelectSide(side);
		if (side == 0) ImGui::SameLine();
	}
	SideState& s = g_side[s_side];
	DrawPaletteRow(s);
	ImGui::SetNextItemWidth(250);
	if (ImGui::Combo("File", &s_file, kFileNames, kFiles)) {
		StopHighlight();
		SyncEditor();
	}
	static int startColor = 0;
	ImGui::SetNextItemWidth(120);
	ImGui::SliderInt("##startcolor", &startColor, 1, kGameColors, "Color %d");
	ImGui::SameLine();
	if (ImGui::Button("Start from game color")) StartFromGameColor(startColor - 1);
	ImGui::Separator();
	ImGui::Checkbox("Show transparency values", &s_showAlpha);
	ImGui::SameLine();
	bool highlight = s_highlight;
	if (ImGui::Checkbox("Highlight mode", &highlight)) {
		if (highlight) {
			s_highlight = true;
			s_highlighted = -2;
			ShowHighlight(-1);
		} else {
			StopHighlight();
		}
	}
	if (ImGui::Button("Gradient generator")) ImGui::OpenPopup("gradient");
	DrawGradient();
	ImGui::SameLine();
	ImGui::BeginDisabled(s_undo.empty() || s_highlight);
	if (ImGui::Button("Undo")) Step(s_undo, s_redo, true);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(s_redo.empty() || s_highlight);
	if (ImGui::Button("Redo")) Step(s_redo, s_undo, false);
	ImGui::EndDisabled();
	if (!s_highlight && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {
		if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) Step(s_undo, s_redo, true);
		if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) Step(s_redo, s_undo, false);
	}
	ImGui::Separator();
	if (s_file > 0) {
		DrawEffectSheet(s_side, s_file, s_highlight ? s_highlightArray : s_edit);
	} else if (!s_sprite[s_side].pixels.empty()) {
		DrawSprite(s_preview, s_side, s_highlight ? s_highlightArray : s_edit, 200.0f);
	}
	DrawBoxes();
	DrawSave();
	ImGui::End();
	if (!g_paletteEditorVisible) StopHighlight();
}

void Palette_SetOnline(bool online, int localSide, unsigned long long peer) {
	LoadSettings();
	if (online && (peer != s_online.peer || localSide != s_online.localSide)) {
		s_online = OnlineState();
		s_onlineMessage.clear();
	}
	s_online.active = online;
	s_online.localSide = localSide;
	s_online.peer = peer;
}

unsigned long long Palette_LocalStamp() {
	if (!s_online.active || !g_paletteScene.valid) return 0;
	const int side = s_online.localSide;
	if (!GameFile(side, 0)) return 0;
	unsigned char files[kFiles][kFileBytes];
	ReadAll(side, files);
	return HashFiles(files) ^ reinterpret_cast<uintptr_t>(g_side[side].list) ^ (s_allowSave ? 1ull : 0ull) ^ (LocalIsCustom(side) ? 2ull : 0ull);
}

int Palette_BuildPacket(unsigned char* out, int cap) {
	if (cap < static_cast<int>(sizeof(PalPacket)) || !s_online.active || !g_paletteScene.valid) return 0;
	const int side = s_online.localSide;
	if (!GameFile(side, 0)) return 0;
	PalPacket* p = reinterpret_cast<PalPacket*>(out);
	memset(p, 0, sizeof(PalPacket));
	p->magic = kPacketMagic;
	p->version = 1;
	p->custom = LocalIsCustom(side) ? 1 : 0;
	p->allowSave = s_allowSave ? 1 : 0;
	memcpy(p->code, g_side[side].code.c_str(), (std::min)(g_side[side].code.size(), sizeof(p->code)));
	if (const PalData* pal = FindCustom(side, g_side[side].selected)) p->data.info = pal->info;
	ReadAll(side, p->data.files);
	return static_cast<int>(sizeof(PalPacket));
}

void Palette_Receive(const unsigned char* msg, int len, unsigned long long from) {
	if (!s_online.active || len != static_cast<int>(sizeof(PalPacket)) || from != s_online.peer) return;
	const PalPacket* p = reinterpret_cast<const PalPacket*>(msg);
	if (p->magic != kPacketMagic || p->version != 1) return;
	const std::string code(p->code, strnlen(p->code, sizeof(p->code)));
	if (!KnownCode(code)) return;
	static PalData incoming;
	incoming = p->data;
	CleanInfo(incoming.info);
	for (int i = 1; i < kColors; ++i) incoming.files[0][i * 4 + 3] = 0xFF;
	const unsigned long long hash = HashFiles(incoming.files);
	const bool changed = !s_online.have || hash != s_online.hash || code != s_online.code || (p->custom != 0) != s_online.custom ||
	                     (p->allowSave != 0) != s_online.allowSave || memcmp(&incoming.info, &s_online.data.info, sizeof(PalInfo)) != 0;
	if (!changed) return;
	const bool wasApplied = s_online.applied;
	s_online.have = true;
	s_online.custom = p->custom != 0;
	s_online.allowSave = p->allowSave != 0;
	s_online.code = code;
	s_online.data = incoming;
	s_online.hash = hash;
	const std::string line = "PALETTE: received " + s_online.code + " " +
	                         (s_online.custom ? std::string("custom \"") + s_online.data.info.name + "\"" : std::string("game color")) +
	                         (s_online.allowSave ? ", saving allowed" : ", saving not allowed") + (ReceivedBlocked() ? ", blocked" : "");
	Palette_Log(line.c_str());
	if (s_online.custom) ApplyReceived();
	else if (wasApplied) RestoreRemote();
}

void Palette_SetPeerName(unsigned long long id, const char* name) {
	if (!id || !name || !*name) return;
	const std::string clean = CleanText(std::string(name, strnlen(name, 128)));
	s_names[id] = clean;
	const auto it = s_blockedPlayers.find(id);
	if (it != s_blockedPlayers.end() && it->second != clean) {
		it->second = clean;
		SaveBlockSection("players", s_blockedPlayers);
	}
}

void Palette_DrawOnlineSection() {
	LoadSettings();
	ImGui::SeparatorText("Palettes");
	if (ImGui::Checkbox("Show the opponent's custom palette", &s_showOpponent)) {
		SaveSettings();
		if (s_showOpponent) ApplyReceived();
		else if (s_online.applied) RestoreRemote();
	}
	if (ImGui::Checkbox("Let opponents save my palette", &s_allowSave)) SaveSettings();
	if (s_online.active && s_online.have && s_online.custom) {
		const PalInfo& info = s_online.data.info;
		ImGui::Text("Opponent (%s): %s%s%s", PeerName(s_online.peer).c_str(), info.name[0] ? info.name : "custom palette",
		            info.creator[0] ? " by " : "", info.creator);
		if (ReceivedBlocked()) ImGui::TextDisabled("Blocked, not shown.");
		ImGui::BeginDisabled(!s_online.allowSave);
		if (ImGui::Button("Save their palette")) SaveReceived();
		ImGui::EndDisabled();
		if (!s_online.allowSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The opponent does not allow saving.");
		ImGui::SameLine();
		if (ImGui::Button("Block palette")) {
			s_blockedPalettes[s_online.hash] = std::string(info.name[0] ? info.name : "custom palette") + " (" + s_online.code + ", from " + PeerName(s_online.peer) + ")";
			SaveBlockSection("palettes", s_blockedPalettes);
			if (s_online.applied) RestoreRemote();
		}
		ImGui::SameLine();
		if (ImGui::Button("Block player")) {
			s_blockedPlayers[s_online.peer] = PeerName(s_online.peer);
			SaveBlockSection("players", s_blockedPlayers);
			if (s_online.applied) RestoreRemote();
		}
	} else if (s_online.active) {
		ImGui::TextDisabled("Opponent uses a game color.");
	}
	if (!s_onlineMessage.empty()) ImGui::TextDisabled("%s", s_onlineMessage.c_str());
	for (int list = 0; list < 2; ++list) {
		auto& blocked = list == 0 ? s_blockedPalettes : s_blockedPlayers;
		if (blocked.empty() || !ImGui::TreeNode(list == 0 ? "Blocked palettes" : "Blocked players")) continue;
		for (auto it = blocked.begin(); it != blocked.end();) {
			ImGui::PushID(static_cast<int>(it->first ^ (it->first >> 32)));
			const bool remove = ImGui::SmallButton("Unblock");
			ImGui::SameLine();
			ImGui::TextUnformatted(it->second.c_str());
			ImGui::PopID();
			if (!remove) {
				++it;
				continue;
			}
			it = blocked.erase(it);
			SaveBlockSection(list == 0 ? "palettes" : "players", blocked);
			ApplyReceived();
		}
		ImGui::TreePop();
	}
}

namespace {
using PackFn = const unsigned char*(__thiscall*)(void*, unsigned);
PackFn s_packEntry = nullptr;
PackFn s_packName = nullptr;
std::map<std::string, std::vector<PalData>> s_cssPalettes;
DWORD s_cssLastCall = 0;
unsigned char s_cssEntry[0x20 + kFileBytes];

}

void Palette_SetPackFunctions(void* entry, void* name) {
	s_packEntry = reinterpret_cast<PackFn>(entry);
	s_packName = reinterpret_cast<PackFn>(name);
}

const unsigned char* CssEntry(void* pack, unsigned index, const unsigned char* src);

const unsigned char* __fastcall Palette_CssEntry(void* pack, void*, unsigned index) {
	const unsigned char* src = s_packEntry(pack, index);
	if (!src || index % kFiles) return src;
	try {
		return CssEntry(pack, index, src);
	} catch (...) {
		return src;
	}
}

const unsigned char* CssEntry(void* pack, unsigned index, const unsigned char* src) {
	const char* name = reinterpret_cast<const char*>(s_packName(pack, index));
	if (!name || strnlen(name, 4) < 3) return src;
	const DWORD now = GetTickCount();
	if (now - s_cssLastCall > 2000) s_cssPalettes.clear();
	s_cssLastCall = now;
	const std::string code(name, 2);
	auto it = s_cssPalettes.find(code);
	if (it == s_cssPalettes.end()) it = s_cssPalettes.emplace(code, LoadFolder(code)).first;
	const std::vector<std::string> pool = IniPool(code, static_cast<int>(index / kFiles), it->second, true);
	const PalData* p = pool.empty() ? nullptr : FindIn(it->second, pool.front());
	if (!p || IsEmpty(p->files[0])) return src;
	memcpy(s_cssEntry, src, 0x20);
	memcpy(s_cssEntry + 0x20, p->files[0], kFileBytes);
	return s_cssEntry;
}

void Palette_SetDevice(IDirect3DDevice9* device) {
	if (device != s_device) {
		ReleaseThumbs();
		ReleaseTexture(s_preview);
		for (auto& e : s_effects)
			for (auto& kv : e.textures) ReleaseTexture(kv.second);
	}
	s_device = device;
}

void MoveOldFolder(const std::string& code) {
	const std::string from = Root() + "\\" + code, to = Folder(code);
	if (GetFileAttributesA(from.c_str()) == INVALID_FILE_ATTRIBUTES) return;
	if (_stricmp(from.c_str(), to.c_str()) == 0) {
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(from.c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		FindClose(h);
		if (CharacterNames(code) == fd.cFileName) return;
	}
	if (_stricmp(from.c_str(), to.c_str()) == 0 || GetFileAttributesA(to.c_str()) == INVALID_FILE_ATTRIBUTES) {
		if (MoveFileA(from.c_str(), to.c_str())) Palette_Log(("PALETTE: moved bbcse-palettes\\" + code + " to " + CharacterNames(code)).c_str());
		return;
	}
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			if (!MoveFileA((from + "\\" + fd.cFileName).c_str(), (to + "\\" + fd.cFileName).c_str()))
				Palette_Log(("PALETTE: kept bbcse-palettes\\" + code + "\\" + fd.cFileName + " (" + CharacterNames(code) + " has a file with that name)").c_str());
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	if (RemoveDirectoryA(from.c_str())) Palette_Log(("PALETTE: moved bbcse-palettes\\" + code + " into " + CharacterNames(code)).c_str());
}

void MoveOldSection(const char* from, const char* to) {
	static char buf[32768];
	char existing[8] = {};
	const DWORD n = GetPrivateProfileSectionA(from, buf, sizeof(buf), IniPath().c_str());
	if (!n || n >= sizeof(buf) - 2) return;
	if (GetPrivateProfileSectionA(to, existing, sizeof(existing), IniPath().c_str())) return;
	if (WritePrivateProfileSectionA(to, buf, IniPath().c_str())) WritePrivateProfileStringA(from, nullptr, nullptr, IniPath().c_str());
}

void Palette_Init() {
	EnsureIni();
	MoveOldSection("Nu-13", "Lambda");
	MoveOldSection("Mu-12", "Mu");
	for (const char* code : {"ar", "bn", "ca", "ha", "hz", "jn", "lc", "mk", "mu", "no", "ny", "pt", "rc", "rg", "rl", "tb", "tg", "tk", "vh"}) {
		MoveOldFolder(code);
		CreateDirectoryA(Folder(code).c_str(), nullptr);
	}
}
