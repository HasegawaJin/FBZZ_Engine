// FBZZ Engine
// VFXGraphAsset.hpp | fbzz::asset
// 複数の描画・音響エフェクトを時間依存DAGとして束ねる.vfxアセット定義
#pragma once

#include <Engine/Asset/VFXParameter.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

enum class VFXNodeType : std::uint8_t {
    Entry = 0,
    Delay,
    Particle,
    Trail,
    MeshTrail,
    Light,
    Audio,
    Decal,
    SubGraph,
    // WHY: 数値は .vfx へ直接書かれるため、既存アセットを壊さないよう必ず末尾へ追加する。
    ForceField, // パーティクルへ作用する力場 (風/吸引/渦/乱流)
    Mesh,       // 膨張シェル・斬撃メッシュなどのメッシュ単体エフェクト
    ScreenEffect, // 画面フラッシュ・色収差・ブルームブーストの一時的な上乗せ
    CameraShake,  // カメラ揺れ (被弾・着弾の手応え)
    TimeScale,    // ヒットストップ (一時的な時間減速)
    Wind,         // 風域。フォリッジ・水面・布など環境側へ効く
    // 配線の中継点。実体も時間も持たず、リンクを 1 本通すだけ。
    // WHY: ノードが 20 を超えると配線が交差だらけになり、どのリンクがどこへ行くのか
    //      Canvas 上で追えなくなる。実行 DAG を変えずに線を折り曲げられる逃げ道が要る。
    // NOTE: startOffset / duration は常に 0 として扱う (BuildVFXGraphSchedule 参照)。
    //       ここで時間を持たせられると「見た目を整えるだけ」のはずの操作が挙動を変える。
    Reroute,
    // Skinned ModelをAnimator Controllerで再生するゲーム実行可能な描画ノード。
    // WHY: Previewだけの一時Clip設定を保存せず、State/Layer/EventをControllerへ集約する。
    AnimatedMesh,
};

// 実体 (GameObject) も時間も持たない純粋な構造ノードか。
// WHY: Entry / Delay / Reroute の 3 種を「実体を持たない」として扱う箇所が
//      ランタイム・診断・Editor に散っている。判定を 1 か所へ寄せて取りこぼしを防ぐ。
// NOTE: Delay は時間は持つ (duration が効く) が実体は持たない。
[[nodiscard]] constexpr bool VFXNodeHasNoInstance(VFXNodeType type)
{
    return type == VFXNodeType::Entry || type == VFXNodeType::Delay
        || type == VFXNodeType::Reroute;
}

// スケジュール上の時間を一切消費しないノードか (Reroute のみ)。
[[nodiscard]] constexpr bool VFXNodeIsPassthrough(VFXNodeType type)
{
    return type == VFXNodeType::Reroute;
}

enum class VFXLinkTrigger : std::uint8_t {
    OnComplete = 0,
    OnStart,
    OnCollision,
    OnDeath,
    OnAnimationEvent,
    OnTrigger,
};

struct VFXTrailSettings {
    // MeshTrailノードでは残像元の静的Meshとして使う。Trailノードでは未使用。
    std::string meshPath;
    std::string materialPath;
    math::Vector4 colorStart = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 colorEnd = { 1.0f, 1.0f, 1.0f, 0.0f };
    float lifetime = 1.0f;
    float widthStart = 0.2f;
    float widthEnd = 0.02f;
    bool beamMode = false;
    math::Vector3 beamStart = math::Vector3::ZERO;
    math::Vector3 beamEnd = { 0.0f, 0.0f, 5.0f };
};

struct VFXLightSettings {
    math::Vector3 color = { 1.0f, 0.7f, 0.3f };
    float intensity = 4.0f;
    float range = 8.0f;
    // ノード生存時間に沿った閃光の減衰。定数の intensity だけでは
    // 「一瞬強く光ってすっと消える」爆発のフラッシュが作れない。
    // intensity への倍率として掛かる (0〜1 想定だが 1 超で増幅も可)。
    bool useIntensityCurve = false;
    scene::ParticleCurve intensityCurve;
    // 色の時間変化。alpha は明るさ倍率として color へ乗算する
    // (白熱 -> 橙 -> 暗赤 のような色温度変化を 1 本で作れるようにするため)。
    bool useColorGradient = false;
    scene::ParticleGradient colorGradient;
};

struct VFXAudioSettings {
    std::string clipPath;
    float volume = 1.0f;
    float pitch = 1.0f;
    float spatialBlend = 1.0f;
    bool loop = false;
};

struct VFXDecalSettings {
    // .mat (render_path = "decal") への参照。空なら下のテクスチャと色で組み込み描画。
    // WHY テクスチャと別に持つか: 弾痕は「1 枚貼って薄める」用途が圧倒的多数で、
    //      .mat を必須にすると弾痕を 1 種類足すたびにアセットが 1 つ増える。
    //      マテリアルは組み込みでは描けない絵を作りたいときの上乗せにする。
    std::string materialPath;
    std::string albedoPath;
    std::string normalPath;
    std::string emissivePath;
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    float normalStrength = 1.0f;
    float emissiveScale = 0.0f;
    float fadeTime = 0.25f;
    // ノード生存時間に沿った不透明度カーブ。fadeTime の線形フェードでは
    // 「しばらく濃く残ってから急に消える」焼け跡のような減り方が作れない。
    // alpha と emissiveScale の両方へ倍率として掛かる。
    bool useFadeCurve = false;
    scene::ParticleCurve fadeCurve;
    // 角度フェード。受け面が投影軸から傾くほど薄める。
    // WHY: 焼け跡・血痕・着弾痕は必ず壁と床の角をまたぐ。角度で薄めないと
    //      斜面でテクスチャが引き伸ばされ、「伸びた汚れ」として露見する。
    float angleFadeStrength = 1.0f;
    float angleFadeDegrees = 70.0f;
};

struct VFXSubGraphSettings {
    std::string graphPath;
};

// ForceField ノード — 生成した GameObject へ scene::ParticleForceField を載せるための設定。
// WHY: エミッター単体の gravity/damping では「渦を巻く煙」「吸い込まれる魔法陣」を表現できない。
//      力場は既に CPU/GPU 双方のパーティクルシミュレーションが解釈するため、
//      VFX グラフから配置できるようにするだけで表現力が一段上がる。
struct VFXForceFieldSettings {
    // scene::ParticleForceFieldType と同じ並び (Wind/Attract/Repulse/Vortex/Turbulence/Drag)。
    // WHY: Asset 層から Scene 層の enum へ依存させないため int で持ち、ランタイムで変換する。
    int fieldType = 0;
    float strength = 5.0f;     // 加速度 [m/s^2]。Drag では減衰係数 [1/s]
    float radius = 5.0f;       // 影響半径 [m]。0 以下でシーン全体へ減衰なし
    float falloffPower = 2.0f; // (1 - dist/radius)^falloffPower
    math::Vector3 direction = { 0.0f, 1.0f, 0.0f }; // Wind: 風向 / Vortex: 回転軸
    float noiseFrequency = 0.5f; // Turbulence 用
    float noiseSpeed = 1.0f;
};

// Mesh ノード — メッシュ単体を出し、生存時間に沿ってスケールとマテリアル値を動かす。
// WHY: 衝撃波の膨張シェル、斬撃の板ポリ、魔法陣のディゾルブなど、AAA では
//      「パーティクルではないメッシュ」が画作りの主役になる場面が多い。
//      見た目は .mat 側に任せ、このノードは時間変化(エンベロープ)だけを受け持つ。
// Mesh ノードのマテリアル未割当時に使う既定 .mat。
// WHY: 未設定を「描かない」でも「マゼンタ」でも終わらせないための保険。
//      加算・両面の Unlit なので、置いただけで衝撃波シェルとして成立する。
inline constexpr const char* VFX_MESH_FALLBACK_MATERIAL =
    "Assets/Materials/Fallback/VFXMeshFallback.mat";

struct VFXMeshSettings {
    // "primitive:sphere" / "primitive:cube" / "Assets/Models/foo.fbx:0"
    std::string meshPath = "primitive:sphere";
    // 空なら VFX_MESH_FALLBACK_MATERIAL が使われる。
    std::string materialPath;
    // ノードの生存時間 0->1 に対するスケール倍率。localScale へ乗算される。
    float scaleStart = 0.1f;
    float scaleEnd = 4.0f;
    // 立ち上がり/減衰の形。1.0 で線形、<1 で最初に速く伸びる (衝撃波らしい減速)。
    float scaleEasePower = 0.45f;
    // 生存時間に沿った色の推移。MaterialComponent::paramOverrides の "albedo" へ書く。
    // WHY: albedo はどのマテリアルにも存在する唯一の共通パラメーターなので、
    //      どの .mat を割り当てても必ずフェードが効く。加算ブレンドでは
    //      out = src.rgb * src.a + dst.rgb のため、RGB を落とすことで消える。
    math::Vector4 colorStart = { 1.0f, 0.95f, 0.85f, 1.0f };
    math::Vector4 colorEnd = { 0.0f, 0.0f, 0.0f, 0.0f };
    // 追加で動かしたいシェーダー変数 (任意)。空なら書き込まない。
    // 例: Surface/Dissolve.hlsl の "alphaCutoff" (0=表示, 1=消滅)。
    std::string animatedParam;
    float paramStart = 0.0f;
    float paramEnd = 1.0f;
};

struct VFXAnimatedMeshSettings {
    std::string modelPath;
    std::string controllerPath;
    std::string materialPath;
    std::string initialState;
    float speed = 1.0f;
    float startNormalizedTime = 0.0f;
    int meshIndex = -1;
    bool loop = false;
    bool syncToGraphTime = true;
    bool applyRootMotion = false;
};

// ScreenEffect ノード — 画面全体へ一時的な演出を上乗せする。
// WHY: 爆発の白フラッシュ・被弾時の色収差は「シーンのカラーグレーディングを置き換える」のではなく
//      「一瞬だけ足す」ものなので、PostProcessVolume(全置換・先勝ち)とは別経路で加算する。
struct VFXScreenEffectSettings {
    math::Vector3 flashColor = { 1.0f, 1.0f, 1.0f };
    float flashIntensity = 0.0f;       // screenFadeAlpha として加算 (0=無効)
    float bloomBoost = 0.0f;           // bloom.intensity への加算量
    float chromaticAberration = 0.0f;  // lens.chromaticAberration への加算量
    float lensDistortion = 0.0f;       // lens.distortion への加算量
    float vignette = 0.0f;             // vignette.intensity への加算量
    // 生存時間に対する立ち上がり/減衰 [秒]。間はピーク値を維持する。
    float fadeInTime = 0.03f;
    float fadeOutTime = 0.25f;
};

// CameraShake ノード — 被弾・着弾の手応えを作るカメラ揺れ。
// WHY: 打撃感はパーティクルの量ではなくカメラの反応で決まる。エフェクト側から
//      オーサリングできないと、演出とコードが別々の場所に散る。
struct VFXCameraShakeSettings {
    float amplitude = 0.12f;         // 平行移動の振れ幅 [m]
    float rotationAmplitude = 1.2f;  // 回転の振れ幅 [度]
    float frequency = 22.0f;         // 振動周波数 [Hz]
    // 減衰カーブ: weight = (1 - t)^falloffPower。大きいほど頭でっかちに減衰する。
    float falloffPower = 2.0f;
    // 発生源からの到達距離 [m]。0 以下で距離減衰なし (常に最大)。
    float radius = 25.0f;
};

// TimeScale ノード — ヒットストップ。ノード生存中だけ時間を遅くする。
struct VFXTimeScaleSettings {
    float timeScale = 0.15f;    // 目標倍率。0 で完全停止
    float blendInTime = 0.0f;   // 目標へ落ちるまで [秒]。0 で即時
    float blendOutTime = 0.12f; // 等速へ戻すまで [秒]
};

// Wind ノード — 風域。ParticleForceField(Wind) と違い、フォリッジ・水面・布など
// WindZoneComponent を読む環境システム全体へ効く。
struct VFXWindSettings {
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    float strength = 3.0f;
    float turbulence = 0.3f;
    float pulseFrequency = 0.5f;
    // NOTE: WindZoneComponent は範囲を持たないシーン全体の風として働くため、
    //       半径のような効かないパラメーターは意図的に持たせない。
};

struct VFXGraphNode {
    int id = 0;
    VFXNodeType type = VFXNodeType::Particle;
    std::string name = "Effect";
    // 無効ノードはDAG上の時間・接続を維持したまま、実行物の生成とbudget消費だけを止める。
    // WHY: 配線を壊さず個々の表現を比較でき、AIも同じアセットを非破壊で調整できる。
    bool enabled = true;
    float editorX = 0.0f;
    float editorY = 0.0f;
    float startOffset = 0.0f;
    float duration = 1.0f;
    math::Vector3 localPosition = math::Vector3::ZERO;
    math::Vector3 localRotationDegrees = math::Vector3::ZERO;
    math::Vector3 localScale = math::Vector3::ONE;
    // 空ならowner追従。指定時はowner配下のBoneComponent名をsocketとして明示追従する。
    std::string attachBone;
    // 空間上の親ノード。-1 で owner 直下 (従来どおりのフラット構成)。
    // WHY: link (VFXGraphLink) が表すのは「実行の因果」であって「空間の入れ子」ではない。
    //      衝撃波メッシュとそこから出る煙・火花のように、一緒に動かしたいノード群は
    //      因果とは別の軸でまとまる。両者を link 1 本に兼任させると、
    //      「傾けたいだけなのに発火順まで変わる」という形で必ず破綻する。
    //      そのため Transform の親子は独立した参照として持つ。
    // NOTE: 親の localPosition/Rotation/Scale がそのまま子へ乗る (通常の Transform 階層)。
    //       attachBone を持つノードはボーン socket 追従が優先され、parentNodeId は無視される
    //       (どちらも「誰に付くか」を決めるため、二重指定は socket 側を正とする)。
    int parentNodeId = -1;
    // WHY: シーン用ParticleEmitterと同じauthoring型を使い、VFX Graphだけ機能差や保存漏れが
    //      生じる二重実装を避ける。GPUハンドル等のランタイム値はアセットcodecが除外する。
    // オーサリングの塊。GameObject に依存しないので設定型をそのまま持つ。
    // WHY コンポーネント型を持たないか: ランタイム状態 (粒子列・GPU ハンドル) を
    //      アセットのノードが抱えても意味が無く、コピーのたびに運ぶだけになる。
    scene::ParticleEmitterSettings particle;
    VFXTrailSettings trail;
    VFXLightSettings light;
    VFXAudioSettings audio;
    VFXDecalSettings decal;
    VFXSubGraphSettings subGraph;
    VFXForceFieldSettings forceField;
    VFXMeshSettings mesh;
    VFXAnimatedMeshSettings animatedMesh;
    VFXScreenEffectSettings screenEffect;
    VFXCameraShakeSettings cameraShake;
    VFXTimeScaleSettings timeScale;
    VFXWindSettings wind;
};

struct VFXGraphLink {
    int fromNode = 0;
    int toNode = 0;
    VFXLinkTrigger trigger = VFXLinkTrigger::OnComplete;
    float delay = 0.0f;
    // OnAnimationEvent / OnTriggerの名前。Animation Eventは空なら全Eventを通す。
    std::string eventName;
};

// Editor専用の注釈。ノードのグルーピング枠と付箋(コメント)を兼ねる。
// WHY: 実行時DAGには一切関与しない純粋なオーサリング情報。VFXGraphSystemは無視し、
//      グラフの意図をポートフォリオ閲覧者やAIへ伝えるためだけに保存する。
struct VFXGraphGroup {
    int id = 0;
    std::string title = "Group";
    std::string note;                                    // 任意の説明文 (付箋として使う)
    float x = 0.0f;                                       // Grid空間の左上X
    float y = 0.0f;                                       // Grid空間の左上Y
    float width = 320.0f;
    float height = 200.0f;
    math::Vector4 color = { 0.26f, 0.42f, 0.62f, 0.20f }; // 枠の塗り (RGBA, alphaは薄め)
    // Template から取り込んだ塊の出所。手で作った枠では空。
    // WHY: 以前は note へ "Merged from template" と書くだけだったため、
    //      「この塊だけ入れ替える / 消す」という操作の対象を機械的に決められなかった。
    //      文字列ではなく構造として持つことで、後から由来を辿れるようにする。
    std::string sourceTemplate;
    // 取り込み元 Template の VFXGraphAsset::version。差分更新の判断材料。
    int sourceTemplateVersion = 0;
    // 取り込んだノード id。空なら「矩形に含まれるノード」という従来の空間的な所属で扱う。
    // WHY: 矩形だけだと利用者がノードを枠外へ動かした瞬間に所属が消える。
    //      由来を持つ枠に限り、明示的なメンバー表を併せ持つ。
    std::vector<int> memberNodes;
};

// Template が要求する素材ロール。vfx.assetSurvey と同じ語彙を使う。
// WHY: guide の recipe / assetSurvey / Template の要求が別々の語彙を持つと
//      「guide は煙を要求するが survey は判定しない」という食い違いが生まれる。
//      文字列を1か所へ固定し、3つの面が同じ集合を見るようにする。
inline constexpr const char* kVFXAssetRoles[] = { "core", "body", "sparks", "animated" };

struct VFXGraphAsset {
    // version 5: description / tags / requiredRoles / thumbnailTime と
    //            VFXGraphGroup の Template 由来情報を追加 (いずれも省略可能)。
    int version = 5;
    std::string name = "VFX Graph";
    // Template カタログ用のオーサリング メタデータ。実行時は一切参照しない。
    // WHY: 以前は Template の説明を graph.name へ押し込んでいたため、AI は
    //      graphName として読めるのに Editor のカタログはファイル名しか出せず、
    //      人が見る面と AI が読む面が食い違っていた。説明は説明として持つ。
    std::string description;
    std::vector<std::string> tags;         // 検索・カテゴリ分けの補助
    std::vector<std::string> requiredRoles; // kVFXAssetRoles の部分集合
    // サムネイル生成時刻 [秒]。負なら「グラフ全長の 25% 地点」を自動採用する。
    float thumbnailTime = -1.0f;
    // 最悪時のリソース膨張を制作段階で可視化するアセット単位のbudget。
    int maxParticles = 100000;
    int maxLights = 8;
    int maxAudioVoices = 16;
    std::vector<VFXGraphNode> nodes;
    std::vector<VFXGraphLink> links;
    std::vector<VFXGraphGroup> groups; // Editor専用の注釈枠。実行時は無視される。
    std::vector<VFXParamDefinition> parameters;
    std::vector<VFXParamBinding> bindings;
    std::vector<VFXVariantSet> variants;
    std::vector<VFXSubGraphForward> subGraphForwards;
    std::vector<VFXSignalNode> signalNodes;
    std::vector<VFXSignalOutput> signalOutputs;
    // 保存時にNode/Controllerから再構築するBuild/Packager向け依存一覧。
    std::vector<std::string> dependencies;
};

struct VFXGraphBudgetStats {
    int particles = 0;
    int lights = 0;
    int audioVoices = 0;
};

[[nodiscard]] bool LoadVFXGraphAsset(const std::string& path,
                                     VFXGraphAsset& outAsset,
                                     std::string* outError = nullptr);
// Editor/AIの修復用途。TOMLとして読めればDAGが不正でも構造を返す。
[[nodiscard]] bool ParseVFXGraphAsset(const std::string& path,
                                      VFXGraphAsset& outAsset,
                                      std::string* outError = nullptr);
[[nodiscard]] bool SaveVFXGraphAsset(const std::string& path,
                                     const VFXGraphAsset& asset,
                                     std::string* outError = nullptr);
[[nodiscard]] bool ValidateVFXGraphAsset(const VFXGraphAsset& asset,
                                         std::string* outError = nullptr);
// DAGをトポロジカル評価し、nodesと同じ順序の開始時刻とグラフ全長を返す。
[[nodiscard]] bool BuildVFXGraphSchedule(const VFXGraphAsset& asset,
                                         std::vector<float>& outStartTimes,
                                         float& outDuration,
                                         std::string* outError = nullptr);
[[nodiscard]] const char* VFXNodeTypeName(VFXNodeType type);
[[nodiscard]] const char* VFXLinkTriggerName(VFXLinkTrigger trigger);
[[nodiscard]] VFXGraphBudgetStats CalculateVFXGraphBudget(const VFXGraphAsset& asset);
[[nodiscard]] std::vector<std::string> CollectVFXGraphDependencies(
    const VFXGraphAsset& asset);
// thumbnailTime を解決する。負なら全長の 25% 地点 (立ち上がりが終わり、まだ
// 消え際に入っていない時刻) を返す。Template のカタログ画と AI の評価画が
// 同じ時刻を指すよう、この 1 か所だけが既定値を決める。
[[nodiscard]] float ResolveVFXThumbnailTime(const VFXGraphAsset& asset);
// 解決できないアセット参照だけを列挙する。
// WHY: Template を別プロジェクトへ適用すると素材が無いまま保存も検証も通り、
//      「赤くならず何も出ない」という最も原因を辿りにくい形で失敗する。
//      適用の前に不足を出せるよう、警告本文ではなくパスの集合として返す。
[[nodiscard]] std::vector<std::string> CollectMissingVFXReferences(
    const VFXGraphAsset& asset);

// グラフとして正しいが、ほぼ確実に意図しない見た目になる設定。
// WHY: ValidateVFXGraphAsset は保存を拒否する「エラー」専用にしてある。一方で
//      「回転させながら非等方サイズにする」「bind 済みフィールドを直接編集する」ような
//      設定は保存を止めるほどではないのに、黙って通すと原因が突き止められない。
//      拒否せず可視化するための第二の面として警告を分けている。
struct VFXGraphWarning {
    int         nodeId = 0;   // 0 ならグラフ全体に対する警告
    // 機械可読な識別子 (例 "SHEARED_SPRITE")。AI の lint 応答と Editor の警告 banner が
    // 同じ集合を出すための鍵で、AI 側は文言ではなくこれで分岐する。
    std::string code;
    std::string message;
};
// checkAssetReferences: テクスチャ等の参照切れも検出するか。
//   AssetManager::Init 済みのプロセス (Editor / ランタイム) でのみ意味を持つ。
//   未初期化のプロセスでは全参照が「見つからない」と判定されてしまうため、
//   独自にルートを知っているテストなどは false を渡して自前で確認すること。
[[nodiscard]] std::vector<VFXGraphWarning> CollectVFXGraphWarnings(
    const VFXGraphAsset& asset, bool checkAssetReferences = true);

} // namespace fbzz::asset
