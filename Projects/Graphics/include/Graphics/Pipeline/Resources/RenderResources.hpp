/// @file    RenderResources.hpp
/// @brief   描画共有資源とビュー別の履歴・中間ターゲットの寿命管理。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Pipeline/View/ResolvedRenderPlan.hpp>
#include <Graphics/Pipeline/Resources/EnvironmentResources.hpp>
#include <Graphics/RayTracing/RayTracingPipeline.hpp>
#include <Graphics/Renderer/Resources/Buffers/DynamicBufferPool.hpp>
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {
/// @note GPU のボーンパレット契約。Engine の Skeleton と静的検証する。
inline constexpr int RENDER_SKINNING_BONES = 128;
/// @note RGBA16F: albedo / roughness、normal / metallic、線形 HDR emission / typed glass marker。
inline constexpr uint32_t GBUFFER_COLOR_COUNT = 3;

/// @note 照明・反射合成方式と実記録・連続フレームを追跡する。画素別の反射 motion 履歴ではない。
struct TaaProviderHistory {
    RenderMode mode = RenderMode::RASTER; ///< 履歴の生成時に使用した描画モード。
    bool reflectionActive = false; ///< 履歴の生成時にレイ反射を使用したか。
    bool reflectionResolveActive = false; ///< 履歴の生成時に反射を解決するパスを使ったか。
    bool screenReflectionActive = false; ///< 履歴の生成時に画面空間の反射を使用したか。
    bool valid = false; ///< 前回フレームの方式と連続性を比較できるか。
    uint64_t frameStamp = 0; ///< 履歴の生成元の描画フレーム番号。
    uint64_t epoch = 0; ///< 履歴が属する資源世代。
};

struct FarShadowCacheKey {
    uint64_t sceneGeneration = 0; ///< 深度の生成元のシーンを識別する世代。
    uint64_t hierarchyRevision = 0; ///< 静的キャスターの階層変更を検出する版番号。
    uint64_t terrainRevision = 0; ///< 深度を生成したときの地形の版番号。
    uint64_t light = 0; ///< ベイク・描画条件の比較に使うライトの識別値。
    uint32_t cullingMask = 0; ///< 影に含めた描画レイヤーのビットマスク。
    bool operator==(const FarShadowCacheKey&) const = default;
};

/// @note 静的深度とその投影は同じビューに属し、描画完了時にまとめて有効化する。
struct ShadowCascadeCache {
    ShadowCascade cascade{}; ///< 再利用する深度と一対になる前回の投影・描画領域。
    FarShadowCacheKey key; ///< 深度の生成元となったシーンと光源の識別情報。
    math::Vector3 cameraForward; ///< キャッシュを作成したときのカメラの前方ベクトル。
    math::Vector3 lightDirection; ///< キャッシュを作成したときの平行光源の向き。
    uint64_t updateFrame = 0; ///< カスケードの深度を最後に更新した描画フレーム。
    uint64_t previousFrame = 0; ///< 前回キャッシュが評価された描画フレーム。
    float precedingSplit = 0.0f; ///< このカスケードの手前側を区切る分割距離。
    bool valid = false; ///< 再利用できる深度と投影の組があるか。
    uint64_t contentKey = 0; ///< 公開する深度内容の一致を照合するキー。
};

/// @note Viewport ごとに解像度依存の中間リソースを保持する。
/// @note static で共有すると SceneView と GameView が 1 フレーム内でリサイズし合う。
struct RenderViewResources {
    /// @note View ごとの RenderGraph 計画と transient RT をフレーム間で保持する。
    /// @note スタック生成だと DX12 の descriptor heap を毎フレーム作り直し、CPU が詰まる。
    RenderPipeline pipeline;
    SizedRenderTarget staticShadowAtlas;
    std::array<ShadowCascadeCache, kMaxShadowCascades> shadowCascadeCaches;
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
    SizedRenderTarget occluderDepth;
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
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;           ///< SSR CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        volumetricRaw;       ///< 半解像度の散乱 RGB / 正規化した視空間深度 A (空は -1)
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;    ///< Volumetric CS 出力
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;         ///< TAA ping-pong A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;         ///< TAA ping-pong B
    /// @note 8 位相の未蓄積色と画素別の履歴有効長。解像度変更と TAA 無効化で破棄する。
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaRawHistory[8]{};
    renderer::ResourceHandle<renderer::ConstantBufferTag> taaCycleCB; ///< TAA のサンプリング位相の更新に使う定数バッファのハンドル。
    /// @note 確保失敗後は次のリサイズか TAA 切替まで再試行せず、既存履歴の EMA を継続する。
    bool taaRawHistoryAllocationFailed = false;
    /// @note TAA の ping-pong の向き。RenderPassHandles はフレームごとに作り直すので、
    /// @note ここに持たないと毎フレーム false から始まり «A を読んで B に書く» しか起きない。
    /// @note A は一度も書かれず、履歴は最初の中身のまま固定される。
    bool taaFlip = false;
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;    ///< Motion Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;             ///< GTAO RAW CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoBlur;            ///< GTAO Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        contactShadowResult; ///< Contact Shadow CS 出力
    /// @note ビュー別定数バッファ / 再投影行列
    /// @note static で共有すると SceneView と GameView が互いのカメラ行列を引き、
    /// @note MotionBlur / TAA の再投影が常に壊れる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB; ///< ビュー単位の描画効果の設定に使う定数バッファのハンドル。
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
    renderer::SizedRenderTarget cloudRT; ///< 雲の体積散乱の評価に使う描画ターゲットとその寸法。
    renderer::SizedRenderTarget cloudDepthRT; ///< 雲の描画で参照する深度の記録に使う描画ターゲットとその寸法。
    /// @note 水面の屈折用コピー (ビュー単位・解像度依存)
    renderer::SizedRenderTarget waterSceneColorRT; ///< 水面の屈折で参照するシーン色の保持に使う描画ターゲットとその寸法。
    renderer::SizedRenderTarget waterSceneDepthRT; ///< 水面の下にあるシーン深度の保持に使う描画ターゲットとその寸法。
    /// @note One view-local atlas and nearest visible interface mask shared by lighting and water composition.
    renderer::SizedRenderTarget waterMediumAtlasRT; ///< 水中の媒体情報のアトラスへの記録に使う描画ターゲットとその寸法。
    renderer::SizedRenderTarget waterInterfaceRT; ///< 水面境界の記録に使う描画ターゲットとその寸法。
    /// @note Reused per-view rows; the pool's frame lanes protect recorded views from later updates.
    renderer::DynamicStructuredBufferPool waterMediumTablePool{ 16 };
    /// @note 歪みパーティクルの背景退避 / 重なり計数 / コースティクスの深度コピー
    /// @note 水面と同じくビュー単位。共有すると 2 ビューで寸法を取り合い、毎フレーム作り直す。
    renderer::SizedRenderTarget particleSceneColorRT; ///< 粒子の屈折で参照するシーン色の保持に使う描画ターゲットとその寸法。
    renderer::SizedRenderTarget particleOverdrawRT; ///< 粒子の重なりの診断表示に使う描画ターゲットとその寸法。
    renderer::SizedRenderTarget particleReactiveRT; ///< 粒子が時間方向の合成に与える影響の記録に使う描画ターゲットとその寸法。
    renderer::SizedRenderTarget causticsDepthRT; ///< 集光模様を受ける面の深度記録に使う描画ターゲットとその寸法。
    /// @note フロクセル霧 (ビュー単位・解像度非依存)
    /// @note グリッドは視錐台に貼り付くので、共有すると互いの履歴を上書きして霧が明滅する。
    /// @note 寸法は設定値 (既定 160x90x64) で画面サイズと無関係なので、リサイズでは作り直さない。
    renderer::ResourceHandle<renderer::TextureTag> froxelScatter;
    renderer::ResourceHandle<renderer::TextureTag> froxelScatterHistory;
    renderer::ResourceHandle<renderer::TextureTag> froxelIntegrated;
    uint32_t                     froxelGrid[3] = { 0u, 0u, 0u };
    FroxelFogViewState           froxelState;
    math::Matrix4 prevViewProjection    = math::Matrix4::Identity(); ///< 現フレームのワールド位置を履歴画像上の位置へ投影する前フレームの行列。
    math::Matrix4 invPrevViewProjection = math::Matrix4::Identity(); ///< 前フレームのクリップ座標をワールド座標に戻す逆変換。
    /// @note 前回の VP と同じ射影の深度復元条件。
    float previousNearZ = 0.1f, previousFarZ = 1000.0f;
    bool previousOrthographic = false;
    /// @note TAA ジッター列の現在位置。ビュー別に持たないと SceneView と GameView が
    /// @note 同じ番号を取り合って、どちらもサンプル点が飛び飛びになる。
    uint32_t taaFrameIndex = 0;
    /// @brief TAA 履歴 (taaHistoryA/B) に «前のフレームの絵» が入っているか。
    /// @note 作り直した直後と TAA を切っていた後は中身が未定義か古い絵。false の間は taaFeedback を 0 にし、
    /// @note 今のフレームだけで履歴を作り直す。リサイズでは保存・復元しないので false に戻る。
    bool taaHistoryValid = false;
    TaaProviderHistory taaProviderHistory;
    /// @note 前回と今回の不透明深度入力を値で完全比較する。GPU 実体や Scene の所有権は保持しない。
    std::vector<uint64_t> taaStaticViewPrevious;
    std::vector<uint64_t> taaStaticViewCurrent;
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
    ResourceHandle<ConstantBufferTag> gbufferMaterialCB; ///< G-buffer に格納する材質の指定に使う定数バッファのハンドル。
};

/// @brief Shared shader and metadata-buffer state for static meshlet draws.
/// @note GPU 実体は ResourceManager が所有し、この状態も同じ Manager の Reset/終了時に破棄する。
struct MeshletDrawResources {
    uint64_t ownerId = 0;
    uint64_t resetVersion = 0;
    uint64_t shaderVersion = 0;
    bool gbufferShaderAttempted = false;
    bool shadowShaderAttempted = false;
    ResourceHandle<ShaderTag> gbufferShader{}; ///< 不透明面の G-buffer への記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> shadowShader{}; ///< 光源から見た深度の記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> maskedShadowShader{}; ///< アルファマスクを考慮したシャドウ深度の生成に使うシェーダーのハンドル。
    bool maskedShadowShaderAttempted = false;
    ResourceHandle<ConstantBufferTag> geometryConstants{};
    ResourceHandle<ConstantBufferTag> windFrame{};
    uint64_t windFrameStamp = UINT64_MAX;
    ResourceHandle<ShaderTag> compactShader{}, resetShader{}; ///< 可視クラスターの描画候補の圧縮に使うシェーダーのハンドル。
    DynamicRWStructuredBufferPool compactIds{32}, compactArguments{1};
    struct Stream {
        std::weak_ptr<const MeshletClusterData> source;
        std::vector<ResourceHandle<StructuredBufferTag>> pages;
        std::vector<uint32_t> descriptors;
        ResourceHandle<StructuredBufferTag> table;
        uint32_t tableDepth = UINT32_MAX;
        uint64_t lastFrame = 0;
        size_t nextPage = 0;
        size_t bytes = 0;
    };
    std::unordered_map<uint64_t, Stream> streams;
    size_t streamBytes = 0;
    size_t uploadedBytes = 0;
    uint64_t streamFrame = UINT64_MAX;
};

/// @note GPU 実体は ResourceManager が所有し、この状態も同じ Manager の Reset/終了時に破棄する。
struct RenderSharedResources {
    RayGeometryCache rayGeometry;
    MeshletDrawResources meshletDraw;
    ResourceHandle<ShaderTag> rayDebugShader; ///< レイ追跡結果の診断表示に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> rayReflectionShader; ///< レイによる反射光の計算に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> rayReflectionReconstructionShader; ///< レイ反射結果の再構成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> rayPathShader; ///< パストレーシングの記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> rayPathResolveShader; ///< パストレーシング結果の解決に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> rayGameReconstructionShader; ///< ゲーム用レイ描画結果の再構成に使うシェーダーのハンドル。
    ResourceHandle<PipelineStateTag> rayPathResolvePSO; ///< パストレーシング結果の解決に使うパイプライン状態のハンドル。
    SizedRenderTarget shadowMapRT{}; ///< 光源から見た深度の記録に使う描画ターゲットとその寸法。
    SizedRenderTarget punctualShadowRT{}; ///< 局所光源のシャドウアトラスと投影の設定に使う描画ターゲットとその寸法。
    SizedRenderTarget lightCookieRT{}; ///< 照明に投影するクッキーの格納に使う描画ターゲットとその寸法。
    ResourceHandle<ShaderTag> cookieBlitShader{}; ///< ライトクッキーのアトラスへの転写に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> shadowShader{}; ///< 光源から見た深度の記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> shadowInstancedShader{}; ///< インスタンスをまとめたシャドウ深度描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> skinnedShadowShader{}; ///< スキニングした形状のシャドウ深度描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> velocityShader{}; ///< 画面上の移動量の記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> velocityInstancedShader{}; ///< インスタンスごとの画面上の移動量の記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> velocitySkinnedShader{}; ///< スキニングした形状の画面上の移動量の記録に使うシェーダーのハンドル。
    /// @note 切り抜き速度の材質定数も Manager ごとに保持し、Reset で失効させる。
    ResourceHandle<ShaderTag> velocityAlphaShader{}; ///< アルファ判定付きの面の移動量の記録に使うシェーダーのハンドル。
    ResourceHandle<ConstantBufferTag> velocityMaterialCB{}; ///< 速度パスでの材質設定の転送に使う定数バッファのハンドル。
    ResourceHandle<ShaderTag> skinningComputeCS{}; ///< ボーン変換による頂点の変形に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ConstantBufferTag> bindPoseSkinningCB{}; ///< バインドポーズ基準のスキニングに使う定数バッファのハンドル。
    ResourceHandle<ShaderTag> compositeShader{}; ///< 描画済みの各画像の最終合成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> causticsShader{}; ///< 水面を通過する集光模様の描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> volumetricCloudShader{}; ///< 雲の体積散乱の評価に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> cloudUpscaleShader{}; ///< 低解像度の雲の拡大合成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> ssaoShader{}; ///< 画面深度からの環境遮蔽の算出に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> ssaoBlurShader{}; ///< 画面空間の環境遮蔽の平滑化に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> bloomDownShader{}; ///< 高輝度成分の縮小と Bloom のミップ生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> bloomUpShader{}; ///< 小さいミップからの Bloom の合成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> selectionMaskShader{}; ///< Editor で選択中の物体のマスク生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> selectionMaskSkinnedShader{}; ///< 選択したスキニング形状のマスク生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> selectionMaskParticleShader{}; ///< 選択した粒子のマスク生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> selectionMaskParticleGpuShader{}; ///< 選択した GPU 粒子のマスク生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> selectionOutlineShader{}; ///< 選択マスクを使った輪郭線の合成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> objectMaskShader{}; ///< ランタイム輪郭用のオブジェクトマスクの生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> objectMaskInstancedShader{}; ///< インスタンス描画時の輪郭マスクの生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> objectMaskSkinnedShader{}; ///< スキニングした形状の輪郭マスクの生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> copyColorShader{}; ///< 描画済みのカラー画像の転送に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> customComposeShader{}; ///< カスタム効果の画像合成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> fxaaShader{}; ///< 画面空間の輪郭に対する FXAA 処理に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> upscaleShader{}; ///< 描画した画像の出力解像度への拡大に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> downscaleShader{}; ///< 描画結果の縮小に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> iblBrdfBakeShader{}; ///< 環境照明の BRDF 積分 LUT の生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> gtaoShader{}; ///< 画面深度からの GTAO の算出に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> gtaoBlurShader{}; ///< GTAO で求めた遮蔽量の平滑化に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> ssrShader{}; ///< スクリーン空間での反射の計算に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> ssrResolveShader{}; ///< スクリーン空間の反射結果の解決に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> volumetricShader{}; ///< ビューに沿った体積光の描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> contactShadowShader{}; ///< 深度を使ったスクリーン空間の接触影の計算に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> taaShader{}; ///< 履歴を用いた時間方向のアンチエイリアスに使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> motionBlurShader{}; ///< 速度画像を使ったモーションブラーの合成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> lensFlareShader{}; ///< 光源のレンズフレア描画に使うシェーダーのハンドル。
    ResourceHandle<TextureTag> iblBrdfLut{};
    ResourceHandle<TextureTag> proceduralColorLut{};
    uint64_t proceduralColorLutHash{};
    ResourceHandle<ShaderTag> skydomeShader{}; ///< スカイドームの描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> skyCloudCaptureShader{}; ///< 環境マップに含める雲の描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> sunMoonShader{}; ///< 天空の太陽と月の描画に使うシェーダーのハンドル。
    std::unique_ptr<Mesh> skydomeMesh;
    EnvironmentResources sEnvironmentResources{};
    ResourceHandle<RenderTargetTag> skyEnvCubeRT{}; ///< 空の各方向のキューブマップへの描画に使う描画ターゲットのハンドル。
    ResourceHandle<ConstantBufferTag> skyCaptureFrameCB{}; ///< 環境光を撮影する視点の指定に使う定数バッファのハンドル。
    ResourceHandle<TextureTag> cloudShapeTex{};
    ResourceHandle<TextureTag> cloudDetailTex{};
    ResourceHandle<ShaderTag> gbufferShader{}; ///< 不透明面の G-buffer への記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> gbufferInstancedShader{}; ///< インスタンスをまとめた G-buffer 描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> gbufferSkinnedShader{}; ///< スキニングした形状の G-buffer 描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> deferredLightingShader{}; ///< G-buffer に記録した面のライティングに使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> depthCopyShader{}; ///< 後続パスが参照する深度のコピーに使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> clusterCullCS{}; ///< クラスターごとの局所光源の選別に使うコンピュートシェーダーのハンドル。
    DynamicStructuredBufferPool punctualLightPool{};
    ResourceHandle<StructuredBufferTag> clusterIndexBuffer{};
    ResourceHandle<ShaderTag> exposureHistogramCS{}; ///< 自動露出用の輝度ヒストグラムの集計に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> exposureAverageCS{}; ///< 輝度の集計からの露出算出に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> froxelInjectCS{}; ///< フロクセルごとの散乱光の計算に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> froxelIntegrateCS{}; ///< フロクセル列に沿った散乱光の積分に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> lightProbeProjectCS{}; ///< キャプチャ結果の照明プローブへの投影に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ConstantBufferTag> lightProbeProjectCB{}; ///< キャプチャ結果の照明プローブへの投影に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> lightProbeCaptureFrameCB{}; ///< 照明プローブのベイク視点の指定に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> lightProbeCaptureAdvancedCB{}; ///< 照明プローブのベイク時の描画効果の設定に使う定数バッファのハンドル。
    ResourceHandle<ShaderTag> lightProbeDilateCS{}; ///< 照明プローブの値の近傍への拡張に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ConstantBufferTag> lightProbeDilateCB{}; ///< 照明プローブの値の近傍への拡張に使う定数バッファのハンドル。
    ResourceHandle<ShaderTag> lightProbeFacingShader{}; ///< 照明プローブから見た面の向きの計測に使うシェーダーのハンドル。
    ResourceHandle<ConstantBufferTag> clusterCB{}; ///< クラスター照明のタイル・深度分割に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> clusterLinearCB{}; ///< 線形の深度分割を使うクラスター照明に使う定数バッファのハンドル。
    ResourceHandle<ShaderTag> decalShader{}; ///< デカールの投影に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> decalMaskShader{}; ///< デカールを受ける面のマスク生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> decalMaskSkinnedShader{}; ///< スキニングした面のデカール受け取りマスク生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleShader{}; ///< 粒子の描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleGpuSimCS{}; ///< GPU 上の粒子の生成と状態更新に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleGpuShader{}; ///< GPU 粒子の描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleGpuSortKeysCS{}; ///< GPU 粒子の並べ替えキーの生成に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleGpuSortStepCS{}; ///< GPU 粒子の並べ替えの各段に使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleGpuSortLocalCS{}; ///< グループ内での GPU 粒子の並べ替えに使うコンピュートシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleGpuMeshShader{}; ///< GPU 粒子のメッシュ描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleSelfShadowShader{}; ///< 粒子同士による自己影の生成に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> trailShader{}; ///< 連続する点列からなる軌跡の描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> meshTrailShader{}; ///< メッシュを用いた軌跡の描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> skinnedMeshTrailShader{}; ///< スキニングしたメッシュ軌跡の描画に使うシェーダーのハンドル。
    ResourceHandle<ConstantBufferTag> frameCB{}; ///< ビューとカメラのフレーム定数の転送に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> objectCB{}; ///< オブジェクトごとの変換と描画設定の転送に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> lightCB{}; ///< 光源情報の転送に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> shadowCB{}; ///< 光源から見た深度の記録に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> punctualShadowCB{}; ///< 局所光源のシャドウアトラスと投影の設定に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> cookieBlitCB{}; ///< ライトクッキーのアトラスへの転写に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> exposureCB{}; ///< 自動露出の算出に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> froxelFogCB{}; ///< カメラのフロクセル格子への霧の設定に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> skinningCB{}; ///< ボーン変換による頂点の変形に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> postprocCB{}; ///< ポストプロセスによる画面効果の合成に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> outlineCB{}; ///< マスクからの輪郭描画に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> objectMaskCB{}; ///< ランタイム輪郭用のオブジェクトマスクの生成に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> atmCB{}; ///< 大気散乱の評価に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> decalCB{}; ///< デカールの投影に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> decalMaterialCB{}; ///< デカール材質の色・法線の適用に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> decalReceiverCB{}; ///< デカールを受ける面の条件の指定に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> volumetricCloudCB{}; ///< 雲の体積散乱の評価に使う定数バッファのハンドル。
    SizedRenderTarget particleSelfShadowRT{}; ///< 粒子同士による自己影の生成に使う描画ターゲットとその寸法。
    ResourceHandle<ConstantBufferTag> particleSelfShadowFrameCB{}; ///< 粒子自己影の投影情報の転送に使う定数バッファのハンドル。
    ResourceHandle<PipelineStateTag> defaultPSO{}; ///< 通常のラスタ描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> wireframePSO{}; ///< ワイヤーフレーム描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> selectionMaskPso{}; ///< Editor で選択中の物体のマスク生成に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> skydomePSO{}; ///< スカイドームの描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> sunMoonPSO{}; ///< 天空の太陽と月の描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> particlePSO{}; ///< 粒子の描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> particleAlphaPSO{}; ///< アルファ合成による粒子描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> particlePremultipliedPSO{}; ///< 乗算済みアルファでの粒子描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> particleGpuPSO{}; ///< GPU 粒子の描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> particleGpuAlphaPSO{}; ///< GPU 粒子のアルファ合成に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> particleGpuPremultipliedPSO{}; ///< 乗算済みアルファでの GPU 粒子描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> trailPSO{}; ///< 連続する点列からなる軌跡の描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> meshTrailPSO{}; ///< メッシュを用いた軌跡の描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> meshTrailDoubleSidedPSO{}; ///< 両面を表示するメッシュ軌跡の描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> postprocPSO{}; ///< ポストプロセスによる画面効果の合成に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> causticsPSO{}; ///< 水面を通過する集光模様の描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> volumetricCloudPSO{}; ///< 雲の体積散乱の評価に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> volumetricCloudPremultipliedPSO{}; ///< 乗算済みアルファによる体積雲の合成に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> taaPSO{}; ///< 履歴を用いた時間方向のアンチエイリアスに使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> lensFlarePSO{}; ///< 光源のレンズフレア描画に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> decalPSO{}; ///< デカールの投影に使うパイプライン状態のハンドル。
    ResourceHandle<PipelineStateTag> decalMaskPso{}; ///< デカールを受ける面のマスク生成に使うパイプライン状態のハンドル。
    ResourceHandle<BufferTag> particleIB{};
    /// @note DynamicScene 捕捉は主カメラの前フレーム atlas を読まず、現在の光源だけを独立して束縛する。
    ResourceHandle<ConstantBufferTag> reflectionProbeCaptureShadowCB{}; ///< 反射プローブの撮影時の影の設定に使う定数バッファのハンドル。
    ResourceHandle<ConstantBufferTag> reflectionProbeCapturePunctualCB{}; ///< 反射プローブの撮影時の局所光源の設定に使う定数バッファのハンドル。
    /// @note 地形の描画資源は Manager ごとに所有し、Reset で他の共有資源と共に失効させる。
    ResourceHandle<ShaderTag> terrainShader{}, terrainGBufferShader{}, terrainDepthShader{}; ///< 地形パッチの描画に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> opaqueOccluderDepthShader; ///< 遮蔽物になる不透明面を深度へ描くシェーダー。
    ResourceHandle<ShaderTag> meshOcclusionShader; ///< メッシュの可視性を深度から判定するシェーダー。
    ResourceHandle<ConstantBufferTag> opaqueOccluderMaterialCB; ///< 遮蔽用深度パスで使う材質定数。
    ResourceHandle<ConstantBufferTag> meshOcclusionViewCB; ///< メッシュの遮蔽判定で使うビュー定数。
    ResourceHandle<PipelineStateTag> terrainPSO{}, terrainWireframePSO{}; ///< 地形パッチの描画に使うパイプライン状態のハンドル。
    ResourceHandle<ConstantBufferTag> terrainCameraCB{}, terrainObjectCB{}; ///< 地形描画で使うカメラ情報の転送に使う定数バッファのハンドル。
    /// @note 遅延生成する粒子・カスタム合成資源も Manager の寿命に閉じる。
    ResourceHandle<ShaderTag> particleReactiveShader{}, particleReactiveGpuShader{}; ///< 粒子が時間方向の合成に与える影響の記録に使うシェーダーのハンドル。
    ResourceHandle<ShaderTag> particleOverdrawShader{}, particleHeatmapShader{}; ///< 粒子の重なりの診断表示に使うシェーダーのハンドル。
    ResourceHandle<PipelineStateTag> customPassPSO[4]{}; ///< ユーザー定義のポスト処理に使うパイプライン状態のハンドル。
    DynamicVertexBufferPool particleVertexPool{}, particleTrailVertexPool{};
    /// @note 記録に成功した LUT だけを焼き済みとして扱う。
    ResourceHandle<TextureTag> bakedBrdfTarget{};

    FBZZ_GRAPHICS_API void Initialize(ResourceManager& resources);
    FBZZ_GRAPHICS_API bool Prepare(ResourceManager& resources, const RenderSettings& rs);
private:
    bool m_initialized = false;
};

/// @note 描画スレッド専用。返したビュー参照は ReleaseView または Manager の Reset まで有効。
/// @see Docs/design/graphics-library.md GPU 資源・終了順
class RenderResources {
public:
    FBZZ_GRAPHICS_API explicit RenderResources(ResourceManager& resources);
    /// @pre ビューの準備・記録開始前に呼ぶ。
    /// @note 許可を解除したら全ビューの実験 RT 専用資源を退役する。描画設定・Raster 資源は変更しない。
    /// @see Docs/design/developer-mode.md
    FBZZ_GRAPHICS_API void SetExperimentalRayTracingEnabled(bool enabled);
    RenderSharedResources& Shared() { return m_shared; }
    FBZZ_GRAPHICS_API RenderViewResources& View(uint32_t key);
    /// @return 未登録なら nullptr。診断の照会でビューや GPU 資源を生成しない。
    [[nodiscard]] const RenderViewResources* FindView(uint32_t key) const
    {
        const auto found = m_views.find(key);
        return found == m_views.end() ? nullptr : &found->second;
    }
    FBZZ_GRAPHICS_API bool PrepareView(RenderViewResources& view, IRenderer& renderer,
                     ResourceHandle<RenderTargetTag> output, const RenderSettings& settings);
    /// @note ビュー所有者の破棄時に呼ぶ。共有シェーダーや他ビューの履歴は解放しない。
    FBZZ_GRAPHICS_API void ReleaseView(uint32_t key);
    FBZZ_GRAPHICS_API void ReleaseOutput(ResourceHandle<RenderTargetTag> output);
    FBZZ_GRAPHICS_API void BindPassHandles(RenderViewResources& view, RenderPassHandles& handles);
private:
    ResourceManager& m_resources;
    RenderSharedResources m_shared;
    std::unordered_map<uint32_t, RenderViewResources> m_views;
    bool m_experimentalRayTracingEnabled = false;
};
}
