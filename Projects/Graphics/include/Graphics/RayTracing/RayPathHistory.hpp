/// @file    RayPathHistory.hpp
/// @brief   Path の実内容・カメラ・推定器契約で管理する Progressive サンプル履歴。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Renderer/Camera.hpp>
#include <array>
#include <cstdint>

namespace fbzz::renderer {

struct RayPathScene;

/// @note sample count / dispatch 当たりのサンプル数は推定器の意味を変えず、履歴キーに入れない。
struct RayPathIntegratorSettings {
    uint32_t maxBounces = 8;
    uint32_t rouletteStartBounce = 3;
    uint32_t seed = 0;
    uint32_t samplerVersion = 1;
    uint32_t integratorVersion = 3;
    float maxDistance = 1000;
    float radianceClamp = 0;
    [[nodiscard]] bool operator==(const RayPathIntegratorSettings& other) const;
};

/// @note cameraWords は jitter 前の姿勢・射影入力の bit pattern。Math の近似比較を使わない。
/// @note 露出・bloom・tonemap・UI・フレーム番号・AS 再配置を含めない。
struct RayPathHistoryKey {
    uint64_t sceneGeneration = 0;
    uint64_t sceneContentRevision = 0;
    uint64_t deviceEpoch = 0;
    uint64_t shaderEpoch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::array<uint32_t, 13> cameraWords{};
    RayPathIntegratorSettings integrator;
    bool operator==(const RayPathHistoryKey&) const = default;
};

[[nodiscard]] RayPathHistoryKey MakeRayPathHistoryKey(
    const RayPathScene& scene, const Camera& camera, uint32_t width, uint32_t height,
    const RayPathIntegratorSettings& integrator, uint64_t deviceEpoch, uint64_t shaderEpoch);

/// @note 線形 HDR の生の和と対になるビュー専用履歴。失敗 dispatch は Commit せず再試行する。
/// @see https://pbr-book.org/4ed/Monte_Carlo_Integration PBRT, Monte Carlo sample average
class RayPathHistory {
public:
    /// @return 初回または exact key 変更で履歴をリセットしたとき true。
    [[nodiscard]] bool Prepare(const RayPathHistoryKey& key);
    /// @return 未準備・0 サンプル・有効サンプル上限超過なら false。サンプル数は未変更。
    /// @note UINT32_MAX は GPU RAW の sticky error sentinel。正常な蓄積数の上限は UINT32_MAX-1。
    [[nodiscard]] bool Commit(uint32_t sampleCount);
    [[nodiscard]] uint32_t GetSampleCount() const { return m_sampleCount; }
    [[nodiscard]] bool IsPrepared() const { return m_prepared; }
    void Reset();

private:
    RayPathHistoryKey m_key;
    uint32_t m_sampleCount = 0;
    bool m_prepared = false;
};

} /// @note namespace fbzz::renderer
