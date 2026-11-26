/// @file    RenderLightExtraction.hpp
/// @brief   影・Cookie 候補とライト抽出で共有する処理。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Rendering/Extraction/Lighting/RenderLightExtractor.hpp>
#include <string>
#include <vector>

namespace fbzz::scene::light_extraction_detail {

inline constexpr float DEGREES_TO_RADIANS = 3.14159265f / 180.0f;

/// @note 影を落とせるライトの候補 (Directional 以外の全型)。
/// @note アトラスは 16 タイルしかなく、走査順に配ると「シーンのどこに置いたか」で
/// @note 影の有無が決まる。全部集めてから捨てる相手を選ぶ。
struct PunctualShadowCandidate {
    size_t        punctualIndex; ///< punctualLights 内の位置
    int           legacySlot; ///< b3 側の位置 (点 0-7 / スポット 8-11)。-1 = b3 に入らない
    bool          needsCube; ///< 全方位のライトはキューブ 6 面 = 6 タイルを使う。Spot / Area は 1 タイル。
    math::Vector3 position;
    math::Vector3 direction; ///< 1 タイル側の照射方向 (キューブでは未使用)
    float         range;
    float         outerCone; ///< [degrees] 1 タイル側の半画角
    float         nearPlane;
    float         bias;
    float         strength;
    float         sourceRadius; ///< 半影の広がりを決める光源半径 [m]
    float         cameraDistanceSquared;
};

/// @note Cookie を持つスポットの候補。割り当ての考え方は影と同じで、タイル数が
/// @note 有限 (8 枚) なのでカメラから近い順に配る。
struct LightCookieCandidate {
    size_t        punctualIndex;
    int           legacySlot;
    math::Vector3 position;
    math::Vector3 direction;
    float         range;
    float         outerCone; ///< 角度の単位は度。
    float         nearPlane;
    float         rotationRad;
    std::string   path;
    float         cameraDistanceSquared;
};

void BuildPunctualShadowAtlas(std::vector<PunctualShadowCandidate>& shadowCandidates,
    const renderer::RenderSettings& settings, uint32_t atlasResolution, RenderLightExtraction& output);
void BuildLightCookieAtlas(std::vector<LightCookieCandidate>& cookieCandidates, RenderLightExtraction& output);
void AppendParticleLights(Scene& scene, bool rayTracingEnabled, RenderLightExtraction& output);
void ApplySkyLighting(Scene& scene, bool rayTracingEnabled, RenderLightExtraction& output);

}
