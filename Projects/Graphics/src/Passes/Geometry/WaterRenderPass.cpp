/// @file    WaterRenderPass.cpp
/// @brief   WaterComponent → GPU 水面メッシュ・泡マスク・波紋テクスチャ生成と描画 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Effects/WaterGrid.hpp>
#include "WaterNoiseBake.hpp"
#include <algorithm>
#include <cmath>
namespace fbzz::renderer {
namespace {
bool AabbVisible(const math::Frustum& frustum, const math::Matrix4& world,
                 const math::Vector3& localMin, const math::Vector3& localMax)
{
    const math::Vector3 localCenter = {
        (localMin.x + localMax.x) * 0.5f,
        (localMin.y + localMax.y) * 0.5f,
        (localMin.z + localMax.z) * 0.5f,
    };
    const math::Vector3 localExtents = {
        (localMax.x - localMin.x) * 0.5f,
        (localMax.y - localMin.y) * 0.5f,
        (localMax.z - localMin.z) * 0.5f,
    };

    const math::Vector4 center =
        world * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    /// @note 回転した箱を軸並行で包み直す = |M| を half-extent に掛ける。
    auto projectRow = [&](int row) {
        return std::abs(world.m[row][0]) * localExtents.x
             + std::abs(world.m[row][1]) * localExtents.y
             + std::abs(world.m[row][2]) * localExtents.z;
    };
    const math::Vector3 extents = { projectRow(0), projectRow(1), projectRow(2) };
    return frustum.IntersectsAABB({ center.x, center.y, center.z }, extents);
}

/// @note 波のマージンを乗せたローカル AABB。チャンクにも水面全体にも同じ広げ方をする。
void ExpandByWaterWaveMargin(const WaterWaveMargin& margin, math::Vector3& outMin, math::Vector3& outMax)
{
    outMin.x -= margin.horizontal;
    outMin.z -= margin.horizontal;
    outMax.x += margin.horizontal;
    outMax.z += margin.horizontal;
    outMin.y -= margin.vertical;
    outMax.y += margin.vertical;
}

/// @brief 頂点シェーダーと同じ有限格子の写像をチャンクの境界へ適用する。
/// @note 格子の集中はビューごとに異なる。抽出時の等間隔 AABB では近景チャンクを誤って捨てる。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-18-using-vertex-texture-displacement GPU Gems 2 Chapter 18, camera-centered grid
void FocusWaterChunkBounds(const RenderWaterInput& input, const math::Vector2& focus,
                           math::Vector3& outMin, math::Vector3& outMax)
{
    if (input.constants.gridParams.x <= 0.5f) return;
    const auto& world = input.constants.worldMatrix;
    const math::Vector3 axisX = { world.m[0][0], world.m[1][0], world.m[2][0] };
    const math::Vector3 axisZ = { world.m[0][2], world.m[1][2], world.m[2][2] };
    auto mapAxis = [&](float minimum, float maximum, float extent, const math::Vector3& axis,
                       uint32_t resolution, float axisFocus) {
        const float axisScale = (std::max)(axis.Length(), 1.0e-4f);
        const float nearCellSize = input.constants.gridParams.y / axisScale;
        /// @note CPU/GPU の積和の丸めで中心の量子化が 1 セル違っても、見えるチャンクを捨てない。
        return math::Vector2{
            WaterGridAxisPosition(minimum / extent + 0.5f, extent, resolution, axisFocus, nearCellSize) - nearCellSize,
            WaterGridAxisPosition(maximum / extent + 0.5f, extent, resolution, axisFocus, nearCellSize) + nearCellSize
        };
    };
    const auto x = mapAxis(outMin.x, outMax.x, input.aabbMax.x - input.aabbMin.x, axisX,
                           static_cast<uint32_t>(input.constants.gridParams.z), focus.x);
    const auto z = mapAxis(outMin.z, outMax.z, input.aabbMax.z - input.aabbMin.z, axisZ,
                           static_cast<uint32_t>(input.constants.gridParams.w), focus.y);
    outMin.x = x.x; outMax.x = x.y;
    outMin.z = z.x; outMax.z = z.y;
}

}
const WaterDetailNoise& GetWaterDetailNoise(renderer::ResourceManager& resources)
{
    static WaterDetailNoise s_noise;
    static uint64_t s_bakedAt = 0xFFFFFFFFFFFFFFFFull;

    /// @note 焼き直しを Reset に紐づける。デバイスロストで実体が消えてもハンドルは残るため、
    /// @note 世代が変わったときだけ焼き直せば失敗時に毎フレーム焼き続けずに済む。
    const uint64_t resetVersion = resources.GetResetVersion();
    if (s_bakedAt == resetVersion)
        return s_noise;
    s_bakedAt = resetVersion;

    const waternoise::Tile tile = waternoise::BakeDetailTile();
    std::vector<renderer::TextureMipData> mips;
    mips.reserve(tile.mips.size());
    for (size_t level = 0; level < tile.mips.size(); ++level)
        mips.push_back({ tile.mips[level].data(), tile.sizes[level], tile.sizes[level] });

    s_noise.texture = resources.CreateTextureWithMips(mips.data(), static_cast<uint32_t>(mips.size()));
    s_noise.derivativeScale = tile.derivativeScale;
    s_noise.invTileCells = 1.0f / static_cast<float>(waternoise::kTileCells);
    return s_noise;
}
void WaterSelectionMaskSystem(RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;
    auto& h = ctx.handles;
    if (!h.selectionMaskShader.IsValid() || !h.selectionMaskPSO.IsValid()) return;

    if (!ctx.renderScene) return;
    for (const auto& input : ctx.renderScene->water) {
        if (!input.selected || input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        const auto& world = input.constants.worldMatrix;
        PerObjectCB objData{};
        objData.world             = world;
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        ctx.resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));
        for (const auto& chunk : input.patches) {
            renderer::DrawCall dc;
            dc.vertexBuffer       = chunk.vertexBuffer;
            dc.indexBuffer        = chunk.indexBuffer;
            dc.indexCount         = chunk.indexCount;
            dc.shader             = h.selectionMaskShader;
            dc.pipelineState      = h.selectionMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            ctx.renderer.Submit(dc, ctx.resources);
        }
    }
}

/// @name IRenderPass

std::string_view WaterRenderPass::Name() const { return "WaterForward"; }

void WaterRenderPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note 平行光の影 (t9) に加え、BindForwardShadingResources が Spot/Point の影 (t28) と
    /// @note Cookie (t31) を束縛する。申告しないと Shadow / LightCookie より先に走ってよいことになる。
    ///
    /// @note 屈折用のシーンカラー / 深度のコピーはこのパスの中で作って読み切る作業用で、
    /// @note 他のパスからは見えない。元の HDR は下の ReadWrite で押さえてある。
    /// @note SetAutoTarget は呼ばない。屈折用のコピーを作る間に束縛を 3 回切り替えるので、
    /// @note 描き先は Execute の中で自分で張る。
    builder.ReadWrite("HDR").Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
}
void WaterRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::IRenderer& renderer = ctx.renderer;
    renderer::ResourceManager& resources = ctx.resources;
    const renderer::Camera& camera = ctx.camera;
    const renderer::RenderSettings* settings = &ctx.settings;
    const auto lightCB = ctx.handles.lightCB;
    const auto shadowDepthTexture = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    const auto shadowCB = ctx.handles.shadowCB;
    const float elapsedTime = ctx.time;

    /// @note static ローカルは初回のみ初期化される。`ResourceManager::Reset()` で世代が
    /// @note 変わった場合だけ再生成し、旧ハンドル (失効済み) へのアクセスを防ぐ。
    static uint64_t s_resetVersion = resources.GetResetVersion();
    static auto waterShader = resources.LoadShader("Assets/Shaders/Water/Water.hlsl");
    /// @note 半透明だが深度書き込みありで描く。水面は 1 枚の面で重なるのは自分自身だけなので、
    /// @note 書き込みを切るとチャンクの submit 順 (-Z→+Z の行優先) がそのままブレンド順に
    /// @note なり、+Z 向きカメラでは奥のチャンクが後から手前へ上塗りされ、向こう側の縁や
    /// @note うねりの裏面 (SOLID_NOCULL で描かれる) が手前の水面に線となって浮く。深度を
    /// @note 書けば提出順に関係なく一番手前の水面だけが残る。水中の向こうの不透明物は屈折用
    /// @note シーンカラーコピーから引いており、深度を書いても水底は見えたままになる。
    static auto waterPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto waterWireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto cameraCBH = resources.CreateConstantBuffer(288);
    static auto waterCBH  = resources.CreateConstantBuffer(sizeof(WaterCB));
    static auto defaultEffectCBH = [&] {
        WaterEffectParams defaults{};
        auto h = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
        resources.Update(h, &defaults, sizeof(WaterEffectParams));
        return h;
    }();
    static auto waterEffectCBH = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
    static auto blackTex = [&] {
        const uint8_t b[4] = { 0, 0, 0, 255 };
        return resources.CreateTexture(b, 1, 1);
    }();
    /// @note B は «高さ 0» の 128。255 のままだと波紋が 1 枚も無い水面の頂点が
    /// @note kWaterRippleHeightScale ぶん持ち上がる。
    static auto neutralRippleTex = [&] {
        const uint8_t r[4] = { 128, 128, 128, 255 };
        return resources.CreateTexture(r, 1, 1);
    }();

    /// @note 水面の屈折用にシーンカラーをコピーする。Water シェーダーが屈折で HDR をシーン
    /// @note カラーとして読むため、hdrRT を出力 RT として束ねる前にコピーする必要がある
    /// @note (DX11 は同時読み書きを禁止する)。
    static auto copyColorShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    static auto depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    if (s_resetVersion != resources.GetResetVersion()) {
        s_resetVersion    = resources.GetResetVersion();
        waterShader       = resources.LoadShader("Assets/Shaders/Water/Water.hlsl");
        waterPSO          = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_ON });
        waterWireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        cameraCBH         = resources.CreateConstantBuffer(288);
        waterCBH          = resources.CreateConstantBuffer(sizeof(WaterCB));
        defaultEffectCBH  = [&] { WaterEffectParams d{}; auto h = resources.CreateConstantBuffer(sizeof(WaterEffectParams)); resources.Update(h, &d, sizeof(WaterEffectParams)); return h; }();
        waterEffectCBH    = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
        blackTex          = [&] { const uint8_t b[4] = {   0,   0,   0, 255 }; return resources.CreateTexture(b, 1, 1); }();
        neutralRippleTex  = [&] { const uint8_t r[4] = { 128, 128, 128, 255 }; return resources.CreateTexture(r, 1, 1); }();
        copyColorShader   = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        depthCopyShader   = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    }
    /// @note カリング錐台はジッター無しのまま。半ピクセルのために可視判定を揺らす意味がない。
    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());

    /// @note 描画用の行列だけ TAA ジッターを乗せる。乗せないと水面だけ AA が効かず、
    /// @note b0 経由で描く不透明物とサブピクセルずれた深度になって TAA の再投影が濁る。
    const math::Matrix4 jitteredProj =
        MakeJitteredProjection(camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    const math::Matrix4 jitteredVP = jitteredProj * camera.GetViewMatrix();

    if (!ctx.renderScene) return;
    bool hasVisibleWater = false;
    for (const auto& input : ctx.renderScene->water) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        auto low = input.aabbMin, high = input.aabbMax;
        ExpandByWaterWaveMargin(input.margin, low, high);
        if (AabbVisible(frustum, input.constants.worldMatrix, low, high)) { hasVisibleWater = true; break; }
    }
    if (!hasVisibleWater) return;
    /// @note 作業 RT はビューが持つ (理由は RenderPassHandles::waterSceneColorRT を参照)。
    if (!ctx.handles.waterSceneColorRT || !ctx.handles.waterSceneDepthRT) return;
    renderer::SizedRenderTarget& sceneColorRT = *ctx.handles.waterSceneColorRT;
    renderer::SizedRenderTarget& sceneDepthRT = *ctx.handles.waterSceneDepthRT;

    (void)sceneColorRT.Ensure(resources, ctx.width, ctx.height, 1);
    (void)sceneDepthRT.Ensure(resources, ctx.width, ctx.height, renderer::CameraDepthTargetDesc(0));
    renderer.SetRenderTarget(sceneColorRT, resources);
    if (copyColorShader.IsValid()) {
        renderer::DrawCall copyDC;
        copyDC.shader = copyColorShader;
        copyDC.pipelineState = ctx.handles.postprocPSO;
        copyDC.vertexCount = 3;
        copyDC.textures[5] = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
        renderer.Submit(copyDC, resources);
    }
    const auto sceneColor = resources.GetColorTexture(sceneColorRT, 0);

    /// @note HDR の depth を Water 専用の深度 RT へコピーし、PS ではその SRV (t5) を読む。
    /// @note hdrRT を RTV/DSV として使いながら同じ depth を SRV(t5) で読むと DX11 の
    /// @note read/write 競合で SRV が解除され、背景判定・水深・泡が破綻する。
    renderer.SetRenderTarget(sceneDepthRT, resources);
    renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
        renderer.Submit(depthDC, resources);
    }
    const auto sceneDepth = resources.GetDepthTexture(sceneDepthRT);

    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    /// @note しぶきの GameObject は UpdateWaterSplashes (WaterSystem) が作って消す。
    /// @note 描画中にシーンを書き換えると、後続パスが握るエミッターがすり替わる。

    {
        struct CameraCB {
            math::Matrix4 view;
            math::Matrix4 projection;
            math::Matrix4 viewProjection;
            math::Matrix4 invViewProjection;
            math::Vector3 cameraPos;
            float nearZ;
            float farZ;
            /// @note LAYOUT: PerFrameCB / Constants.hlsli の CameraConstants と一致させること
            /// @note (waterSsrEnabled は共通側で _reserved になっている枠)。
            float waterSsrEnabled;
            float isOrthographic;
            /// @note Water 専用 b0 の末尾。0=通常、1=Wireframe Lit、2=Wireframe Unlit。
            float wireframeMode;
        };
        static_assert(sizeof(CameraCB) == 288, "CameraCB size mismatch");

        CameraCB camData{};
        camData.view = camera.GetViewMatrix();
        camData.projection = jitteredProj;
        camData.viewProjection = jitteredVP;
        camData.invViewProjection = math::Matrix4::Inverse(jitteredVP);
        camData.cameraPos = camera.m_position;
        camData.nearZ = camera.m_near;
        camData.farZ = camera.m_far;
        camData.waterSsrEnabled = (ctx.isDeferred && ctx.settings.ssr.enabled) ? 1.0f : 0.0f;
        camData.isOrthographic  =
            camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f : 0.0f;
        camData.wireframeMode = ctx.settings.IsWireframe() ? (ctx.settings.IsUnlit() ? 2.0f : 1.0f) : 0.0f;
        resources.Update(cameraCBH, &camData, sizeof(camData));
    }

    /// @note 輪の寿命は WaterSystem が進める。ここで進めると SceneView と GameView で
    /// @note 2 回進み、ビューを 2 つ開いた瞬間に波紋が倍速で消える。

    for (const auto& input : ctx.renderScene->water) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        const auto& waterWorld = input.constants.worldMatrix;
        const auto& margin = input.margin;
        auto cb = input.constants;
        math::Vector2 focus{};
        if (cb.gridParams.x > 0.5f) {
            /// @note 上空では視線が当たる水面へ集中する。足元だけを細かくすると俯瞰の画面中央が粗いままになる。
            focus = ResolveWaterGridFocus(waterWorld, camera.m_position, camera.GetForward(),
                { cb.gridParams.z, cb.gridParams.w }, cb.gridParams.y);
            cb.waveShapeParams.z = focus.x;
            cb.waveShapeParams.w = focus.y;
        }
        cb.wvpMatrix = jitteredVP * waterWorld;
        if (!ctx.handles.iblPrefilter.IsValid()) cb.reflectParams.x = 0;
        resources.Update(waterCBH, &cb, sizeof(cb));
        auto effectCBH = defaultEffectCBH;
        if (waterEffectCBH.IsValid()) { resources.Update(waterEffectCBH, &input.effects, sizeof(input.effects)); effectCBH = waterEffectCBH; }
        const auto depthTex = sceneDepth;
        const auto colorTex = sceneColor.IsValid() ? sceneColor : blackTex;
        const auto rippleTex = input.ripple.IsValid() ? input.ripple : neutralRippleTex;
        const auto activeShader = input.shader.IsValid() ? input.shader : waterShader;
        for (const auto& chunk : input.patches) {
            /// @note 水面チャンクも地形と同様、独立にカリングされる描画候補として数える。
            ++ctx.statsTotalObjects;
            /// @note 地形チャンクと同様、カメラの Frustum Culling を切っている間は落とさない。
            math::Vector3 chunkMin = chunk.aabbMin;
            math::Vector3 chunkMax = chunk.aabbMax;
            FocusWaterChunkBounds(input, focus, chunkMin, chunkMax);
            ExpandByWaterWaveMargin(margin, chunkMin, chunkMax);
            if (ctx.frustumCullingEnabled &&
                !AabbVisible(frustum, waterWorld, chunkMin, chunkMax)) {
                ++ctx.statsFrustumCulled;
                continue;
            }

            renderer::DrawCall call;
            call.vertexBuffer = chunk.vertexBuffer;
            call.indexBuffer  = chunk.indexBuffer;
            call.shader       = activeShader;
            call.pipelineState = (settings && settings->IsWireframe()) ? waterWireframePSO : waterPSO;
            call.indexCount   = chunk.indexCount;
            call.layer        = renderer::RenderLayer::TRANSPARENT_LAYER;
            call.topology     = renderer::PrimitiveTopology::TRIANGLE_LIST;
            call.constantBuffers[0] = cameraCBH;
            call.constantBuffers[1] = waterCBH;
            call.constantBuffers[2] = effectCBH;
            call.constantBuffers[3] = lightCB;
            call.constantBuffers[4] = shadowCB;
            call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
            BindForwardShadingResources(call, ctx);
            call.textures[3]  = input.foam;
            /// @note さざ波タイル (全水面で共有)
            call.textures[4]  = input.detailNoise;
            call.textures[5]  = depthTex;
            call.textures[6]  = colorTex;
            call.textures[8]  = rippleTex;
            call.textures[9]  = shadowDepthTexture;
            /// @note TEX_IBL_PREFILTER: 空反射
            call.textures[17] = ctx.handles.iblPrefilter;
            /// @note TEX_VELOCITY_FIELD: 焼いた速度場のアトラス。場が 1 枚も無くても **必ず束縛する**
            /// @note — DX12 の null ディスクリプタは Texture2D 固定で、Texture3D を宣言した
            /// @note スロットを空にすると次元が食い違う。
            call.textures[26] = input.velocityField;
            SubmitCounted(ctx, call);
        }
    }
}

}
