/// @file    BehaviorTreeEvaluator.cpp
/// @brief   木の評価。Scene に触るノードは IBTActionHandler へ委ねる。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// 評価モデル (Docs/design/game-ai-layer.md の D7): Running 中もルートから再入するが、
/// Composite は cursor[] に子スロットを持つので兄弟の再評価は起きず、根から Running リーフ
/// までの O(depth) で済む。Decorator の TimeLimit/Cooldown はリーフへの経路上にあるため、
/// リーフへ直接ジャンプすると評価がスキップされ時間切れが効かなくなる。
#include <Engine/AI/BehaviorTreeEvaluator.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::ai {

/// @name BTInstanceState

void BTInstanceState::Resize(std::size_t nodeCount)
{
    flags.assign(nodeCount, 0);
    cursor.assign(nodeCount, 0);
    timers.assign(nodeCount, 0.0f);
    cooldowns.assign(nodeCount, 0.0f);
    runningLeaf = kInvalidNode;
}

void BTInstanceState::Clear()
{
    std::fill(flags.begin(), flags.end(), static_cast<std::uint8_t>(0));
    std::fill(cursor.begin(), cursor.end(), static_cast<std::uint16_t>(0));
    std::fill(timers.begin(), timers.end(), 0.0f);
    std::fill(cooldowns.begin(), cooldowns.end(), 0.0f);
    runningLeaf = kInvalidNode;
}

void BTInstanceState::ClearRange(std::uint16_t lo, std::uint16_t hi)
{
    const std::size_t end = std::min<std::size_t>(hi, flags.size());
    for (std::size_t i = lo; i < end; ++i) {
        flags[i]  = 0;
        cursor[i] = 0;
        timers[i] = 0.0f;
        /// @note cooldowns は意図的に触らない (中断で消すと連打できてしまう)。
    }
}

namespace {

/// @brief 決定的な xorshift32。
/// @note エージェントごとに状態を持つ必要があり mt19937 は 2.5KB と重い。用途は「重み付き選択」
///       だけなので 4 バイトで足りる。
std::uint32_t NextRandom(std::uint32_t& state)
{
    std::uint32_t x = state ? state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

float RandomUnit(std::uint32_t& state)
{
    return static_cast<float>(NextRandom(state) >> 8) / static_cast<float>(1u << 24);
}

void RecordStatus(BTTickContext& ctx, std::uint16_t node, BTStatus status)
{
    if (!ctx.outNodeStatus) return;
    if (node >= ctx.outNodeStatus->size()) return;

    switch (status) {
    case BTStatus::Success: (*ctx.outNodeStatus)[node] = 1; break;
    case BTStatus::Failure: (*ctx.outNodeStatus)[node] = 2; break;
    case BTStatus::Running: (*ctx.outNodeStatus)[node] = 3; break;
    }
}

/// 比較演算を型ごとに適用する。
bool CompareValues(BTCompareOp op, float lhs, float rhs)
{
    switch (op) {
    case BTCompareOp::Equal:        return lhs == rhs;
    case BTCompareOp::NotEqual:     return lhs != rhs;
    case BTCompareOp::Less:         return lhs <  rhs;
    case BTCompareOp::LessEqual:    return lhs <= rhs;
    case BTCompareOp::Greater:      return lhs >  rhs;
    case BTCompareOp::GreaterEqual: return lhs >= rhs;
    default:                        return false;
    }
}

/// Blackboard の値と params の期待値を比較する。副作用なし。
bool EvaluateBlackboardCompare(const BTNodeParams& params, const Blackboard& blackboard)
{
    if (params.key == kInvalidBlackboardKey) return false;

    /// @note 「N 秒以内に書かれた値か」の時間条件。「最後にプレイヤーを見てから 5 秒経ったら
    ///       警戒を解く」という記憶の減衰を、各ノードが自前タイマーを持たずに書けるようにする。
    if (params.withinSeconds > 0.0f) {
        if (!blackboard.IsSet(params.key)) return false;
        const float elapsed = blackboard.GetTime() - blackboard.GetLastWriteTime(params.key);
        if (elapsed > params.withinSeconds) return false;
    }

    switch (params.valueType) {
    case BlackboardType::Bool: {
        bool value = false;
        if (!blackboard.GetBool(params.key, value)) return false;
        const bool equal = value == params.valueBool;
        return params.compareOp == BTCompareOp::NotEqual ? !equal : equal;
    }
    case BlackboardType::Int: {
        int value = 0;
        if (!blackboard.GetInt(params.key, value)) return false;
        return CompareValues(params.compareOp, static_cast<float>(value),
                             static_cast<float>(params.valueInt));
    }
    case BlackboardType::Float: {
        float value = 0.0f;
        if (!blackboard.GetFloat(params.key, value)) return false;
        return CompareValues(params.compareOp, value, params.valueFloat);
    }
    case BlackboardType::Vector3: {
        /// @note 距離での比較にする。座標の完全一致は浮動小数では実用にならない。
        math::Vector3 value = math::Vector3::ZERO;
        if (!blackboard.GetVector3(params.key, value)) return false;
        const math::Vector3 diff = value - params.valueVector3;
        return CompareValues(params.compareOp, diff.Length(), params.valueFloat);
    }
    case BlackboardType::Entity: {
        scene::EntityID value = scene::EntityID::INVALID;
        if (!blackboard.GetEntity(params.key, value)) return false;
        /// @note Entity は有効/無効の判定にのみ使う (== で比較したい相手が無い)。
        const bool valid = value.IsValid();
        return params.compareOp == BTCompareOp::NotEqual ? !valid : valid;
    }
    case BlackboardType::String: {
        std::string value;
        if (!blackboard.GetString(params.key, value)) return false;
        const bool equal = value == params.valueString;
        return params.compareOp == BTCompareOp::NotEqual ? !equal : equal;
    }
    default:
        return false;
    }
}

/// Wait / Cooldown / TimeLimit の実効時間。durationRandom でばらつかせる。
float ResolveDuration(const BTNodeParams& params, std::uint32_t& rngState)
{
    if (params.durationRandom <= 0.0f) return std::max(params.duration, 0.0f);
    const float offset = (RandomUnit(rngState) * 2.0f - 1.0f) * params.durationRandom;
    return std::max(params.duration + offset, 0.0f);
}

} // namespace

/// @name 条件評価 (副作用なし)

bool EvaluateBTCondition(const BehaviorTreeRuntime& tree, std::uint16_t node,
                         const Blackboard& blackboard, const BTTickContext& ctx)
{
    if (node >= tree.nodes.size()) return false;

    const BTNode& btNode = tree.nodes[node];
    const BTNodeParams& params = tree.params[btNode.paramIndex];

    switch (btNode.type) {
    case BTNodeType::AlwaysSucceed: return true;
    case BTNodeType::AlwaysFail:    return false;

    case BTNodeType::BlackboardCondition:
    case BTNodeType::BlackboardCompare:
        return EvaluateBlackboardCompare(params, blackboard);

    /// @note 知覚に依存する条件は ActionHandler へ委ねる (段階 3 以降で実装)。
    case BTNodeType::HasTarget:
    case BTNodeType::IsTargetInRange:
    case BTNodeType::IsHealthBelow:
    case BTNodeType::HasLineOfSight:
        return ctx.actions ? ctx.actions->EvaluateCondition(tree, node, blackboard, ctx) : false;

    default:
        return false;
    }
}

namespace {

/// 前方宣言 (Composite / Decorator が再帰する)
BTStatus TickNode(const BehaviorTreeRuntime& tree, std::uint16_t node,
                  BTInstanceState& state, Blackboard& blackboard, BTTickContext& ctx);

BTStatus TickComposite(const BehaviorTreeRuntime& tree, std::uint16_t node,
                       BTInstanceState& state, Blackboard& blackboard, BTTickContext& ctx)
{
    const BTNode& btNode = tree.nodes[node];
    const BTNodeParams& params = tree.params[btNode.paramIndex];

    const std::uint16_t first = btNode.firstChild;
    const std::uint16_t count = btNode.childCount;

    /// @note 子が無い Composite は「やることが無い」= 失敗。Success にすると空の Selector を
    ///       通過して先へ進み、未完成の枝が黙って成功扱いになりバグが隠れる。
    if (count == 0) return BTStatus::Failure;

    switch (btNode.type) {
    case BTNodeType::Sequence: {
        for (std::uint16_t i = state.cursor[node]; i < count; ++i) {
            const BTStatus status = TickNode(tree, static_cast<std::uint16_t>(first + i),
                                             state, blackboard, ctx);
            if (status == BTStatus::Running) {
                state.cursor[node] = i;
                state.flags[node] |= BTNodeFlag::Running;
                return BTStatus::Running;
            }
            if (status == BTStatus::Failure) {
                state.cursor[node] = 0;
                state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
                return BTStatus::Failure;
            }
        }
        state.cursor[node] = 0;
        state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
        return BTStatus::Success;
    }

    case BTNodeType::Selector: {
        for (std::uint16_t i = state.cursor[node]; i < count; ++i) {
            const BTStatus status = TickNode(tree, static_cast<std::uint16_t>(first + i),
                                             state, blackboard, ctx);
            if (status == BTStatus::Running) {
                state.cursor[node] = i;
                state.flags[node] |= BTNodeFlag::Running;
                return BTStatus::Running;
            }
            if (status == BTStatus::Success) {
                state.cursor[node] = 0;
                state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
                return BTStatus::Success;
            }
        }
        state.cursor[node] = 0;
        state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
        return BTStatus::Failure;
    }

    case BTNodeType::Parallel: {
        int succeeded = 0;
        int failed    = 0;

        for (std::uint16_t i = 0; i < count; ++i) {
            const auto child = static_cast<std::uint16_t>(first + i);

            /// @note 既に決着した子は再実行しない (結果は子の flags に保存してある)。
            if (state.flags[child] & BTNodeFlag::Done) {
                if (state.flags[child] & BTNodeFlag::DoneSuccess) ++succeeded;
                else                                              ++failed;
                continue;
            }

            const BTStatus status = TickNode(tree, child, state, blackboard, ctx);
            if (status == BTStatus::Running) continue;

            state.flags[child] |= BTNodeFlag::Done;
            if (status == BTStatus::Success) {
                state.flags[child] |= BTNodeFlag::DoneSuccess;
                ++succeeded;
            } else {
                ++failed;
            }
        }

        const bool requireAll = params.successPolicy == BTParallelPolicy::RequireAll;
        const bool finished = requireAll
            ? (failed > 0 || succeeded == count)
            : (succeeded > 0 || failed == count);

        if (!finished) {
            state.flags[node] |= BTNodeFlag::Running;
            return BTStatus::Running;
        }

        const bool success = requireAll ? (failed == 0) : (succeeded > 0);

        /// @note 決着したので子の状態を全部畳む (次回は最初から)。
        state.ClearRange(first, tree.subtreeEnd[node]);
        state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
        state.cursor[node] = 0;
        return success ? BTStatus::Success : BTStatus::Failure;
    }

    case BTNodeType::RandomSelector: {
        /// @note 実行中なら前回選んだ子を継続する。
        std::uint16_t chosen = state.cursor[node];
        if (!(state.flags[node] & BTNodeFlag::Running)) {
            /// @note 重み付き抽選。weights が空 / 合計 0 なら均等。
            float total = 0.0f;
            for (std::uint16_t i = 0; i < count; ++i) {
                const float w = i < params.childWeights.size()
                    ? std::max(params.childWeights[i], 0.0f) : 1.0f;
                total += w;
            }
            if (total <= 0.0f) return BTStatus::Failure;

            float roll = RandomUnit(state.rngState) * total;
            chosen = 0;
            for (std::uint16_t i = 0; i < count; ++i) {
                const float w = i < params.childWeights.size()
                    ? std::max(params.childWeights[i], 0.0f) : 1.0f;
                /// @note 重み 0 の子は roll がどうであれ選ばれない。
                if (w <= 0.0f) continue;
                roll -= w;
                if (roll <= 0.0f) { chosen = i; break; }
                chosen = i;
            }
            state.cursor[node] = chosen;
        }

        const BTStatus status = TickNode(tree, static_cast<std::uint16_t>(first + chosen),
                                         state, blackboard, ctx);
        if (status == BTStatus::Running) {
            state.flags[node] |= BTNodeFlag::Running;
            return BTStatus::Running;
        }
        state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
        state.cursor[node] = 0;
        return status;
    }

    default:
        return BTStatus::Failure;
    }
}

BTStatus TickDecorator(const BehaviorTreeRuntime& tree, std::uint16_t node,
                       BTInstanceState& state, Blackboard& blackboard, BTTickContext& ctx)
{
    const BTNode& btNode = tree.nodes[node];
    const BTNodeParams& params = tree.params[btNode.paramIndex];
    const bool hasChild = btNode.childCount > 0;
    const auto child = static_cast<std::uint16_t>(btNode.firstChild);

    switch (btNode.type) {
    case BTNodeType::Inverter: {
        if (!hasChild) return BTStatus::Failure;
        const BTStatus status = TickNode(tree, child, state, blackboard, ctx);
        if (status == BTStatus::Running) return BTStatus::Running;
        return status == BTStatus::Success ? BTStatus::Failure : BTStatus::Success;
    }

    case BTNodeType::Succeeder: {
        if (!hasChild) return BTStatus::Success;
        const BTStatus status = TickNode(tree, child, state, blackboard, ctx);
        return status == BTStatus::Running ? BTStatus::Running : BTStatus::Success;
    }

    case BTNodeType::Repeat: {
        if (!hasChild) return BTStatus::Failure;
        const BTStatus status = TickNode(tree, child, state, blackboard, ctx);
        if (status == BTStatus::Running) {
            state.flags[node] |= BTNodeFlag::Running;
            return BTStatus::Running;
        }
        if (params.repeatUntilFailure && status == BTStatus::Failure) {
            state.cursor[node] = 0;
            state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
            return BTStatus::Success;
        }

        ++state.cursor[node];
        /// @note repeatCount = 0 は無限ループ。永久に Running を返し続ける。
        if (params.repeatCount > 0 && state.cursor[node] >= params.repeatCount) {
            state.cursor[node] = 0;
            state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
            return BTStatus::Success;
        }
        state.flags[node] |= BTNodeFlag::Running;
        return BTStatus::Running;
    }

    case BTNodeType::Cooldown: {
        if (!hasChild) return BTStatus::Failure;
        if (state.cooldowns[node] > 0.0f) {
            state.cooldowns[node] -= ctx.dt;
            return BTStatus::Failure;
        }
        const BTStatus status = TickNode(tree, child, state, blackboard, ctx);
        if (status == BTStatus::Running) {
            state.flags[node] |= BTNodeFlag::Running;
            return BTStatus::Running;
        }
        /// @note 子が決着した時点でクールダウンを開始する。
        state.cooldowns[node] = ResolveDuration(params, state.rngState);
        state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
        return status;
    }

    case BTNodeType::BlackboardCondition: {
        if (!EvaluateBlackboardCompare(params, blackboard)) {
            /// @note 条件が偽なら子には入らない。実行中だった場合は畳む。
            if (state.flags[node] & BTNodeFlag::Running)
                state.ClearRange(node, tree.subtreeEnd[node]);
            return BTStatus::Failure;
        }
        if (!hasChild) return BTStatus::Success;

        const BTStatus status = TickNode(tree, child, state, blackboard, ctx);
        if (status == BTStatus::Running) state.flags[node] |= BTNodeFlag::Running;
        else state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
        return status;
    }

    case BTNodeType::TimeLimit: {
        if (!hasChild) return BTStatus::Failure;

        state.timers[node] += ctx.dt;
        const float limit = std::max(params.duration, 0.0f);
        if (limit > 0.0f && state.timers[node] > limit) {
            /// @note 時間切れ。子のサブツリーを畳んで Failure。
            state.ClearRange(node, tree.subtreeEnd[node]);
            return BTStatus::Failure;
        }

        const BTStatus status = TickNode(tree, child, state, blackboard, ctx);
        if (status == BTStatus::Running) {
            state.flags[node] |= BTNodeFlag::Running;
            return BTStatus::Running;
        }
        state.timers[node] = 0.0f;
        state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Running);
        return status;
    }

    default:
        return BTStatus::Failure;
    }
}

BTStatus TickLeaf(const BehaviorTreeRuntime& tree, std::uint16_t node,
                  BTInstanceState& state, Blackboard& blackboard, BTTickContext& ctx)
{
    const BTNode& btNode = tree.nodes[node];
    const BTNodeParams& params = tree.params[btNode.paramIndex];

    switch (btNode.type) {
    case BTNodeType::AlwaysSucceed: return BTStatus::Success;
    case BTNodeType::AlwaysFail:    return BTStatus::Failure;
    case BTNodeType::AlwaysRunning:
        state.runningLeaf = node;
        return BTStatus::Running;

    case BTNodeType::Wait: {
        /// @note 初回入場時に実効時間を決めて timers へ「残り時間」として積む。
        if (!(state.flags[node] & BTNodeFlag::Entered)) {
            state.flags[node] |= BTNodeFlag::Entered;
            state.timers[node] = ResolveDuration(params, state.rngState);
        }
        state.timers[node] -= ctx.dt;
        if (state.timers[node] > 0.0f) {
            state.runningLeaf = node;
            return BTStatus::Running;
        }
        state.flags[node] &= static_cast<std::uint8_t>(~BTNodeFlag::Entered);
        state.timers[node] = 0.0f;
        return BTStatus::Success;
    }

    case BTNodeType::SetBlackboard: {
        if (params.key == kInvalidBlackboardKey) return BTStatus::Failure;
        bool ok = false;
        switch (params.valueType) {
        case BlackboardType::Bool:    ok = blackboard.SetBool   (params.key, params.valueBool);    break;
        case BlackboardType::Int:     ok = blackboard.SetInt    (params.key, params.valueInt);     break;
        case BlackboardType::Float:   ok = blackboard.SetFloat  (params.key, params.valueFloat);   break;
        case BlackboardType::Vector3: ok = blackboard.SetVector3(params.key, params.valueVector3); break;
        case BlackboardType::String:  ok = blackboard.SetString (params.key, params.valueString);  break;
        /// @note 定数の Entity は表現できない
        case BlackboardType::Entity:  ok = false; break;
        default: ok = false; break;
        }
        return ok ? BTStatus::Success : BTStatus::Failure;
    }

    case BTNodeType::BlackboardCompare:
        return EvaluateBlackboardCompare(params, blackboard)
            ? BTStatus::Success : BTStatus::Failure;

    /// @name 知覚に依存する条件
    case BTNodeType::HasTarget:
    case BTNodeType::IsTargetInRange:
    case BTNodeType::IsHealthBelow:
    case BTNodeType::HasLineOfSight:
        return EvaluateBTCondition(tree, node, blackboard, ctx)
            ? BTStatus::Success : BTStatus::Failure;

    /// @name Scene に触るアクション
    /// @note 段階 1 では actions が null なので Failure。段階 3 で実装が差される。
    case BTNodeType::MoveTo:
    case BTNodeType::Patrol:
    case BTNodeType::LookAt:
    case BTNodeType::PlayAnimation:
    case BTNodeType::PlayAudio:
    case BTNodeType::RunScript: {
        if (!ctx.actions) return BTStatus::Failure;
        const BTStatus status = ctx.actions->Execute(tree, node, state, blackboard, ctx);
        if (status == BTStatus::Running) state.runningLeaf = node;
        return status;
    }

    default:
        return BTStatus::Failure;
    }
}

BTStatus TickNode(const BehaviorTreeRuntime& tree, std::uint16_t node,
                  BTInstanceState& state, Blackboard& blackboard, BTTickContext& ctx)
{
    if (node >= tree.nodes.size()) return BTStatus::Failure;

    const BTNodeType type = tree.nodes[node].type;
    BTStatus status;

    if (BTNodeIsComposite(type))      status = TickComposite(tree, node, state, blackboard, ctx);
    else if (BTNodeIsDecorator(type)) status = TickDecorator(tree, node, state, blackboard, ctx);
    else                              status = TickLeaf(tree, node, state, blackboard, ctx);

    RecordStatus(ctx, node, status);
    return status;
}

} // namespace

BTStatus TickBehaviorTree(const BehaviorTreeRuntime& tree, BTInstanceState& state,
                          Blackboard& blackboard, BTTickContext& ctx)
{
    if (tree.nodes.empty()) return BTStatus::Failure;

    /// @note 木の差し替え後などでサイズが合っていなければ張り直す。
    if (state.flags.size() != tree.nodes.size()) state.Resize(tree.nodes.size());

    if (ctx.outNodeStatus) {
        if (ctx.outNodeStatus->size() != tree.nodes.size())
            ctx.outNodeStatus->assign(tree.nodes.size(), 0);
        else
            std::fill(ctx.outNodeStatus->begin(), ctx.outNodeStatus->end(),
                      static_cast<std::uint8_t>(0));
    }

    /// @note Running リーフは毎 tick で再確定させる。
    ///       (このリセットが無いと、中断で消えたリーフが残り続ける)
    const std::uint16_t previousLeaf = state.runningLeaf;
    state.runningLeaf = kInvalidNode;
    (void)previousLeaf;

    const BTStatus status = TickNode(tree, 0, state, blackboard, ctx);

    /// @note ルートが決着したら全状態を畳んで、次回は最初から始める。
    if (status != BTStatus::Running) {
        state.runningLeaf = kInvalidNode;
        /// @note cooldowns は保持する (木が一巡しただけでクールダウンが消えるのは誤り)。
        for (std::size_t i = 0; i < state.flags.size(); ++i) {
            state.flags[i]  = 0;
            state.cursor[i] = 0;
            state.timers[i] = 0.0f;
        }
    }

    return status;
}

} // namespace fbzz::ai
