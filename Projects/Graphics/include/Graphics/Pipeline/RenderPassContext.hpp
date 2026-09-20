/// @file    RenderPassContext.hpp
/// @brief   RenderGraph 注入パスと各描画パスが共有する実行コンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Renderer/RenderLightingInput.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Effects/RenderSkinningInput.hpp>
namespace fbzz::renderer {
struct RenderPassContext : RenderLightingInput {
    renderer::IRenderer& renderer;
    renderer::ResourceManager& resources;
    const renderer::Camera& camera;
    const renderer::RenderSettings& settings;
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT;
    uint32_t cullingMask;

    /// @note ここまでが集成体初期化で埋める前半。参照メンバー handles より前に
    /// @note 既定値付きフィールドを挿すと呼び出し側の初期化子が 1 つずつずれるため、
    /// @note 新しい設定は必ず handles より後ろへ追加すること。
    RenderPassHandles& handles;

    /// @note スキニング完了後に抽出し、派生ビューを含む記録終了まで共有する不変入力。
    std::shared_ptr<const renderer::RenderScene> renderScene;
    std::vector<RenderSkinningInput> skinningRequests;
    bool skinningExecuted = false;
    uint64_t frameStamp = 0;
    float time = 0.0f;
    float deltaTime = 0.0f;
    float unscaledDeltaTime = 0.0f;
    RenderEnvironmentInput environment;

    /// @name カリング挙動 (CameraComponent 由来)
    /// @{
    /// @note パスが直接 CameraComponent を読むと、Scene View や VFX プレビューでゲームカメラの
    /// @note 設定が効いてしまい、エディタ上の見え方が編集対象と食い違う。
    bool  frustumCullingEnabled   = true;
    bool  occlusionCullingEnabled = true;
    /// @note 全バウンディング球へ加算するワールド単位の余白 [m]。
    float cullingBoundsPadding    = 0.0f;
    /// @note 距離カリング。値は解決済み (負値なし)。0 は「無効」。
    float cullMaxDistance         = 0.0f;
    float cullLayerDistances[32] = {};
    /// @note レイヤー別距離が 1 つでも設定されているか。全 0 のときは 32 要素の探索ごと省く。
    bool  hasLayerCullDistances   = false;
    bool  cullDistanceSpherical   = true;
    /// @note 極小オブジェクトカリングのしきい値 (画面高さ比)。0 は無効。
    float smallObjectScreenHeight = 0.0f;
    /// @note 画面高さ比の算出に使う射影スケール = projection.m[1][1] = 1/tan(fovY/2)。
    /// @note Camera::GetProjectionMatrix() は毎回行列を組み直して返すため、オブジェクトごとに
    /// @note 呼ぶと判定より行列生成の方が高くつく。事前計算して持つ。
    float cullProjScaleY          = 0.0f;
    /// @note 平行投影か。画面高さ比は距離で割らないので、判定式が分岐する。
    bool  cullOrthographic        = false;
    /// @note 距離計算用のカメラ前方ベクトル (深度距離モードでのみ使う)。同じ理由で事前計算する。
    math::Vector3 cullCameraForward = { 0.0f, 0.0f, 1.0f };

    uint32_t width = 0;
    uint32_t height = 0;
    /// @note 出力先 (outputRT) の実寸。width/height は描画スケールを掛けた内部解像度なので、
    /// @note 両者が食い違うフレームだけ UpscalePass が最終解像へ引き伸ばす。
    uint32_t outputWidth  = 0;
    uint32_t outputHeight = 0;
    /// @note LDR チェーンの終着点。等倍なら outputRT そのもので、描画スケールが効いている
    /// @note フレームは内部解像度の中継 RT を指す。
    /// @note ここを噛ませないと Composite/CustomPP/SelectionOutline/FXAA のうち outputRT へ
    /// @note 直接書くパスだけが実寸で走ってしまう。全段を内部解像度に揃え、実寸で走るのは
    /// @note UpscalePass の 1 回だけにする。
    renderer::ResourceHandle<renderer::RenderTargetTag> chainOutputRT;
    /// @note Composite の書き先。後段のチェーンがあれば ldrRT、無ければ chainOutputRT。
    /// @note 以前は Composite 側が «後段があるか» を自前で判定しており、RenderSystem の
    /// @note hasPostCompositeEffects (数え方が違う) と食い違い、AfterOpaque 段のカスタム
    /// @note パスだけのシーンで «Output を誰も書かない» フレームが出た。判定は 1 か所に置く。
    renderer::ResourceHandle<renderer::RenderTargetTag> compositeOutputRT;
    /// @note 名前 → 実ハンドルの対応表。パイプラインを組む側が毎フレーム埋め、
    /// @note パスは PassResources 越しに «申告した名前» でだけ引く。
    RenderResourceRegistry resourceRegistry;

    /// @note 実行中のパスの引き出し口。RenderPipeline が Execute の直前に差し、直後に外す。
    /// @note 自由関数の引数にしないのは、パス本体が 35 本以上の自由関数とヘルパーに
    /// @note 散っていて、署名を全部変えても «申告した名前からしか引けない» という保証
    /// @note (強制しているのは PassResources 自身) が強くならないため。
    PassResources* passResources = nullptr;

    /// @note 名前から実体を引く。パス実行中は «申告した名前しか引けない» PassResources を通す。
    /// @note グラフの外から呼ばれる処理 (反射プローブ捕捉・空のキャプチャ・IBL ベイク) は
    /// @note 申告する相手が居ないので登録簿を直接引く。
    /// @note 以前は *passResources を無条件に返しており、グラフ外の呼び出しが null 参照に
    /// @note なっていた。ExecuteReflectionProbeCapturePass がそこを踏み、影マップを引けず
    /// @note «プローブが真っ黒 = 環境光が死ぬ» という形で出た。
    [[nodiscard]] PassResources Res() const
    {
        return passResources
            ? *passResources
            : PassResources{ resourceRegistry, kOutsideGraphAccesses, "<outside graph>" };
    }
    /// @note UI 合成パスの設定。呼び出し元の Viewport が所有する。null なら UI を描かない。
    /// @note ctx へ通すのは、これだけが «RenderSystem の引数» のままだとパス本体を
    /// @note RenderSystem の外へ出せなくなるため。
    bool selectionOutlineEnabled = false;
    /// @note 今フレームに 1 件でも生きた輪郭要求があるか (RenderSettings::objectMaskRequests)。
    bool objectMaskEnabled = false;
    /// @note 今フレーム SSR のパスが実際に走るか。
    /// @note 出力先 (ssrResult) は RenderGraph の外にある永続テクスチャで、設定が有効でも
    /// @note パスが登録されない / 早期 return する経路がある (GBuffer 無し、Unlit、シェーダー未ロード)。
    /// @note 設定だけを見て Composite が読むと、最後に書かれた絵がそのまま毎フレーム乗り続ける。
    /// @note VolumetricLight は自分のパス内で HDR へ加算するので、この種のフラグは要らない。
    bool ssrPassActive = false;
    /// @note 選択マスクへ UI 要素の矩形を追記する。UISystemContext を握っているのは
    /// @note Viewport ごとの呼び出し元なので、パス側は「入っていれば呼ぶ」だけにする。
    std::function<void()> appendUISelectionMask;

    /// @note TAA サブピクセルジッター (NDC 単位)。TAA が無効なフレームは 0。
    /// @note ジッターが無いと静止カメラでは同じ絵に収束してアンチエイリアスにならない。
    /// @note b0 を組む各パスが MakeCameraFrameCB へ渡す。
    float taaJitterNdcX = 0.0f;
    float taaJitterNdcY = 0.0f;

    /// @}

    /// @name クラスタライトカリング
    /// @{

    /// @note フロクセル霧のフレーム間状態。実体は描画中のビューが持つ。
    /// @note 霧を走らせないフレームは null のことがある。
    FroxelFogViewState*           froxelFogState = nullptr;

    /// @note 影の光源視点。ビルボードを光源へ正対させる必要があるパス
    /// @note (パーティクル自己影の密度積み) が lightVP の内訳を要求する。
    /// @note GBuffer を使う不透明パイプラインが有効かどうか。
    /// @note RenderSettings の Forward/Deferred 名ではなく、各パスが GBuffer 入力を
    /// @note 読めるかで判定する。
    bool                        isDeferred  = false;
    bool                        ssaoEnabled = false;
    /// @note GBuffer の深度が「この時点で書き終わっている」か。
    /// @note Deferred では常に true、Forward では GBuffer プリパスを走らせたときだけ true。
    /// @note isDeferred は「不透明を GBuffer でライティングするか」でこちらとは別。同一視すると
    /// @note 接触影が hdrRT のまだ書かれていない深度を読む。
    bool                        gbufferDepthReady = false;
    /// @note 前方描画のマテリアルへ渡す画面空間の遮蔽。無効ハンドルなら束縛しない。
    /// @note AO は GTAO / SSAO のうち実際に走った方が入る (シェーダーからは区別しない)。
    renderer::ResourceHandle<renderer::TextureTag> screenAoTexture;
    renderer::ResourceHandle<renderer::TextureTag> screenContactShadowTexture;
    /// @note ライト正射影の深度範囲で正規化済みの NDC バイアス。固定 NDC 値だとシーンスケール
    /// @note 依存になるので、0.005 / depthRange としてワールド約 5mm 相当を保つ。
    /// @}

    /// @name カスケードシャドウ
    /// @{
    /// @}

    /// @name Spot / Point シャドウ
    /// @{
    /// @note アトラス全体の一辺 [px]。タイルサイズは これ / 4。

    /// @note レガシーライト経路 (b3 の固定長配列) 用のスロット番号。-1 = 無し。
    /// @note [0..7]  → lightData.pointLights[0..7]
    /// @note [8..11] → lightData.spotLights[0..3]
    /// @note b3 経路のシェーダーは PunctualLightGPU を読まないので、構造体に埋めた
    /// @note shadowIndex / cookieIndex が届かない。ここを通さないと影が Forward+ でだけ出る。
    /// @note 既定値は必ず -1。0 埋めだと「スロット 0」という有効な番号になり、影を持たない
    /// @note ライトが他のライトの深度を引く。
    /// @}

    /// @name ライト Cookie
    /// @{
    /// @}

    /// @name レガシー経路 (b3) 向けの「大きさを持つ光源」
    /// @{
    /// @note punctualLights から Area / Sphere / Tube を先頭 kMaxLegacyShapedLights 本まで
    /// @note 写したもの。b3 はこれらの型を運べないため、b12 側へ実体ごと載せる。
    /// @note b3 の点光源 / スポットの光源半径 [m]。添字は legacyShadowSlots と同じ。
    /// @note 別配列にする理由: b3 の PointLightData は 32 バイトぴったりで 1 float も空きが無い。

    /// @note 雲シャドウ (Phase C) — RenderSystem が SkyRenderer から設定し、影パスが ShadowConstantsCB へ転送する。

    const math::Frustum* cameraFrustum = nullptr;
    /// @note 最遠カスケードの錐台 (= 影が届く範囲全体)。
    /// @note ShadowPass はカスケードごとに shadowCascades[i].frustum でカリングする。
    /// @note こちらは「影の到達範囲に入るか」を 1 回で判定したいパス向けの代表値。
    const math::Frustum* lightFrustum  = nullptr;
    OcclusionCuller*      occlusionCuller = nullptr;
    /// @note 空連動 IBL の永続状態 (フレームをまたぐ。RenderSystem が static 実体を指す)。
    EnvironmentResources* environmentResources = nullptr;

    /// @note カメラ視点で実際に発行した描画の統計。
    /// @note パスごとに手書きで加算すると新パス追加時に数え漏れるため、ジオメトリ系パスは
    /// @note SubmitCounted() 経由で Submit し集計を 1 か所に集める。
    int statsTotalObjects    = 0;
    int statsFrustumCulled   = 0;
    int statsOcclusionCulled = 0;
    int statsDistanceCulled    = 0;
    int statsSmallObjectCulled = 0;
    int statsDrawCalls       = 0;
    int statsVertexCount     = 0;
    int statsTriangleCount   = 0;
    /// @note 束ねて発行した Instanced Draw の回数と、それによって減ったドロー数。
    /// @note カメラ視点と影で分けない。«束ねが効いているか» を見る数字で、発行先の区別は
    /// @note statsDrawCalls / statsShadowDrawCalls 側が既に持っているため。
    /// @see Docs/design/gpu-instancing.md
    int statsInstancedBatches    = 0;
    int statsInstancedDrawsSaved = 0;
    /// @note SkinningComputePass がこのフレームのポーズについて実際に処理した仕事量。
    /// @note Scene/Game View が結果を共有した場合も、後側のビューへ同じ値を引き継ぐ。
    uint64_t statsSkinningVertexCount = 0;
    uint32_t statsSkinningDispatchCount = 0;
    /// @note シャドウマップ描画は同じジオメトリを光源視点で再描画するため、
    /// @note カメラ統計に混ぜず独立したカウンターへ集計する。
    int statsShadowDrawCalls     = 0;
    int statsShadowTriangleCount = 0;
    int statsParticleEmitters = 0;
    int statsParticleVisible = 0;
    int statsParticleCulled = 0;
    int statsParticleBudgetDropped = 0;
    /// @}

};

/// @note DrawCall 1 件が描く三角形数。
/// @note indexCount=0 の非インデックス描画 (フルスクリーン三角形・SV_VertexID 生成ジオメトリ) は
/// @note vertexCount を 3 で割る必要があり、加算側で毎回書き分けると数え間違いが起きる。
[[nodiscard]] inline int DrawCallTriangleCount(const renderer::DrawCall& call)
{
    if (call.topology != renderer::PrimitiveTopology::TRIANGLE_LIST) return 0;
    const uint32_t perInstance = call.indexCount > 0 ? call.indexCount / 3u : call.vertexCount / 3u;
    return static_cast<int>(perInstance * (call.instanceCount > 0 ? call.instanceCount : 1u));
}

/// @note DrawCall 1 件が描く頂点数 (インスタンシングを含む)。
/// @note インデックス描画では「メッシュのユニーク頂点数」を表示したいので vertexCount を
/// @note 優先し、埋めていないパスのために indexCount へフォールバックする。
[[nodiscard]] inline int DrawCallVertexCount(const renderer::DrawCall& call)
{
    const uint32_t perInstance = call.vertexCount > 0 ? call.vertexCount : call.indexCount;
    return static_cast<int>(perInstance * (call.instanceCount > 0 ? call.instanceCount : 1u));
}

/// @note ジオメトリ系パス共通の Submit ラッパー。カメラ視点の描画統計を同時に加算する。
/// @note Stats パネルは「実際に GPU へ投げた描画」の数値でなければ意味がない。各パスが
/// @note このヘルパーを使うことで、パスを増やしても統計が自動的に追従する。
inline void SubmitCounted(RenderPassContext& ctx, const renderer::DrawCall& call)
{
    ctx.renderer.Submit(call, ctx.resources);
    ++ctx.statsDrawCalls;
    ctx.statsVertexCount   += DrawCallVertexCount(call);
    ctx.statsTriangleCount += DrawCallTriangleCount(call);
}

/// @note シャドウマップ用 Submit ラッパー。光源視点の描画をカメラ統計と分けて集計する。
inline void SubmitCountedShadow(RenderPassContext& ctx, const renderer::DrawCall& call)
{
    ctx.renderer.Submit(call, ctx.resources);
    ++ctx.statsShadowDrawCalls;
    ctx.statsShadowTriangleCount += DrawCallTriangleCount(call);
}

} /// @note namespace fbzz::renderer
