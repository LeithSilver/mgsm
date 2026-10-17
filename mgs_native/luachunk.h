#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The game's Lua 5.1 has no parser ("parser not loaded"), so host-side scripts are built as precompiled
// chunks (same header as the shipped .luc: int32 lua_Number). LuaChunk assembles straight-line code from
// a few statement forms, in order, then returns a list of values:
//
//     if not CharacterIsAlive("Snake") then return end      -- GuardSnakeAlive (every result is then nil)
//     fn(k1, k2, ...)                                        -- Call
//     name = k  /  name = fn(k...)                           -- Set / SetFromCall
//     t[k1] = k2                                             -- SetField
//     return name, fn(k...), k, ...                          -- Return*
//
// Arguments are constants (nil, bool, int32 number, string). S3ERuntime::RunLua loads and runs the result.
struct LuaValue {
    enum Type : uint8_t { Nil = 0, Bool = 1, Number = 3, String = 4 } type = Nil;
    int32_t n = 0;          // Bool: 0/1, Number: value
    std::string s;          // String

    LuaValue() = default;
    LuaValue(bool b) : type(Bool), n(b ? 1 : 0) {}
    LuaValue(int v) : type(Number), n(v) {}
    LuaValue(const char* v) : type(String), s(v) {}
    LuaValue(std::string v) : type(String), s(std::move(v)) {}
    bool IsNil() const { return type == Nil; }
    bool Truthy() const { return type != Nil && !(type == Bool && n == 0); }
    int Int(int fallback = 0) const { return type == Number ? n : fallback; }
};

class LuaChunk {
public:
    LuaChunk& GuardSnakeAlive();
    LuaChunk& Call(const std::string& fn, std::vector<LuaValue> args = {});
    LuaChunk& Set(const std::string& name, const LuaValue& v);
    LuaChunk& SetFromCall(const std::string& name, const std::string& fn, std::vector<LuaValue> args = {});
    LuaChunk& SetField(const std::string& table, const LuaValue& key, const LuaValue& v);
    LuaChunk& ReturnGlobal(const std::string& name);
    LuaChunk& ReturnCall(const std::string& fn, std::vector<LuaValue> args = {});
    LuaChunk& ReturnConst(const LuaValue& v);

    int Results() const { return static_cast<int>(returns_.size()); }
    bool Empty() const { return code_.empty() && returns_.empty(); }
    std::string Build() const;   // the binary chunk for luaL_loadbuffer

private:
    struct Ret { int kind; std::string name; std::vector<LuaValue> args; LuaValue k; };   // 0 global, 1 call, 2 const
    uint32_t K(const LuaValue& v);                     // constant index (deduplicated)
    void Load(uint32_t reg, const LuaValue& v);        // reg = v
    void LoadCall(uint32_t reg, const std::string& fn, const std::vector<LuaValue>& args, uint32_t nresults);
    void Use(uint32_t regs) { if (regs > max_stack_) max_stack_ = regs; }

    std::vector<uint32_t> code_;
    std::vector<LuaValue> consts_;
    std::vector<Ret> returns_;
    uint32_t max_stack_ = 2;
};
