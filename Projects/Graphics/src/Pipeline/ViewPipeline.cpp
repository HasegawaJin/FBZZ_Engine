/// @file    ViewPipeline.cpp
/// @brief   ビューの資源宣言と描画・ポスト処理パスの構成。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/Geometry/MeshTrailRenderPass.hpp>
#include <Graphics/Passes/Geometry/TrailRenderPass.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Core/Logger.hpp>
#include <algorithm>

namespace fbzz::renderer {
void BuildViewPipeline(RenderPipeline& pipeline, RenderPassContext& passCtx,
    RenderViewResources& viewTargets, RenderSharedResources& shared,
    const ViewPipelineOptions& options, const ViewPipelineExtensions& extensions)
{
    pipeline.BeginBuild();
    passCtx.rayPathViewActive = false;
    passCtx.ssrPassActive = false;
    passCtx.hybridReflectionResolveActive = false;
    passCtx.hybridReflectionSourcePass = false;
    passCtx.hybridReflectionSsrPlanned = false;
    if (!options.renderPlan.IsValid()) {
        FBZZ_LOG_ERROR("ViewPipeline: 実行できない描画構成です");
        return;
    }
    auto& resources = passCtx.resources;
    auto& passHandles = passCtx.handles;
    const auto& rs = passCtx.settings;
    const bool taaOverriddenOff = std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
        [](const RenderPassOverride& override) { return override.name == "TAA" && !override.enabled; });
    if (!rs.IsTaaActive() || taaOverriddenOff) {
        viewTargets.taaHistoryValid = false;
        viewTargets.taaProviderHistory.valid = false;
    }
    const bool rayDebugReady = PrepareRayDebugView(passCtx, viewTargets, shared);
    const bool rayReflectionReady = PrepareRayReflectionView(passCtx, viewTargets, shared,
        options.renderPlan.rasterPlan);
    const bool rayPathReady = PrepareRayPathView(passCtx, viewTargets, shared);
    const auto renderPlan = PrepareViewRenderPlan(resources, passCtx.renderer, rs, viewTargets, shared, passHandles,
        passCtx.experimentalRayTracingEnabled);
    passCtx.rayReflectionPassActive = rayReflectionReady && renderPlan.reflection.enabled;
    /// @note RT の縮退理由は保持し、使える Deferred 表面の SSR / IBL まで旧 HDR 合成へ戻さない。
    passCtx.hybridReflectionResolveActive = passCtx.experimentalRayTracingEnabled
        && rs.modeRequest.mode == RenderMode::HYBRID
        && renderPlan.rasterPlan.UsesDeferredLighting() && !rs.IsUnlit() && !rs.IsWireframe()
        && !IsRayDebugView(rs.viewMode);
    if (!passCtx.rayReflectionPassActive) {
        viewTargets.rayReflection.reconstruction.historyValid = false;
        viewTargets.rayReflection.reconstruction.prepared = false;
        passCtx.rayReflectionReconstructionPrepared = false;
    }
    if (!renderPlan.IsValid()) return;
    if (rayPathReady && renderPlan.effectiveMode == RenderMode::PATH_TRACING) {
        BuildRayPathViewPipeline(pipeline, passCtx, viewTargets, shared, extensions);
        return;
    }
    viewTargets.rayPath.history.Reset();
    const auto outputRT = passCtx.outputRT;
    const uint32_t nativeW = passCtx.outputWidth;
    const uint32_t nativeH = passCtx.outputHeight;
    const bool needsUpscale = viewTargets.needsUpscale;
    const auto& opaquePlan = renderPlan.rasterPlan;
    const bool screenSpaceReady = opaquePlan.HasScreenSpaceInputs();
    const bool ssrOverriddenOff = std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
        [](const RenderPassOverride& override) { return override.name == "SSR" && !override.enabled; });
    /// @note A persistent GBuffer is graph-readable after its producer is disabled, but cannot prove a new Hybrid surface or SSR receipt.
    const bool hybridGBufferOverriddenOff = passCtx.hybridReflectionResolveActive
        && std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
            [](const RenderPassOverride& override) { return override.name == "DeferredGBuffer" && !override.enabled; });
    const bool ssrAvailable = rs.ssr.enabled && screenSpaceReady && !ssrOverriddenOff && !hybridGBufferOverriddenOff
        && resources.Get(passHandles.ssrShader) && resources.Get(viewTargets.ssrResult)
        && resources.Get(viewTargets.gbuffer);
    const bool ssrSourceOverriddenOff = std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
        [](const RenderPassOverride& override) {
            return override.name == "ReflectionSourceLighting" && !override.enabled;
        });
    /// @note source を止めたフレームは未照明 HDR を SSR として採用せず、RT-only resolve に戻す。
    passCtx.hybridReflectionSsrPlanned = passCtx.hybridReflectionResolveActive
        && ssrAvailable && !ssrSourceOverriddenOff;
    const bool ssaoEnabled = passCtx.ssaoEnabled;
    const bool selectionOutlineEnabled = passCtx.selectionOutlineEnabled;
    const bool objectMaskEnabled = passCtx.objectMaskEnabled;
    const bool clusteredEnabled = renderPlan.clusteredLighting;
    const auto& customAfterOpaqueIndices = options.customAfterOpaqueIndices;
    const auto& customSceneHdrIndices = options.customSceneHdrIndices;
    const auto& customPostProcessIndices = options.customPostProcessIndices;
    const uint32_t punctualShadowRes = passCtx.punctualShadowResolution;
    auto& hdrRT                   = viewTargets.hdr;
    auto& ldrRT                   = viewTargets.ldr;
    auto& selectionMaskRT         = viewTargets.selectionMask;
    auto& outlineRT               = viewTargets.outline;
    auto& objectMaskRT           = viewTargets.objectMask;
    auto& customPostProcessRT     = viewTargets.customPostProcess;
    auto& upscaleSrcRT            = viewTargets.upscaleSrc;
    auto& gbufferRT               = viewTargets.gbuffer;
    auto& velocityRT              = viewTargets.velocity;
    auto& decalDepthRT            = viewTargets.decalDepth;
    auto& bloomHalf               = viewTargets.bloomHalf;
    auto& bloomFull               = viewTargets.bloomFull;
    auto& ssaoBlur                = viewTargets.ssaoBlur;
    auto& ssrResult               = viewTargets.ssrResult;
    auto& volumetricResult        = viewTargets.volumetricResult;
    auto& motionBlurResult        = viewTargets.motionBlurResult;
    auto& gtaoBlur                = viewTargets.gtaoBlur;
    auto& contactShadowResult     = viewTargets.contactShadowResult;
    uint32_t& sHdrW               = viewTargets.width;
    uint32_t& sHdrH               = viewTargets.height;
    auto& shadowMapRT = shared.shadowMapRT;
    auto& punctualShadowRT = shared.punctualShadowRT;
    auto& lightCookieRT = shared.lightCookieRT;
    /// @note エディターが編集した «このパスは載せない / これを待つ» をこのフレームへ効かせる。
    pipeline.SetPassOverrides(rs.passOverrides);
    pipeline.SetSchedulePolicy(rs.schedulePolicy);
    /// @note 申告を条件で組み立てるパス用。initializer_list には if を書けないので、
    /// @note 読むものが構成で変わるパスは vector を渡す。
    using RA = renderer::RenderGraph::ResourceAccess;
    using RU = renderer::RenderGraph::ResourceUsage;
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::BuildPipeline", "Rendering"));
    /// @note 論理リソースの宣言
    /// @note 申告 (依存解析に使う «形») と実体 (名前 → ハンドル) を同じ 1 行で渡す。宣言と登録を
    /// @note 分けると、片方だけ足しても Plan が通り «申告したのに実体が無い» が静かに成立してしまう。
    /// @note 登録簿は RenderPipeline がここから組み立てる。
    using RK = renderer::RenderGraph::ResourceKind;
    constexpr auto kResFormat = renderer::Format::RGBA16F;

    /// @note 内部解像度・フレーム内だけ生きる RT。違うのは MRT 枚数と深度の有無だけ。
    const auto declareViewTarget = [&](const char* name,
                                       renderer::ResourceHandle<renderer::RenderTargetTag> handle,
                                       uint32_t colorCount, bool withDepth) {
        pipeline.DeclareTarget(name, handle,
            { RK::RenderTarget, sHdrW, sHdrH, kResFormat, colorCount, withDepth, false, true });
    };
    /// @note CS 出力のテクスチャ。解像度以外の «形» は全部同じ。
    const auto declareViewTexture = [&](const char* name,
                                        renderer::ResourceHandle<renderer::TextureTag> handle,
                                        uint32_t width, uint32_t height) {
        pipeline.DeclareTexture(name, handle,
            { RK::Texture, width, height, kResFormat, 1, false, false, true });
    };

    /// @note Output だけは出力先そのものなので実寸で申告する (中間 RT は内部解像度)。
    pipeline.DeclareTarget("Output", outputRT,
        { RK::RenderTarget, nativeW, nativeH, kResFormat, 1, true, true, false });
    pipeline.DeclareTarget("ShadowMap", shadowMapRT,
        { RK::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, kResFormat, 0, true, false, false });
    pipeline.DeclareTarget("PunctualShadowMap", punctualShadowRT,
        { RK::RenderTarget, punctualShadowRes, punctualShadowRes, kResFormat, 0, true, false, false });
    pipeline.DeclareTarget("LightCookieAtlas", lightCookieRT,
        { RK::RenderTarget, kLightCookieAtlasWidth, kLightCookieAtlasHeight, kResFormat, 1, true, false, false });

    declareViewTarget("HDR",                hdrRT,                  1, true);
    declareViewTarget("LDR",                ldrRT,                  1, false);
    declareViewTarget("SelectionMask",      selectionMaskRT,        1, true);
    declareViewTarget("Outline",            outlineRT,              1, false);
    declareViewTarget("ObjectMask",         objectMaskRT,           1, true);
    declareViewTarget("Velocity",           velocityRT,             1, true);
    declareViewTarget("CustomPostProcess0", customPostProcessRT[0], 1, false);
    declareViewTarget("CustomPostProcess1", customPostProcessRT[1], 1, false);

    /// @note 実体は upscaleSrcRT。等倍のフレームは誰も触らないので申告もしない。
    const bool upscaleActive = needsUpscale && upscaleSrcRT.IsValid();
    if (upscaleActive)
        declareViewTarget("UpscaleSrc", upscaleSrcRT, 1, false);

    declareViewTexture("Bloom",            bloomFull,        sHdrW, sHdrH);
    declareViewTexture("SSRResult",        ssrResult,        sHdrW, sHdrH);
    declareViewTexture("MotionBlurResult", motionBlurResult, sHdrW, sHdrH);
    declareViewTexture("VolumetricResult", volumetricResult, sHdrW, sHdrH);

    /// @note Forward もプリパスで GBuffer へ描くので、ここを Deferred 限定にすると
    /// @note 「宣言されていないリソース」への書き込みになり RenderGraph の検証が落ちる。
    if (screenSpaceReady)
        pipeline.DeclareTarget("GBuffer", gbufferRT,
            { RK::RenderTarget, sHdrW, sHdrH, kResFormat, GBUFFER_COLOR_COUNT, true, false, false });

    /// @note AO と接触影は半解像度で持つ (実体は curW/2 x curH/2)。ここをフル解像度で
    /// @note 申告していると、エイリアシングが全画面 RT と同じ枠を貸してしまう。
    const uint32_t halfW = (std::max)(1u, sHdrW / 2);
    const uint32_t halfH = (std::max)(1u, sHdrH / 2);
    declareViewTexture("LensFlareSource", bloomHalf, halfW, halfH);
    if (ssaoEnabled)
        declareViewTexture("SSAO", ssaoBlur, halfW, halfH);
    /// @note GTAO / ContactShadows は GBuffer を読んで独自の UAV へ書く。専用名で宣言しないと
    /// @note GBuffer への偽書き込みとみなされ、DeferredLighting との依存順が崩れる。
    if (screenSpaceReady && rs.IsGtaoActive())
        declareViewTexture("GTAOResult", gtaoBlur, halfW, halfH);
    if (screenSpaceReady && rs.contactShadow.enabled)
        declareViewTexture("ContactShadowResult", contactShadowResult, halfW, halfH);
    pipeline.SetOutputs({ "Output" });

    /// @note IBL BRDF LUT 焼き付け。512x512 の積分テーブルはシーンにも設定にも依存しない定数なので、
    /// @note 毎フレーム呼ぶが実処理は世代追跡で初回のみ走る。
    pipeline.AddPass<IBLBrdfBakePass>();

    if (extensions.begin) extensions.begin();

    /// @note HDR の段 (AfterOpaque / SceneHDR) のカスタムパスを積む。どちらも hdrRT を
    /// @note 読んで書くので、違うのは «パイプラインのどこへ挿すか» だけ。
    /// @note マスクを reads に入れないと、グラフは «マスクを描く前に» このパスを走らせてよいことになる。
    /// @note 読む宣言をしていない効果でも同じ段に読む効果が混ざれば順序は共有されるので、
    /// @note 有無で分けずまとめて宣言する。
    auto appendCustomHdrPasses = [&](const char* label, const std::vector<uint32_t>& indices) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(indices.size()); ++i) {
            const uint32_t customIndex = indices[i];
            const auto body = [&, customIndex]() { ExecuteCustomHdrPass(passCtx, customIndex); };
            const std::string name = label + std::to_string(i);
            if (objectMaskEnabled)
                pipeline.AddRawPass(name, { "HDR", "ObjectMask" }, { "HDR" }, body);
            else
                pipeline.AddRawPass(name, { "HDR" }, { "HDR" }, body);
        }
    };

    BuildGeometryPreparation(pipeline, clusteredEnabled);
    BuildGeometryPipeline(pipeline, opaquePlan, [&]() {
        if (passCtx.rayReflectionPassActive)
            BuildRayReflectionPipeline(pipeline, viewTargets, shared, passCtx);
    }, passCtx.hybridReflectionSsrPlanned);
    BuildWaterComposition(pipeline);

    if (extensions.setup) extensions.setup();

    if (extensions.userPasses) extensions.userPasses(UserRenderPassInjectionPoint::AfterOpaque);

    /// @note オブジェクトマスク。描くのはジオメトリなので不透明が出揃ったここで済ませる。
    /// @note 半透明より前でよい: マスクの中身は «不透明の形と、その時点の深度» で決まり、
    /// @note 深度を書かない半透明を待っても結果は変わらない。ここより後ろへ置くと、
    /// @note AfterOpaque 段のカスタムパスがマスクを読めなくなる (順序が閉じない)。
    pipeline.AddPass<ObjectMaskPass>();

    /// @note AfterOpaque 段のユーザーシェーダー
    /// @note 背景だけが描かれていて、デカール・トレイル・パーティクル・半透明はまだ乗っていない。
    /// @note 画面を歪める効果をこの後 (SceneHDR) に置くと、既に描かれたパーティクルごと曲がって
    /// @note «エフェクトだけ別の場所に居る» 絵になる。
    appendCustomHdrPasses("CustomAfterOpaque", customAfterOpaqueIndices);

    /// @note デカール用深度スナップショット
    /// @note 深度専用 RT (colorCount = 0)。カラーを持つ RT と貸し回してはいけない。
    declareViewTarget("DecalDepth", decalDepthRT, 0, true);
    pipeline.AddPass<DecalDepthCopyPass>();

    /// @note Decal + Trail + Particle
    pipeline.AddPass<DecalPass>();

    pipeline.AddPass<MeshTrailRenderPass>();
    pipeline.AddPass<TrailRenderPass>();

    /// @note ShadowMap は粒子の自己影が読む (t8)。申告していないと影より前に走れてしまう。
    /// @note 粒子より前に登録するのは、粒子が同じフレームの霧を読んで «自分の奥行きの霧» を逆算する
    /// @note (ParticleLighting.hlsli の ApplyParticleFog) ため。依存を申告しあわない 2 パスは登録順に並ぶ。
    pipeline.AddPass<FroxelFogPass>();

    /// @note PunctualShadowMap / LightCookieAtlas は «点光源を受ける» .mat の粒子が読む (ParticleLighting.hlsli)。
    pipeline.AddPass<ParticlePass>();

    /// @note Overdraw 可視化は診断表示。有効なときだけ Particle の直後に HDR を上書きする。
    /// @note GPU 時間を Particle パスの実測値と混ぜないよう、別パスとして計測させる。
    pipeline.AddPass<ParticleOverdrawPass>();

    /// @note TAA の反応マスク。HDR へは書かないが、Particle の後・Composite (→ TAA) の前に並べるために
    /// @note HDR の書き手として申告する (Overdraw と同じ申告の仕方)。
    pipeline.AddPass<ParticleReactivePass>();

    if (extensions.userPasses) extensions.userPasses(UserRenderPassInjectionPoint::AfterTransparent);

    /// @note Selection / Debug
    if (extensions.selectionMask) extensions.selectionMask();

    /// @note SceneHDR 段のユーザーシェーダー
    /// @note 絵が出揃っていて、まだブルームにも露出にも触れていない唯一の場所。
    /// @note Bloom より後ろへ置くと «光っているのに滲まない»、AutoExposure より後ろへ
    /// @note 置くと «明るくしたのに露出が反応しない» という、段を選べる意味が消える並びになる。
    /// @note デバッグ描画より前なのは、ギズモを効果で歪ませないため。
    appendCustomHdrPasses("CustomSceneHDR", customSceneHdrIndices);

    /// @note 自動露出はデバッグ描画より前。測るのは «シーンの明るさ» で、グリッド・ギズモ・
    /// @note コライダー・NavMesh は Scene View にしか無い。後ろへ置くと Scene View だけ露出が
    /// @note Game View と食い違う (RenderGraph は登録順より前の書き手を読み手の世代とする)。
    /// @note MotionBlur / LensFlare より前なのは、フレアを測光へ入れると «明るい→露出が下がる→
    /// @note フレアが弱る» の輪ができるため。Bloom は HDR を書かないので位置に関わらず同じ。
    /// @note 出力は StructuredBuffer (Composite が t29 で読む) で論理リソースに乗らないため、
    /// @note FroxelFog と同じ理由でカリング対象から外す。
    pipeline.AddPass<AutoExposurePass>();

    if (extensions.depthDebug) extensions.depthDebug();

    if (extensions.userPasses) extensions.userPasses(UserRenderPassInjectionPoint::BeforePostProcess);

    /// @note モーションベクター
    /// @note TAA とモーションブラーは深度再投影だけでは「カメラの動き」しか復元できない。
    /// @note 不透明ジオメトリの実際の移動量を専用 RT へ描いて両者へ供給する。
    /// @note 消費側が 1 つも無いフレームは丸ごと省く (不透明をもう一度ラスタライズするため)。
    const bool velocityNeeded =
        velocityRT.IsValid() && (rs.motionBlur.enabled || rs.IsTaaActive());
    if (velocityNeeded) {
        pipeline.AddPass<VelocityPass>();
    }

    /// @note PostProcess チェーン
    /// @note MotionBlur CS — HDR 空間で計算し motionBlurResult へ書く (Composite が hdrRT の代わりに読む)。
    /// @note Bloom の前に走らせるので blur 後の輝度が Bloom に乗る。
    if (rs.motionBlur.enabled) {
        /// @note Velocity は誰も書かないフレームがある。書かれないものを読むと申告した瞬間に
        /// @note «producer が居ない» で Plan が落ちるので、要るときだけ足す。
        std::vector<RA> motionBlurAccesses = { { "MotionBlurResult", RU::Write }, { "HDR", RU::ReadWrite } };
        if (velocityNeeded) motionBlurAccesses.push_back({ "Velocity", RU::Read });
        pipeline.AddRawPass("MotionBlur", std::move(motionBlurAccesses), [&]() {
            ExecuteMotionBlurPass(passCtx);
        });
    }
    /// @note LensFlare PS — 輝度抽出した光源を ADDITIVE で HDR へ合成する。
    /// @note Bloom の前に置くのでフレアも Bloom に乗るが、その順序では bloomHalf に今フレームの
    /// @note 輝点がまだ無い。パス自身が bloomHalf へ焼いてから読む (LensFlareSource がこの出力)。
    pipeline.AddPass<LensFlarePass>();
    pipeline.AddPass<BloomPass>();

    /// @note フロクセル霧。シャドウマップを読むので Shadow より後、Composite より前。
    /// @note 霧はライトとシャドウだけから作るので HDR の完成を待つ必要はない。
    /// @note 無効でも積むのは、パス側が b13 へ「無効」を書き戻さないと前フレームの定数が残り
    /// @note 画面が真っ黒になるため (FroxelFogPass 参照)。
    /// @note 出力先の 3D ボリュームは論理リソースに乗らないので、writes が空でもカリングさせない。

    const bool customPostProcessEnabled =
        !customPostProcessIndices.empty() &&
        customPostProcessRT[0].IsValid() &&
        customPostProcessRT[1].IsValid();
    /// @note hasPostCompositeEffects: Composite の出力先が "LDR" かチェーン終端かを決める。
    /// @note このフラグが true なら Composite は ldrRT に書き、後続エフェクトがチェーンを形成する。
    const bool hasPostCompositeEffects =
        rs.IsTaaActive() || customPostProcessEnabled || selectionOutlineEnabled || rs.postProcess.fxaaEnabled;

    /// @note ここが «Composite がどこへ書くか» の唯一の正本。グラフへの申告 (下の writes) と
    /// @note パスが実際に束縛するハンドルを、同じ 1 つの判定から配る。
    passCtx.compositeOutputRT = hasPostCompositeEffects ? ldrRT : passCtx.chainOutputRT;

    /// @note LDR チェーンの終端リソース。passCtx.chainOutputRT の «グラフ側の名前» で、
    /// @note 実寸へ引き伸ばすのは UpscalePass だけという対応を保つ。
    const char* const chainOutRes = upscaleActive ? "UpscaleSrc" : "Output";

    {
        /// @note Bloom を読むのは bloom.enabled のときだけ (CompositePass の bloomWritten と同条件)。
        std::vector<RA> compositeAccesses = {
            { "HDR", RU::Read },
            { hasPostCompositeEffects ? "LDR" : chainOutRes, RU::Write },
        };
        if (rs.postProcess.bloom.enabled) compositeAccesses.push_back({ "Bloom", RU::Read });
        if (!passCtx.hybridReflectionResolveActive && ssrAvailable)
            compositeAccesses.push_back({ "SSRResult", RU::Read });
        if (passCtx.rayReflectionPassActive) compositeAccesses.push_back({ "RayReflectionResult", RU::Read });
        if (passCtx.rayReflectionReconstructionPrepared) compositeAccesses.push_back({ "RayReflectionRaw", RU::Read });
        pipeline.AddRawPass("Composite", std::move(compositeAccesses), [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    /// @note Post-composite チェーン
    /// @note ppCurrent は「LDR 空間の最新フレームを持つリソース名」。これを進めるだけで
    /// @note TAA/CustomPP/SelectionOutline/FXAA の任意の組み合わせが 1 本の直列チェーンになる。
    std::string ppCurrent  = hasPostCompositeEffects ? "LDR" : chainOutRes;
    /// @note customPostProcessRT の ping-pong インデックス
    int         ppPingPong = 0;

    /// @note TAA — 最初に適用することで後続の CustomPP/SelectionOutline が TAA 済み映像に乗る。
    /// @note 登録順が RenderGraph のタイブレークになる (Kahn's algorithm)。
    if (rs.IsTaaActive()) {
        const auto taaBody = [&]() {
            PrepareTaaProviderHistory(passCtx, viewTargets);
            ExecuteTAAPass(passCtx);
            viewTargets.taaHistoryValid = true;
            /// @note taaFlip は ExecuteTAAPass 内で反転済み — 反転後のフラグで「書いた方」を特定する。
            auto& taaOut = passHandles.taaFlip ? passHandles.taaHistoryB : passHandles.taaHistoryA;
            passHandles.fxaaInput = resources.GetColorTexture(taaOut, 0);
            /// @note TAA 後は履歴バッファが最新フレーム。更新しないと後続が TAA 前の ldrRT を読む。
            passHandles.postProcessInput = passHandles.fxaaInput;
            /// @note LDR の論理的な最新世代は履歴 RT に移る。診断も後続パスも同じ実体を引く。
            passCtx.resourceRegistry.BindTarget("LDR", taaOut);
        };
        /// @note MotionBlur と同じ理由で Velocity は要るときだけ足す。
        /// @note HDR は深度 (t7) を読むための申告。速度の無い画素の再投影に使う。
        std::vector<RA> taaAccesses = { { ppCurrent, RU::ReadWrite }, { "HDR", RU::Read } };
        if (velocityNeeded) taaAccesses.push_back({ "Velocity", RU::Read });
        pipeline.AddRawPass("TAA", std::move(taaAccesses), taaBody);
    }

    /// @note Custom PostProcess チェーン
    for (uint32_t i = 0; i < static_cast<uint32_t>(customPostProcessIndices.size()); ++i) {
        const uint32_t customIndex  = customPostProcessIndices[i];
        const bool     isLastEffect = (i + 1 == static_cast<uint32_t>(customPostProcessIndices.size()))
                                       && !selectionOutlineEnabled
                                       && !rs.postProcess.fxaaEnabled;
        /// @note outputIndex == 2 → ExecuteCustomPostProcessPass が ctx.outputRT に直書きする規約
        const uint32_t    outputIndex = isLastEffect ? 2u : static_cast<uint32_t>(ppPingPong % 2);
        const std::string outRes      = isLastEffect
            ? chainOutRes
            : ("CustomPostProcess" + std::to_string(outputIndex));
        const auto customBody = [&, customIndex, outputIndex]() {
            ExecuteCustomPostProcessPass(passCtx, customIndex, outputIndex);
        };
        /// @note 輪郭マスクはユーザーシェーダーの入力になりうる。読むと宣言しないと、
        /// @note グラフはマスクを描く前にこのパスを走らせてよいことになる (reads が
        /// @note initializer_list なので MotionBlur と同じく 2 通りに分ける)。
        if (objectMaskEnabled) {
            pipeline.AddRawPass(
                "CustomPostProcess" + std::to_string(i),
                { ppCurrent, "ObjectMask" }, { outRes }, customBody);
        } else {
            pipeline.AddRawPass(
                "CustomPostProcess" + std::to_string(i),
                { ppCurrent }, { outRes }, customBody);
        }
        ppCurrent = outRes;
        ++ppPingPong;
    }

    /// @note SelectionOutline
    if (selectionOutlineEnabled) {
        const bool        isLastEffect = !rs.postProcess.fxaaEnabled;
        const std::string outRes       = isLastEffect ? chainOutRes : "Outline";
        /// @note HDR は輪郭の深度比較が読む (t7)。
        pipeline.AddRawPass("SelectionOutline",
            { ppCurrent, "SelectionMask", "HDR" },
            { outRes },
            [execute = extensions.selectionOutline](PassResources& res) { if (execute) execute(res); });
        ppCurrent = outRes;
    }

    /// @note FXAA
    if (rs.postProcess.fxaaEnabled) {
        pipeline.AddRawPass("FXAA", { ppCurrent }, { chainOutRes }, [&]() { ExecuteFxaaPass(passCtx); });
        ppCurrent = chainOutRes;
    }

    /// @note TAA_Blit — TAA は ping-pong 履歴にしか書かないので、後続エフェクトが 1 つも無いときは
    /// @note ppCurrent が "LDR" のまま残る。ここでチェーン終端へ届ける。
    if (ppCurrent != chainOutRes) {
        pipeline.AddRawPass("TAA_Blit", { ppCurrent }, { chainOutRes }, [&]() {
            ExecuteTAABlitPass(passCtx);
        });
        ppCurrent = chainOutRes;
    }

    /// @note デバッグ線 (深度なし) は LDR チェーンの終端へ、Upscale より前に内部解像度で重ねる。
    /// @note HDR に描くと MotionBlur / Bloom / LensFlare / 露出 / トーンマップ / TAA で線の色と形が変わる。
    /// @note 終端 RT はシーン深度を持たないので、深度テストが要る線は上の HDR 段に残してある。
    /// @note 同じ描き先を ReadWrite するので登録順がそのまま描画順。コライダーは破線で最後に描き、
    /// @note 同じ形のスクリプト Gizmo (実線) と重なっても両方読めるようにする。
    if (rayDebugReady) BuildRayDebugPipeline(pipeline, viewTargets, shared, resources, chainOutRes);
    if (extensions.overlayDebug) extensions.overlayDebug(chainOutRes);

    /// @note Upscale — 内部解像度で仕上がった絵を出力先の実寸へ解像する。
    /// @note UI より «前» に置くのが要点。後ろに回すと UI まで引き伸ばされて滲む。
    if (upscaleActive) {
        pipeline.AddRawPass("Upscale", { ppCurrent }, { "Output" }, [&]() {
            ExecuteUpscalePass(passCtx);
        });
    }

    if (extensions.ui) extensions.ui();
    profiler::Profiler::EndSample();
}
}
