/// @file    ScriptModules.hpp
/// @brief   親が所有する内部 Script のライフサイクルと反射をまとめる。
/// @author  Hasegawa Jin
/// @date    2026-09-28
#pragma once

#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <vector>

namespace fbzz::scene {

/// @brief 通知対象のフェーズと順番。未指定の更新フェーズには参加しない。
struct ScriptModule {
    explicit ScriptModule(Script& value) : script(&value) {}

    ScriptModule& Update(int order = 0) { update = true; updateOrder = order; return *this; }
    ScriptModule& LateUpdate(int order = 0) { late = true; lateOrder = order; return *this; }
    ScriptModule& FixedUpdate(int order = 0) { fixed = true; fixedOrder = order; return *this; }

    Script* script;
    bool update = false;
    bool late = false;
    bool fixed = false;
    int updateOrder = 0;
    int lateOrder = 0;
    int fixedOrder = 0;
};

/// @brief アタッチを増やさず、メンバーとして持った Script を駆動する。
/// @pre 親と登録した Script はこのオブジェクトより長く生存し、アドレスを変えない。
/// @note 親の対応するコールバックから各メソッドを一度ずつ呼ぶ。所有権とゲーム固有の依存注入は親に残す。
/// @see Docs/design/script-modules.md
class ScriptModules {
public:
    ScriptModules(Script& owner, std::initializer_list<ScriptModule> modules) : m_owner(owner)
    {
        for (const auto& module : modules) {
            const bool duplicate = std::any_of(m_modules.begin(), m_modules.end(),
                [&](const ScriptModule& entry) { return entry.script == module.script; });
            assert(module.script && module.script != &owner && !duplicate);
            if (module.script && module.script != &owner && !duplicate) m_modules.push_back(module);
        }
        BuildOrder(m_update, &ScriptModule::update, &ScriptModule::updateOrder);
        BuildOrder(m_late, &ScriptModule::late, &ScriptModule::lateOrder);
        BuildOrder(m_fixed, &ScriptModule::fixed, &ScriptModule::fixedOrder);
    }

    ScriptModules(const ScriptModules&) = delete;
    ScriptModules& operator=(const ScriptModules&) = delete;

    void Bind()
    {
        for (const auto& entry : m_modules) entry.script->AdoptContext(m_owner);
    }

    void Start()
    {
        if (m_started || m_destroying) return;
        Bind();
        m_started = true;
        for (const auto& entry : m_modules) {
            auto& module = *entry.script;
            if (m_hasStarted) module.ResetLifecycleState();
            module.ExecuteCallback(&Script::OnAwake, "OnAwake");
            module.SynchronizeEnabledState(m_owner.scene.IsActiveAndEnabled());
            if (auto* object = m_owner.scene.Self()) {
                std::vector<ScriptRequirementIssue> issues;
                CollectScriptRequirementIssues(*object, module, issues);
                for (const auto& issue : issues)
                    FBZZ_LOG_ERROR("Script module requirement: %s", FormatScriptRequirementIssue(issue).c_str());
            }
            module.ExecuteCallback(&Script::OnStart, "OnStart");
        }
        m_hasStarted = true;
    }

    void Enable() { Synchronize(); }

    void Disable()
    {
        if (!m_started) return;
        Bind();
        for (const auto& entry : m_modules) entry.script->SynchronizeEnabledState(false);
    }

    void Update(float dt)
    {
        if (!Synchronize()) return;
        for (const auto& entry : m_modules) {
            auto& module = *entry.script;
            if (!module.enabled || !m_owner.scene.IsActiveAndEnabled()) continue;
            module.UpdateFrameDelays();
            if (module.scene.IsActiveAndEnabled()) module.UpdateInvocations(dt);
            if (module.scene.IsActiveAndEnabled()) module.UpdateCoroutines();
        }
        Dispatch(m_update, &Script::OnUpdate, "OnUpdate");
    }

    void LateUpdate()
    {
        if (Synchronize()) Dispatch(m_late, &Script::OnLateUpdate, "OnLateUpdate");
    }

    void FixedUpdate()
    {
        if (Synchronize()) Dispatch(m_fixed, &Script::OnFixedUpdate, "OnFixedUpdate");
    }

    void Destroy()
    {
        if (!m_started || m_destroying) return;
        m_destroying = true;
        Bind();
        for (const auto& entry : m_modules) {
            auto& module = *entry.script;
            module.ExecuteCallback(&Script::OnDestroy, "OnDestroy");
            module.CancelInvoke();
            module.StopAllCoroutines();
            module.CancelEventSubscriptions();
            module.SetContext(nullptr, nullptr);
        }
        m_started = false;
        m_destroying = false;
    }

    /// @note Editor の選択表示でも必要なため、Start 前にも使える。
    void DrawGizmos()
    {
        Bind();
        for (const auto& entry : m_modules)
            entry.script->ExecuteCallback(&Script::OnDrawGizmos, "OnDrawGizmos");
    }

    /// @note 既存シーンの保存キーを保つため平坦に展開する。親・子のフィールド名は一意にする。
    void Reflect(IReflector& reflector)
    {
        for (const auto& entry : m_modules) entry.script->Reflect(reflector);
    }

    /// @return 未登録または複数一致なら nullptr。返す参照の寿命は親のメンバーと同じ。
    template<typename T>
    [[nodiscard]] T* Get() const
    {
        T* result = nullptr;
        for (const auto& entry : m_modules) {
            if (void* match = entry.script->FbzzAsType(T::TYPE_NAME)) {
                if (result) return nullptr;
                result = static_cast<T*>(match);
            }
        }
        return result;
    }

private:
    void BuildOrder(std::vector<Script*>& target, bool ScriptModule::*phase, int ScriptModule::*order)
    {
        std::vector<ScriptModule> sorted = m_modules;
        std::stable_sort(sorted.begin(), sorted.end(),
            [order](const ScriptModule& a, const ScriptModule& b) { return a.*order < b.*order; });
        for (const auto& entry : sorted)
            if (entry.*phase) target.push_back(entry.script);
    }

    bool Synchronize()
    {
        if (!m_started || m_destroying) return false;
        Bind();
        const bool active = m_owner.scene.IsActiveAndEnabled();
        for (const auto& entry : m_modules) entry.script->SynchronizeEnabledState(active);
        return active;
    }

    void Dispatch(const std::vector<Script*>& order, void (Script::*callback)(), const char* name)
    {
        for (auto* module : order) {
            const bool active = m_owner.scene.IsActiveAndEnabled();
            module->SynchronizeEnabledState(active);
            if (active && module->enabled) module->ExecuteCallback(callback, name);
        }
    }

    Script& m_owner;
    std::vector<ScriptModule> m_modules;
    std::vector<Script*> m_update;
    std::vector<Script*> m_late;
    std::vector<Script*> m_fixed;
    bool m_started = false;
    bool m_hasStarted = false;
    bool m_destroying = false;
};

/// @brief 親の自動フィールド反射と登録モジュールの反射を一つの入口にする。
/// @note FBZZ_REFLECT の代わりにクラス直後へ置く。親の新しい FBZZ_FIELD を手動列挙から漏らさない。
#define FBZZ_REFLECT_MODULES(T, Modules)                                         \
    inline void T::Reflect(::fbzz::scene::IReflector& r_) {                      \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<                         \
                          (__COUNTER__ - T::_fbzz_base - 1)>{}, r_);             \
        Modules.Reflect(r_);                                                    \
    }

} /// @note namespace fbzz::scene
