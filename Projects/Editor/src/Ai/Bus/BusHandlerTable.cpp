/// @file    BusHandlerTable.cpp
/// @brief   Command Bus の型名 → ハンドラー表。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <cassert>

namespace fbzz::editor::ai::bus {

void BusHandlerTable::Add(BusHandlerEntry entry)
{
    /// @note 同じ型の二重登録は «後から登録した方が黙って勝つ» 事故になるので開発中に止める。
    assert(Find(entry.type) == nullptr && "Command Bus の型が二重登録されています");
    m_entries.push_back(std::move(entry));
}

void BusHandlerTable::AddQuery(std::string type, CommandFn handler)
{
    BusHandlerEntry entry;
    entry.type = std::move(type);
    entry.kind = BusKind::QUERY;
    entry.handler = handler;
    Add(std::move(entry));
}

void BusHandlerTable::AddCommand(std::string type, CommandFn handler, BuilderFn transactionBuilder)
{
    BusHandlerEntry entry;
    entry.type = std::move(type);
    entry.kind = BusKind::COMMAND;
    entry.handler = handler;
    entry.builder = transactionBuilder;
    Add(std::move(entry));
}

void BusHandlerTable::AddBuilder(std::string type, BuilderFn builder, bool returnsCreatedId)
{
    BusHandlerEntry entry;
    entry.type = std::move(type);
    entry.kind = BusKind::BUILDER;
    entry.builder = builder;
    entry.returnsCreatedId = returnsCreatedId;
    Add(std::move(entry));
}

const BusHandlerEntry* BusHandlerTable::Find(std::string_view type) const
{
    for (const BusHandlerEntry& entry : m_entries) {
        if (entry.type == type) return &entry;
    }
    return nullptr;
}

} // namespace fbzz::editor::ai::bus
