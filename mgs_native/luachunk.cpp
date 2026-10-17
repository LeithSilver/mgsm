#include "luachunk.h"

namespace {
// Lua 5.1 opcodes and instruction layout: op 6 bits, A 8 (<<6), C 9 (<<14), B 9 (<<23); Bx 18 (<<14).
enum { MOVE, LOADK, LOADBOOL, LOADNIL, GETUPVAL, GETGLOBAL, GETTABLE, SETGLOBAL, SETUPVAL, SETTABLE,
       JMP = 22, TEST = 26, CALL = 28, RETURN = 30 };
uint32_t ABC(uint32_t op, uint32_t a, uint32_t b, uint32_t c) { return op | a << 6 | c << 14 | b << 23; }
uint32_t ABx(uint32_t op, uint32_t a, uint32_t bx) { return op | a << 6 | bx << 14; }
uint32_t Jmp(int offset) { return ABx(JMP, 0, static_cast<uint32_t>(offset + 131071)); }   // sBx
}

uint32_t LuaChunk::K(const LuaValue& v) {
    for (size_t i = 0; i < consts_.size(); ++i) {
        const LuaValue& c = consts_[i];
        if (c.type == v.type && c.n == v.n && c.s == v.s) return static_cast<uint32_t>(i);
    }
    consts_.push_back(v);
    return static_cast<uint32_t>(consts_.size() - 1);
}

void LuaChunk::Load(uint32_t reg, const LuaValue& v) {
    Use(reg + 1);
    if (v.type == LuaValue::Nil) code_.push_back(ABC(LOADNIL, reg, reg, 0));
    else if (v.type == LuaValue::Bool) code_.push_back(ABC(LOADBOOL, reg, v.n ? 1 : 0, 0));
    else code_.push_back(ABx(LOADK, reg, K(v)));
}

void LuaChunk::LoadCall(uint32_t reg, const std::string& fn, const std::vector<LuaValue>& args, uint32_t nresults) {
    code_.push_back(ABx(GETGLOBAL, reg, K(fn)));
    for (size_t i = 0; i < args.size(); ++i) Load(reg + 1 + static_cast<uint32_t>(i), args[i]);
    Use(reg + 1 + static_cast<uint32_t>(args.size()));
    code_.push_back(ABC(CALL, reg, static_cast<uint32_t>(args.size()) + 1, nresults + 1));
}

LuaChunk& LuaChunk::GuardSnakeAlive() {
    LoadCall(0, "CharacterIsAlive", {"Snake"}, 1);
    code_.push_back(ABC(TEST, 0, 0, 1));   // alive: take the jump below over the return
    code_.push_back(Jmp(1));
    code_.push_back(ABC(RETURN, 0, 1, 0));
    return *this;
}

LuaChunk& LuaChunk::Call(const std::string& fn, std::vector<LuaValue> args) {
    LoadCall(0, fn, args, 0);
    return *this;
}

LuaChunk& LuaChunk::Set(const std::string& name, const LuaValue& v) {
    Load(0, v);
    code_.push_back(ABx(SETGLOBAL, 0, K(name)));
    return *this;
}

LuaChunk& LuaChunk::SetFromCall(const std::string& name, const std::string& fn, std::vector<LuaValue> args) {
    LoadCall(0, fn, args, 1);
    code_.push_back(ABx(SETGLOBAL, 0, K(name)));
    return *this;
}

LuaChunk& LuaChunk::SetField(const std::string& table, const LuaValue& key, const LuaValue& v) {
    code_.push_back(ABx(GETGLOBAL, 0, K(table)));
    Load(1, key);
    Load(2, v);
    code_.push_back(ABC(SETTABLE, 0, 1, 2));
    return *this;
}

LuaChunk& LuaChunk::ReturnGlobal(const std::string& name) { returns_.push_back({0, name, {}, {}}); return *this; }
LuaChunk& LuaChunk::ReturnCall(const std::string& fn, std::vector<LuaValue> args) { returns_.push_back({1, fn, std::move(args), {}}); return *this; }
LuaChunk& LuaChunk::ReturnConst(const LuaValue& v) { returns_.push_back({2, "", {}, v}); return *this; }

std::string LuaChunk::Build() const {
    LuaChunk c = *this;   // the return list is emitted into a copy
    for (size_t i = 0; i < c.returns_.size(); ++i) {
        const Ret& r = c.returns_[i];
        const uint32_t reg = static_cast<uint32_t>(i);
        if (r.kind == 0) { c.Use(reg + 1); c.code_.push_back(ABx(GETGLOBAL, reg, c.K(r.name))); }
        else if (r.kind == 1) c.LoadCall(reg, r.name, r.args, 1);
        else c.Load(reg, r.k);
    }
    c.code_.push_back(ABC(RETURN, 0, static_cast<uint32_t>(c.returns_.size()) + 1, 0));

    std::string b("\x1bLuaQ\x00\x01\x04\x04\x04\x04\x01", 12);
    auto u8 = [&](uint8_t v) { b.push_back(static_cast<char>(v)); };
    auto i32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) u8(static_cast<uint8_t>(v >> (8 * k))); };
    auto str = [&](const std::string& s) { i32(static_cast<uint32_t>(s.size() + 1)); b += s; u8(0); };
    i32(0);                                        // source: none
    i32(0); i32(0);                                // line defined / last line
    u8(0); u8(0); u8(2);                           // upvalues, params, vararg
    u8(static_cast<uint8_t>(c.max_stack_ < 2 ? 2 : c.max_stack_));
    i32(static_cast<uint32_t>(c.code_.size()));
    for (uint32_t ins : c.code_) i32(ins);
    i32(static_cast<uint32_t>(c.consts_.size()));
    for (const LuaValue& k : c.consts_) {
        u8(k.type);
        if (k.type == LuaValue::Bool) u8(static_cast<uint8_t>(k.n));
        else if (k.type == LuaValue::Number) i32(static_cast<uint32_t>(k.n));
        else if (k.type == LuaValue::String) str(k.s);
    }
    i32(0);                                        // nested functions
    i32(0); i32(0); i32(0);                        // line info, locals, upvalue names
    return b;
}
