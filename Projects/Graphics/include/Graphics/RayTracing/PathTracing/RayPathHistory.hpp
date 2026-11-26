/// @file    RayPathHistory.hpp
/// @brief   Path の実内容・カメラ・推定器契約で管理する Progressive サンプル履歴。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/View/Camera.hpp>
#include <array>
#include <cstdint>

namespace fbzz::renderer {

struct RayPathScene;

/// @note sample count / dispatch 当たりのサンプル数は推定器の意味を変えず、履歴キーに入れない。
struct RayPathIntegratorSettings {
    uint32_t maxBounces = 8; ///< パスを打ち切る散乱反復数の上限。
    uint32_t rouletteStartBounce = 3; ///< ロシアンルーレットを開始できる反復番号。
    uint32_t seed = 0; ///< サンプル系列を選ぶ疑似乱数シード。
    uint32_t samplerVersion = 1; ///< サンプル系列の意味が変わったときに履歴を切るための版。
    uint32_t integratorVersion = 3; ///< 積分器の意味が変わったときに履歴を切るための版。
    float maxDistance = 1000; ///< レイの距離上限を表す履歴キーの設定値。
    float radianceClamp = 0; ///< 履歴キーに含める放射輝度の制限設定。
    [[nodiscard]] FBZZ_GRAPHICS_API bool operator==(const RayPathIntegratorSettings& other) const;
};

/// @note cameraWords は jitter 前の姿勢・射影入力の bit pattern。Math の近似比較を使わない。
/// @note 露出・bloom・tonemap・UI・フレーム番号・AS 再配置を含めない。
struct RayPathHistoryKey {
    uint64_t sceneGeneration = 0; ///< シーンの入れ替えを区別する世代。
    uint64_t sceneContentRevision = 0; ///< 形状・材質・光源など、積分対象の実内容の版。
    uint64_t deviceEpoch = 0; ///< GPU 資源を作ったデバイスの世代。
    uint64_t shaderEpoch = 0; ///< 積分に使うシェーダーの世代。
    uint32_t width = 0; ///< 蓄積画像の幅 [px]。
    uint32_t height = 0; ///< 蓄積画像の高さ [px]。
    std::array<uint32_t, 13> cameraWords{}; ///< ジッターを含まないカメラ姿勢と射影入力のビット列。
    RayPathIntegratorSettings integrator; ///< 蓄積済みサンプルと混ぜてよい積分器の設定。
    bool operator==(const RayPathHistoryKey&) const = default;
};

[[nodiscard]] FBZZ_GRAPHICS_API RayPathHistoryKey MakeRayPathHistoryKey(
    const RayPathScene& scene, const Camera& camera, uint32_t width, uint32_t height,
    const RayPathIntegratorSettings& integrator, uint64_t deviceEpoch, uint64_t shaderEpoch);

/// @note 線形 HDR の生の和と対になるビュー専用履歴。失敗 dispatch は Commit せず再試行する。
/// @see https://pbr-book.org/4ed/Monte_Carlo_Integration PBRT, Monte Carlo sample average
class RayPathHistory {
public:
    /// @return 初回または exact key 変更で履歴をリセットしたとき true。
    [[nodiscard]] FBZZ_GRAPHICS_API bool Prepare(const RayPathHistoryKey& key);
    /// @return 未準備・0 サンプル・有効サンプル上限超過なら false。サンプル数は未変更。
    /// @note UINT32_MAX は GPU RAW の sticky error sentinel。正常な蓄積数の上限は UINT32_MAX-1。
    [[nodiscard]] FBZZ_GRAPHICS_API bool Commit(uint32_t sampleCount);
    [[nodiscard]] uint32_t GetSampleCount() const { return m_sampleCount; }
    [[nodiscard]] bool IsPrepared() const { return m_prepared; }
    FBZZ_GRAPHICS_API void Reset();

private:
    RayPathHistoryKey m_key; ///< 現在準備済みの蓄積を識別するキー。
    uint32_t m_sampleCount = 0; ///< 成功した dispatch を反映した蓄積サンプル数。
    bool m_prepared = false; ///< 現在のキーで蓄積を開始できるか。
};

} // namespace fbzz::renderer
