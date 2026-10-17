#include "story.h"

#include <algorithm>
#include <cstring>

namespace Story {
namespace {
// Per stage: the minimum md, codec level (Current_Speech_Level), cardkey level (cw) and escape step (Vj).
const int kMd[STAGE_COUNT]      = { 1, 1, 2, 3, 3, 4, 5, 5 };
const int kSpeech[STAGE_COUNT]  = { 1, 1, 2, 2, 3, 4, 5, 5 };
const int kCardkey[STAGE_COUNT] = { 1, 2, 2, 3, 3, 3, 4, 4 };
const int kVj[STAGE_COUNT]      = { 0, 0, 0, 3, 4, 4, 4, 4 };

// Room -> first-visit stage and the md values its script has branches for (bit n = md n).
struct Room { const char* prefix; int stage; unsigned mds; };
constexpr unsigned M(int a, int b) { return ((1u << (b + 1)) - 1) & ~((1u << a) - 1); }   // md a..b
const Room kRooms[] = {
    {"01_Equipment_Store", LIFT, M(3, 5)},
    {"01_", START, M(1, 5)},
    {"02_South_Corridor", B1, M(1, 5)},
    {"02_Security_Control_Room", B1, M(2, 5)},
    {"02_Lift_Access", ESCAPE, M(3, 5)},
    {"02_Control_Room", RETURN, M(4, 5)},
    {"02_Observation_Area", RETURN, M(4, 5)},
    {"02_", B1, (1u << 2) | M(4, 5)},
    {"03_", REVEALED, M(5, 5)},
};
const Room* FindRoom(const std::string& room) {
    for (const Room& r : kRooms) if (room.compare(0, strlen(r.prefix), r.prefix) == 0) return &r;
    return nullptr;
}
}  // namespace

const char* StageName(int stage) {
    static const char* names[STAGE_COUNT] = {
        "New game", "Utility Control Room login", "Entered basement 1", "Security Control escape",
        "Lift ride up", "Back in basement 1", "Observation Area login", "Submarine revealed" };
    return (stage >= 0 && stage < STAGE_COUNT) ? names[stage] : "?";
}

int State::CurrentStage() const {
    if (md >= 5) return (ak && !bk) ? REVEALED : OBSERVATION;
    if (md >= 4) return RETURN;
    if (md >= 3) return vj >= 4 ? LIFT : ESCAPE;
    if (md >= 2) return B1;
    return ye ? LOGIN : START;
}

void AddReads(LuaChunk& c) {
    c.ReturnGlobal("md").ReturnGlobal("Current_Speech_Level").ReturnGlobal("cw").ReturnGlobal("Ye")
     .ReturnGlobal("Vj").ReturnGlobal("Ak").ReturnGlobal("Bk");
}

State ParseReads(const std::vector<LuaValue>& v, size_t first, int layer) {
    State s;
    if (v.size() < first + kReadCount || v[first].type != LuaValue::Number) return s;
    s.valid = true;
    s.md = v[first].Int(1);
    s.speech = v[first + 1].Int(1);
    s.cardkey = v[first + 2].Int(0);
    s.ye = v[first + 3].Truthy();
    s.vj = v[first + 4].Int(0);
    s.ak = v[first + 5].Truthy();
    s.bk = v[first + 6].Truthy();
    s.layer = layer;
    return s;
}

int RoomMinStage(const std::string& room) {
    const Room* r = FindRoom(room);
    return r ? r->stage : START;
}

int TargetStage(const std::string& room, int current) {
    const Room* r = FindRoom(room);
    if (!r) return current;
    int t = (std::max)(current, r->stage);
    while (t + 1 < STAGE_COUNT && !(r->mds & (1u << kMd[t]))) ++t;
    return t;
}

void AddApply(LuaChunk& c, const State& cur, int target) {
    const int from = cur.CurrentStage();
    auto passes = [&](int s) { return from < s && s <= target; };
    // One-off events of the stages passed (what the scripts set at that point besides the counters below).
    if (passes(LOGIN)) {   // 01_Utility_Control_Room, terminal login
        c.Set("qe", true).Set("Ye", true).Set("Me", true).Set("Og", 4).Set("Pg", 4).Set("kf", 8).Set("Qg", 7)
         .Set("Dg", 3).Set("Rg", 0).Set("Sg", false).Set("Mg", false);
    }
    if (passes(ESCAPE)) {  // 02_Security_Control_Room: login, plot reveal codec, shutdown (Tk)
        c.Set("al", false).Set("Sg", true).Set("lg", true).Set("Uk", true);
    }
    if (passes(RETURN))    // 02_South_Corridor, md 3 -> 4: kills counted from here for the Observation achievements
        c.SetFromCall("xk", "GetStatistic", {"GS_ENEMIESKILLED"});
    if (passes(REVEALED) || (target >= REVEALED && cur.bk))   // 02_Observation_Area reveal, cleared in 03_Metal_Gear
        c.Set("Ak", true).Set("Bk", false);
    // Counters, raised only.
    if (cur.md < kMd[target]) c.Set("md", kMd[target]);
    if (cur.speech < kSpeech[target]) c.Set("Current_Speech_Level", kSpeech[target]);
    if (cur.vj < kVj[target]) c.Set("Vj", kVj[target]);
    if (cur.cardkey < kCardkey[target]) c.Call("Ng", {kCardkey[target]});   // swaps the cardkey item too
    // Guards and cameras per room layout: layer 2 from the Security Control double-cross on.
    if (target >= ESCAPE && (cur.layer < 0 ? from < ESCAPE : cur.layer < 2)) c.Call("SetLayerNumber", {2});
}

}  // namespace Story
