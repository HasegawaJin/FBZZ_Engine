/// @file    SystemScheduler.hpp
/// @brief   ISystem を Phase × DAG で管理し、フレーム内並列実行を調停するスケジューラ。
/// @author  Hasegawa Jin
/// @date    2026-06-18

/// @note Build() 後に Update() / LateUpdate() を呼ぶ。
#pragma once
#include <Engine/Api.hpp>
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
    /// @brief Phase の固定刻みと追いつき上限を設定する。Build 後の更新は次の実行から反映する。
    FBZZ_ENGINE_API void ConfigurePhase(Phase phase, PhaseConfig cfg);

    /// @brief デフォルトコンストラクタ可能な System を登録する。
    template<typename T>
    requires std::derived_from<T, ISystem> && std::default_initializable<T>
    void AddSystem() { AddSystemPtr(std::make_unique<T>()); }

    /// @brief コンストラクタ引数付き System を登録する。
    FBZZ_ENGINE_API void AddSystemPtr(std::unique_ptr<ISystem> sys);

    /// @brief 全登録後に一度呼ぶ。DAG 構築 + 循環依存チェック + OnInit() 呼び出し。
    /// @return 循環があれば false。System を初期化せず、Update も実行しない。
    [[nodiscard]] FBZZ_ENGINE_API bool Build();

    /// @brief PreScript から Cleanup までを進め、フレーム番号を更新する。
    FBZZ_ENGINE_API void Update(SystemContext ctx);

    /// @brief Update の残り時間から補間係数を求め、LateUpdate を実行する。
    FBZZ_ENGINE_API void LateUpdate(SystemContext ctx);

    /// @brief true にすると次の Update() で固定ステップを 1 回だけ実行し、自動で false に戻す。
    FBZZ_ENGINE_API void SetSingleStep(bool enabled);

    /// @brief シミュレーション停止時に accumulator をリセットする。
    FBZZ_ENGINE_API void ResetAccumulator();

    /// @brief 名前で System インスタンスを取得する (エディタ統合用)。
    /// @note RTTI による dynamic_cast は実行時コストと DLL 境界の型依存を生むため、公開 API では ISystem::Name() を使った明示的な検索に限定する。
    [[nodiscard]] FBZZ_ENGINE_API ISystem* FindSystem(std::string_view name) const;

    /// @brief Build が各 Phase 内に組んだ実行バッチをログに出力する。
    FBZZ_ENGINE_API void DumpGraph() const;

    FBZZ_ENGINE_API void Shutdown();

private:
    FBZZ_ENGINE_API void RunPhase(Phase p, SystemContext& ctx);

    std::vector<std::unique_ptr<ISystem>> m_systems; ///< スケジューラーが所有するシステムの登録順一覧。
    std::array<std::vector<std::vector<ISystem*>>, static_cast<size_t>(Phase::Count)> m_batches; ///< フェーズ別の実行順。各バッチが m_systems の非所有参照を持つ。
    std::array<PhaseConfig, static_cast<size_t>(Phase::Count)> m_phaseConfigs{}; ///< Phase の列挙値で引く固定刻み・追いつき上限の設定。
    float m_accumulator = 0.0f; ///< 固定ステップへまだ渡していない経過時間 [s]。
    bool m_singleStep = false; ///< 次の固定フェーズを蓄積時間から独立して一度だけ進める要求。
    bool m_built = false; ///< 依存関係の検証と全システムの初期化が完了しているか。
    int m_frameCounter = 0; ///< 更新間隔の判定に使う外側のフレーム番号。
};

} // namespace fbzz
