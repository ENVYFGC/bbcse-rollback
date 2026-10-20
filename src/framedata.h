#pragma once

struct MoveFrameData {
	int startup = -1;
	int invulFirst = -1;
	int invulLast = -1;
	unsigned int invulFlags = 0;
};

struct FrameAdvantageView {
	bool valid = false;
	int advantage = 0;
	int p1Gap = -1;
	int p2Gap = -1;
	int hitSide = -1;
	bool onBlock = false;
	MoveFrameData move[2];
};

extern FrameAdvantageView g_frameAdv;
extern bool g_frameAdvVisible;

void FrameData_Update(const unsigned char* p1, const unsigned char* p2, int worldFrame, const unsigned char* const* objects, int objectCount);
void FrameData_Reset();
