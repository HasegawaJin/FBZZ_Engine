/// @file    Blackboard.cpp
/// @brief   型付き共有データ領域の実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/AI/Blackboard.hpp>

namespace fbzz::ai {

const std::vector<BlackboardDef>& ReservedBlackboardDefs()
{
    // bb:: の添字と 1 対 1 で対応させること。順序を変えると固定添字が壊れる。
    static const std::vector<BlackboardDef> kDefs = [] {
        std::vector<BlackboardDef> defs;
        const auto add = [&defs](const char* name, BlackboardType type) {
            BlackboardDef def;
            def.name     = name;
            def.type     = type;
            def.reserved = true;
            defs.push_back(std::move(def));
        };
        add("Self",              BlackboardType::Entity);   // bb::Self              = 0
        add("TargetEntity",      BlackboardType::Entity);   // bb::TargetEntity      = 1
        add("TargetPosition",    BlackboardType::Vector3);  // bb::TargetPosition    = 2
        add("LastKnownPosition", BlackboardType::Vector3);  // bb::LastKnownPosition = 3
        add("HasTarget",         BlackboardType::Bool);     // bb::HasTarget         = 4
        add("HomePosition",      BlackboardType::Vector3);  // bb::HomePosition      = 5
        add("Health01",          BlackboardType::Float);    // bb::Health01          = 6
        add("Awareness",         BlackboardType::Float);    // bb::Awareness         = 7
        add("NoisePosition",     BlackboardType::Vector3);  // bb::NoisePosition     = 8
        return defs;
    }();

    static_assert(bb::ReservedCount == 9, "bb:: の添字と ReservedBlackboardDefs を同期させること");
    return kDefs;
}

void Blackboard::Reset(const std::vector<BlackboardDef>& defs)
{
    m_entries.clear();
    m_entries.resize(defs.size());
    m_tick = 0;
    m_time = 0.0f;

    for (std::size_t i = 0; i < defs.size(); ++i) {
        const BlackboardDef& def = defs[i];
        Entry& entry = m_entries[i];

        entry.type          = def.type;
        entry.written       = false;
        entry.lastWriteTick = 0;
        entry.lastWriteTime = 0.0f;

        // 既定値は「書き込み」として扱わない。
        // WHY: IsSet() / GetLastWriteTick() は「実行中に誰かが書いたか」を
        //      問うためのもの。既定値で埋めた時点で書かれた扱いにすると、
        //      「一度もターゲットを見ていない」を判定できなくなる。
        entry.b = def.defaultBool;
        entry.i = def.defaultInt;
        entry.f = def.defaultFloat;
        entry.v = def.defaultVector3;
        entry.s = def.defaultString;
        entry.e = scene::EntityID::INVALID;
    }
}

Blackboard::Entry* Blackboard::Acquire(BlackboardKey key, BlackboardType expected)
{
    if (key >= m_entries.size()) return nullptr;
    Entry& entry = m_entries[key];
    if (entry.type != expected) return nullptr;

    entry.written       = true;
    entry.lastWriteTick = m_tick;
    entry.lastWriteTime = m_time;
    return &entry;
}

const Blackboard::Entry* Blackboard::Peek(BlackboardKey key, BlackboardType expected) const
{
    if (key >= m_entries.size()) return nullptr;
    const Entry& entry = m_entries[key];
    if (entry.type != expected) return nullptr;
    return &entry;
}

// ── 書き込み ─────────────────────────────────────────────────────────────────

bool Blackboard::SetBool(BlackboardKey key, bool value)
{
    Entry* entry = Acquire(key, BlackboardType::Bool);
    if (!entry) return false;
    entry->b = value;
    return true;
}

bool Blackboard::SetInt(BlackboardKey key, int value)
{
    Entry* entry = Acquire(key, BlackboardType::Int);
    if (!entry) return false;
    entry->i = value;
    return true;
}

bool Blackboard::SetFloat(BlackboardKey key, float value)
{
    Entry* entry = Acquire(key, BlackboardType::Float);
    if (!entry) return false;
    entry->f = value;
    return true;
}

bool Blackboard::SetVector3(BlackboardKey key, const math::Vector3& value)
{
    Entry* entry = Acquire(key, BlackboardType::Vector3);
    if (!entry) return false;
    entry->v = value;
    return true;
}

bool Blackboard::SetEntity(BlackboardKey key, scene::EntityID value)
{
    Entry* entry = Acquire(key, BlackboardType::Entity);
    if (!entry) return false;
    entry->e = value;
    return true;
}

bool Blackboard::SetString(BlackboardKey key, std::string_view value)
{
    Entry* entry = Acquire(key, BlackboardType::String);
    if (!entry) return false;
    entry->s.assign(value);
    return true;
}

// ── 読み出し ─────────────────────────────────────────────────────────────────

bool Blackboard::GetBool(BlackboardKey key, bool& out) const
{
    const Entry* entry = Peek(key, BlackboardType::Bool);
    if (!entry) return false;
    out = entry->b;
    return true;
}

bool Blackboard::GetInt(BlackboardKey key, int& out) const
{
    const Entry* entry = Peek(key, BlackboardType::Int);
    if (!entry) return false;
    out = entry->i;
    return true;
}

bool Blackboard::GetFloat(BlackboardKey key, float& out) const
{
    const Entry* entry = Peek(key, BlackboardType::Float);
    if (!entry) return false;
    out = entry->f;
    return true;
}

bool Blackboard::GetVector3(BlackboardKey key, math::Vector3& out) const
{
    const Entry* entry = Peek(key, BlackboardType::Vector3);
    if (!entry) return false;
    out = entry->v;
    return true;
}

bool Blackboard::GetEntity(BlackboardKey key, scene::EntityID& out) const
{
    const Entry* entry = Peek(key, BlackboardType::Entity);
    if (!entry) return false;
    out = entry->e;
    return true;
}

bool Blackboard::GetString(BlackboardKey key, std::string& out) const
{
    const Entry* entry = Peek(key, BlackboardType::String);
    if (!entry) return false;
    out = entry->s;
    return true;
}

// ── 書き込み履歴 ─────────────────────────────────────────────────────────────

std::uint32_t Blackboard::GetLastWriteTick(BlackboardKey key) const
{
    if (key >= m_entries.size()) return 0;
    return m_entries[key].lastWriteTick;
}

float Blackboard::GetLastWriteTime(BlackboardKey key) const
{
    if (key >= m_entries.size()) return 0.0f;
    return m_entries[key].lastWriteTime;
}

bool Blackboard::IsSet(BlackboardKey key) const
{
    if (key >= m_entries.size()) return false;
    return m_entries[key].written;
}

BlackboardType Blackboard::TypeOf(BlackboardKey key) const
{
    if (key >= m_entries.size()) return BlackboardType::Bool;
    return m_entries[key].type;
}

} // namespace fbzz::ai
