/// @file    RenderResources.hpp
/// @brief   描画共有資源とビュー別の履歴・中間ターゲットの寿命管理。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/ResolvedRenderPlan.hpp>
#include <Graphics/Pipeline/EnvironmentResources.hpp>
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <unordered_map>

namespace fbzz::renderer {
/// @note GPU のボーンパレット契約。Engine の Skeleton と静的検証する。
inline constexpr int RENDER_SKINNING_BONES = 128;
/// @note RGBA16F: albedo / roughness、normal / metallic、線形 HDR emission / typed glass marker。
inline constexpr uint32_t GBUFFER_COLOR_COUNT = 3;

/// @note 照明・反射合成方式と実記録・連続フレームを追跡する。画素別の反射 motion 履歴ではない。
struct TaaProviderHistory {
    RenderMode mode = RenderMode::RASTER;
    bool reflectionActive = false;
    bool reflectionResolveActive = false;
    bool screenReflectionActive = false;
    bool valid = false;
    uint64_t frameStamp = 0;
    uint64_t epoch = 0;
};

/// @note Viewport ごとに解像度依存の中間リソースを保持する。
/// @note static で共有すると SceneView と GameView が 1 フレーム内でリサイズし合う。
struct RenderViewResources {
    /// @note View ごとの RenderGraph 計画と transient RT をフレーム間で保持する。
    /// @note スタック生成だと DX12 の descriptor heap を毎フレーム作り直し、CPU が詰まる。
    RenderPipeline pipeline;
    uint32_t nativeWidth = 0;
    uint32_t nativeHeight = 0;
    bool needsUpscale = false;
    ResourceHandle<RenderTargetTag> output;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdr;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldr;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> outline;
    /// @note ランタイム輪郭のシルエット (RGB=色 / A=太さ)。エディタ選択のマスクとは別物。
    renderer::ResourceHandle<renderer::RenderTargetTag> objectMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcess[2];
    /// @note ポストプロセスチェーンの終着点。描画スケールが等倍でないフレームだけ持ち、
    /// @note UpscalePass がここから出力先の実寸へ解像する。等倍なら確保しない。
    renderer::ResourceHandle<renderer::RenderTargetTag> upscaleSrc;
    renderer::ResourceHandle<renderer::RenderTargetTag> gbuffer;
    /// @note モーションベクター (RG=速度, B=書き込み済みフラグ)。TAA / MotionBlur が有効な
    /// @note フレームだけ描く。深度は自前で持つので、本描画の深度バッファとは共有しない。
    renderer::ResourceHandle<renderer::RenderTargetTag> velocity;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalDepth;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalMask;
    /// @note Bloom のミップ連鎖。bloomChain[0] が半解像度で、以降 1/2 ずつ。
    /// @note bloomHalf は bloomChain[0] の別名 (Composite 側の参照名)。
    /// @note 1 枚のミップ付きにしないのは CreateComputeTexture がミップを持たないため。
    renderer::ResourceHandle<renderer::TextureTag> bloomChain[kBloomMipCount];
    renderer::ResourceHandle<renderer::TextureTag> bloomUpChain[kBloomMipCount];
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    /// @note Advanced Graphics (解像度依存・ビュー単位)
    /// @note 解像度非依存の LUT 等は RenderSharedResources が保持する。
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;           ///< @note SSR CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;    ///< @note Volumetric CS 出力
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;         ///< @note TAA ping-pong A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;         ///< @note TAA ping-pong B
    /// @note TAA の ping-pong の向き。RenderPassHandles はフレームごとに作り直すので、
    /// @note ここに持たないと毎フレーム false から始まり «A を読んで B に書く» しか起きない。
    /// @note A は一度も書かれず、履歴は最初の中身のまま固定される。
    bool taaFlip = false;
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;    ///< @note Motion Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;             ///< @note GTAO RAW CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoBlur;            ///< @note GTAO Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        contactShadowResult; ///< @note Contact Shadow CS 出力
    /// @note ビュー別定数バッファ / 再投影行列
    /// @note static で共有すると SceneView と GameView が互いのカメラ行列を引き、
    /// @note MotionBlur / TAA の再投影が常に壊れる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB;
    /// @note TAA 直前の feedback 更新は typed CPU snapshot から行い、別の照明定数を失わない。
    AdvancedGraphicsCB advancedGraphicsSnapshot{};
    bool advancedGraphicsSnapshotValid = false;
    /// @note 自動露出 (ビュー単位・解像度非依存)
    /// @note exposureResult は「順応済みの平均輝度」でフレームをまたぐ状態。static で共有すると
    /// @note SceneView と GameView が交互に順応を進め、互いの明るさへ引きずられて露出が振れる。
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureHistogram;
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureResult;
    uint32_t exposureResetGeneration = 0;
    /// @note 体積雲の作業 RT (ビュー単位・解像度依存)
    renderer::SizedRenderTarget cloudRT;
    renderer::SizedRenderTarget cloudDepthRT;
    /// @note 水面の屈折用コピー (ビュー単位・解像度依存)
    renderer::SizedRenderTarget waterSceneColorRT;
    renderer::SizedRenderTarget waterSceneDepthRT;
    /// @note 歪みパーティクルの背景退避 / 重なり計数 / コースティクスの深度コピー
    /// @note 水面と同じくビュー単位。共有すると 2 ビューで寸法を取り合い、毎フレーム作り直す。
    renderer::SizedRenderTarget particleSceneColorRT;
    renderer::SizedRenderTarget particleOverdrawRT;
    renderer::SizedRenderTarget particleReactiveRT;
    renderer::SizedRenderTarget causticsDepthRT;
    /// @note フロクセル霧 (ビュー単位・解像度非依存)
    /// @note グリッドは視錐台に貼り付くので、共有すると互いの履歴を上書きして霧が明滅する。
    /// @note 寸法は設定値 (既定 160x90x64) で画面サイズと無関係なので、リサイズでは作り直さない。
    renderer::ResourceHandle<renderer::TextureTag> froxelScatter;
    renderer::ResourceHandle<renderer::TextureTag> froxelScatterHistory;
    renderer::ResourceHandle<renderer::TextureTag> froxelIntegrated;
    uint32_t                     froxelGrid[3] = { 0u, 0u, 0u };
    FroxelFogViewState           froxelState;
    math::Matrix4 prevViewProjection    = math::Matrix4::Identity();
    math::Matrix4 invPrevViewProjection = math::Matrix4::Identity();
    /// @note TAA ジッター列の現在位置。ビュー別に持たないと SceneView と GameView が
    /// @note 同じ番号を取り合って、どちらもサンプル点が飛び飛びになる。
    uint32_t taaFrameIndex = 0;
    /// @brief TAA 履歴 (taaHistoryA/B) に «前のフレームの絵» が入っているか。
    /// @note 作り直した直後と TAA を切っていた後は中身が未定義か古い絵。false の間は taaFeedback を 0 にし、
    /// @note 今のフレームだけで履歴を作り直す。リサイズでは保存・復元しないので false に戻る。
    bool taaHistoryValid = false;
    TaaProviderHistory taaProviderHistory;
    uint32_t width = 0;
    uint32_t height = 0;
    /// @note 当該ビューで最後に準備した構成。PrepareView の開始時に無効へ戻す。
    ResolvedRenderPlan renderPlan;
    RayDebugViewResources rayDebug;
    RayReflectionViewResources rayReflection;
    bool rayReflectionCovered = false;
    RayPathViewResources rayPath;
    bool rayPathCovered = false;
    bool rayPathPrepared = false;
    /// @note Canonical material bytes plus the typed glass marker. Survives resize, retires with this view and ResourceManager.
    ResourceHandle<ConstantBufferTag> gbufferMaterialCB;
};

/// @note GPU 実体は ResourceManager が所有し、この状態も同じ Manager の Reset/終了時に破棄する。
struct RenderSharedResources {
    RayGeometryCache rayGeometry;
    ResourceHandle<ShaderTag> rayDebugShader;
    ResourceHandle<ShaderTag> rayReflectionShader;
    ResourceHandle<ShaderTag> rayReflectionReconstructionShader;
    ResourceHandle<ShaderTag> rayPathShader;
    ResourceHandle<ShaderTag> rayPathResolveShader;
    ResourceHandle<ShaderTag> rayGameReconstructionShader;
    ResourceHandle<PipelineStateTag> rayPathResolvePSO;
    SizedRenderTarget shadowMapRT{};
    SizedRenderTarget punctualShadowRT{};
    SizedRenderTarget lightCookieRT{};
    ResourceHandle<ShaderTag> cookieBlitShader{};
    ResourceHandle<ShaderTag> shadowShader{};
    ResourceHandle<ShaderTag> shadowInstancedShader{};
    ResourceHandle<ShaderTag> skinnedShadowShader{};
    ResourceHandle<ShaderTag> velocityShader{};
    ResourceHandle<ShaderTag> velocityInstancedShader{};
    ResourceHandle<ShaderTag> velocitySkinnedShader{};
    ResourceHandle<ShaderTag> skinningComputeCS{};
    ResourceHandle<ConstantBufferTag> bindPoseSkinningCB{};
    ResourceHandle<ShaderTag> compositeShader{};
    ResourceHandle<ShaderTag> causticsShader{};
    ResourceHandle<ShaderTag> volumetricCloudShader{};
    ResourceHandle<ShaderTag> cloudUpscaleShader{};
    ResourceHandle<ShaderTag> ssaoShader{};
    ResourceHandle<ShaderTag> ssaoBlurShader{};
    ResourceHandle<ShaderTag> bloomDownShader{};
    ResourceHandle<ShaderTag> bloomUpShader{};
    ResourceHandle<ShaderTag> selectionMaskShader{};
    ResourceHandle<ShaderTag> selectionMaskSkinnedShader{};
    ResourceHandle<ShaderTag> selectionMaskParticleShader{};
    ResourceHandle<ShaderTag> selectionMaskParticleGpuShader{};
    ResourceHandle<ShaderTag> selectionOutlineShader{};
    ResourceHandle<ShaderTag> objectMaskShader{};
    ResourceHandle<ShaderTag> objectMaskInstancedShader{};
    ResourceHandle<ShaderTag> objectMaskSkinnedShader{};
    ResourceHandle<ShaderTag> copyColorShader{};
    ResourceHandle<ShaderTag> customComposeShader{};
    ResourceHandle<ShaderTag> fxaaShader{};
    ResourceHandle<ShaderTag> upscaleShader{};
    ResourceHandle<ShaderTag> downscaleShader{};
    ResourceHandle<ShaderTag> iblBrdfBakeShader{};
    ResourceHandle<ShaderTag> gtaoShader{};
    ResourceHandle<ShaderTag> gtaoBlurShader{};
    ResourceHandle<ShaderTag> ssrShader{};
    ResourceHandle<ShaderTag> volumetricShader{};
    ResourceHandle<ShaderTag> contactShadowShader{};
    ResourceHandle<ShaderTag> taaShader{};
    ResourceHandle<ShaderTag> motionBlurShader{};
    ResourceHandle<ShaderTag> lensFlareShader{};
    ResourceHandle<TextureTag> iblBrdfLut{};
    ResourceHandle<TextureTag> proceduralColorLut{};
    uint64_t proceduralColorLutHash{};
    ResourceHandle<ShaderTag> skydomeShader{};
    ResourceHandle<ShaderTag> sunMoonShader{};
    std::unique_ptr<Mesh> skydomeMesh;
    EnvironmentResources sEnvironmentResources{};
    ResourceHandle<RenderTargetTag> skyEnvCubeRT{};
    ResourceHandle<ConstantBufferTag> skyCaptureFrameCB{};
    ResourceHandle<TextureTag> cloudShapeTex{};
    ResourceHandle<TextureTag> cloudDetailTex{};
    ResourceHandle<ShaderTag> gbufferShader{};
    ResourceHandle<ShaderTag> gbufferInstancedShader{};
    ResourceHandle<ShaderTag> gbufferSkinnedShader{};
    ResourceHandle<ShaderTag> deferredLightingShader{};
    ResourceHandle<ShaderTag> depthCopyShader{};
    ResourceHandle<ShaderTag> clusterCullCS{};
    DynamicStructuredBufferPool punctualLightPool{};
    ResourceHandle<StructuredBufferTag> clusterIndexBuffer{};
    ResourceHandle<ShaderTag> exposureHistogramCS{};
    ResourceHandle<ShaderTag> exposureAverageCS{};
    ResourceHandle<ShaderTag> froxelInjectCS{};
    ResourceHandle<ShaderTag> froxelIntegrateCS{};
    ResourceHandle<ShaderTag> lightProbeProjectCS{};
    ResourceHandle<ConstantBufferTag> lightProbeProjectCB{};
    ResourceHandle<ConstantBufferTag> lightProbeCaptureFrameCB{};
    ResourceHandle<ConstantBufferTag> lightProbeCaptureAdvancedCB{};
    ResourceHandle<ShaderTag> lightProbeDilateCS{};
    ResourceHandle<ConstantBufferTag> lightProbeDilateCB{};
    ResourceHandle<ShaderTag> lightProbeFacingShader{};
    ResourceHandle<ConstantBufferTag> clusterCB{};
    ResourceHandle<ConstantBufferTag> clusterLinearCB{};
    ResourceHandle<ShaderTag> decalShader{};
    ResourceHandle<ShaderTag> decalMaskShader{};
    ResourceHandle<ShaderTag> decalMaskSkinnedShader{};
    ResourceHandle<ShaderTag> particleShader{};
    ResourceHandle<ShaderTag> particleGpuSimCS{};
    ResourceHandle<ShaderTag> particleGpuShader{};
    ResourceHandle<ShaderTag> particleGpuSortKeysCS{};
    ResourceHandle<ShaderTag> particleGpuSortStepCS{};
    ResourceHandle<ShaderTag> particleGpuSortLocalCS{};
    ResourceHandle<ShaderTag> particleGpuMeshShader{};
    ResourceHandle<ShaderTag> particleSelfShadowShader{};
    ResourceHandle<ShaderTag> trailShader{};
    ResourceHandle<ShaderTag> meshTrailShader{};
    ResourceHandle<ShaderTag> skinnedMeshTrailShader{};
    ResourceHandle<ConstantBufferTag> frameCB{};
    ResourceHandle<ConstantBufferTag> objectCB{};
    ResourceHandle<ConstantBufferTag> lightCB{};
    ResourceHandle<ConstantBufferTag> shadowCB{};
    ResourceHandle<ConstantBufferTag> punctualShadowCB{};
    ResourceHandle<ConstantBufferTag> cookieBlitCB{};
    ResourceHandle<ConstantBufferTag> exposureCB{};
    ResourceHandle<ConstantBufferTag> froxelFogCB{};
    ResourceHandle<ConstantBufferTag> skinningCB{};
    ResourceHandle<ConstantBufferTag> postprocCB{};
    ResourceHandle<ConstantBufferTag> outlineCB{};
    ResourceHandle<ConstantBufferTag> objectMaskCB{};
    ResourceHandle<ConstantBufferTag> atmCB{};
    ResourceHandle<ConstantBufferTag> decalCB{};
    ResourceHandle<ConstantBufferTag> decalMaterialCB{};
    ResourceHandle<ConstantBufferTag> decalReceiverCB{};
    ResourceHandle<ConstantBufferTag> volumetricCloudCB{};
    SizedRenderTarget particleSelfShadowRT{};
    ResourceHandle<ConstantBufferTag> particleSelfShadowFrameCB{};
    ResourceHandle<PipelineStateTag> defaultPSO{};
    ResourceHandle<PipelineStateTag> wireframePSO{};
    ResourceHandle<PipelineStateTag> selectionMaskPso{};
    ResourceHandle<PipelineStateTag> skydomePSO{};
    ResourceHandle<PipelineStateTag> sunMoonPSO{};
    ResourceHandle<PipelineStateTag> particlePSO{};
    ResourceHandle<PipelineStateTag> particleAlphaPSO{};
    ResourceHandle<PipelineStateTag> particlePremultipliedPSO{};
    ResourceHandle<PipelineStateTag> particleGpuPSO{};
    ResourceHandle<PipelineStateTag> particleGpuAlphaPSO{};
    ResourceHandle<PipelineStateTag> particleGpuPremultipliedPSO{};
    ResourceHandle<PipelineStateTag> trailPSO{};
    ResourceHandle<PipelineStateTag> meshTrailPSO{};
    ResourceHandle<PipelineStateTag> meshTrailDoubleSidedPSO{};
    ResourceHandle<PipelineStateTag> postprocPSO{};
    ResourceHandle<PipelineStateTag> causticsPSO{};
    ResourceHandle<PipelineStateTag> volumetricCloudPSO{};
    ResourceHandle<PipelineStateTag> volumetricCloudPremultipliedPSO{};
    ResourceHandle<PipelineStateTag> taaPSO{};
    ResourceHandle<PipelineStateTag> lensFlarePSO{};
    ResourceHandle<PipelineStateTag> decalPSO{};
    ResourceHandle<PipelineStateTag> decalMaskPso{};
    ResourceHandle<BufferTag> particleIB{};
    /// @note DynamicScene 捕捉は主カメラの前フレーム atlas を読まず、現在の光源だけを独立して束縛する。
    ResourceHandle<ConstantBufferTag> reflectionProbeCaptureShadowCB{};
    ResourceHandle<ConstantBufferTag> reflectionProbeCapturePunctualCB{};

    void Initialize(ResourceManager& resources);
    bool Prepare(ResourceManager& resources, const RenderSettings& rs);
private:
    bool m_initialized = false;
};

/// @note 描画スレッド専用。返したビュー参照は ReleaseView または Manager の Reset まで有効。
/// @see Docs/design/graphics-library.md GPU 資源・終了順
class RenderResources {
public:
    explicit RenderResources(ResourceManager& resources);
    /// @pre ビューの準備・記録開始前に呼ぶ。
    /// @note 許可を解除したら全ビューの実験 RT 専用資源を退役する。描画設定・Raster 資源は変更しない。
    /// @see Docs/design/developer-mode.md
    void SetExperimentalRayTracingEnabled(bool enabled);
    RenderSharedResources& Shared() { return m_shared; }
    RenderViewResources& View(uint32_t key);
    /// @return 未登録なら nullptr。診断の照会でビューや GPU 資源を生成しない。
    [[nodiscard]] const RenderViewResources* FindView(uint32_t key) const
    {
        const auto found = m_views.find(key);
        return found == m_views.end() ? nullptr : &found->second;
    }
    bool PrepareView(RenderViewResources& view, IRenderer& renderer,
                     ResourceHandle<RenderTargetTag> output, const RenderSettings& settings);
    /// @note ビュー所有者の破棄時に呼ぶ。共有シェーダーや他ビューの履歴は解放しない。
    void ReleaseView(uint32_t key);
    void ReleaseOutput(ResourceHandle<RenderTargetTag> output);
    void BindPassHandles(RenderViewResources& view, RenderPassHandles& handles);
private:
    ResourceManager& m_resources;
    RenderSharedResources m_shared;
    std::unordered_map<uint32_t, RenderViewResources> m_views;
    bool m_experimentalRayTracingEnabled = false;
};
}
