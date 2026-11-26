/// @file    RenderLightAtlases.cpp
/// @brief   影と Cookie のアトラス割り当てと投影行列。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderLightExtraction.hpp"
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene::light_extraction_detail {

void BuildPunctualShadowAtlas(std::vector<PunctualShadowCandidate>& shadowCandidates,
    const renderer::RenderSettings& settings, uint32_t atlasResolution, RenderLightExtraction& output)
{
    // キューブ 1 面ぶんの半画角 (= 90 度の半分)。Point シャドウの 6 面で使う。
    constexpr float CUBE_FACE_HALF_FOV = 3.14159265f / 4.0f;
    auto& punctualLights = output.punctualLights;

    // Spot / Point シャドウのスロット割り当てと行列の組み立て
    // カメラから近い順。遠いライトの影は数ピクセルにしかならず落としても気づかれにくい。
    // 距離キーは連続に変化するので、あふれの切り替わりも端から 1 つずつ起きる。
    std::sort(shadowCandidates.begin(), shadowCandidates.end(),
              [](const PunctualShadowCandidate& a, const PunctualShadowCandidate& b) {
                  return a.cameraDistanceSquared < b.cameraDistanceSquared;
              });

    // アトラスは 4x4 = kMaxPunctualShadows タイル。Spot が 1 枚、Point が 6 枚を使う。
    constexpr uint32_t PUNCTUAL_TILES_PER_SIDE = 4u;
    static_assert(PUNCTUAL_TILES_PER_SIDE * PUNCTUAL_TILES_PER_SIDE
                      == static_cast<uint32_t>(kMaxPunctualShadows),
                  "punctual shadow atlas tiling must cover exactly kMaxPunctualShadows tiles");
    const uint32_t punctualTileSize =
        (std::max)(atlasResolution / PUNCTUAL_TILES_PER_SIDE, 1u);
    const float    atlasResolutionFloat = static_cast<float>(atlasResolution);
    const float    punctualUvScale   =
        static_cast<float>(punctualTileSize) / atlasResolutionFloat;

    // キューブ 6 面の向きと up。順序は PunctualShadow.hlsli の FBZZ_CubeFaceIndex と
    // 一致させること (+X, -X, +Y, -Y, +Z, -Z)。
    // up は描く行列と引く行列が同じなら何でもよい (両方ここで作った 1 本を使う)。
    static constexpr math::Vector3 CUBE_FACE_DIRECTIONS[6] = {
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
    };
    static constexpr math::Vector3 CUBE_FACE_UP[6] = {
        { 0.0f, 1.0f,  0.0f }, { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f },
        { 0.0f, 1.0f,  0.0f }, { 0.0f, 1.0f, 0.0f },
    };

    auto& punctualViews = output.punctualShadowViews;
    auto& punctualViewCount = output.punctualShadowViewCount;
    // キューブ 6 面を使ったライトの本数 (Point / Sphere / Tube)。
    int shadowedCubeCount   = 0;
    // b3 経路 (既定の Forward) 向けのスロット番号。-1 = 影なし。
    auto& legacyShadowSlots = output.legacyShadowSlots;
    for (int& slot : legacyShadowSlots) {
        slot = -1;
    }

    // タイル 1 枚を組み立てる。halfFovRad はそのタイルの投影半画角。
    const auto buildPunctualView =
        [&](int slot, const math::Vector3& lightPosition, const math::Vector3& direction,
            const math::Vector3& up, float halfFovRad, float nearDepth, float farDepth,
            float biasScale, float strength, float sourceRadius) {
        PunctualShadowView& view = punctualViews[slot];
        view.view           = math::Matrix4::LookAt(lightPosition, lightPosition + direction, up);
        view.viewProjection =
            math::Matrix4::Perspective(halfFovRad * 2.0f, 1.0f, nearDepth, farDepth) * view.view;
        view.eyePos         = lightPosition;
        view.frustum        = math::Frustum::FromViewProjection(view.viewProjection);
        view.shadowStrength = strength;

        const uint32_t tileX = static_cast<uint32_t>(slot) % PUNCTUAL_TILES_PER_SIDE;
        const uint32_t tileY = static_cast<uint32_t>(slot) / PUNCTUAL_TILES_PER_SIDE;
        view.viewportX    = tileX * punctualTileSize;
        view.viewportY    = tileY * punctualTileSize;
        view.viewportSize = punctualTileSize;
        view.atlasRect    = {
            static_cast<float>(view.viewportX) / atlasResolutionFloat,
            static_cast<float>(view.viewportY) / atlasResolutionFloat,
            punctualUvScale, punctualUvScale
        };

        // 基本バイアスは「1 テクセルが覆うワールド距離」。アクネはテクセルの幅の中で
        // 面の深度が変わることから出るので、補正量はテクセルの実寸そのものになる
        // (斜め面ぶんの tan(theta) はシェーダー側の FBZZ_PunctualSlopeBias が掛ける)。
        // 評価点が range の中ほどなのは、透視投影ではテクセル実寸が深度に比例するため。
        const float middleDepth       = (std::max)((nearDepth + farDepth) * 0.5f, nearDepth * 2.0f);
        const float texelWorldSize =
            2.0f * middleDepth * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);
        // 透視投影の NDC 深度は非線形なので、ワールド距離をそのまま渡せない。
        // z_ndc = f/(f-n) * (1 - n/z)  →  dz_ndc/dz = f*n / ((f-n) * z^2)
        const float ndcPerWorld =
            (farDepth * nearDepth) / ((std::max)(farDepth - nearDepth, 0.001f) * middleDepth * middleDepth);
        view.biasNDC = texelWorldSize * ndcPerWorld * biasScale;

        // 1 テクセルが張る角度。ShadowPass の極小 caster カリングが使う。
        view.texelAngularSize =
            2.0f * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);

        // 光源半径がシャドウマップ上で何テクセルぶんの半影になるか。
        // 本来は「光源の大きさ × 遮蔽物と受光面の距離比」だが、ブロッカー探索が無いので
        // 比を 1 とみなす。遮蔽物が遠いほど硬くなるが「大きな電球ほど柔らかい」は出る。
        view.penumbraTexels = (texelWorldSize > 0.0f) ? (sourceRadius / texelWorldSize) : 0.0f;
    };

    if (settings.shadowEnabled) {
        for (const PunctualShadowCandidate& shadowCandidate : shadowCandidates) {
            const int requiredTiles = shadowCandidate.needsCube ? 6 : 1;
            // break ではなく continue。全方位のライトが入らなかっただけで、後ろに続く
            // Spot / Area は 1 枚で収まる可能性がある。
            if (punctualViewCount + requiredTiles > kMaxPunctualShadows) {
                continue;
            }
            if (shadowCandidate.needsCube && shadowedCubeCount >= settings.shadow.maxShadowedPointLights) {
                continue;
            }

            // Inspector で range より大きい shadowNearPlane を入れられるので、
            // ここで潰さないと Matrix4::Perspective の assert を踏む。
            const float farDepth  = shadowCandidate.range;
            const float nearDepth = (std::min)(shadowCandidate.nearPlane, farDepth * 0.5f);

            const int baseSlot = punctualViewCount;
            if (shadowCandidate.needsCube) {
                for (int face = 0; face < 6; ++face) {
                    buildPunctualView(baseSlot + face, shadowCandidate.position,
                                      CUBE_FACE_DIRECTIONS[face], CUBE_FACE_UP[face],
                                      CUBE_FACE_HALF_FOV, nearDepth, farDepth, shadowCandidate.bias, shadowCandidate.strength,
                                      shadowCandidate.sourceRadius);
                }
                punctualViewCount += 6;
                ++shadowedCubeCount;
            } else {
                // 錐台は円錐へ外接させる。outerCone は半角なので画角はその 2 倍。
                // 少し広げるのは、ぴったり切ると PCF が縁ではみ出して影が欠けるため。
                // Area はコーンを持たないので kAreaShadowOuterConeDeg が入っている。
                const float halfFov = (std::min)(
                    std::clamp(shadowCandidate.outerCone, 1.0f, 79.0f) * DEGREES_TO_RADIANS * 1.05f,
                    CUBE_FACE_HALF_FOV * 1.9f);
                const math::Vector3 up = (std::abs(shadowCandidate.direction.y) > 0.99f)
                                         ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                         : math::Vector3{ 0.0f, 1.0f, 0.0f };
                buildPunctualView(baseSlot, shadowCandidate.position, shadowCandidate.direction, up,
                                  halfFov, nearDepth, farDepth, shadowCandidate.bias, shadowCandidate.strength,
                                  shadowCandidate.sourceRadius);
                punctualViewCount += 1;
            }
            // クラスタ経路はライト構造体から、レガシー経路は b12 の対応表から番号を引く。
            // どちらの経路でも同じスロットを指すよう、ここで両方へ書く。
            punctualLights[shadowCandidate.punctualIndex].shadowIndex = baseSlot;
            if (shadowCandidate.legacySlot >= 0 && shadowCandidate.legacySlot < kMaxLegacyPunctualLights) {
                legacyShadowSlots[shadowCandidate.legacySlot] = baseSlot;
            }
        }
    }
}

void BuildLightCookieAtlas(std::vector<LightCookieCandidate>& cookieCandidates, RenderLightExtraction& output)
{
    auto& punctualLights = output.punctualLights;

    // Cookie のスロット割り当て
    // 影と同じくカメラから近い順。タイルは 8 枚しかない。
    std::sort(cookieCandidates.begin(), cookieCandidates.end(),
              [](const LightCookieCandidate& a, const LightCookieCandidate& b) {
                  return a.cameraDistanceSquared < b.cameraDistanceSquared;
              });

    auto& cookieViews = output.lightCookieViews;
    auto& cookieViewCount = output.lightCookieViewCount;
    auto& legacyCookieSlots = output.legacyCookieSlots;
    for (int& slot : legacyCookieSlots) {
        slot = -1;
    }

    for (const LightCookieCandidate& cookieCandidate : cookieCandidates) {
        if (cookieViewCount >= kMaxLightCookies) {
            break;
        }

        const int slot = cookieViewCount++;
        LightCookieView& view = cookieViews[slot];

        // 投影は影と同じ「スポットの円錐に外接する透視錐台」。
        // 影の行列を流用しないのは、Cookie が影を落とさないライトにも付くため。
        // 縁を広げないのは、近傍サンプルが無く広げると模様がコーンより内側で終わるため。
        const float farDepth    = cookieCandidate.range;
        const float nearDepth   = (std::min)(cookieCandidate.nearPlane, farDepth * 0.5f);
        const float halfFov = std::clamp(cookieCandidate.outerCone, 1.0f, 79.0f) * DEGREES_TO_RADIANS;
        const math::Vector3 up = (std::abs(cookieCandidate.direction.y) > 0.99f)
                                 ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                 : math::Vector3{ 0.0f, 1.0f, 0.0f };

        const math::Matrix4 cookieView =
            math::Matrix4::LookAt(cookieCandidate.position, cookieCandidate.position + cookieCandidate.direction, up);
        view.viewProjection =
            math::Matrix4::Perspective(halfFov * 2.0f, 1.0f, nearDepth, farDepth) * cookieView;

        const uint32_t tileX = static_cast<uint32_t>(slot) % kLightCookieAtlasCols;
        const uint32_t tileY = static_cast<uint32_t>(slot) / kLightCookieAtlasCols;
        view.viewportX = tileX * kLightCookieTileSize;
        view.viewportY = tileY * kLightCookieTileSize;
        view.atlasRect = {
            static_cast<float>(view.viewportX) / static_cast<float>(kLightCookieAtlasWidth),
            static_cast<float>(view.viewportY) / static_cast<float>(kLightCookieAtlasHeight),
            static_cast<float>(kLightCookieTileSize) / static_cast<float>(kLightCookieAtlasWidth),
            static_cast<float>(kLightCookieTileSize) / static_cast<float>(kLightCookieAtlasHeight)
        };
        view.sourcePath  = cookieCandidate.path;
        view.rotationRad = cookieCandidate.rotationRad;

        punctualLights[cookieCandidate.punctualIndex].cookieIndex = slot;
        if (cookieCandidate.legacySlot >= 0 && cookieCandidate.legacySlot < kMaxLegacyPunctualLights) {
            legacyCookieSlots[cookieCandidate.legacySlot] = slot;
        }
    }
}

} // namespace fbzz::scene::light_extraction_detail
