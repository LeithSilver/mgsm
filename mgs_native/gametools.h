#pragma once

#include "story.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Debug-mode tools on top of the game's own Lua functions (S3ERuntime::RunLua): inventory editor, cheats
// (infinite health / ammo) and story stage. A snapshot of the state is read every half second in gameplay
// and after every edit; the overlay shows it and queues edits from any thread. Edits made while the overlay
// is open run in a single game frame let through for them (see the pause hook in main.cpp).
class GameTools {
public:
    static GameTools& Instance();

    struct Item {
        const char* id;      // inventory item name (the game hashes it lower-case)
        const char* label;
        int give;            // amount when added (weapons: rounds; others: count)
        bool weapon;         // weapon slot (else item slot)
    };
    static const std::vector<Item>& Items();
    static constexpr int kMaxCardkey = 4;   // the facility's doors go up to level 4 (Ng(4) in 02_Observation_Area)

    struct Snapshot {
        bool valid = false;  // read in gameplay with Snake alive
        Story::State story;
        int health = 0, max_health = 0;
        bool bandana = false;   // bandana equipped (its own infinite ammo)
        std::vector<bool> owned, equipped;   // equipped items can't be removed (unequip them in the game first)
        std::vector<int> amount;
    };
    Snapshot Get();

    // Overlay (any thread). Items are indices into Items().
    void Give(int item);
    void Remove(int item);
    void SetAmount(int item, int amount);
    void Fill(int item);                 // restock to the item's maximum
    void SetCardkey(int level);
    void RefillHealth();
    void AdvanceStory(int stage);        // raise the story to `stage` (never lowers it), like level select does
    void RunScript(const std::string& script);   // debug: MGS_TOOLS actions, see gametools.cpp

    // Emu thread, once per frame: snapshot and cheats every 15 frames in gameplay (debug mode only).
    void Tick(bool gameplay);

private:
    GameTools() = default;
    // Runs fill(chunk) after a Snake-alive guard, then reads the snapshot; then(snapshot) on the emu thread.
    void Request(const std::function<void(LuaChunk&)>& fill, std::function<void(const Snapshot&)> then = nullptr);
    void AddSnapshotReads(LuaChunk& c);
    void Parse(const std::vector<LuaValue>& v);

    std::mutex mutex_;
    Snapshot snap_;
    int frames_ = 0;
    bool health_applied_ = false, ammo_applied_ = false;   // emu thread
};
