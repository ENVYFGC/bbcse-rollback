#include "framedata.h"
#include "hitboxes.h"

#include <cstring>

FrameAdvantageView g_frameAdv;
bool g_frameAdvVisible = false;

namespace {
constexpr int kInvulOff = 0x924;
constexpr unsigned int kInvulOn = 0x2;
constexpr int kOwnerOff = 0x1DC;
constexpr int kActionOff = 0x1EC0;
constexpr int kActionLen = 32;
constexpr int kPosYOff = 0x24C;
constexpr int kBlockstunOff = 0x2114;
constexpr int kHitstunOff = 0x46D0;

const char* const kIdleActions[] = {
	"_NEUTRAL", "CmnActStand", "CmnActStandTurn", "CmnActStand2Crouch",
	"CmnActCrouch", "CmnActCrouchTurn", "CmnActCrouch2Stand",
	"CmnActFWalk", "CmnActBWalk", "CmnActFWalkEnd", "CmnActBWalkEnd",
	"CmnActFDash", "CmnActFDashStop",
	"CmnActJumpLanding", "CmnActLandingStiffEnd",
	"CmnActUkemiLandNLanding", "CmnActJumpPre",
	"CmnActCrouchGuardPre", "CmnActCrouchGuardLoop", "CmnActCrouchGuardEnd",
	"CmnActCrouchHeavyGuardPre", "CmnActCrouchHeavyGuardLoop", "CmnActCrouchHeavyGuardEnd",
	"CmnActMidGuardPre", "CmnActMidGuardLoop", "CmnActMidGuardEnd",
	"CmnActMidHeavyGuardPre", "CmnActMidHeavyGuardLoop", "CmnActMidHeavyGuardEnd",
	"CmnActHighGuardPre", "CmnActHighGuardLoop", "CmnActHighGuardEnd",
	"CmnActHighHeavyGuardPre", "CmnActHighHeavyGuardLoop", "CmnActHighHeavyGuardEnd",
};

const char* const kAirIdleActions[] = {
	"_NEUTRAL", "CmnActAirGuardPre", "CmnActAirGuardLoop", "CmnActAirGuardEnd",
	"CmnActAirHeavyGuardLoop", "CmnActAirHeavyGuardEnd",
};

template <size_t N>
bool InList(const char* action, const char* const (&list)[N]) {
	for (const char* name : list)
		if (strcmp(action, name) == 0) return true;
	return false;
}

struct PlayerState {
	const unsigned char* data = nullptr;
	char action[kActionLen + 1] = {};
	char previousAction[kActionLen + 1] = {};
	int positionY = 0;
	int previousPositionY = 0;

	void Read(const unsigned char* d) {
		data = d;
		memcpy(action, d + kActionOff, kActionLen);
		action[kActionLen] = 0;
		positionY = *reinterpret_cast<const int*>(d + kPosYOff);
	}
	void KeepAsPrevious() {
		memcpy(previousAction, action, sizeof(action));
		previousPositionY = positionY;
	}
	int Blockstun() const { return *reinterpret_cast<const int*>(data + kBlockstunOff); }
	int Hitstun() const { return *reinterpret_cast<const int*>(data + kHitstunOff); }
	bool IsAirborne() const { return positionY > 0; }
	bool IsMovingVertically() const { return positionY != previousPositionY; }
	bool IsBlocking() const { return Blockstun() > 0; }
	bool IsInHitstun() const { return Hitstun() > 0 && !InList(action, kIdleActions); }
	bool IsIdle() const {
		if ((IsAirborne() || previousPositionY > 0) && InList(action, kAirIdleActions)) return !IsMovingVertically();
		if (strcmp(action, "CmnActUkemiLandNLanding") == 0 && strcmp(action, previousAction) != 0) return false;
		if (IsBlocking() || IsInHitstun()) return false;
		return InList(action, kIdleActions);
	}
};

struct Interaction {
	int previousFrame = -1;
	int timer = 0;
	bool started = false;
	int p1Gap = -1;
	int p2Gap = -1;
	int hitSide = -1;
	bool onBlock = false;
};

struct MoveTrack {
	bool busy = false;
	bool activeSeen = false;
	bool published = false;
	int frame = 0;
	char action[kActionLen + 1] = {};
	MoveFrameData data;
};

PlayerState g_p1, g_p2;
Interaction g_state;
MoveTrack g_move[2];

bool OwnsActiveHitbox(const unsigned char* owner, const unsigned char* const* objects, int count) {
	for (int i = 0; i < count; ++i) {
		const unsigned char* o = objects[i];
		if (!o) continue;
		if (o != owner && *reinterpret_cast<const unsigned char* const*>(o + kOwnerOff) != owner) continue;
		if (Hitbox_Active(o)) return true;
	}
	return false;
}

void TrackMove(const PlayerState& p, int side, const unsigned char* const* objects, int count) {
	MoveTrack& t = g_move[side];
	MoveFrameData& m = g_frameAdv.move[side];
	const bool busy = !p.IsIdle() && !p.IsBlocking() && !p.IsInHitstun();
	if (!busy) {
		t.busy = false;
		return;
	}
	const bool newMove = !t.busy;
	const bool followUp = t.busy && t.activeSeen && strcmp(p.action, t.action) != 0;
	if (newMove || followUp) {
		t = MoveTrack();
		t.busy = true;
		t.published = newMove;
		if (newMove) m = MoveFrameData();
	}
	memcpy(t.action, p.action, sizeof(t.action));
	++t.frame;
	const bool active = OwnsActiveHitbox(p.data, objects, count);
	if (active) {
		if (t.data.startup < 0) t.data.startup = t.frame;
		t.activeSeen = true;
	}
	const unsigned int invul = *reinterpret_cast<const unsigned int*>(p.data + kInvulOff);
	if (invul & kInvulOn) {
		if (t.data.invulFirst < 0) t.data.invulFirst = t.frame;
		t.data.invulLast = t.frame;
		t.data.invulFlags |= invul;
	}
	if (active || (invul & kInvulOn)) t.published = true;
	if (t.published) m = t.data;
}

void NoteDefender(const PlayerState& p, int side) {
	if (p.IsInHitstun()) {
		g_state.hitSide = side;
		g_state.onBlock = false;
	} else if (p.IsBlocking()) {
		g_state.hitSide = side;
		g_state.onBlock = true;
	}
}

void ComputeGap(const PlayerState& p, int& counter, int& result) {
	if (p.IsBlocking() || p.IsInHitstun()) {
		if (counter > 0 && counter <= 30) result = counter;
		counter = 0;
	} else {
		++counter;
		result = -1;
	}
}

void ComputeAdvantage(const PlayerState& p1, const PlayerState& p2) {
	const bool idle1 = p1.IsIdle();
	const bool idle2 = p2.IsIdle();
	if (!idle1 && !idle2) {
		if (!g_state.started) g_state.hitSide = -1;
		g_state.started = true;
		g_state.timer = 0;
		NoteDefender(p1, 0);
		NoteDefender(p2, 1);
	}
	if (!g_state.started) return;
	if (idle1 && idle2) {
		g_state.started = false;
		g_frameAdv.advantage = g_state.timer;
		g_frameAdv.hitSide = g_state.hitSide;
		g_frameAdv.onBlock = g_state.onBlock;
	}
	if (!idle1) --g_state.timer;
	if (!idle2) ++g_state.timer;
}

}

void FrameData_Reset() {
	g_state = Interaction();
	g_p1 = PlayerState();
	g_p2 = PlayerState();
	g_move[0] = MoveTrack();
	g_move[1] = MoveTrack();
	g_frameAdv = FrameAdvantageView();
}

void FrameData_Update(const unsigned char* p1, const unsigned char* p2, int worldFrame, const unsigned char* const* objects, int objectCount) {
	if (!p1 || !p2) {
		g_frameAdv.valid = false;
		return;
	}
	g_p1.Read(p1);
	g_p2.Read(p2);
	g_frameAdv.valid = true;
	const bool moved = worldFrame > g_state.previousFrame;
	if (worldFrame < g_state.previousFrame) {
		FrameData_Reset();
		g_frameAdv.valid = true;
	}
	g_state.previousFrame = worldFrame;
	if (!moved) return;
	ComputeGap(g_p1, g_state.p1Gap, g_frameAdv.p1Gap);
	ComputeGap(g_p2, g_state.p2Gap, g_frameAdv.p2Gap);
	ComputeAdvantage(g_p1, g_p2);
	TrackMove(g_p1, 0, objects, objectCount);
	TrackMove(g_p2, 1, objects, objectCount);
	g_p1.KeepAsPrevious();
	g_p2.KeepAsPrevious();
}
