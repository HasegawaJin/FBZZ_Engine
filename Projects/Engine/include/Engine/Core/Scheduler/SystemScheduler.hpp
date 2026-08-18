// FBZZ Engine
// SystemScheduler.hpp | fbzz
// ISystem を Phase × DAG で管理し、フレーム内並列実行を調停するスケジューラ。
// Build() 後に Update() / LateUpdate() を呼ぶ。
#pragma once
#include "ISystem.hpp"
#include "Phase.hpp"
#include "SystemContext.hpp"
#include <array>
#include <concepts>
#include <memory>
#include <string_view>
#include <vector>

namespace fbzz {

class SystemScheduler {
public:
    // Physics Phase など固定ステップを使う Phase の設定（Build() より前に呼ぶ）
    void ConfigurePhase(Phase phase, PhaseConfig cfg);

    // デフォルトコンストラクタ可能な System を登録
    template<typename T>
    requires std::derived_from<T, ISystem> && std::default_initializable<T>
    void AddSystem() { AddSystemPtr(std::make_unique<T>()); }

    // コンストラクタ引数付き System を登録
    void AddSystemPtr(std::unique_ptr<ISystem> sys);

    // 全登録後に一度呼ぶ。DAG 構築 + 循環依存チェック + OnInit() 呼び出し
    void Build();

    void Update    (SystemContext ctx);   // PreScript 〜 Cleanup
    void LateUpdate(SystemContext ctx);   // LateUpdate のみ

    // true にすると次の Update() で固定ステップを 1 回だけ実行し、自動で false に戻す
    void SetSingleStep(bool enabled);

    // シミュレーション停止時に accumulator をリセット
    void ResetAccumulator();

    // 名前で System インスタンスを取得（エディタ統合用）。
    // WHY: RTTI による dynamic_cast は実行時コストと DLL 境界の型依存を生むため、
    //      公開 API では ISystem::Name() を使った明示的な検索に限定する。
    [[nodiscard]] ISystem* FindSystem(std::string_view name) const;

    void DumpGraph() const;

    void Shutdown();

private:
    void RunPhase(Phase p, SystemContext& ctx);

    std::vector<std::unique_ptr<ISystem>> m_systems;
    std::array<std::vector<std::vector<ISystem*>>, static_cast<size_t>(Phase::Count)> m_batches;
    std::array<PhaseConfig, static_cast<size_t>(Phase::Count)>                        m_phaseConfigs{};
    float m_accumulator  = 0.0f;
    float m_physicsAlpha = 0.0f;
    bool  m_singleStep   = false;
    bool  m_built        = false;
    int   m_frameCounter = 0;
};

} // namespace fbzz
