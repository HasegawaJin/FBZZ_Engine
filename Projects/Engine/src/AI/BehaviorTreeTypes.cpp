/// @file    BehaviorTreeTypes.cpp
/// @brief   ノード種別の分類と表示名。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/AI/BehaviorTreeTypes.hpp>

namespace fbzz::ai {

bool BTNodeIsComposite(BTNodeType type) noexcept
{
    switch (type) {
    case BTNodeType::Sequence:
    case BTNodeType::Selector:
    case BTNodeType::Parallel:
    case BTNodeType::RandomSelector:
        return true;
    default:
        return false;
    }
}

bool BTNodeIsDecorator(BTNodeType type) noexcept
{
    switch (type) {
    case BTNodeType::Inverter:
    case BTNodeType::Succeeder:
    case BTNodeType::Repeat:
    case BTNodeType::Cooldown:
    case BTNodeType::BlackboardCondition:
    case BTNodeType::TimeLimit:
        return true;
    default:
        return false;
    }
}

bool BTNodeIsLeaf(BTNodeType type) noexcept
{
    if (type >= BTNodeType::Count) return false;
    return !BTNodeIsComposite(type) && !BTNodeIsDecorator(type);
}

bool BTNodeIsPureCondition(BTNodeType type) noexcept
{
    switch (type) {
    /// @note Blackboard と知覚結果を読むだけで、何も書き換えない。
    case BTNodeType::BlackboardCondition:
    case BTNodeType::BlackboardCompare:
    case BTNodeType::HasTarget:
    case BTNodeType::IsTargetInRange:
    case BTNodeType::IsHealthBelow:
    case BTNodeType::HasLineOfSight:
        return true;

    /// @note AlwaysSucceed/AlwaysFail は状態を読み書きしないため純粋条件に含める。
    ///       AlwaysRunning は「実行中である」こと自体が状態なので除外する。
    case BTNodeType::AlwaysSucceed:
    case BTNodeType::AlwaysFail:
        return true;

    default:
        return false;
    }
}

const char* BTNodeTypeName(BTNodeType type) noexcept
{
    switch (type) {
    case BTNodeType::Sequence:            return "Sequence";
    case BTNodeType::Selector:            return "Selector";
    case BTNodeType::Parallel:            return "Parallel";
    case BTNodeType::RandomSelector:      return "Random Selector";
    case BTNodeType::Inverter:            return "Inverter";
    case BTNodeType::Succeeder:           return "Succeeder";
    case BTNodeType::Repeat:              return "Repeat";
    case BTNodeType::Cooldown:            return "Cooldown";
    case BTNodeType::BlackboardCondition: return "Blackboard Condition";
    case BTNodeType::TimeLimit:           return "Time Limit";
    case BTNodeType::MoveTo:              return "Move To";
    case BTNodeType::Patrol:              return "Patrol";
    case BTNodeType::Wait:                return "Wait";
    case BTNodeType::LookAt:              return "Look At";
    case BTNodeType::PlayAnimation:       return "Play Animation";
    case BTNodeType::PlayAudio:           return "Play Sound";
    case BTNodeType::SetBlackboard:       return "Set Blackboard";
    case BTNodeType::RunScript:           return "Run Script";
    case BTNodeType::HasTarget:           return "Has Target";
    case BTNodeType::IsTargetInRange:     return "Is Target In Range";
    case BTNodeType::IsHealthBelow:       return "Is Health Below";
    case BTNodeType::BlackboardCompare:   return "Blackboard Compare";
    case BTNodeType::HasLineOfSight:      return "Has Line Of Sight";
    case BTNodeType::AlwaysSucceed:       return "Always Succeed";
    case BTNodeType::AlwaysFail:          return "Always Fail";
    case BTNodeType::AlwaysRunning:       return "Always Running";
    default:                              return "Unknown";
    }
}

int BTNodeMaxChildren(BTNodeType type) noexcept
{
    /// @note 無制限
    if (BTNodeIsComposite(type)) return -1;
    if (BTNodeIsDecorator(type)) return 1;
    /// @note リーフ
    return 0;
}

const char* BTStatusName(BTStatus status) noexcept
{
    switch (status) {
    case BTStatus::Success: return "Success";
    case BTStatus::Failure: return "Failure";
    case BTStatus::Running: return "Running";
    default:                return "Unknown";
    }
}

const char* BlackboardTypeName(BlackboardType type) noexcept
{
    switch (type) {
    case BlackboardType::Bool:    return "Bool";
    case BlackboardType::Int:     return "Int";
    case BlackboardType::Float:   return "Float";
    case BlackboardType::Vector3: return "Vector3";
    case BlackboardType::Entity:  return "Entity";
    case BlackboardType::String:  return "String";
    default:                      return "Unknown";
    }
}

const char* BTCompareOpName(BTCompareOp op) noexcept
{
    switch (op) {
    case BTCompareOp::Equal:        return "==";
    case BTCompareOp::NotEqual:     return "!=";
    case BTCompareOp::Less:         return "<";
    case BTCompareOp::LessEqual:    return "<=";
    case BTCompareOp::Greater:      return ">";
    case BTCompareOp::GreaterEqual: return ">=";
    default:                        return "?";
    }
}

} // namespace fbzz::ai
