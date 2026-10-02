/// @file    RenderLightExtractor.cpp
/// @brief   ライト・影・Cookie の抽出入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Engine/Scene/Systems/RenderLightExtractor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/ParticleLightSelection.hpp>
#include <Graphics/Renderer/ColorTemperature.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <algorithm>
#include <cmath>
namespace fbzz::scene {
RenderLightExtraction ExtractRenderLights(Scene& scene, const renderer::Camera& camera, const renderer::RenderSettings& rs, uint32_t punctualShadowRes)
{
    RenderLightExtraction output;
    output.rayLightsComplete = true;
    /// @note ライト定数バッファを構築
    auto& lightData = output.lightData;
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;

    /// @note Directional Light のシャドウ設定 (LightComponent から取得)
    auto& dirCastShadows = output.dirCastShadows;
    auto& dirShadowBias = output.dirShadowBias;
    auto& dirShadowStrength = output.shadowStrength;
    auto& dirShadowDistance = output.dirShadowDistance;

    /// @note クラスタライティング用の統合ライト配列。b3 の固定長配列と並行して構築する。
    /// @note b3 は点 8 / スポット 4 で打ち切るが、こちらは 256 本まで拾う。
    /// @note b3 の詰め方は変えないので、クラスタ未対応のパスの見た目は据え置き。
    auto& punctualLights = output.punctualLights;
    punctualLights.reserve(32);

    /// @note 影を落とせるライトの候補 (Directional 以外の全型)。
    /// @note アトラスは 16 タイルしかなく、走査順に配ると「シーンのどこに置いたか」で
    /// @note 影の有無が決まる。全部集めてから捨てる相手を選ぶ。
    struct PunctualShadowCandidate {
        size_t        punctualIndex;   ///< @note punctualLights 内の位置
        int           legacySlot;      ///< @note b3 側の位置 (点 0-7 / スポット 8-11)。-1 = b3 に入らない
        /// @note 全方位のライトはキューブ 6 面 = 6 タイルを使う。Spot / Area は 1 タイル。
        bool          needsCube;
        math::Vector3 position;
        math::Vector3 direction;       ///< @note 1 タイル側の照射方向 (キューブでは未使用)
        float         range;
        float         outerCone;       ///< @note [degrees] 1 タイル側の半画角
        float         nearPlane;
        float         bias;
        float         strength;
        float         sourceRadius;  ///< @note 半影の広がりを決める光源半径 [m]
        float         cameraDistSq;
    };
    std::vector<PunctualShadowCandidate> shadowCandidates;

    /// @note Cookie を持つスポットの候補。割り当ての考え方は影と同じで、タイル数が
    /// @note 有限 (8 枚) なのでカメラから近い順に配る。
    struct LightCookieCandidate {
        size_t        punctualIndex;
        int           legacySlot;
        math::Vector3 position;
        math::Vector3 direction;
        float         range;
        float         outerCone;   ///< @note [degrees]
        float         nearPlane;
        float         rotationRad;
        std::string   path;
        float         cameraDistSq;
    };
    std::vector<LightCookieCandidate> cookieCandidates;

    /// @note b3 経路の点光源 / スポットの光源半径。添字は legacyShadowSlots と同じ。
    auto& legacySourceRadius = output.legacySourceRadius;

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    /// @note キューブ 1 面ぶんの半画角 (= 90 度の半分)。Point シャドウの 6 面で使う。
    constexpr float kQuarterPi = 3.14159265f / 4.0f;
    /// @note Area の影を焼く錐台の半画角 [degrees]。Spot の outerCone に相当する値として渡す。
    /// @note 面光源は法線側の半球 (= 90 度) を照らすが、透視投影は 90 度で無限に広がるため
    /// @note 張れない。75 度は「パネルの正面に置いた物の影は出る / 真横は諦める」の線。
    constexpr float kAreaShadowOuterConeDeg = 75.0f;
    /// @note View<> だと GameObject が取れず activeInHierarchy() を見られないので、GO を切っても
    /// @note 光だけが残る。GetEntities<> は View<> と同じ基底 span なので走査順は変わらない。
    for (EntityID id : scene.GetEntities<LightComponent>()) {
        GameObject*     go    = scene.GetGameObject(id);
        LightComponent* light = scene.GetComponent<LightComponent>(id);
        if (!go || !light || !go->activeInHierarchy() || !light->enabled) continue;
        const Transform&      tf = go->transform;
        const LightComponent& lc = *light;

        /// @note 色温度モードでは color 欄ではなく colorTemperature が正本。
        /// @note 毎フレーム引き直す。キャッシュは Inspector の反映漏れという見つけにくい種になる。
        const math::Vector3 lightColor =
            lc.useColorTemperature ? renderer::ColorFromTemperature(lc.colorTemperature)
                                   : lc.color;

        renderer::RayLightInput ray;
        ray.objectId = {scene.GetRenderSceneGeneration(), id.index, id.generation};
        ray.layerMask = go->layer >= 0 && go->layer < 32 ? (uint32_t{1} << go->layer) : 0;
        ray.type = static_cast<renderer::RayLightType>(lc.type);
        ray.position = tf.worldPosition;
        ray.direction = tf.forward;
        ray.tangent = tf.right;
        ray.bitangent = tf.up;
        ray.color = lightColor;
        ray.intensity = lc.intensity;
        ray.range = lc.range;
        ray.innerCone = lc.innerCone;
        ray.outerCone = lc.outerCone;
        ray.areaWidth = lc.areaWidth;
        ray.areaHeight = lc.areaHeight;
        ray.sourceRadius = lc.sourceRadius;
        ray.sourceLength = lc.sourceLength;
        ray.twoSided = lc.areaTwoSided;
        ray.castShadows = rs.shadowEnabled && lc.castShadows;
        ray.shadowStrength = lc.shadowStrength;
        if (!lc.cookiePath.empty()) ray.unsupportedFlags |= renderer::RAY_LIGHT_UNSUPPORTED_COOKIE;
        if (ray.layerMask == 0) ray.unsupportedFlags |= renderer::RAY_LIGHT_INVALID_LAYER;
        output.rayLights.push_back(ray);

        /// @note 点光源 / スポット / 大きさを持つ光源は上限に達するまで統合配列へも積む。
        /// @note b3 は「点を全部→スポットを全部」の 2 配列だがこちらは 1 本なので評価順が変わりうる。
        /// @note 加算なので結果は同じ (順序による丸め差のみ)。
        if (lc.type != LightComponent::Type::Directional
            && punctualLights.size() < kMaxPunctualLights) {
            const size_t punctualIndex = punctualLights.size();
            PunctualLightGPU& gpu = punctualLights.emplace_back();
            /// @note worldPosition を使う: Transform::position は親基準のローカル座標。
            /// @note 子 GameObject にライトを置くと (キャラクターの発光部・車のヘッドライト・
            /// @note ボーンに付けた松明)、親の姿勢が一切効かず原点付近に光が落ちる。
            /// @note 向き (forward / right / up) は worldRotation から作られるので既に
            /// @note ワールド空間で、位置だけが取り残されていた。
            gpu.position  = tf.worldPosition;
            gpu.range     = lc.range;
            gpu.color     = lightColor;
            gpu.intensity = lc.intensity;
            /// @note 既定は Point。他の型が以降で上書きする。
            gpu.direction   = { 0.0f, -1.0f, 0.0f };
            gpu.innerCos    = 0.0f;
            gpu.outerCos    = 0.0f;
            gpu.type        = static_cast<uint32_t>(PunctualLightType::Point);
            gpu.shadowIndex = -1;
            gpu.cookieIndex = -1;
            gpu.tangent     = { 1.0f, 0.0f, 0.0f };
            gpu.bitangent   = { 0.0f, 1.0f, 0.0f };
            /// @note 点光源 / スポットでも halfWidth は光源半径として意味を持つ
            /// @note (形状は点のまま、ハイライトの広がりと影のにじみ幅にだけ効く)。
            gpu.halfWidth   = (std::max)(lc.sourceRadius, 0.0f);
            gpu.halfHeight  = 0.0f;

            if (lc.type == LightComponent::Type::Spot) {
                gpu.direction = tf.forward.Normalized();
                gpu.innerCos  = std::cos(lc.innerCone * kDegToRad);
                gpu.outerCos  = std::cos(lc.outerCone * kDegToRad);
                gpu.type      = static_cast<uint32_t>(PunctualLightType::Spot);
            } else if (lc.type == LightComponent::Type::Sphere) {
                gpu.type      = static_cast<uint32_t>(PunctualLightType::Sphere);
            } else if (lc.type == LightComponent::Type::Tube) {
                /// @note 管の軸は Transform の Right。蛍光灯を横向きに置く姿勢が既定になる。
                gpu.tangent    = tf.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                gpu.halfHeight = (std::max)(lc.sourceLength, 0.0f) * 0.5f;
                gpu.type       = static_cast<uint32_t>(PunctualLightType::Tube);
            } else if (lc.type == LightComponent::Type::Area) {
                gpu.direction  = tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                gpu.tangent    = tf.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                gpu.bitangent  = tf.up.NormalizedOr({ 0.0f, 1.0f, 0.0f });
                gpu.halfWidth  = (std::max)(lc.areaWidth,  0.001f) * 0.5f;
                gpu.halfHeight = (std::max)(lc.areaHeight, 0.001f) * 0.5f;
                /// @note Area では innerCos / outerCos が空くので、両面フラグの運搬に使う。
                /// @note 専用フィールドを足すと 96 バイトの構造体がキャッシュライン 2 本に収まらない。
                gpu.outerCos   = lc.areaTwoSided ? 1.0f : 0.0f;
                gpu.type       = static_cast<uint32_t>(PunctualLightType::Area);
            }

            const math::Vector3 toCamera = tf.worldPosition - camera.m_position;
            const float cameraDistSq = math::Vector3::Dot(toCamera, toCamera);

            /// @note b3 側でこのライトが取る添字。直後のブロックが末尾へ 1 つ足すだけなので、
            /// @note 採番される番号は今のカウンタ値そのもの。
            int legacySlot = -1;
            if (lc.type == LightComponent::Type::Point && lightData.pointLightCount < 8)
                legacySlot = lightData.pointLightCount;
            else if (lc.type == LightComponent::Type::Spot && lightData.spotLightCount < 4)
                legacySlot = kLegacySpotSlotBase + lightData.spotLightCount;
            if (legacySlot >= 0)
                legacySourceRadius[legacySlot] = (std::max)(lc.sourceRadius, 0.0f);

            /// @note Cookie の候補。Spot 専用 — Point はキューブマップ、Directional は
            /// @note ワールド空間のタイリングという別の仕組みが要る。
            if (lc.type == LightComponent::Type::Spot && !lc.cookiePath.empty()) {
                LightCookieCandidate cookie{};
                cookie.punctualIndex = punctualIndex;
                cookie.legacySlot    = legacySlot;
                cookie.position      = tf.worldPosition;
                cookie.direction     = tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                cookie.range         = (std::max)(lc.range, 0.05f);
                cookie.outerCone     = lc.outerCone;
                cookie.nearPlane     = (std::max)(lc.shadowNearPlane, 0.01f);
                cookie.rotationRad   = lc.cookieRotation * kDegToRad;
                cookie.path          = lc.cookiePath;
                cookie.cameraDistSq  = cameraDistSq;
                cookieCandidates.push_back(std::move(cookie));
            }

            /// @note 影の候補として控える。Directional 以外は全型が落とせる。
            /// @note 形状を持つ光源も点から焼いた影でよい: 影の形は遮蔽物と受光面の配置でほぼ決まり、
            /// @note 光源の大きさは半影の広さ (sourceRadius から作る penumbraTexels) にしか効かない。
            /// @note 管が長いと本来は半影が軸方向へ伸びるが、それには軸に沿った複数枚が要り 16 タイルでは足りない。
            const bool canCastShadow =
                lc.castShadows && lc.shadowStrength > 0.0f &&
                lc.type != LightComponent::Type::Directional;
            if (canCastShadow) {
                /// @note 遠すぎるライトへタイルを割り当てない。判定距離に range を足すのは、
                /// @note range の大きいライトは離れていても画面を広く照らすため。
                const float limit = rs.shadow.punctualShadowDistance + lc.range;
                if (cameraDistSq <= limit * limit) {
                    PunctualShadowCandidate cand{};
                    cand.punctualIndex = punctualIndex;
                    cand.legacySlot    = legacySlot;
                    /// @note Sphere / Tube は Point と同じ全方位。Area だけが向きを持つ。
                    cand.needsCube     = (lc.type == LightComponent::Type::Point
                                       || lc.type == LightComponent::Type::Sphere
                                       || lc.type == LightComponent::Type::Tube);
                    cand.position      = tf.worldPosition;
                    cand.direction     = cand.needsCube
                                       ? math::Vector3{ 0.0f, -1.0f, 0.0f }
                                       : tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                    cand.range         = (std::max)(lc.range, 0.05f);
                    /// @note Area は法線側の半球を照らすが、1 枚の透視投影では 180 度を張れない。
                    /// @note 実用上そこまでで、これ以上広げると端のテクセル密度が落ちるだけ。
                    cand.outerCone     = (lc.type == LightComponent::Type::Area)
                                       ? kAreaShadowOuterConeDeg
                                       : lc.outerCone;
                    cand.nearPlane     = (std::max)(lc.shadowNearPlane, 0.01f);
                    cand.bias          = (std::max)(lc.shadowBias, 0.0f);
                    cand.strength      = std::clamp(lc.shadowStrength, 0.0f, 1.0f);
                    cand.sourceRadius  = (std::max)(lc.sourceRadius, 0.0f);
                    cand.cameraDistSq  = cameraDistSq;
                    shadowCandidates.push_back(cand);
                }
            }
        }

        if (lc.type == LightComponent::Type::Directional) {
            lightData.lightDir       = tf.forward.Normalized();
            lightData.lightColor     = lightColor;
            lightData.lightIntensity = lc.intensity;
            dirCastShadows    = lc.castShadows;
            dirShadowBias     = lc.shadowBias;
            dirShadowStrength = lc.shadowStrength;
            dirShadowDistance = lc.shadowDistance;
        } else if (lc.type == LightComponent::Type::Point
                   && lightData.pointLightCount < 8) {
            auto& pl    = lightData.pointLights[lightData.pointLightCount++];
            pl.position  = tf.worldPosition;
            pl.range     = lc.range;
            pl.color     = lightColor;
            pl.intensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Spot
                   && lightData.spotLightCount < 4) {
            auto& sl    = lightData.spotLights[lightData.spotLightCount++];
            sl.position  = tf.worldPosition;
            sl.direction = tf.forward.Normalized();
            sl.range     = lc.range;
            sl.innerCos  = std::cos(lc.innerCone * kDegToRad);
            sl.outerCos  = std::cos(lc.outerCone * kDegToRad);
            sl.color     = lightColor;
            sl.intensity = lc.intensity;
        }
    }

    /// @note 粒子を点光源にする (ParticleEmitter の Lights モジュール)。LightComponent の後に積むので、
    /// @note 枠が足りないときに削られるのは粒子の光の方。Legacy (b3) には載せない。
    /// @note GPU シミュレーションの粒子は位置が GPU にしか無いので対象外 (Inspector に注記がある)。
    std::vector<ParticleLightEmission> particleLights;
    /// @note 粒子光源の未対応は Raster の 256 本上限や GPU runtime の準備状態で消さない。
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        const GameObject* go = scene.GetGameObject(id);
        const ParticleEmitter* emitter = scene.GetComponent<ParticleEmitter>(id);
        if (!go || !emitter || !go->activeInHierarchy() || !emitter->settings.enabled
            || !emitter->settings.light.lightEnabled) continue;
        renderer::RayLightInput ray;
        ray.objectId = {scene.GetRenderSceneGeneration(), id.index, id.generation};
        ray.layerMask = go->layer >= 0 && go->layer < 32 ? (uint32_t{1} << go->layer) : 0;
        ray.position = go->transform.worldPosition;
        ray.unsupportedFlags = renderer::RAY_LIGHT_UNSUPPORTED_PARTICLE;
        if (ray.layerMask == 0) ray.unsupportedFlags |= renderer::RAY_LIGHT_INVALID_LAYER;
        output.rayLights.push_back(ray);
    }
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        if (punctualLights.size() >= kMaxPunctualLights) break;
        GameObject*      go      = scene.GetGameObject(id);
        ParticleEmitter* emitter = scene.GetComponent<ParticleEmitter>(id);
        if (!go || !emitter || !go->activeInHierarchy() || !emitter->settings.enabled
            || !emitter->settings.light.lightEnabled
            || CanUseGpuSimulation(emitter->settings, &emitter->runtime.material))
            continue;
        SelectParticleLights(emitter->settings.light, emitter->runtime.particles,
                             kMaxPunctualLights - punctualLights.size(), particleLights);
        const bool localSpace = emitter->settings.simulationSpace == ParticleSimulationSpace::Local;
        for (const ParticleLightEmission& emission : particleLights) {
            PunctualLightGPU& gpu = punctualLights.emplace_back();
            gpu.position  = localSpace ? TransformEmitterPoint(go->transform, emission.position) : emission.position;
            gpu.range     = emission.range;
            gpu.color     = emission.color;
            gpu.intensity = emission.intensity;
            gpu.direction = { 0.0f, -1.0f, 0.0f };
            gpu.type      = static_cast<uint32_t>(PunctualLightType::Point);
        }
    }

    /// @name Spot / Point シャドウのスロット割り当てと行列の組み立て
    /// @note カメラから近い順。遠いライトの影は数ピクセルにしかならず落としても気づかれにくい。
    /// @note 距離キーは連続に変化するので、あふれの切り替わりも端から 1 つずつ起きる。
    std::sort(shadowCandidates.begin(), shadowCandidates.end(),
              [](const PunctualShadowCandidate& a, const PunctualShadowCandidate& b) {
                  return a.cameraDistSq < b.cameraDistSq;
              });

    /// @note アトラスは 4x4 = kMaxPunctualShadows タイル。Spot が 1 枚、Point が 6 枚を使う。
    constexpr uint32_t kPunctualTilesPerSide = 4u;
    static_assert(kPunctualTilesPerSide * kPunctualTilesPerSide
                      == static_cast<uint32_t>(kMaxPunctualShadows),
                  "punctual shadow atlas tiling must cover exactly kMaxPunctualShadows tiles");
    const uint32_t punctualTileSize =
        (std::max)(punctualShadowRes / kPunctualTilesPerSide, 1u);
    const float    punctualAtlasResF = static_cast<float>(punctualShadowRes);
    const float    punctualUvScale   =
        static_cast<float>(punctualTileSize) / punctualAtlasResF;

    /// @note キューブ 6 面の向きと up。順序は PunctualShadow.hlsli の FBZZ_CubeFaceIndex と
    /// @note 一致させること (+X, -X, +Y, -Y, +Z, -Z)。
    /// @note up は描く行列と引く行列が同じなら何でもよい (両方ここで作った 1 本を使う)。
    static constexpr math::Vector3 kCubeFaceDir[6] = {
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
    };
    static constexpr math::Vector3 kCubeFaceUp[6] = {
        { 0.0f, 1.0f,  0.0f }, { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f },
        { 0.0f, 1.0f,  0.0f }, { 0.0f, 1.0f, 0.0f },
    };

    auto& punctualViews = output.punctualShadowViews;
    auto& punctualViewCount = output.punctualShadowViewCount;
    /// @note キューブ 6 面を使ったライトの本数 (Point / Sphere / Tube)。
    int shadowedCubeCount   = 0;
    /// @note b3 経路 (既定の Forward) 向けのスロット番号。-1 = 影なし。
    auto& legacyShadowSlots = output.legacyShadowSlots;
    for (int& slot : legacyShadowSlots) slot = -1;

    /// @note タイル 1 枚を組み立てる。halfFovRad はそのタイルの投影半画角。
    const auto buildPunctualView =
        [&](int slot, const math::Vector3& eye, const math::Vector3& dir,
            const math::Vector3& up, float halfFovRad, float nearZ, float farZ,
            float biasScale, float strength, float sourceRadius) {
        PunctualShadowView& view = punctualViews[slot];
        view.view           = math::Matrix4::LookAt(eye, eye + dir, up);
        view.viewProjection =
            math::Matrix4::Perspective(halfFovRad * 2.0f, 1.0f, nearZ, farZ) * view.view;
        view.eyePos         = eye;
        view.frustum        = math::Frustum::FromViewProjection(view.viewProjection);
        view.shadowStrength = strength;

        const uint32_t tileX = static_cast<uint32_t>(slot) % kPunctualTilesPerSide;
        const uint32_t tileY = static_cast<uint32_t>(slot) / kPunctualTilesPerSide;
        view.viewportX    = tileX * punctualTileSize;
        view.viewportY    = tileY * punctualTileSize;
        view.viewportSize = punctualTileSize;
        view.atlasRect    = {
            static_cast<float>(view.viewportX) / punctualAtlasResF,
            static_cast<float>(view.viewportY) / punctualAtlasResF,
            punctualUvScale, punctualUvScale
        };

        /// @note 基本バイアスは「1 テクセルが覆うワールド距離」。アクネはテクセルの幅の中で
        /// @note 面の深度が変わることから出るので、補正量はテクセルの実寸そのものになる
        /// @note (斜め面ぶんの tan(theta) はシェーダー側の FBZZ_PunctualSlopeBias が掛ける)。
        /// @note 評価点が range の中ほどなのは、透視投影ではテクセル実寸が深度に比例するため。
        const float midZ       = (std::max)((nearZ + farZ) * 0.5f, nearZ * 2.0f);
        const float texelWorld =
            2.0f * midZ * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);
        /// @note 透視投影の NDC 深度は非線形なので、ワールド距離をそのまま渡せない。
        /// @note z_ndc = f/(f-n) * (1 - n/z)  →  dz_ndc/dz = f*n / ((f-n) * z^2)
        const float ndcPerWorld =
            (farZ * nearZ) / ((std::max)(farZ - nearZ, 0.001f) * midZ * midZ);
        view.biasNDC = texelWorld * ndcPerWorld * biasScale;

        /// @note 1 テクセルが張る角度。ShadowPass の極小 caster カリングが使う。
        view.texelAngularSize =
            2.0f * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);

        /// @note 光源半径がシャドウマップ上で何テクセルぶんの半影になるか。
        /// @note 本来は「光源の大きさ × 遮蔽物と受光面の距離比」だが、ブロッカー探索が無いので
        /// @note 比を 1 とみなす。遮蔽物が遠いほど硬くなるが「大きな電球ほど柔らかい」は出る。
        view.penumbraTexels = (texelWorld > 0.0f) ? (sourceRadius / texelWorld) : 0.0f;
    };

    if (rs.shadowEnabled) {
        for (const PunctualShadowCandidate& cand : shadowCandidates) {
            const int needed = cand.needsCube ? 6 : 1;
            /// @note break ではなく continue。全方位のライトが入らなかっただけで、後ろに続く
            /// @note Spot / Area は 1 枚で収まる可能性がある。
            if (punctualViewCount + needed > kMaxPunctualShadows) continue;
            if (cand.needsCube && shadowedCubeCount >= rs.shadow.maxShadowedPointLights) continue;

            /// @note Inspector で range より大きい shadowNearPlane を入れられるので、
            /// @note ここで潰さないと Matrix4::Perspective の assert を踏む。
            const float farZ  = cand.range;
            const float nearZ = (std::min)(cand.nearPlane, farZ * 0.5f);

            const int baseSlot = punctualViewCount;
            if (cand.needsCube) {
                for (int face = 0; face < 6; ++face) {
                    buildPunctualView(baseSlot + face, cand.position,
                                      kCubeFaceDir[face], kCubeFaceUp[face],
                                      kQuarterPi, nearZ, farZ, cand.bias, cand.strength,
                                      cand.sourceRadius);
                }
                punctualViewCount += 6;
                ++shadowedCubeCount;
            } else {
                /// @note 錐台は円錐へ外接させる。outerCone は半角なので画角はその 2 倍。
                /// @note 少し広げるのは、ぴったり切ると PCF が縁ではみ出して影が欠けるため。
                /// @note Area はコーンを持たないので kAreaShadowOuterConeDeg が入っている。
                const float halfFov = (std::min)(
                    std::clamp(cand.outerCone, 1.0f, 79.0f) * kDegToRad * 1.05f,
                    kQuarterPi * 1.9f);
                const math::Vector3 up = (std::abs(cand.direction.y) > 0.99f)
                                         ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                         : math::Vector3{ 0.0f, 1.0f, 0.0f };
                buildPunctualView(baseSlot, cand.position, cand.direction, up,
                                  halfFov, nearZ, farZ, cand.bias, cand.strength,
                                  cand.sourceRadius);
                punctualViewCount += 1;
            }
            /// @note クラスタ経路はライト構造体から、レガシー経路は b12 の対応表から番号を引く。
            /// @note どちらの経路でも同じスロットを指すよう、ここで両方へ書く。
            punctualLights[cand.punctualIndex].shadowIndex = baseSlot;
            if (cand.legacySlot >= 0 && cand.legacySlot < kMaxLegacyPunctualLights)
                legacyShadowSlots[cand.legacySlot] = baseSlot;
        }
    }

    /// @name Cookie のスロット割り当て
    /// @note 影と同じくカメラから近い順。タイルは 8 枚しかない。
    std::sort(cookieCandidates.begin(), cookieCandidates.end(),
              [](const LightCookieCandidate& a, const LightCookieCandidate& b) {
                  return a.cameraDistSq < b.cameraDistSq;
              });

    auto& cookieViews = output.lightCookieViews;
    auto& cookieViewCount = output.lightCookieViewCount;
    auto& legacyCookieSlots = output.legacyCookieSlots;
    for (int& slot : legacyCookieSlots) slot = -1;

    for (const LightCookieCandidate& cand : cookieCandidates) {
        if (cookieViewCount >= kMaxLightCookies) break;

        const int slot = cookieViewCount++;
        LightCookieView& view = cookieViews[slot];

        /// @note 投影は影と同じ「スポットの円錐に外接する透視錐台」。
        /// @note 影の行列を流用しないのは、Cookie が影を落とさないライトにも付くため。
        /// @note 縁を広げないのは、近傍サンプルが無く広げると模様がコーンより内側で終わるため。
        const float farZ    = cand.range;
        const float nearZ   = (std::min)(cand.nearPlane, farZ * 0.5f);
        const float halfFov = std::clamp(cand.outerCone, 1.0f, 79.0f) * kDegToRad;
        const math::Vector3 up = (std::abs(cand.direction.y) > 0.99f)
                                 ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                 : math::Vector3{ 0.0f, 1.0f, 0.0f };

        const math::Matrix4 cookieView =
            math::Matrix4::LookAt(cand.position, cand.position + cand.direction, up);
        view.viewProjection =
            math::Matrix4::Perspective(halfFov * 2.0f, 1.0f, nearZ, farZ) * cookieView;

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
        view.sourcePath  = cand.path;
        view.rotationRad = cand.rotationRad;

        punctualLights[cand.punctualIndex].cookieIndex = slot;
        if (cand.legacySlot >= 0 && cand.legacySlot < kMaxLegacyPunctualLights)
            legacyCookieSlots[cand.legacySlot] = slot;
    }

    /// @name 昼夜の色・強度カーブ (Phase B)
    /// @note 太陽の向きは DirectionalLight の transform が唯一のソース (lightDir は上書きしない)。
    /// @note dayNightEnabled のときは、その光源の太陽高度から色と強度の遷移だけを駆動する。
    /// @note ライトを回すと 太陽ディスク・空・月・空連動 IBL・ライティングが一緒に動く。
    /// @note 雲シャドウ params (Phase C) も SkyRenderer から読み、passCtx へ後で転送する。
    auto& skyCloudShadowStrength = output.cloudShadowStrength;
    auto& skyCloudShadowCoverage = output.cloudShadowCoverage;
    auto& skyCloudShadowScale = output.cloudShadowScale;
    auto& skyCloudShadowSpeed = output.cloudShadowSpeed;
    /// @note 太陽の向きは DirectionalLight 側で決まるため SkyRenderer の Transform は使わない。
    for (EntityID id : scene.GetEntities<SkyRenderer>()) {
        GameObject* go     = scene.GetGameObject(id);
        auto*       skyPtr = scene.GetComponent<SkyRenderer>(id);
        if (!go || !skyPtr || !go->activeInHierarchy() || !skyPtr->enabled) continue;
        const SkyRenderer& sky = *skyPtr;

        /// @note 雲シャドウは昼夜サイクルとは独立に常に反映する。
        skyCloudShadowStrength = sky.cloudShadowStrength;
        skyCloudShadowCoverage = sky.cloudShadowCoverage;
        /// @note Component は「まだら 1 周期の大きさ [m]」。シェーダーは world→UV スケールを要る。
        skyCloudShadowScale    = 1.0f / (std::max)(sky.cloudShadowSize, 1.0f);
        skyCloudShadowSpeed    = sky.cloudShadowSpeed;

        if (sky.dayNightEnabled) {
            /// @note 太陽方向 (toward sun) = -lightDir。その高度 [度] を軸に 夜 ↔ 夕方 ↔ 昼 を補間する。
            /// @note 高度 0° を夕方のキーに置くと「ライトを水平 = 夕方」になり、昼側と夜側それぞれ
            /// @note 独立した帯幅で抜けられる。旧実装は夕焼けの重みに昼の重みを掛けており、
            /// @note 地平線上で重みが 0.17 まで落ちて夕方を作れなかった。
            const math::Vector3 sunToSun =
                math::Vector3{ -lightData.lightDir.x, -lightData.lightDir.y, -lightData.lightDir.z }.Normalized();

            auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            auto lerp1   = [](float a, float b, float t) { return a + (b - a) * t; };
            auto lerp3   = [](const math::Vector3& a, const math::Vector3& b, float t) {
                return math::Vector3{ a.x + (b.x - a.x) * t,
                                      a.y + (b.y - a.y) * t,
                                      a.z + (b.z - a.z) * t };
            };

            constexpr float kRadToDeg = 57.29577951f;
            const float sinAlt      = (std::max)(-1.0f, (std::min)(1.0f, sunToSun.y));
            const float altitudeDeg = std::asin(sinAlt) * kRadToDeg;

            const bool  above = altitudeDeg >= 0.0f;
            const float span  = above ? (std::max)(sky.dayAltitude,   0.1f)
                                      : (std::max)(sky.nightAltitude, 0.1f);
            float t = clamp01(std::fabs(altitudeDeg) / span);
            /// @note smoothstep: 帯の端で色・明るさが折れないようにする
            t = t * t * (3.0f - 2.0f * t);

            lightData.lightColor     = lerp3(sky.sunsetColor,
                                             above ? sky.dayColor : sky.nightColor, t);
            lightData.lightIntensity = lerp1(sky.sunsetIntensity,
                                             above ? sky.dayIntensity : sky.nightIntensity, t);
            /// @note 空の明るさは太陽光の強さとは別軸。共用していた頃は太陽を強くすると空も白飛びした。
            /// @note 詳細は SkyRenderer::skyDayBrightness。
            lightData.skyDimmer      = lerp1(sky.skySunsetBrightness,
                                             above ? sky.skyDayBrightness : sky.skyNightBrightness, t);
            /// @note independent RayLightTable は初期段で昼夜カーブを解決しない。primary owner を診断に残す。
            const auto primary = std::find_if(output.rayLights.rbegin(), output.rayLights.rend(),
                [](const renderer::RayLightInput& light) { return light.type == renderer::RayLightType::DIRECTIONAL; });
            if (primary != output.rayLights.rend()) primary->unsupportedFlags |= renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT;
            else {
                renderer::RayLightInput ray;
                ray.objectId = {scene.GetRenderSceneGeneration(), id.index, id.generation};
                ray.layerMask = go->layer >= 0 && go->layer < 32 ? (uint32_t{1} << go->layer) : 0;
                ray.type = renderer::RayLightType::DIRECTIONAL;
                ray.unsupportedFlags = renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT;
                if (ray.layerMask == 0) ray.unsupportedFlags |= renderer::RAY_LIGHT_INVALID_LAYER;
                output.rayLights.push_back(ray);
            }
        }
        break;
    }

    /// @note ambientColor: Lit モードでは AMBIENT_SCALE 相当値、Unlit 系では白に上書き
    lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
    if (rs.IsUnlit()) {
        lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity  = 0.0f;
        lightData.pointLightCount = 0;
        lightData.spotLightCount  = 0;
        /// @note 空も消灯する。分離前は lightIntensity=0 が空系シェーダーにも効いていたので、
        /// @note Unlit 表示で空が黒く落ちる従来の挙動を skyDimmer 側で維持する。
        lightData.skyDimmer       = 0.0f;
    }

    return output;
}
} /// @note namespace fbzz::scene
