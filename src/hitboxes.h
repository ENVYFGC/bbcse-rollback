#pragma once

#include <vector>

struct ImDrawList;

struct HitboxScene {
	bool valid = false;
	const float* view = nullptr;
	const float* proj = nullptr;
	float dstLeft = 0, dstTop = 0, dstWidth = 0, dstHeight = 0;
	float backBufferWidth = 0, backBufferHeight = 0;
	std::vector<const unsigned char*> objects;
	const unsigned char* chars = nullptr;
	const unsigned char* poolBegin = nullptr;
	unsigned int poolStride = 0, poolCount = 0;
};

extern HitboxScene g_hitboxScene;
extern bool g_hitboxVisible;

void Hitboxes_Draw(ImDrawList* list, float displayWidth, float displayHeight);
bool Hitbox_Active(const unsigned char* obj);
