/// @file    RenderPassContext.hpp
/// @brief   RenderGraph 注入パスと各描画パスが共有する実行コンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Renderer/View/Lighting/RenderLightingInput.hpp>
#include <Graphics/Renderer/Geometry/SurfacePatchLod.hpp>
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <Graphics/Effects/Animation/RenderSkinningInput.hpp>
#include <array>
namespace fbzz::renderer {
/// @note 検証済みの当該ビュー用の借用ハンドル。フレームを越えて保持しない。
struct DirectionalShadowBakeView {
    ResourceHandle<TextureTag> depth; ///< ベイク済みの通常 Z 深度を参照する非所有ハンドル。
    ResourceHandle<ShaderTag> restoreShader; ///< ベイク深度を当該カスケードに復元するシェーダー。
    ResourceHandle<PipelineStateTag> restorePipeline; ///< ベイク深度の復元に使うパイプライン状態。
    math::Matrix4 cascadeToBakeClip; ///< カスケードのクリップ座標をベイク時のクリップ座標へ変換する行列。
    float maxSmoothDepthStep = 0.0f; ///< ベイク深度から連続面として補間を許す最大深度差。
    float sourceDepthBias = 0.0f; ///< ベイク元深度を復元する際の深度バイアス。
    bool includesStaticMeshes = false; ///< ベイク済み深度に静的メッシュが含まれるかどうか。
};

struct StaticShadowCacheView {
    ResourceHandle<RenderTargetTag> atlas; ///< 当該ビューの静的シャドウを保持する非所有アトラスハンドル。
    uint32_t cascadeMask = 0; ///< 静的キャッシュの対象となるカスケードのビット集合。
    uint32_t reuseMask = 0; ///< 前回の内容を再利用できるカスケードのビット集合。
    uint32_t readyMask = 0; ///< キャッシュから当該フレームへ公開できるカスケードのビット集合。
    uint32_t updateMask = 0; ///< このフレームに再描画するカスケードのビット集合。
    std::array<uint64_t, kMaxShadowCascades> contentKeys{}; ///< カスケード番号に対応するキャッシュ内容の照合キー。
};

/// @note 成功したベイクだけが、その後書き換えないテクスチャを公開できる。資源管理者・世代と正確なハンドルを照合する。
struct RenderTexturePublication {
    ResourceHandle<TextureTag> texture;
    const ResourceManager* owner = nullptr; ///< 公開テクスチャを所有する ResourceManager への借用ポインター。
    uint64_t resourceEpoch = 0; ///< 公開時の ResourceManager のリセット世代。
    [[nodiscard]] bool Matches(ResourceHandle<TextureTag> handle, const ResourceManager* resources, uint64_t epoch) const
    {
        return texture.IsValid() && texture == handle && owner == resources && resourceEpoch == epoch;
    }
};

/// @note 色パスで実際に描いた粒子のバインディングを、同じビューの後続パスに渡す。
struct ParticleViewDraws {
    struct Bindings {
        ResourceHandle<ConstantBufferTag> renderCB; ///< 色パスの描画定数を後続パスで再利用する非所有ハンドル。
        ResourceHandle<TextureTag> texture;
        ResourceHandle<TextureTag> motionVectorTexture; ///< 後続パスで同じフリップブック補間を評価する速度テクスチャ。
    };
    struct Billboard {
        Bindings bindings; ///< 色パスで粒子に使った定数とテクスチャの組。
        ResourceHandle<BufferTag> vertexBuffer; ///< 色パスで生成したビルボード頂点の非所有ハンドル。
        int quadCount = 0; ///< vertexBuffer に描画した四角形の数。
    };
    struct Gpu {
        Bindings bindings; ///< 色パスで粒子に使った定数とテクスチャの組。
        ResourceHandle<StructuredBufferTag> particleBuffer; ///< 色パスが参照した GPU 粒子状態のバッファ。
        ResourceHandle<StructuredBufferTag> sortBuffer; ///< 色パスでの GPU 粒子の並べ替え結果。
        int drawCount = 0; ///< 色パスで描画した GPU 粒子の数。
    };
    std::vector<Billboard> billboards; ///< 色パスで描画した CPU ビルボードの記録列。
    std::vector<Gpu> gpu; ///< 色パスで描画した GPU 粒子エミッターの記録列。
    std::weak_ptr<const RenderScene> snapshot; ///< 色パスで用いたシーンの同一性を確認する弱参照。
    uint64_t frame = 0; ///< 描画の記録元の資源フレーム番号。
    uint64_t resourceResetVersion = 0; ///< 記録元の資源管理器のリセット世代。
};

struct RenderPassContext : RenderLightingInput {
    renderer::IRenderer& renderer; ///< 描画コマンドを記録する呼び出し元所有のレンダラー。
    renderer::ResourceManager& resources; ///< GPU 資源を解決・更新する呼び出し元所有の管理器。
    const renderer::Camera& camera; ///< このビューの姿勢・射影を提供する借用カメラ。
    const renderer::RenderSettings& settings; ///< 当該ビューの有効パスと表示品質を決める借用設定。
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT; ///< 最終出力先となる非所有レンダーターゲット。
    uint32_t cullingMask; ///< 当該ビューで描画を許すレイヤーのビット集合。

    /// @note ここまでが集成体初期化で埋める前半。参照メンバー handles より前に
    /// @note 既定値付きフィールドを挿すと呼び出し側の初期化子が 1 つずつずれるため、
    /// @note 新しい設定は必ず handles より後ろへ追加すること。
    RenderPassHandles& handles; ///< 当該ビューへ割り当てたパス資源の非所有参照。
    /// @note 呼び出し専用の形状データ。RenderSystem は上限付きのビュー領域を借用でき、直接の呼び出し元はローカル領域を保持する。
    SurfacePatchSelectionCache surfacePatchSelectionCache; ///< 形状パッチの選択結果を実行中のパス間で再利用するキャッシュ。
    /// @note ホストが実験 RT のセッション許可を渡す。保存された描画要求や実機能力とは独立する。
    /// @see Docs/design/developer-mode.md
    bool experimentalRayTracingEnabled = false; ///< ホストが当該セッションで実験機能のレイトレーシングを許可したかどうか。

    /// @note スキニング完了後に抽出し、派生ビューを含む記録終了まで共有する不変入力。
    std::shared_ptr<const renderer::RenderScene> renderScene; ///< 派生ビューのコマンド記録完了まで保持する、このフレームの不変シーン入力。
    std::vector<RenderSkinningInput> skinningRequests; ///< スキニングパスが処理する姿勢・形状の要求列。
    std::vector<math::Matrix4> skinningMatrices; ///< 要求のオフセットから読むボーン行列列。
    bool skinningExecuted = false; ///< このコンテキストでスキニングパスを処理済みかどうか。
    /// @note 明示的に有効化したワーカーは値だけを返す。実行記録はネイティブポインターや生成側の入力を借用しない。
    RenderSkinningBatchReceipt skinningBatchReceipt; ///< 借用ネイティブ資源を持たない、スキニングバッチの記録結果。
    uint64_t frameStamp = 0; ///< このビューが属する資源フレームの番号。
    StaticShadowCacheView staticShadowCache; ///< このビューが参照できる静的影キャッシュの状態。
    float time = 0.0f; ///< 描画のアニメーションへ渡すシミュレーション経過時間 [s]。
    float deltaTime = 0.0f; ///< ゲーム時間での前回更新からの経過 [s]。
    float unscaledDeltaTime = 0.0f; ///< 時間倍率を適用しない前回更新からの経過 [s]。
    RenderEnvironmentInput environment; ///< このビューで描く空、光、雲の解決済み入力。
    RenderTexturePublication iblIrradiancePublication, iblPrefilterPublication;

    /// @note カリング挙動 (CameraComponent 由来)
    /// @{
    /// @note パスが直接 CameraComponent を読むと、Scene View や VFX プレビューでゲームカメラの
    /// @note 設定が効いてしまい、エディタ上の見え方が編集対象と食い違う。
    bool  frustumCullingEnabled   = true; ///< 視錐台の外の描画候補を落とすかどうか。
    bool  occlusionCullingEnabled = true; ///< 深度による遮蔽判定を使うかどうか。
    /// @note 全バウンディング球へ加算するワールド単位の余白 [m]。
    float cullingBoundsPadding    = 0.0f; ///< 境界球のカリング判定に加えるワールド余白 [m]。
    /// @note 距離カリング。値は解決済み (負値なし)。0 は「無効」。
    float cullMaxDistance         = 0.0f; ///< このビューのカリング距離上限 [m]。0 は無効。
    float cullLayerDistances[32] = {}; ///< レイヤー番号ごとのカリング距離上限 [m]。0 の要素はレイヤー別上限を指定しない。
    /// @note レイヤー別距離が 1 つでも設定されているか。全 0 のときは 32 要素の探索ごと省く。
    bool  hasLayerCullDistances   = false; ///< 正のレイヤー別距離設定が存在するかどうか。
    bool  cullDistanceSpherical   = true; ///< 距離判定でカメラからの球面距離を使うかどうか。
    /// @note 極小オブジェクトカリングのしきい値 (画面高さ比)。0 は無効。
    float smallObjectScreenHeight = 0.0f; ///< 極小物を除く境界球の画面高さ比。0 は無効。
    /// @note 画面高さ比の算出に使う射影スケール = projection.m[1][1] = 1/tan(fovY/2)。
    /// @note Camera::GetProjectionMatrix() は毎回行列を組み直して返すため、オブジェクトごとに
    /// @note 呼ぶと判定より行列生成の方が高くつく。事前計算して持つ。
    float cullProjScaleY          = 0.0f; ///< 画面高さ比の算出に使う射影行列の Y スケール。
    /// @note 平行投影か。画面高さ比は距離で割らないので、判定式が分岐する。
    bool  cullOrthographic        = false; ///< カリングの画面高さ計算で平行投影の式を使うかどうか。
    /// @note 距離計算用のカメラ前方ベクトル (深度距離モードでのみ使う)。同じ理由で事前計算する。
    math::Vector3 cullCameraForward = { 0.0f, 0.0f, 1.0f }; ///< 深度距離によるカリング用のワールド空間のカメラ前方。

    uint32_t width = 0; ///< 描画スケールを適用したビューの内部幅 [px]。
    uint32_t height = 0; ///< 描画スケールを適用したビューの内部高さ [px]。
    /// @note 出力先 (outputRT) の実寸。width/height は描画スケールを掛けた内部解像度なので、
    /// @note 両者が食い違うフレームだけ UpscalePass が最終解像へ引き伸ばす。
    uint32_t outputWidth  = 0; ///< outputRT の最終出力幅 [px]。
    uint32_t outputHeight = 0; ///< outputRT の最終出力高さ [px]。
    /// @note LDR チェーンの終着点。等倍なら outputRT そのもので、描画スケールが効いている
    /// @note フレームは内部解像度の中継 RT を指す。
    /// @note ここを噛ませないと Composite/CustomPP/SelectionOutline/FXAA のうち outputRT へ
    /// @note 直接書くパスだけが実寸で走ってしまう。全段を内部解像度に揃え、実寸で走るのは
    /// @note UpscalePass の 1 回だけにする。
    renderer::ResourceHandle<renderer::RenderTargetTag> chainOutputRT; ///< LDR チェーンの出力先。描画スケールが等倍なら outputRT。
    /// @note Composite の書き先。後段のチェーンがあれば ldrRT、無ければ chainOutputRT。
    /// @note 以前は Composite 側が «後段があるか» を自前で判定しており、RenderSystem の
    /// @note hasPostCompositeEffects (数え方が違う) と食い違い、AfterOpaque 段のカスタム
    /// @note パスだけのシーンで «Output を誰も書かない» フレームが出た。判定は 1 か所に置く。
    renderer::ResourceHandle<renderer::RenderTargetTag> compositeOutputRT; ///< Composite の書き込み先。LDR 後処理の有無を反映する。
    /// @note 名前 → 実ハンドルの対応表。パイプラインを組む側が毎フレーム埋め、
    /// @note パスは PassResources 越しに «申告した名前» でだけ引く。
    RenderResourceRegistry resourceRegistry; ///< パスが申告した論理名と現在の GPU 資源ハンドルの対応表。

    /// @note 実行中のパスの引き出し口。RenderPipeline が Execute の直前に差し、直後に外す。
    /// @note 自由関数の引数にしないのは、パス本体が 35 本以上の自由関数とヘルパーに
    /// @note 散っていて、署名を全部変えても «申告した名前からしか引けない» という保証
    /// @note (強制しているのは PassResources 自身) が強くならないため。
    PassResources* passResources = nullptr; ///< 現在実行中のパスの資源アクセス。グラフ外とパス間では null。

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
    bool selectionOutlineEnabled = false; ///< エディター等の選択輪郭を描くビューかどうか。
    /// @note 今フレームに 1 件でも生きた輪郭要求があるか (RenderSettings::objectMaskRequests)。
    bool objectMaskEnabled = false; ///< アクティブなオブジェクト輪郭要求があるかどうか。
    /// @note 当該ビューで入力パスと消費先を登録できる。出力を参照する際は記録結果を確認する。
    bool ssrPassPlanned = false; ///< SSR の入出力を当該ビューで登録できるかどうか。
    /// @note 同期 TryDispatch が当該ビューの SSRResult への記録を受理した場合だけ true。
    bool ssrDispatchRecorded = false; ///< 当該ビューの SSR 出力への GPU 記録が受理されたかどうか。
    /// @note 当該ビューの SSR を参照して HDR への描画を Submit した。TAA はこの状態で照明履歴を照合する。
    bool ssrContributionSubmitted = false; ///< SSR を読んで HDR へ反映する描画を提出したかどうか。
    /// @note 当該ビューの Velocity がクリア・更新済みの場合だけ消費側へ公開する。
    bool velocityWritten = false; ///< 今回のビューで速度バッファを消去・更新し、後続パスから読めるかどうか。
    /// @note 水中の散乱だけCompositeへ遅延合成する。今フレームの記録成功をビュー単位で保持する。
    bool underwaterLightPassActive = false; ///< 水中散乱の Composite への遅延合成を有効化できたかどうか。
    /// @note このフレームの水面受光処理は、ビュー固有の媒質テーブルとワールド XZ アトラスを参照できる。
    bool waterMediumPassActive = false; ///< 当該ビューで水域の媒質テーブルと世界 XZ アトラスを参照できるかどうか。
    /// @note 水面界面マスクが有効になるのは、グラフパスが現在のビューの出力を書いた後。
    bool waterInterfacePassActive = false; ///< 当該ビューの水面界面マスクが生成済みかどうか。
    /// @note 0 の場合はすべての画素を厳密な経路で処理する。
    std::array<uint32_t, 8> waterOpticalDryTiles{}; ///< 光学処理を省略できる水域外タイルのビット集合。すべて 0 なら省略しない。
    /// @note 可視の水域が固定のアトラス画素予算を共有する場合、品質を下げる。
    bool waterMediumQualityLimited = false; ///< 水域に割り当てたアトラス画素予算で品質を抑えたかどうか。
    /// @note HYBRID を要求した Lit Deferred の反射 resolver。RT 縮退・記録失敗でも全 HDR 補間へ戻さない。
    bool hybridReflectionResolveActive = false; ///< 当該ビューでハイブリッド反射の合成パスを有効にしたかどうか。
    /// @note SSR の照明入力を作る間だけ true。RT と SSR の差し替え前の baseline を描く。
    bool hybridReflectionSourcePass = false; ///< SSR に渡す反射合成前の照明入力を描画中かどうか。
    /// @note 生きた SSR 資源と pass override を確認した当該ビューの graph 宣言条件。
    [[nodiscard]] bool IsHybridSsrPlanned() const { return hybridReflectionResolveActive && ssrPassPlanned; }
    /// @note 当該ビューで RayReflection が実行可能なフレームだけ有効。材質・形状の被覆不足では false。
    bool rayReflectionPassActive = false; ///< 当該ビューで材質・形状がそろい、レイ反射を記録できるかどうか。
    /// @note Graph は filtered/RAW の両方を申告し、再構成の記録失敗時も同フレーム RAW だけを読む。
    bool rayReflectionReconstructionPrepared = false; ///< レイ反射の再構成と、失敗時に読む同フレームの生出力を申告済みかどうか。
    /// @note Reference Path の照明・深度がこのビューで有効な場合のみ true。
    bool rayPathPassActive = false; ///< 参照パストレーシングに必要な照明と深度が当該ビューで有効かどうか。
    /// @note dispatch 失敗でも専用 Path 表示では Raster の履歴・画面空間効果を束縛しない。
    bool rayPathViewActive = false; ///< ラスター履歴と画面空間効果を束縛しない専用パス表示かどうか。
    /// @note hit の照明へ未対応の天候・空間プローブが有効な場合は false。
    bool rayHitLightingSupported = true; ///< ビューの天候・プローブをレイ交差点の照明で扱えるかどうか。
    /// @note Reference は経路を自ら積分するため、Raster 用プローブの準備状態では縮退しない。
    bool rayPathLightingSupported = true; ///< ラスター用プローブとは独立した参照パスの照明条件を満たすかどうか。
    /// @note 選択マスクへ UI 要素の矩形を追記する。UISystemContext を握っているのは
    /// @note Viewport ごとの呼び出し元なので、パス側は「入っていれば呼ぶ」だけにする。
    std::function<void()> appendUISelectionMask; ///< ビューに属する UI の矩形を選択マスクに追記する任意の処理。

    /// @note TAA サブピクセルジッター (NDC 単位)。TAA が無効なフレームは 0。
    /// @note ジッターが無いと静止カメラでは同じ絵に収束してアンチエイリアスにならない。
    /// @note b0 を組む各パスが MakeCameraFrameCB へ渡す。
    float taaJitterNdcX = 0.0f; ///< TAA がこのフレームの射影へ加える X 偏移 [NDC]。
    float taaJitterNdcY = 0.0f; ///< TAA がこのフレームの射影へ加える Y 偏移 [NDC]。

    /// @}

    /// @note クラスタライトカリング
    /// @{

    /// @note フロクセル霧のフレーム間状態。実体は描画中のビューが持つ。
    /// @note 霧を走らせないフレームは null のことがある。
    FroxelFogViewState*           froxelFogState = nullptr; ///< ビューが所有する体積霧の履歴状態。未使用時は null。

    /// @note 影の光源視点。ビルボードを光源へ正対させる必要があるパス
    /// @note (パーティクル自己影の密度積み) が lightVP の内訳を要求する。
    /// @note GBuffer を使う不透明パイプラインが有効かどうか。
    /// @note RenderSettings の Forward/Deferred 名ではなく、各パスが GBuffer 入力を
    /// @note 読めるかで判定する。
    bool                        isDeferred  = false; ///< 不透明描画で GBuffer の材質入力を使うかどうか。
    bool                        ssaoEnabled = false; ///< このビューの描画で画面空間 AO が有効かどうか。
    /// @note GBuffer の深度が「この時点で書き終わっている」か。
    /// @note Deferred では常に true、Forward では GBuffer プリパスを走らせたときだけ true。
    /// @note isDeferred は「不透明を GBuffer でライティングするか」でこちらとは別。同一視すると
    /// @note 接触影が hdrRT のまだ書かれていない深度を読む。
    bool                        gbufferDepthReady = false; ///< この時点で GBuffer の深度書き込みが完了しているかどうか。
    /// @note 前方描画のマテリアルへ渡す画面空間の遮蔽。無効ハンドルなら束縛しない。
    /// @note AO は GTAO / SSAO のうち実際に走った方が入る (シェーダーからは区別しない)。
    renderer::ResourceHandle<renderer::TextureTag> screenAoTexture; ///< 前方描画に渡す SSAO または GTAO の出力。無効なら束縛しない。
    renderer::ResourceHandle<renderer::TextureTag> screenContactShadowTexture; ///< 前方描画に渡す画面空間接触影。無効なら束縛しない。
    /// @note ライト正射影の深度範囲で正規化済みの NDC バイアス。固定 NDC 値だとシーンスケール
    /// @note 依存になるので、0.005 / depthRange としてワールド約 5mm 相当を保つ。
    /// @}

    /// @note カスケードシャドウ
    /// @{
    /// @}

    /// @note Spot / Point シャドウ
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

    /// @note ライト Cookie
    /// @{
    /// @}

    /// @note レガシー経路 (b3) 向けの「大きさを持つ光源」
    /// @{
    /// @note punctualLights から Area / Sphere / Tube を先頭 kMaxLegacyShapedLights 本まで
    /// @note 写したもの。b3 はこれらの型を運べないため、b12 側へ実体ごと載せる。
    /// @note b3 の点光源 / スポットの光源半径 [m]。添字は legacyShadowSlots と同じ。
    /// @note 別配列にする理由: b3 の PointLightData は 32 バイトぴったりで 1 float も空きが無い。

    /// @note 雲シャドウ (Phase C) — RenderSystem が SkyRenderer から設定し、影パスが ShadowConstantsCB へ転送する。

    const math::Frustum* cameraFrustum = nullptr; ///< カメラのワールド空間の視錐台を指す借用ポインター。
    /// @note 最遠カスケードの錐台 (= 影が届く範囲全体)。
    /// @note ShadowPass はカスケードごとに shadowCascades[i].frustum でカリングする。
    /// @note こちらは「影の到達範囲に入るか」を 1 回で判定したいパス向けの代表値。
    const math::Frustum* lightFrustum  = nullptr; ///< 最遠カスケードのワールド空間の視錐台を指す借用ポインター。
    OcclusionCuller*      occlusionCuller = nullptr; ///< 当該ビューの遮蔽を準備する借用カリング器。
    ResourceHandle<StructuredBufferTag> occlusionGpuRows; ///< GPU の遮蔽判定へアップロードした描画候補の行バッファ。
    bool gpuOcclusionEnabled = false; ///< GPU 遮蔽に必要な資源とパス構成がそろっているかどうか。
    bool gpuOcclusionTerrain = false; ///< このビューの CPU 地形深度を遮蔽の準備で参照できるかどうか。
    bool gpuOccluderDepthReady = false; ///< 当該ビューの GPU 遮蔽深度を読み取れるかどうか。
    /// @note このスナップショットのメッシュ項目をグラフの間接引数バッファへ対応付ける。
    std::vector<uint32_t> gpuOcclusionArguments; ///< スナップショットのメッシュ項目をグラフの間接引数行へ対応付ける添字列。
    std::vector<bool> gpuOccluderItems; ///< メッシュ項目ごとに GPU 遮蔽深度へ描く候補かどうかを示す列。
    std::vector<bool> gpuOcclusionCounted; ///< RenderScene::Objects() の添字ごとに GPU 遮蔽の統計へ計上したかどうかを示す列。
    /// @note CPU 深度と転送済みの行は、この不変の公開データと該当ビューに属する。コールバックでいずれかの入力が置き換わると照合が失効する。
    std::shared_ptr<const renderer::RenderScene> occlusionPreparedScene; ///< CPU 深度と GPU 入力を準備したシーンの同一性を照合する共有参照。
    const OcclusionCuller* occlusionPreparedCuller = nullptr; ///< 遮蔽準備時のカリング器の同一性を確認する借用ポインター。
    uint64_t occlusionPreparedCullerVersion = 0; ///< 遮蔽準備で使ったカリング器の内容版。
    std::array<uint32_t, 96> occlusionPreparedView{}; ///< 遮蔽準備時のビュー入力を比較用に保存した語列。
    std::vector<RenderPassOverride> occlusionPreparedPassOverrides; ///< 遮蔽準備時に適用されていたパスの上書き設定。
    uint64_t occlusionPreparedSerial = 0, occlusionPreparedSceneGeneration = 0, occlusionPreparedFrame = 0;
    uint64_t occlusionPreparedResourceEpoch = 0, occlusionPreparedResourceFrame = 0;
    /// @note 空連動 IBL の永続状態 (フレームをまたぐ。RenderSystem が static 実体を指す)。
    EnvironmentResources* environmentResources = nullptr; ///< 空に連動する IBL のビュー間・フレーム間状態への借用参照。

    /// @note カメラ視点で実際に発行した描画の統計。
    /// @note パスごとに手書きで加算すると新パス追加時に数え漏れるため、ジオメトリ系パスは
    /// @note SubmitCounted() 経由で Submit し集計を 1 か所に集める。
    int statsTotalObjects    = 0; ///< カメラ視点の描画候補として数えた物体数。
    int statsFrustumCulled   = 0; ///< 視錐台判定で除外した物体数。
    int statsOcclusionCulled = 0; ///< 深度遮蔽で除外した物体数。
    int statsDistanceCulled    = 0; ///< 距離上限で除外した物体数。
    int statsSmallObjectCulled = 0; ///< 画面高さが閾値未満で除外した物体数。
    /// @note GPU が可視判定する未読戻し候補数。可視数やカリング数へ推測で加えない。
    int statsGpuVisibilityUnknownObjects = 0; ///< GPU が可視判定し、CPU へ結果を戻さない候補数。
    int statsDrawCalls       = 0; ///< カメラ視点で発行した描画コマンド数。
    int statsVertexCount     = 0; ///< カメラ視点の描画で提出した頂点数。インスタンス数を含む。
    int statsTriangleCount   = 0; ///< カメラ視点の描画で提出した三角形数。インスタンス数を含む。
    /// @note 束ねて発行した Instanced Draw の回数と、それによって減ったドロー数。
    /// @note カメラ視点と影で分けない。«束ねが効いているか» を見る数字で、発行先の区別は
    /// @note statsDrawCalls / statsShadowDrawCalls 側が既に持っているため。
    /// @see Docs/design/gpu-instancing.md
    int statsInstancedBatches    = 0; ///< 複数個を束ねて発行したインスタンシング描画の件数。
    int statsInstancedDrawsSaved = 0; ///< インスタンシングによって発行を省けた描画コマンド数。
    /// @note 永続配置行のコピー量 [バイト]。配置が変わらなければウォームアップ後は 0。
    uint64_t statsInstanceUploadBytes = 0; ///< 永続配置の更新でコピーしたデータ量 [byte]。
    /// @note SkinningComputePass がこのフレームのポーズについて実際に処理した仕事量。
    /// @note Scene/Game View が結果を共有した場合も、後側のビューへ同じ値を引き継ぐ。
    uint64_t statsSkinningVertexCount = 0; ///< GPU スキニングが実際に処理した頂点数。
    uint32_t statsSkinningDispatchCount = 0; ///< GPU へ記録したスキニングのディスパッチ数。
    uint64_t statsSkinningPaletteUploadBytes = 0; ///< ボーン行列パレットの GPU 転送量 [byte]。
    uint32_t statsSkinningPaletteUploads = 0; ///< ボーン行列パレットを転送した回数。
    /// @note demand の候補・準備・実投入を分け、CPU 側の保留を GPU 省略として誤計上しない。
    uint64_t statsSkinningDemandMode = 0; ///< 計測時に選択されていたスキニング要求の処理モード。
    uint64_t statsSkinningDemandCandidates = 0; ///< スキニング要求の候補数。
    uint64_t statsSkinningDemandRequired = 0; ///< パス構成によってスキニングが必要になった要求数。
    uint64_t statsSkinningDemandPrepared = 0; ///< 実行用の資源を準備できたスキニング要求数。
    uint64_t statsSkinningDemandSubmitted = 0; ///< GPU 記録まで進んだスキニング要求数。
    uint64_t statsSkinningDemandSkipped = 0; ///< 不要と判断して省略したスキニング要求数。
    /// @note シャドウマップ描画は同じジオメトリを光源視点で再描画するため、
    /// @note カメラ統計に混ぜず独立したカウンターへ集計する。
    int statsShadowDrawCalls     = 0; ///< 光源視点で発行した影の描画コマンド数。
    int statsShadowTriangleCount = 0; ///< 影パスで提出した三角形数。カメラ統計には含めない。
    int statsParticleEmitters = 0; ///< 当該ビューで評価した粒子エミッター数。
    int statsParticleVisible = 0; ///< 抽出時に描画を割り当てた全エミッターの粒子スロット数の合計。
    int statsParticleCulled = 0; ///< 当該ビューのカリングで描画しなかったエミッター数。
    int statsParticleBudgetDropped = 0; ///< 粒子の一部または全部が描画上限から外れたエミッター数。
    ParticleViewDraws particleDraws; ///< 色パスの粒子描画を後続パスへ引き継ぐ記録。
    /// @}

    /// @note 選択中の Directional Light が Auto Bake のとき true。ベイクの有効性は directionalShadowBake が持つ。
    bool directionalShadowBakeEnabled = false; ///< 選択中の平行光源が静的影の自動ベイクを使うかどうか。
    DirectionalShadowBakeView directionalShadowBake; ///< 当該ビューで検証済みの平行光源のベイク深度入力。
};

/// @note DrawCall 1 件が描く三角形数。
/// @note indexCount=0 の非インデックス描画 (フルスクリーン三角形・SV_VertexID 生成ジオメトリ) は
/// @note vertexCount を 3 で割る必要があり、加算側で毎回書き分けると数え間違いが起きる。
[[nodiscard]] inline int DrawCallTriangleCount(const renderer::DrawCall& call)
{
    uint32_t perInstance = 0;
    switch (call.topology) {
    case renderer::PrimitiveTopology::TRIANGLE_LIST:
        perInstance = (call.indexCount > 0 ? call.indexCount : call.vertexCount) / 3u;
        break;
    case renderer::PrimitiveTopology::TRIANGLE_STRIP:
        perInstance = call.indexCount > 0 ? call.stripTriangleCount : (std::max)(call.vertexCount, 2u) - 2u;
        break;
    case renderer::PrimitiveTopology::LINE_LIST:
        break;
    }
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

}
