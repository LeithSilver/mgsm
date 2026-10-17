#include "gametools.h"
#include "runtime.h"
#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

GameTools& GameTools::Instance() {
    static GameTools inst;
    return inst;
}

// The facility's items (ingame.lua: G_Pickup* / G_Equip*); `give` = what the first pickup gives.
const std::vector<GameTools::Item>& GameTools::Items() {
    static const std::vector<Item> items = {
        {"m9Item", "M9 (tranquilliser)", 15, true},
        {"socomItem", "SOCOM", 20, true},
        {"famasItem", "FAMAS", 20, true},
        {"nikitaItem", "Nikita", 10, true},
        {"chaffItem", "Chaff grenades", 4, true},
        {"c4Item", "C4", 6, false},
        {"rationItem", "Ration", 1, false},
        {"boxItem", "Cardboard box", 1, false},
        {"camoItem", "Stealth camo", 1, false},
        {"cameraItem", "Camera", 1, false},
        {"bandanaItem", "Bandana (infinite ammo)", 1, false},
        {"VRgogglesItem", "VR goggles", 1, false},
    };
    return items;
}

GameTools::Snapshot GameTools::Get() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

// Snapshot layout: [0] true (Snake alive: the guard returns nothing otherwise), [1..7] Story::AddReads,
// [8] health, [9] max health, [10] bandana equipped, then per item: owned, amount, equipped.
void GameTools::AddSnapshotReads(LuaChunk& c) {
    c.ReturnConst(true);
    Story::AddReads(c);
    c.ReturnCall("CharacterGetHealth", {"Snake"}).ReturnCall("CharacterGetMaxHealth", {"Snake"})
     .ReturnCall("InventoryCurrentItemHasName", {"bandanaItem"});
    for (const Item& it : Items()) {
        c.ReturnCall("InventoryContainsItem", {it.id}).ReturnCall("InventoryItemGetAmount", {it.id})
         .ReturnCall(it.weapon ? "InventoryCurrentWeaponHasName" : "InventoryCurrentItemHasName", {it.id});
    }
}

void GameTools::Parse(const std::vector<LuaValue>& v) {
    if (v.empty()) return;   // the chunk failed (logged); keep the last snapshot
    Snapshot s;
    s.valid = v[0].Truthy();
    if (s.valid) {
        s.story = Story::ParseReads(v, 1, S3ERuntime::Instance().LayerNumber());
        s.health = v[8].Int();
        s.max_health = v[9].Int();
        s.bandana = v[10].Truthy();
        for (size_t i = 0; i < Items().size(); ++i) {
            const size_t k = 11 + 3 * i;
            s.owned.push_back(k < v.size() && v[k].Truthy());
            s.amount.push_back(k + 1 < v.size() ? v[k + 1].Int() : 0);
            s.equipped.push_back(k + 2 < v.size() && v[k + 2].Truthy());
        }
    }
    if (VerboseLog()) {   // one line whenever the snapshot changes
        std::string line = !s.valid ? "Snake not available" :
            std::string("story ") + Story::StageName(s.story.CurrentStage()) + ", md " + std::to_string(s.story.md) +
            ", cardkey " + std::to_string(s.story.cardkey) + ", layer " + std::to_string(s.story.layer) +
            ", health " + std::to_string(s.health) + "/" + std::to_string(s.max_health);
        for (size_t i = 0; i < s.owned.size(); ++i)
            if (s.owned[i]) line += std::string(i ? ", " : ": ") + Items()[i].id + "=" + std::to_string(s.amount[i]);
        static std::string last;
        if (line != last) { std::cout << "[Tools] " << line << std::endl; last = line; }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snap_ = std::move(s);
}

// Debug: MGS_TOOLS="give:famasItem,amount:famasItem=55,fill:nikitaItem,remove:m9Item,cardkey:3,story:5,refill,
//                   call:Fn[:string]"   (call: any global Lua function, e.g. call:AssertMsg:hello)
// runs once, at the first snapshot in gameplay (story = Story::Stage index).
void GameTools::RunScript(const std::string& script) {
    size_t pos = 0;
    while (pos <= script.size()) {
        size_t end = script.find(',', pos);
        if (end == std::string::npos) end = script.size();
        const std::string cmd = script.substr(pos, end - pos), op = cmd.substr(0, cmd.find(':'));
        const std::string arg = cmd.find(':') == std::string::npos ? "" : cmd.substr(cmd.find(':') + 1);
        auto item = [&](const std::string& id) {
            for (size_t i = 0; i < Items().size(); ++i) if (id == Items()[i].id) return static_cast<int>(i);
            return -1;
        };
        std::cout << "[Tools] " << cmd << std::endl;
        if (op == "give") Give(item(arg));
        else if (op == "remove") Remove(item(arg));
        else if (op == "fill") Fill(item(arg));
        else if (op == "amount") SetAmount(item(arg.substr(0, arg.find('='))), atoi(arg.substr(arg.find('=') + 1).c_str()));
        else if (op == "cardkey") SetCardkey(atoi(arg.c_str()));
        else if (op == "story") AdvanceStory(atoi(arg.c_str()));
        else if (op == "refill") RefillHealth();
        else if (op == "call") {   // call:Fn or call:Fn:string argument
            const std::string fn = arg.substr(0, arg.find(':'));
            const std::string sarg = arg.find(':') == std::string::npos ? "" : arg.substr(arg.find(':') + 1);
            Request([fn, sarg](LuaChunk& c) { if (sarg.empty()) c.Call(fn); else c.Call(fn, {sarg}); });
        }
        pos = end + 1;
    }
}

void GameTools::Request(const std::function<void(LuaChunk&)>& fill, std::function<void(const Snapshot&)> then) {
    LuaChunk c;
    c.GuardSnakeAlive();
    if (fill) fill(c);
    AddSnapshotReads(c);
    S3ERuntime::Instance().RunLua(c, [this, then](const std::vector<LuaValue>& v) {
        Parse(v);
        if (then) then(Get());
    });
}

void GameTools::Give(int i) {
    if (i < 0 || i >= static_cast<int>(Items().size())) return;
    const Item it = Items()[i];
    Request([it](LuaChunk& c) { c.Call("InventoryAddItem", {it.id, it.give}); });
}

void GameTools::Remove(int i) {
    if (i < 0 || i >= static_cast<int>(Items().size())) return;
    const Item it = Items()[i];
    Request([it](LuaChunk& c) { c.Call("InventoryRemoveItem", {it.id}); });
}

// Amounts change through the game's own restock (capped at the item's maximum) and consume (one per call).
void GameTools::SetAmount(int i, int amount) {
    if (i < 0 || i >= static_cast<int>(Items().size())) return;
    const Item it = Items()[i];
    amount = (std::max)(0, amount);
    Request(nullptr, [this, i, it, amount](const Snapshot& s) {
        if (!s.valid || i >= static_cast<int>(s.owned.size())) return;
        const int cur = s.amount[i];
        if (!s.owned[i]) { Request([it, amount](LuaChunk& c) { c.Call("InventoryAddItem", {it.id, amount}); }); return; }
        if (amount > cur) Request([it, amount, cur](LuaChunk& c) { c.Call("InventoryItemRestock", {it.id, amount - cur}); });
        else if (amount < cur)
            Request([it, amount, cur](LuaChunk& c) { for (int n = (std::min)(cur - amount, 999); n > 0; --n) c.Call("InventoryItemConsume", {it.id}); });
    });
}

void GameTools::Fill(int i) {
    if (i < 0 || i >= static_cast<int>(Items().size())) return;
    const Item it = Items()[i];
    const Snapshot s = Get();
    const bool owned = s.valid && i < static_cast<int>(s.owned.size()) && s.owned[i];
    Request([it, owned](LuaChunk& c) {
        if (!owned) c.Call("InventoryAddItem", {it.id, 0});
        c.Call("InventoryItemRestock", {it.id, 9999});
    });
}

void GameTools::SetCardkey(int level) {
    level = (std::max)(1, (std::min)(kMaxCardkey, level));
    Request([level](LuaChunk& c) { c.Call("Ng", {level}); });   // swaps the cardkeyLevel<N>Item and refreshes the doors
}

void GameTools::RefillHealth() {
    Request(nullptr, [this](const Snapshot& s) {
        if (s.valid && s.health < s.max_health)
            Request([s](LuaChunk& c) { c.Call("CharacterChangeHealth", {"Snake", s.max_health - s.health}); });
    });
}

void GameTools::AdvanceStory(int stage) {
    Request(nullptr, [this, stage](const Snapshot& s) {
        if (!s.valid || !s.story.valid) return;
        const Story::State cur = s.story;
        const int target = (std::max)(stage, cur.CurrentStage());
        Request([cur, target](LuaChunk& c) { Story::AddApply(c, cur, target); });
    });
}

// Infinite health = invincible (CharacterSetIsInvincible) plus a top-up; infinite ammo = the bandana's
// SetInfiniteAmmoEnabled. Both are re-applied every 15 frames: room changes and (un)equipping the bandana
// reset them. Turning a cheat off restores the game's own state once.
void GameTools::Tick(bool gameplay) {
    if (!gameplay) {
        frames_ = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.valid = false;
        return;
    }
    if (++frames_ < 15) return;
    frames_ = 0;
    Settings& st = Settings::Instance();
    const bool debug = st.debug_mode;
    static const char* script = getenv("MGS_TOOLS");
    if (script && debug && Get().valid) { const std::string s = script; script = nullptr; RunScript(s); }
    const bool health = debug && st.cheat_infinite_health, ammo = debug && st.cheat_infinite_ammo;
    if (!debug && !health_applied_ && !ammo_applied_) return;   // idle outside debug mode
    if (S3ERuntime::Instance().LuaPending()) return;
    const bool health_off = !health && health_applied_, ammo_off = !ammo && ammo_applied_;
    const bool bandana = Get().bandana;
    health_applied_ = health;
    ammo_applied_ = ammo;
    Request([=](LuaChunk& c) {
        if (health || health_off) c.Call("CharacterSetIsInvincible", {"Snake", health});
        if (ammo || ammo_off) c.Call("SetInfiniteAmmoEnabled", {ammo || bandana});
    }, [this, health](const Snapshot& s) {
        if (health && s.valid && s.health < s.max_health)
            Request([s](LuaChunk& c) { c.Call("CharacterChangeHealth", {"Snake", s.max_health - s.health}); });
    });
}
