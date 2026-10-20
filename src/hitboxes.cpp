#include "hitboxes.h"

#include <cmath>

#include "imgui.h"

HitboxScene g_hitboxScene;
bool g_hitboxVisible = false;

namespace {
constexpr int kBoxesPtrOff = 0x4C;
constexpr int kHurtCountOff = 0x94;
constexpr int kHitCountOff = 0x98;
constexpr int kFacingLeftOff = 0x244;
constexpr int kOffsetX1Off = 0x254;
constexpr int kRotationOff = 0x25C;
constexpr int kScaleXOff = 0x264;
constexpr int kScaleYOff = 0x268;
constexpr int kDrawXOff = 0x2BC;
constexpr int kDrawYOff = 0x2C0;
constexpr int kOffsetX2Off = 0x2D4;
constexpr int kOffsetY2Off = 0x2DC;
constexpr int kLinkedProviderOff = 0x20C;
constexpr float kWorldScale = 0.346f;
constexpr float kPi = 3.14159265f;

struct BoxEntry {
	int type;
	float x, y, width, height;
};

int ReadInt(const unsigned char* obj, int off) { return *reinterpret_cast<const int*>(obj + off); }

bool CanHit(const unsigned char* obj) {
	if (obj[0x484] != 0 && (ReadInt(obj, 0x150) & 0x4000000) != 0) return false;
	if (ReadInt(obj, 0x454) >= ReadInt(obj, 0x458)) return false;
	if ((ReadInt(obj, 0x238) & 0x100) == 0) return false;
	const unsigned int flags = static_cast<unsigned int>(ReadInt(obj, 0x23C));
	return (flags & 0x4000600) == 0 && (flags & 0x80) == 0;
}

ImVec2 Rotate(ImVec2 center, float rad, ImVec2 p) {
	if (rad == 0.0f) return p;
	const float s = sinf(rad), c = cosf(rad);
	const float x = p.x - center.x, y = p.y - center.y;
	return ImVec2(x * c - y * s + center.x, x * s + y * c + center.y);
}

class Projector {
public:
	Projector(const HitboxScene& s, float displayWidth, float displayHeight)
		: m_s(s), m_x(s.dstWidth > 0 ? s.dstLeft : 0), m_y(s.dstHeight > 0 ? s.dstTop : 0),
		  m_w(s.dstWidth > 0 ? s.dstWidth : displayWidth), m_h(s.dstHeight > 0 ? s.dstHeight : displayHeight) {}

	ImVec2 operator()(ImVec2 world) const {
		const float* v = m_s.view;
		const float* p = m_s.proj;
		float e[4];
		for (int c = 0; c < 4; ++c) e[c] = world.x * v[c] + world.y * v[4 + c] + v[12 + c];
		float clip[4];
		for (int c = 0; c < 4; ++c) clip[c] = e[0] * p[c] + e[1] * p[4 + c] + e[2] * p[8 + c] + e[3] * p[12 + c];
		if (clip[3] == 0.0f) return ImVec2(-1e6f, -1e6f);
		const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
		return ImVec2(floorf(m_x + (nx + 1.0f) * 0.5f * m_w), floorf(m_y + (1.0f - ny) * 0.5f * m_h));
	}

private:
	const HitboxScene& m_s;
	float m_x, m_y, m_w, m_h;
};

void DrawObject(ImDrawList* list, const Projector& project, const unsigned char* obj, bool drawHitboxes, bool drawHurtboxes, bool drawOrigin) {
	const auto* boxes = *reinterpret_cast<const BoxEntry* const*>(obj + kBoxesPtrOff);
	const int count = ReadInt(obj, kHurtCountOff) + ReadInt(obj, kHitCountOff);
	if (!boxes || count <= 0 || count > 64) return;

	const bool facingLeft = ReadInt(obj, kFacingLeftOff) != 0;
	const float posX = (ReadInt(obj, kDrawXOff) - ReadInt(obj, kOffsetX1Off) + ReadInt(obj, kOffsetX2Off)) / 1000.0f * kWorldScale;
	const float posY = (ReadInt(obj, kDrawYOff) + ReadInt(obj, kOffsetY2Off)) / 1000.0f * kWorldScale;
	const ImVec2 origin(floorf(posX), floorf(posY));

	float rotationDeg = ReadInt(obj, kRotationOff) / 1000.0f;
	if (!facingLeft && rotationDeg != 0.0f) rotationDeg = 360.0f - rotationDeg;
	const float rotationRad = rotationDeg * kPi / 180.0f;
	const float scaleX = ReadInt(obj, kScaleXOff) / 1000.0f;
	const float scaleY = ReadInt(obj, kScaleYOff) / 1000.0f;

	for (int i = 0; i < count; ++i) {
		const BoxEntry& b = boxes[i];
		if (b.type != 0 && !drawHitboxes) continue;
		if (b.type == 0 && !drawHurtboxes) continue;
		float offsetX = floorf(b.x * kWorldScale * scaleX);
		const float offsetY = -floorf(b.y * kWorldScale * scaleY);
		float width = floorf(b.width * kWorldScale * scaleX);
		const float height = -floorf(b.height * kWorldScale * scaleY);
		if (!facingLeft) {
			offsetX = -offsetX;
			width = -width;
		}
		const ImVec2 a = project(Rotate(origin, rotationRad, ImVec2(origin.x + offsetX, origin.y + offsetY)));
		const ImVec2 bb = project(Rotate(origin, rotationRad, ImVec2(origin.x + offsetX + width, origin.y + offsetY)));
		const ImVec2 c = project(Rotate(origin, rotationRad, ImVec2(origin.x + offsetX + width, origin.y + offsetY + height)));
		const ImVec2 d = project(Rotate(origin, rotationRad, ImVec2(origin.x + offsetX, origin.y + offsetY + height)));
		const ImU32 border = b.type == 0 ? IM_COL32(0x00, 0x33, 0xCC, 0xFF) : IM_COL32(0xFF, 0x00, 0x00, 0xFF);
		const ImU32 fill = (border & 0x00FFFFFF) | (0x40u << 24);
		list->AddQuadFilled(a, bb, c, d, fill);
		list->AddQuad(a, bb, c, d, border, 1.5f);
	}

	if (!drawOrigin) return;
	const unsigned int orange = IM_COL32(0xFF, 0x99, 0x00, 0xFF);
	list->AddLine(project(ImVec2(origin.x - 10, origin.y)), project(ImVec2(origin.x + 10, origin.y)), orange, 1.5f);
	list->AddLine(project(origin), project(ImVec2(origin.x, origin.y + 50)), orange, 1.5f);
}

const unsigned char* LinkedBoxProvider(const HitboxScene& s, const unsigned char* obj) {
	const unsigned char* linked = *reinterpret_cast<const unsigned char* const*>(obj + kLinkedProviderOff);
	if (!linked || linked == obj) return nullptr;
	if (linked == s.chars || linked == s.chars + 0x20140) return linked;
	if (linked < s.poolBegin || linked >= s.poolBegin + s.poolStride * s.poolCount) return nullptr;
	if ((linked - s.poolBegin) % s.poolStride != 0) return nullptr;
	if (ReadInt(linked, 0x10) != 1 || !*reinterpret_cast<const void* const*>(linked + kBoxesPtrOff)) return nullptr;
	return linked;
}

}

bool Hitbox_Active(const unsigned char* obj) {
	return CanHit(obj) && ReadInt(obj, kHitCountOff) > 0;
}

void Hitboxes_Draw(ImDrawList* list, float displayWidth, float displayHeight) {
	const HitboxScene& s = g_hitboxScene;
	if (!s.valid || !s.view || !s.proj) return;
	const Projector project(s, displayWidth, displayHeight);
	for (const unsigned char* obj : s.objects) {
		const bool canHit = CanHit(obj);
		DrawObject(list, project, obj, canHit, true, true);
		if (!canHit) continue;
		const unsigned char* linked = LinkedBoxProvider(s, obj);
		if (linked && !CanHit(linked)) DrawObject(list, project, linked, true, false, false);
	}
}
