/// @file    RenderSettings.hpp
/// @brief   Rendering and post-process settings.
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Engine/Renderer/RenderState.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

enum class RenderingPipeline : uint8_t {
    Forward      = 0,
    Deferred     = 1,
    // クラスタライトカリングを併用する経路。不透明の描き方は上の 2 つと同じで、
    // 「点光源 / スポットをどう供給するか」だけが変わる。
    ForwardPlus  = 2,
    DeferredPlus = 3,
};

// クラスタライトカリング (Forward+ / Deferred+) の設定。
struct ClusteredSettings {
    // pipeline が *Plus のときの実行可否。false ならクラスタ経路を止めて従来動作へ戻す。
    bool  enabled      = true;
    // クラスタ Z 分割の最遠距離 [m]。これより遠いライトは最終スライスへ丸める。
    // WHY camera.far をそのまま使わないか: far が 10000 のようなシーンでは指数分割の
    //     手前側が潰れ、カメラ近傍のクラスタがほとんど機能しなくなる。
    float maxDistance  = 200.0f;
    // 1 クラスタあたりのライト数をヒートマップ表示する (緑=空き, 赤=上限, 青成分=あふれ)。
    bool  debugHeatmap = false;
    // カリングを無効化し、全ライトを線形評価する。
    // WHY: 「評価側のバグ」と「カリング側のバグ」を切り分けるための A/B スイッチ。
    //      これを true にしてレガシー経路と絵が一致すれば、評価側は正しいと確定できる。
    bool  forceAllLights = false;
};

struct RenderSelectionID {
    uint32_t index = 0xFFFFFFFFu;
    uint32_t generation = 0;
};

// オブジェクトマスクへの申告 1 件。エディタ選択 (selectedObjects) とは別系統で、
// ゲームのスクリプトが「この GameObject のシルエットをマスクへ描け」と毎フレーム言う。
//
// マスクの中身は «エンジンが決めた意味» を持たない。RGB と A は書き手が決める
// ペイロードで、読む側 (カスタムパスのシェーダー) と対で意味が決まる:
//   輪郭        … RGB = 縁の色      / A = 太さ
//   選択的グロー … RGB = 光らせる色  / A = 強さ
//   被写界深度   … RGB = 任意        / A = «ここはボカすな» の重み
//   シルエット   … RGB = 透視する色  / A = 濃さ (visibleOnly = false と組で使う)
//
// WHY 値を要求ごとに持つか:
//   同じフレームに違う意味のマスクが並ぶ (＋赤 / −青、あるいは «注目» と «無視»)。
//   1 本の共有値にすると、盤面のどれがどれなのかが後段から区別できない。
//
// WHY フレーム番号を持つか (消す責任を要求側に負わせないため):
//   対象が壊れた・Play を止めた・スクリプトが無効化された、のいずれでも «消し忘れた
//   要求» が残る。EntityID の index は世代を跨いで再利用されるので、古い要求が
//   別の GameObject を光らせうる。申告されたフレームだけ有効にすれば起こらない。
struct RenderObjectMaskRequest {
    RenderSelectionID id;
    // マスクの RGB。意味は書き手と読み手の取り決め。
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    // マスクの A へ入る 0〜1 のスカラー。同上。
    float value = 1.0f;
    // 子の MeshRenderer / SkinnedMeshRenderer も同じ値で描くか。マテリアル単位で
    // メッシュを分けたモデルは、これが無いとルートだけが描かれて形にならない。
    bool includeChildren = true;
    // 手前に何かある画素を捨てるか。
    //
    // WHY 選ばせるか: 輪郭は «見えているもの» の縁でないと透視になるが、壁越しの
    //     シルエット表示は «見えていない» ことそのものが目的で、判定の向きが逆になる。
    //     どちらもマスクの作り方は同じなので、ここ 1 つで両方が書ける。
    bool visibleOnly = true;
    // Time::frameCount。描画側はこのフレームに申告されたものだけを描く。
    uint64_t frame = 0;
};

// 今フレームの申告として扱ってよいか。
//
// WHY «frame + 1 >= 今» という不等式にしないか:
//   Time::frameCount はシーンのリセットで 0 へ戻りうる (Time::Reset)。不等式だと、リセット前に
//   置かれた古い行がすべて «未来のフレーム» として生き残り、EntityID の index 再利用と
//   噛み合って «撃っていない敵が光っている» という形で出てくる。
//
// WHY 1 フレーム前まで許すか:
//   スクリプトの更新より前に描かれるビュー (Editor が 1 フレームに 2 面描く構成) で、
//   片方だけ輪郭が消えて «ビューによって見え方が違う» にならないようにする。
[[nodiscard]] inline bool IsObjectMaskRequestLive(const RenderObjectMaskRequest& request,
                                               uint64_t frameCount)
{
    return request.frame == frameCount || request.frame + 1 == frameCount;
}

struct BloomSettings {
    bool enabled = true;
    float intensity = 0.8f;
    float threshold = 0.7f;
    float softKnee = 0.35f;
};

struct AmbientOcclusionSettings {
    bool enabled = true;
    float intensity = 1.0f;
};

struct FogSettings {
    bool enabled = false;
    float density = 0.06f;
    float farDistance = 10.0f;
    float color[3] = { 0.01f, 0.01f, 0.04f };
    // フォグ色の出どころ。0=Exponential(固定 color), 1=Atmosphere(大気散乱)。
    // AtmosphericScatteringComponent::FogSource と一致させること。
    int   source = 0;
};

struct ColorGradingSettings {
    bool enabled = true;
    float contrast = 0.0f;
    float saturation = 1.0f;
    float hueShift = 0.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
};

struct VignetteSettings {
    bool enabled = false;
    float intensity = 0.25f;
    float smoothness = 0.45f;
    float roundness = 1.0f;
    float color[3] = { 0.0f, 0.0f, 0.0f };
};

struct FilmGrainSettings {
    bool enabled = false;
    float intensity = 0.03f;
    float response = 0.8f;
};

// SharpenSettings — 画面全体の輪郭を軽く強調するポストプロセス設定。
// WHY: モデルやテクスチャ側を変えず、最終出力だけで画の解像感を調整できるようにする。
struct SharpenSettings {
    bool enabled = false;
    float strength = 0.35f;
    float radius = 1.0f;
};

// DepthOfFieldSettings — 深度バッファを使った簡易被写界深度。
// WHY: 速度バッファを必要としない Composite 内の軽量実装に留め、既存 RenderGraph を複雑にしない。
struct DepthOfFieldSettings {
    bool enabled = false;
    float focusDistance = 8.0f;
    float focusRange = 4.0f;
    float blurRadius = 3.0f;
};

struct LensSettings {
    bool chromaticAberrationEnabled = false;
    bool distortionEnabled = false;
    float chromaticAberration = 0.005f;
    float distortion = 0.0f;
    // 放射ブラー。画面中心から外へ引き伸ばす量 [0,1]。
    // WHY ボリューム側に既定値を持たせないか: これは «その瞬間だけ» の演出で、
    //     常時掛かっていると画面端が always ぼける。供給源は VFXScreenEffect の加算だけ。
    float radialBlur = 0.0f;

    // 衝撃波リング。輪の上だけ色を読む座標を半径方向へずらす。amplitude=0 で無効。
    // WHY 加算せず «一番強い 1 枚» を採るか (RenderSystem): 中心と半径を足し合わせると、
    //     2 つの爆発が «画面のどこにも無い中心を持つ 1 つの輪» に化ける。
    float shockRingAmplitude = 0.0f;
    float shockRingRadius = 0.0f;
    float shockRingWidth = 0.12f;
    float shockRingCenter[2] = { 0.5f, 0.5f };
};

// StylizedPostProcessSettings — 演出寄りの色・解像度変換をまとめた設定。
// WHAT: セピア、反転、ポスタライズ、ピクセル化を Composite パス内で順番に適用する。
struct StylizedPostProcessSettings {
    bool sepiaEnabled = false;
    bool invertEnabled = false;
    bool posterizeEnabled = false;
    bool pixelateEnabled = false;
    float sepiaIntensity = 0.75f;
    float invertIntensity = 1.0f;
    float posterizeLevels = 6.0f;
    float pixelSize = 4.0f;
};

// ImageQualitySettings — 写実寄りの最終画質補正をまとめた設定。
// WHY: セピアや反転のような演出効果とは分け、通常のゲーム画面を自然に見やすくする調整を扱う。
struct ImageQualitySettings {
    bool clarityEnabled = true;
    bool shadowHighlightEnabled = true;
    bool colorFilterEnabled = false;
    float clarityStrength = 0.12f;
    float clarityRadius = 2.0f;
    float shadowLift = 0.08f;
    float highlightCompression = 0.08f;
    float colorFilter[3] = { 1.0f, 1.0f, 1.0f };
    float colorFilterIntensity = 0.0f;
};

// ユーザーシェーダーのパスを «いつ» 走らせるか。
//
// WHY 段を選ばせるか:
//   PostProcess (従来) はトーンマップ後の LDR で、画面を作り直す変換 (歪み・階調・
//   ビネット) の場所。1 を超える値は既に潰れているので、ここで足した光は滲まないし
//   露出とも噛み合わない。SceneHDR はトーンマップ前なので、書いた値がそのまま
//   ブルームと露出へ流れる。«盤面の中で光っているもの» はこちらでしか成立しない。
// WHY 番号を詰め直さず末尾へ足すか: .volume は段を整数で持つ。並べ替えると、
//     既に書かれたアセットの «同じ数字» が別の段を指し始める。
enum class CustomPassStage : uint8_t {
    SceneHDR    = 0, // Composite の前 (HDR)。絵が出揃った後。ブルーム・露出が後段に掛かる
    PostProcess = 1, // Composite の後 (LDR)。最終画をそのまま作り直す
    // 不透明の直後 (HDR)。デカール・トレイル・パーティクル・半透明はこの «後» に描かれる。
    //
    // WHY SceneHDR と分けるか: 画面を歪める効果を SceneHDR に置くと、既に描かれた
    //     パーティクルごと曲がって «エフェクトだけ別の場所に居る» 絵になる。
    //     背景だけを歪めて、その上へ半透明を正しく重ねられるのはここだけ。
    AfterOpaque = 2,
};

// カスタムパスが «追加で» 読みたい画面リソース。ビットで足し合わせる。
//
// WHY 既定で全部束縛しないか:
//   Velocity / GBuffer は «有効なフレームにしか存在しない» リソースで、無条件に
//   要求するとパスの成立条件が暗黙になる。宣言しておけば、足りないときに
//   «なぜ動かないか» をエンジン側から名指しで言える。
//
// NOTE: 深度 (SceneDepth) は SceneHDR / AfterOpaque では束縛されない。描き先が
//       hdrRT で、その深度は書き込み先として押さえられているため。
enum CustomPassInput : uint32_t {
    CUSTOM_PASS_INPUT_NONE        = 0u,
    CUSTOM_PASS_INPUT_SCENE_DEPTH = 1u << 0, // t7  シーン深度
    CUSTOM_PASS_INPUT_OBJECT_MASK = 1u << 1, // t6  オブジェクトマスク (+ t8 その深度)
    CUSTOM_PASS_INPUT_VELOCITY    = 1u << 2, // t11 モーションベクター (TAA/MotionBlur 有効時)
    CUSTOM_PASS_INPUT_NORMAL      = 1u << 3, // t12 GBuffer 法線 (Deferred 系のみ)
    CUSTOM_PASS_INPUT_BLOOM       = 1u << 4, // t10 ブルーム (PostProcess 段のみ意味を持つ)
    CUSTOM_PASS_INPUT_SSAO        = 1u << 5, // t9  遮蔽
};

struct CustomPostProcessSettings {
    std::string name = "Custom";
    bool enabled = false;
    std::string shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl";

    // .mat を指すと、シェーダー・テクスチャ・名前付きパラメーター (b2) がそこから来る。
    //
    // WHY shaderPath と併存させるか:
    //   数値 4〜8 個で足りる効果に .mat を要求するのは重い。逆にテクスチャや
    //   名前付きの値が要る段階で parameters[8] に押し込むのは、書いた本人にしか
    //   読めない設定になる。小さいものは shaderPath、育ったものは .mat へ移す。
    //   materialPath が解決できたときは shaderPath より優先する。
    std::string materialPath;

    float intensity = 1.0f;
    float blend = 1.0f;
    // customParameters (0-3) / customParameters2 (4-7) として b5 へ届く。
    float parameters[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    // 追加入力 (CustomPassInput の OR)。オブジェクトマスクは既定で読める。
    uint32_t inputs = CUSTOM_PASS_INPUT_OBJECT_MASK;

    // 同じシェーダーを何回続けて掛けるか。何回目かは customPassInfo.y で分かる。
    //
    // WHY 回数を持たせるか: ぼかしは «横 → 縦» のように同じ処理を向きだけ変えて
    //     重ねるのが定石で、1 回きりだとパスを人数分だけ登録するしかない。
    int iterations = 1;

    // 1 = 実寸 / 2 = 1/2 / 4 = 1/4 の解像度で走らせ、拡大して合成する。
    //
    // WHY 縮めるだけで «ぼけ» になるか: 縮小して線形拡大した時点で 1 段のぼかしが
    //     掛かる。半径の大きいぼかしは、フル解像度でタップを増やすより桁で安い。
    //
    // NOTE: PostProcess 段では無視される (あちらは ping-pong の 2 枚を直列チェーンが
    //       使い切っていて、縮小結果を置く 3 枚目が無い)。
    int downscale = 1;

    CustomPassStage stage = CustomPassStage::PostProcess;

    // 出力の載せ方。HDR の段 (SceneHDR / AfterOpaque) のときだけ効く。
    // .mat を指している場合は、そちらの blend_mode が優先される。
    //
    //   OPAQUE_BLEND — 画面を作り直す。入力の色が t5 に来て、返した色で置き換わる
    //   それ以外     — 寄与だけを描き足す。描き先を読みながら書けないので t5 は来ない
    //
    // WHY PostProcess 段では無視するか:
    //   あちらは ping-pong する RT の直列チェーンで、各パスが «次の 1 枚» を丸ごと
    //   書く契約になっている。途中に加算を挟むと «まだ何も書かれていない RT へ足す»
    //   ことになり、結果が未定義になる。LDR で足したいだけなら t5 を読んで足せば
    //   同じ絵が出るので、段の契約を壊してまで持たせる意味が無い。
    BlendMode blendMode = BlendMode::OPAQUE_BLEND;
};

struct PostProcessSettings {
    bool fxaaEnabled = true;
    float exposure = 1.0f;
    BloomSettings bloom;
    AmbientOcclusionSettings ambientOcclusion;
    FogSettings fog;
    ColorGradingSettings colorGrading;
    VignetteSettings vignette;
    FilmGrainSettings filmGrain;
    SharpenSettings sharpen;
    DepthOfFieldSettings depthOfField;
    LensSettings lens;
    StylizedPostProcessSettings stylized;
    ImageQualitySettings imageQuality;
    std::vector<CustomPostProcessSettings> customEffects;

    // 画面フェード — 全ポストプロセス完了後の最終 lerp として適用する。
    // alpha 0=透明(通常), 1=完全にフェード色で塗りつぶし。
    float screenFadeAlpha        = 0.0f;
    float screenFadeColor[3]     = { 0.0f, 0.0f, 0.0f };  // RGB (デフォルト黒)
};

enum class ViewMode : uint8_t {
    Lit             = 0,
    Unlit           = 1,
    WireframeLit    = 2,
    WireframeUnlit  = 3,
};

// NavMesh オーバーレイの描き方。showNavMesh が true のときだけ効く。
// 1 枚に全部載せると cellSize が小さいシーンでポリゴン境界の線だけで画面が埋まる。
// Recast Demo と同じく、何を確かめたいかで描くものを切り替える。
enum class NavMeshDrawMode : uint8_t {
    // 歩行可能面を不透明寄りに塗り、外周のエッジだけを強調する。形と穴を読む既定値。
    Solid       = 0,
    // 同じ塗りを薄くして地形を透かす。NavMesh と地面のズレを見るとき。
    Transparent = 1,
    // areaType ごとに色を変える。NavMesh Modifier の塗り分けが効いているかを見るとき。
    Areas       = 2,
    // 隣接ポリゴン間の Portal を描く。経路が繋がらない原因が接続かを見るとき。
    Portals     = 3,
    // ベイクのセル判定 (歩行可 / 急斜面 / 段差 / 障害物 / 半径不足) をそのまま描く。
    // 「なぜここに面が張られないか」を追うとき。ベイク時の格子が残っている場合のみ。
    Voxels      = 4,
};

// ShadowSettings — シャドウマップ品質の一元管理。
// 解像度と PCF 半径は精細度と GPU コストのトレードオフなのでシーン単位で調整できる。
// カスケードシャドウの最大分割数。定数バッファが固定長なので上限を切る。
// 4 は 2x2 のアトラス配置と対応し、HLSL 側のループ展開も現実的な長さに収まる。
inline constexpr int kMaxShadowCascades = 4;

struct ShadowSettings {
    // シャドウマップ「アトラス全体」の解像度 (512/1024/2048/4096/8192)。
    // 8192² は深度だけで 268MB / 67M テクセルあり、クリアと塗りだけで数十 ms に達する。
    // cascadeCount >= 2 のときは 2x2 タイルへ分割され、1 カスケードは mapResolution / 2。
    // メモリと塗り量はカスケードを増やしても変わらず、近距離の密度だけが上がる。
    uint32_t mapResolution = 2048u;
    // カスケード分割数 (1 = 従来の単一シャドウマップ, 2〜4 = CSM)。
    // 単一マップでは「近くを細かく」と「遠くまで届かせる」を同じ密度で両立できない。
    int      cascadeCount = 4;
    // 分割位置の対数/等分ブレンド係数 [0,1]。1 に近いほど手前が細かくなる。
    // WHY: 等分割は手前が粗すぎ、対数分割は遠方が粗すぎる。実務では両者の線形補間
    //      (practical split scheme) を使い、シーンに合わせて係数で寄せる。
    float    cascadeSplitLambda = 0.75f;
    // カスケード境界のクロスフェード幅 [0,1] (カスケード端からの割合)。0 で境界が硬くなる。
    // WHY: カスケードが切り替わるとテクセル密度が跳ぶため、境界に不連続な線が
    //      地面を横切って見える。隣接カスケードを重ねて混ぜると、その線が消える。
    float    cascadeBlend = 0.1f;
    // 影ボリュームをカメラ前方の何 m まで合わせるか [m] = 影の最大到達距離。
    // LightComponent::shadowDistance > 0 の手動指定があるときはそちらが優先される。
    // シーン全体へ合わせると広いレベルほどテクセルが粗くなる。
    float    autoFitDistance = 120.0f;
    int      pcfRadius     = 2;     // PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    // カスケード可視化 (デバッグ)。1 = 影の色をカスケード番号で塗り分ける。
    // WHY: 分割位置と境界ブレンドは数値だけでは詰められない。どこで切り替わっているかを
    //      直接見せるのが、cascadeSplitLambda を調整する唯一の実用的な方法。
    bool     debugVisualizeCascades = false;
    // PCSS (Percentage Closer Soft Shadows) — 距離に応じてペナンブラが変化するソフトシャドウ。
    bool     pcssEnabled     = false;
    float    pcssLightRadius = 3.0f; // 仮想ライト半径 (world space): 大きいほどソフト

    // ── Spot / Point シャドウ ────────────────────────────────────────────────
    // Directional の CSM とは独立したアトラス。4x4 = 16 タイルへ分割する。
    // CSM は「カメラからの距離帯」で決まるが、こちらは range と円錐角で決まる。
    // 既定 2048² なら 1 タイル 512² で、range 10m の Spot がおよそ 2cm/テクセル。
    uint32_t punctualMapResolution = 2048u;
    int      punctualPcfRadius     = 1;   // 0=ハード, 1=3x3, 2=5x5
    // 影を落とす「全方位ライト」(Point / Sphere / Tube) の上限。
    // 1 個でキューブ 6 面 = 6 タイルを消費する。既定 2 個で 12 タイル、
    // 残り 4 タイルが Spot / Area (1 個 1 タイル) へ回る。
    //
    // ⚠ 割り当てはカメラから近い順なので、近くに全方位ライトを置くと 6 枚まとめて
    //   持っていかれ、遠くの Spot が枠から落ちる。蛍光灯を何十本も並べた部屋で
    //   「主照明の影だけ消える」のがこれ。器具側の castShadows を絞って調整する。
    int      maxShadowedPointLights = 2;
    // カメラからこの距離を超えるライトは影を落とさない [m]。
    // WHY: アトラスのタイルは 16 枚しかないので、遠くて画面に数ピクセルしか映らない
    //      ライトへ 1 枚割り当てるより、手前のライトへ回す方が常に得。
    float    punctualShadowDistance = 60.0f;
};

// IBLSettings — Image-Based Lighting による環境光。PBR の ambient を物理的に正確に置き換える。
// WHY: 定数 ambient では金属素材がくすんで見える。
//      事前畳み込みキューブマップで環境光を再現することで金属の映り込みが正しく現れる。
struct IBLSettings {
    bool        enabled       = false;  // テクスチャ未ロード時は無効
    float       intensity     = 1.0f;
    float       diffuseScale  = 1.0f;
    float       specularScale = 1.0f;
    int         maxMipLevel   = 6;      // prefilter キューブマップの最大 mip
    std::string irradiancePath;         // Diffuse irradiance cubemap (.dds)
    std::string prefilterPath;          // Specular prefiltered cubemap (.dds)
};

// SSRSettings — スクリーンスペース反射。金属・濡れた床・水面の映り込みをリアルタイムに表現。
// WHY: キューブマップでは静的シーンしか反射できないが、SSR は動的オブジェクトも映せる。
struct SSRSettings {
    bool  enabled     = false;
    float maxDistance = 50.0f;  // 最大レイ距離
    float thickness   = 0.15f;  // 深度交差判定の厚み
    int   steps       = 32;
    float intensity   = 0.8f;
};

// VolumetricLightSettings — レイマーチによる体積光（ゴッドレイ・霧中の光柱）。
struct VolumetricLightSettings {
    bool  enabled    = false;
    int   steps      = 32;
    float scattering = 0.3f;   // Henyey-Greenstein 散乱係数 g
    float intensity  = 0.8f;
    float maxDist    = 30.0f;
    // この距離まではレイを積分しない。カメラ直前に薄い靄が張り付くのを避ける。
    float minDist    = 0.0f;
    // 大気自身の消散係数。0 で減衰なし (光芒が距離に比例して増え続ける従来挙動)。
    // 上げると遠くの光芒ほど手前の大気に吸われ、奥行きが出る。
    float density    = 0.0f;
    // 高度による密度減衰 [1/m]。0 で無効。地表付近ほど濃い霧になり、光芒が床から立ち上がる。
    float heightFalloff = 0.0f;
    float heightStart   = 0.0f; // 減衰の基準高度 [world Y]
    // maxDist 手前でフェードする幅 (割合 [0,1])。0 だと最遠部で光芒が硬く切れる。
    float edgeFade   = 0.2f;
    float tint[3]    = { 1.0f, 1.0f, 1.0f }; // 光芒に掛ける色
};

// FroxelFogSettings — 視錐台を 3D グリッドへ切って霧を焼く方式の体積フォグ。
// VolumetricLightSettings は画面空間レイマーチで解像度に比例して重く、Directional 1 本
// しか扱えない。こちらは粗いグリッドへ一度焼くだけで、点光源やスポットも霧に映る。
// 両方有効にもできるが二重に霧が乗るので、普通はどちらか一方を使う。
struct FroxelFogSettings {
    bool enabled = false;
    // グリッド寸法。XY は画面を切るタイル数、Z は奥行きのスライス数。
    // WHY 既定を 160x90x64 にするか: 16:9 に合わせた粗さで、1 タイルが 1080p の
    //     12x12 画素ぶん。霧は低周波なのでこれで足り、RGBA16F 2 枚で約 14MB に収まる。
    uint32_t gridX = 160;
    uint32_t gridY = 90;
    uint32_t gridZ = 64;
    // グリッドが覆う奥行き [m]。これより奥は最終スライスの値がそのまま伸びる。
    float nearDistance = 0.1f;
    float farDistance  = 64.0f;
    // 一様な消散係数 [1/m]。0.02 で 50m 先の背景が e^-1 まで霞む。
    float density = 0.02f;
    // 散乱アルベド。霧そのものの色。
    float albedo[3] = { 1.0f, 1.0f, 1.0f };
    // 自己発光。夜間のもやを完全な黒に落とさないための下駄。
    float emissive[3] = { 0.0f, 0.0f, 0.0f };
    // Henyey-Greenstein の g。正で前方散乱 (逆光で霧が光る)。
    float anisotropy = 0.4f;
    // 高度による密度減衰 [1/m]。0 で無効。地表付近ほど濃い霧になる。
    float heightFalloff = 0.0f;
    float heightStart   = 0.0f;
    // 環境光が霧へ寄与する倍率。
    float ambient = 1.0f;
};

// AutoExposureSettings — 画面の明るさから露出を自動で決める (眼の順応)。
// 単純平均だと空や光源のごく一部の超高輝度が支配する。対数輝度のヒストグラムを作り、
// 上下のパーセンタイルを捨てて「見ている物」の明るさに合わせる。
struct AutoExposureSettings {
    bool  enabled = false;
    // ヒストグラムが覆う対数輝度の範囲 [EV]。この外の画素は端のビンへ丸める。
    float minEV = -6.0f;
    float maxEV = 14.0f;
    // 露出計算から外す明るさの割合。lowPercent 未満と highPercent 超のビンを捨てる。
    // WHY: 暗部の黒つぶれと光源のハイライトはどちらも「見ている物」ではない。
    float lowPercent  = 0.45f;
    float highPercent = 0.95f;
    // 順応速度 [1/秒]。明るい方へ / 暗い方へで別々に持つ。
    // WHY 非対称か: 実際の眼も明順応は速く暗順応は遅い。同じ速度にすると、
    //      暗い場所へ入った瞬間に画面が白飛びして見え、体感と合わない。
    float speedUp   = 3.0f;
    float speedDown = 1.0f;
    // 求まった露出への手動オフセット [EV]。絵作りの最終調整。
    float compensation = 0.0f;
    // 露出の下限 / 上限 [EV]。真っ暗な画面で露出が無限に上がるのを防ぐ。
    float minExposureEV = -8.0f;
    float maxExposureEV =  8.0f;
};

// TAASettings — テンポラルアンチエイリアシング。FXAA より大幅に高品質でサブピクセルを安定させる。
// WHY: 前フレームの情報を蓄積してジャギーを消す。静止シーンはほぼ完璧になる。
struct TAASettings {
    bool  enabled  = false;
    float feedback = 0.9f;  // 前フレームブレンド比 (0=無効, 0.9=標準, 1=完全履歴)
};

// MotionBlurSettings — カメラモーションブラー。映像的な動きの残像表現。
struct MotionBlurSettings {
    bool  enabled  = false;
    float strength = 0.5f;
    int   samples  = 8;
};

// GTAOSettings — Ground Truth AO (Horizon-Based AO)。SSAO より高品質で接触部の影が自然になる。
struct GTAOSettings {
    bool  enabled       = false;
    float intensity     = 1.0f;
    float radius        = 1.5f;  // サンプリング半径 (world space)
    int   slices        = 3;
    int   stepsPerSlice = 4;
};

// ContactShadowSettings — スクリーンスペースコンタクトシャドウ。
// シャドウマップが捉えられない小物直下の接触影を高精度に表現する。
struct ContactShadowSettings {
    bool  enabled   = false;
    float strength  = 0.5f;
    float rayLength = 1.5f;
    int   steps     = 8;
    float thickness = 0.2f;
};

// LensFlareSettings — スクリーンスペースレンズフレア。強いライトソースによる光学現象の表現。
struct LensFlareSettings {
    bool  enabled    = false;
    float intensity  = 0.5f;
    int   ghostCount = 4;
    float haloWidth  = 0.4f;
    float distortion = 1.0f;
};

// LUTColorGradingSettings — Renderer互換の32^3 LUTをCPU生成するカラーグレーディング設定。
// WHY: 外部DDSの色空間・RGB軸順・解像度差を排除し、全プロジェクトで同じルックを再現する。
struct LUTColorGradingSettings {
    bool        enabled     = false;
    float       blend       = 1.0f;   // LUT とオリジナルのブレンド比
    float       contrast    = 0.0f;
    float       saturation  = 1.0f;
    float       hueShift    = 0.0f;
    float       temperature = 0.0f;
    float       tint        = 0.0f;
};

// VolumeSettings — PostProcessVolume が上書きできる「シーンのルック」設定の全体。
// ポストプロセスも高度グラフィクスも「この場所ではどう見えるか」を決める設定なので、
// ブレンドできる単位を 1 つの型へ集約する (Unity の Volume と同じ考え方)。
// RenderSettings をそのまま使わないのは、pipeline / shadow / デバッグ表示が
// 「プロジェクトの構成」で、場所ごとに切り替えるものではないため。
// IBL も含めない ─ EnvironmentLightComponent が既に唯一の所有者になっている。
struct VolumeSettings {
    PostProcessSettings     post;
    SSRSettings             ssr;
    GTAOSettings            gtao;
    ContactShadowSettings   contactShadow;
    TAASettings             taa;
    MotionBlurSettings      motionBlur;
    VolumetricLightSettings volumetricLight;
    // NOTE: グリッド寸法は Volume でブレンドしない。ApplyVolumeSettings が
    //       RenderSettings 側の寸法を残す (解像度が変わるとボリュームの再確保が走る)。
    FroxelFogSettings       froxelFog;
    AutoExposureSettings    autoExposure;
    LensFlareSettings       lensFlare;
    LUTColorGradingSettings lutColorGrading;

    // 既定は「全効果 OFF」。ルックの供給源は VolumeOverride のリストで、
    // 「リストに入っている効果だけが効く」のが唯一の規則。
    // 個々の XxxSettings の既定値は他の経路 (旧シーン・単体テスト) が期待しているので触らない。
    VolumeSettings()
    {
        post.fxaaEnabled                       = false;
        post.bloom.enabled                     = false;
        post.ambientOcclusion.enabled          = false;
        post.colorGrading.enabled              = false;
        post.imageQuality.clarityEnabled       = false;
        post.imageQuality.shadowHighlightEnabled = false;
        post.imageQuality.colorFilterEnabled   = false;
        // 残りのセクション (fog / vignette / filmGrain / sharpen / dof / lens /
        // stylized / ssr / gtao / …) は構造体側の既定が既に false。
    }

    // 排他的な論理スロット (AA: FXAA/TAA, AO: SSAO/GTAO) を正規化する。
    // 両方をリストへ入れることは操作として可能なので、履歴バッファを必要としない
    // 既存パス (FXAA / SSAO) を優先する。
    void NormalizeExclusiveSlots()
    {
        if (taa.enabled && post.fxaaEnabled)                taa.enabled  = false;
        if (gtao.enabled && post.ambientOcclusion.enabled)  gtao.enabled = false;
    }
};

/// RenderGraph::SchedulePolicy の設定側の写し。
///
/// WHY 列挙を写すか: RenderSettings は RenderGraph.hpp を include していない
///     (設定は依存解決器を知らなくてよい)。値の対応は RenderPipeline が付ける。
enum class RenderGraphSchedulePolicy : uint8_t {
    RegistrationOrder = 0,  ///< 既定。依存が同じなら登録順が最小のものから出す
    MinimizeLifetimes = 1,  ///< 同時に生きる中間 RT を減らす。申告漏れがあると壊れる
};

/// RenderGraph に載せる 1 パスへの上書き。エディターから編集し、プロジェクトへ保存する。
///
/// WHY 実行順そのものを持たせないか: このグラフの唯一の思想は «順序は申告された依存が
///     決める» で、順番を直接書けるようにするとそれが壊れる。動かしたいときは
///     extraReads で «何を待つか» を足す。結果として実行順は動くが、理由がグラフに残る。
struct RenderPassOverride {
    std::string name;                     ///< RenderGraph 上のパス名
    bool enabled = true;                  ///< false でこのフレームに載せない
    bool allowCulling = true;             ///< false で «出力へ届かなくても残す»
    std::vector<std::string> extraReads;  ///< 追加で待たせるリソース名

    /// 既定と同じなら保存もしない。無害な行がプロジェクトに溜まるのを避ける。
    [[nodiscard]] bool IsDefault() const
    {
        return enabled && allowCulling && extraReads.empty();
    }
};

struct RenderSettings {
    RenderingPipeline pipeline  = RenderingPipeline::Forward;
    ViewMode          viewMode  = ViewMode::Lit;
    bool shadowEnabled = true;
    ShadowSettings shadow;
    bool showColliders        = false;
    bool showTerrainCollision = false;
    bool showDecalBounds      = false;
    bool showNavMesh          = true;
    bool showNavSensors       = false;
    NavMeshDrawMode navMeshDrawMode = NavMeshDrawMode::Solid;
    // NavMesh オーバーレイを描くカメラからの距離 [m]。0 以下で無制限。
    // WHY 既定で切るか: 遠景の細かいポリゴンが画面を線で埋め、手前の形と穴が読めなくなる。
    //     デバッグ描画のバッチも余計に分割される。
    float navMeshDrawDistance = 120.0f;
    bool showSkeleton         = false;
    bool showGrid             = false;
    bool showLightRange       = false;
    // パーティクル力場の影響半径・向きと、エミッターの発生形状をワイヤーで描く。
    // どちらも「見えない体積」なので、無いと radius の違いを粒子の挙動から逆算するしかない。
    bool showVFXGizmos        = false;
    bool showConstraints      = false;
    // ラグドールの剛体・関節の可動域・接触点を重ねて描く。
    // WHY 専用のフラグにするか: 「どの関節が力負けしたか」「足が床に触れているか」は
    //     数字 (Saturated / Contacts) では位置が分からず、可動域の詰めができない。
    //     常時出すと骨の絵が線で埋まるので、調整するときだけ点ける。
    bool showRagdoll          = false;
    // UI 要素の矩形・アンカー・ピボットを Canvas 上へ重ねて描く。
    // WHY: 「見えているのに押せない」「思った場所に出ない」の切り分けが、
    //      これが無いと勘になる。当たり判定に使っている矩形そのものを出す。
    bool showUIRects          = false;
    bool showSelectionOutline = true;
    // true のとき、各パスの RT サムネイルと CPU タイミングを ImGui ウィンドウで表示する。
    // ImGui フレーム内 (ImGuiNewFrame〜Render の間) で RenderSystem を呼ぶ構成が前提。
    bool passViewerEnabled = false;
    /// RenderGraph の実行順の決め方。既定は登録順どおり。
    ///
    /// @note MinimizeLifetimes は同時に生きる中間 RT を減らす代わりに実行順が
    ///       登録順から離れる。申告漏れのあるパス (読むと言っていないものを束縛する
    ///       パス) はそこで即座に壊れるので、«申告が本当に揃っているか» を試す
    ///       診断モードとしても使える。既定を変えないのはそのため。
    RenderGraphSchedulePolicy schedulePolicy = RenderGraphSchedulePolicy::RegistrationOrder;
    // RenderGraph のパス単位の上書き。既定は空 (＝素のパイプライン)。
    // NOTE: StripDebugVisualization では消さない。診断表示ではなく «この構成で描く»
    //       という指定なので、Game View でも同じ絵が出なければ確かめる意味が無い。
    std::vector<RenderPassOverride> passOverrides;
    // 全Particleのフレーム予算。0以下は無制限。RenderPassがエミッター順に残量を配分する。
    int particleBudget = 20000;
    bool particleBudgetEnabled = true;
    // true のとき、パーティクル描画の上へ「重なり枚数」のヒートマップを上書きする診断表示。
    // WHY: パーティクルの実コストは粒子数ではなく fill rate で決まるが、
    //      重なりは通常の絵からは読めない。VFX Editor のプレビューから切り替えて使う。
    bool particleOverdrawView = false;
    // trueならヒートマップ未描画領域に通常のMesh/SkinnedMesh表示を残す。
    bool particleOverdrawIncludeModels = false;
    // true のとき、重なり枚数を GPU から CPU へ読み戻して統計を取る (ParticleOverdrawStats)。
    // ヒートマップは見れば判るが、AI は数値でないと閾値を持てない。
    // 読み戻しは GPU 同期でフレームを止めるので既定は false。
    bool particleOverdrawReadback = false;

    bool IsWireframe() const { return viewMode == ViewMode::WireframeLit || viewMode == ViewMode::WireframeUnlit; }
    bool IsUnlit()     const { return viewMode == ViewMode::Unlit        || viewMode == ViewMode::WireframeUnlit; }

    /// エディター用の診断表示 (ワイヤーフレーム・コライダー・NavMesh・ヒートマップ等) をすべて切る。
    /// Game View へ渡す設定を作るときに使う。
    /// WHY: Debug メニューの表示フラグはこの構造体 (ProjectSettings::render) へ書かれる。
    ///      Game View がそのまま渡すと、Scene View で調べるために点けた表示が
    ///      ゲーム画面にも乗り、ゲームの見た目を確かめられなくなる。
    /// NOTE: showUIRects だけは残す。当たり判定を試すクリックは Game View で行うため。
    ///       診断表示のフラグを足したら、ここと RenderSettingsTests にも足すこと。
    void StripDebugVisualization()
    {
        viewMode                      = ViewMode::Lit;
        showColliders                 = false;
        showTerrainCollision          = false;
        showDecalBounds               = false;
        showNavMesh                   = false;
        showNavSensors                = false;
        showSkeleton                  = false;
        showGrid                      = false;
        showLightRange                = false;
        showVFXGizmos                 = false;
        showConstraints               = false;
        showRagdoll                   = false;
        passViewerEnabled             = false;
        particleOverdrawView          = false;
        particleOverdrawReadback      = false;
        shadow.debugVisualizeCascades = false;
        clustered.debugHeatmap        = false;
    }

    PostProcessSettings postProcess;

    // ユーザー設定レイヤー (Option 画面の「明るさ」)。Composite が画面フェードの
    // 直前に 1 度だけ掛ける。1.0 で無加工、0.5〜2.0 を想定。
    // postProcess の中には置かない ─ ApplyVolumeSettings が丸ごと差し替えるので、
    // PostProcessVolume へ入った瞬間にプレイヤーの設定が消える。
    // exposure も兼用しない (あちらはプロファイルが自由に上書きしてよい絵作りの値)。
    float userBrightness = 1.0f;

    // 描画スケール (Option 画面の「描画解像度」)。内部の描画解像度だけを倍率で変え、
    // 出力先 (ウィンドウ / ビューポート RT) は元の寸法のまま保つ。
    // GPU コストは面積比で効くので 0.7 でおよそ半分になる。1.0 で等倍。
    // UI は全ポストプロセスの後に出力先へ直接描くので、この倍率の影響を受けない。
    // 値が変わると中間 RT を作り直す。UI 側は離散値か「離した時だけ反映」にすること。
    float renderScale = 1.0f;

    // 発光の強さ (Option 画面の「発光の強さ」)。bloom.intensity へ掛ける倍率。
    // 直接動かさないのは userBrightness と同じ理由 (ApplyVolumeSettings が丸ごと差し替える)。
    float userBloomScale = 1.0f;

    // 高度グラフィクス設定 — 追加コストが大きい機能はここでまとめて制御する。
    IBLSettings              ibl;
    SSRSettings              ssr;
    VolumetricLightSettings  volumetricLight;
    FroxelFogSettings        froxelFog;
    AutoExposureSettings     autoExposure;
    TAASettings              taa;
    MotionBlurSettings       motionBlur;
    GTAOSettings             gtao;
    ContactShadowSettings    contactShadow;
    LensFlareSettings        lensFlare;
    LUTColorGradingSettings  lutColorGrading;
    ClusteredSettings        clustered;

    float outlineWidth = 0.045f;
    float outlineColor[4] = { 1.0f, 0.82f, 0.22f, 1.0f };
    std::vector<RenderSelectionID> selectedObjects;

    // ランタイムの輪郭要求。エディタ選択とは別の RT (objectMask) へ描かれ、
    // 見た目は CustomPostProcess のシェーダーが決める。ProjectSettings へは保存しない。
    std::vector<RenderObjectMaskRequest> objectMaskRequests;

    // 排他的な論理スロットを正規化し、修正した競合をビットで返す。
    // WHY: TOML・Inspector・ランタイムで別々の排他規則を持つと、設定経路によって
    //      FXAA/TAA や SSAO/GTAO が二重に有効化されるため、判定をここへ集約する。
    enum PipelineConflict : uint32_t {
        PIPELINE_CONFLICT_NONE    = 0,
        PIPELINE_CONFLICT_AA_SLOT = 1u << 0,
        PIPELINE_CONFLICT_AO_SLOT = 1u << 1,
    };

    uint32_t NormalizeExclusivePipelineSlots()
    {
        uint32_t conflicts = PIPELINE_CONFLICT_NONE;
        if (taa.enabled && postProcess.fxaaEnabled) {
            taa.enabled = false;
            conflicts |= PIPELINE_CONFLICT_AA_SLOT;
        }
        if (gtao.enabled && postProcess.ambientOcclusion.enabled) {
            gtao.enabled = false;
            conflicts |= PIPELINE_CONFLICT_AO_SLOT;
        }
        return conflicts;
    }

    // 外部コードが未正規化の値を直接渡しても、描画時は安定した既存パスを優先する。
    [[nodiscard]] bool IsTaaActive() const
    {
        return taa.enabled && !postProcess.fxaaEnabled;
    }

    [[nodiscard]] bool IsGtaoActive() const
    {
        return gtao.enabled && !postProcess.ambientOcclusion.enabled;
    }

    // クラスタライティング経路を使うか。
    // NOTE: 実際に有効化できるかは CS とバッファが揃っているかにも依存するため、
    //       RenderSystem 側でリソースの有無と AND を取ってから使うこと。
    [[nodiscard]] bool UsesClusteredLighting() const
    {
        return clustered.enabled
            && (pipeline == RenderingPipeline::ForwardPlus
             || pipeline == RenderingPipeline::DeferredPlus);
    }

    // 不透明物を GBuffer へ描くパイプラインか。
    // スクリーンスペース系 (SSAO / GTAO / SSR / 接触影) はどれも GBuffer の法線を要るので、
    // この 1 つの条件が「有効にしても効かない」機能の境界を決める。
    // RenderSystem のパス登録と UI の警告が同じ関数を見ること。
    // 実際に入れるかは RT とシェーダーの有無にも依存するので、呼び出し側で AND を取る。
    [[nodiscard]] bool UsesGBuffer() const
    {
        return pipeline == RenderingPipeline::Deferred
            || pipeline == RenderingPipeline::DeferredPlus;
    }

    // IBL は irradiance と prefilter の両キューブマップが揃って初めて有効になる。
    [[nodiscard]] bool HasValidIblAssets() const
    {
        return !ibl.irradiancePath.empty() && !ibl.prefilterPath.empty();
    }
};

/// @name 描画スケール
///@{
/// renderScale として意味を持つ範囲。これを超える値は丸められる。
/// 上限 2.0 はスーパーサンプリング用。面積が 4 倍になり VRAM も 4 倍要る。
inline constexpr float kMinRenderScale = 0.5f;
inline constexpr float kMaxRenderScale = 2.0f;

/// 縮小してもここより小さくはしない。ポストプロセスが破綻して
/// 「軽くはなったが何も見えない」状態を避けるための床。
inline constexpr uint32_t kMinRenderWidth  = 640u;
inline constexpr uint32_t kMinRenderHeight = 360u;

/// 出力寸法と倍率から内部描画解像度を決める。
///
/// WHY 関数に切り出すか: RenderTarget の生成側とビューポート計算側で同じ値を
///     出す必要があり、丸めが 1 ピクセルずれるだけでポストプロセスの UV が
///     半テクセルずれる。丸め規則を 1 か所に閉じる。
/// NOTE: 元の寸法が床より小さい場合は元の寸法をそのまま使う。小さなビューポートで
///       床が勝ってしまい、縮小のつもりが拡大になるのを防ぐ。
inline void ResolveRenderResolution(uint32_t nativeWidth, uint32_t nativeHeight, float scale,
                                    uint32_t& outWidth, uint32_t& outHeight)
{
    const float clamped = (scale < kMinRenderScale) ? kMinRenderScale
                        : (scale > kMaxRenderScale) ? kMaxRenderScale
                                                    : scale;
    const auto apply = [clamped](uint32_t native, uint32_t minimum) -> uint32_t {
        if (native == 0u) return 0u;
        const float scaled = static_cast<float>(native) * clamped + 0.5f;
        const uint32_t value = static_cast<uint32_t>(scaled);
        const uint32_t floorValue = native < minimum ? native : minimum;
        return value < floorValue ? floorValue : value;
    };
    outWidth  = apply(nativeWidth,  kMinRenderWidth);
    outHeight = apply(nativeHeight, kMinRenderHeight);
}
///@}

// Option 画面の「画質」。1 つのつまみで重い機能をまとめて切り替える。
enum class QualityPreset : uint8_t {
    Low = 0,
    Medium,
    High,
    Ultra,
};

// preset に対応する影・AA・AO・反射・体積光の構成を settings へ流し込む。
// アセット参照 (IBL の cubemap パス) と絵作りのパラメーターには触れない。
// 中身を Engine 側に置くのは、ゲーム側へ散らすと機能追加時に古いまま取り残されるため。
// プリセットは出発点でしかないので、適用後に個別 setter で上書きできる。
void ApplyQualityPreset(RenderSettings& settings, QualityPreset preset);

// settings の構成が どのプリセットと一致するかを引き当てる。一致しなければ
// 直近の (= より軽い側の) プリセットを返す。Option 画面の初期表示に使う。
[[nodiscard]] QualityPreset DetectQualityPreset(const RenderSettings& settings);

} // namespace fbzz::renderer
