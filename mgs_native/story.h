#pragma once

#include "luachunk.h"
#include <string>
#include <vector>

// Facility story progress, for level select (README §6.15). The room scripts branch on the story stage md
// (and a few other progress variables) and only handle the values a player can arrive with, so a jump has to
// bring the progress up to where the room is normally first reached. Stages, in story order:
namespace Story {

enum Stage {
    START,         // md 1: new game
    LOGIN,         // 01_Utility_Control_Room terminal login: cardkey 2 and its flags (Ye, qe, ...)
    B1,            // md 2: 02_South_Corridor first visit (codec level 2 from 01_Auxiliary_Corridor)
    ESCAPE,        // md 3: 02_Security_Control_Room double-cross: layer 2, cardkey 3, guard escape (Vj 3)
    LIFT,          // 02_Lift_Access ride up: codec level 3, Vj 4
    RETURN,        // md 4: back in 02_South_Corridor (codec level 4 from 01_Auxiliary_Corridor)
    OBSERVATION,   // md 5: 02_Observation_Area login: cardkey 4, codec level 5
    REVEALED,      // submarine reveal seen (Ak, Bk cleared by 03_Metal_Gear): basement 2 and the boss
    STAGE_COUNT
};
const char* StageName(int stage);

// The progress variables involved, as read from the game (AddReads) plus the layer (guest memory).
struct State {
    bool valid = false;
    int md = 1, speech = 1, cardkey = 0, vj = 0, layer = -1;
    bool ye = false, ak = false, bk = false;
    int CurrentStage() const;
};
constexpr int kReadCount = 7;
void AddReads(LuaChunk& c);                                        // returns md, speech, cw, Ye, Vj, Ak, Bk
State ParseReads(const std::vector<LuaValue>& v, size_t first, int layer);

int RoomMinStage(const std::string& room);
// The stage a jump to `room` should leave the game at: at least the room's first-visit stage, raised further
// when the room's script has no branch for that md (e.g. 02_Mess_Hall handles md 2, 4 and 5 but not 3).
int TargetStage(const std::string& room, int current);
// Raises every variable to `target` (never lowers anything) and applies the one-off flags of each stage
// passed on the way. Nothing is added when the state is already there.
void AddApply(LuaChunk& c, const State& cur, int target);

}  // namespace Story
