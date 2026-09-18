/// @file    ElectrodeRig.hpp
/// @brief   ＋/− 電極 1 本ぶんの組み立てと極どうしの相互作用。両極コンポーネントの実体。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note ＋と−で実体を共有する: 符号が逆なだけの同じものなので同じ式で書ける。別実装だと
///       片方だけ直したときに挙動が食い違い、引き合いか反発かが画面から読めなくなる。
/// @note 静的な登録簿を持つ: Plus が Minus を include すると循環し、型名指しの
///       scene.FindObjectsOfType も同じ問題になる。極を 1 本の配列へ登録すれば、
///       互いの型を知らないまま盤面全体を見られる。
/// @note 力場はチャンネルで分ける: FlowField はシーン全体へ一律に効くので、＋電極に Sink を
///       置くと−だけでなく＋の粒子まで同じ点へ吸い込まれる。flowFieldChannels/channels を
///       極ごとに分け、「＋は−にだけ引かれ、＋からは押し返される」を成立させる。
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Title/ElectrodeCore.hpp>
#include <Scripts/Title/ElectrodeFieldLines.hpp>
#include <Scripts/Utils/GameCursorComponent.hpp>
#include <Scripts/Title/ElectrodePole.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// 力場チャンネルのビット割り当て。粒子とそれを動かす場を極ごとに分ける唯一の根拠なので、
/// 電極まわりで使うビットはここだけで決める。
inline constexpr uint32_t kElectrodeChannelPlus  = 1u << 0;
inline constexpr uint32_t kElectrodeChannelMinus = 1u << 1;
inline constexpr uint32_t kElectrodeChannelBoth  =
    kElectrodeChannelPlus | kElectrodeChannelMinus;

[[nodiscard]] inline uint32_t ElectrodeChannelOf(Pole pole)
{
    return pole == Pole::Minus ? kElectrodeChannelMinus : kElectrodeChannelPlus;
}

/// 電極 1 本の調整値。Plus/Minus コンポーネントが自分の FBZZ_FIELD から詰めて渡す。
struct ElectrodeTuning {
    /// @name 粒子
    /// @{
    /// @note «小さいのを大量» でなく «大きいのを少なめ» にする: 加算合成は重なり枚数が
    ///       そのまま飽和に効くので、細粒を数千枚重ねると彩度があっても白へ抜ける。
    ///       粒を大きく枚数を落とすと重なりが減り、1 粒 1 粒が «粒として» 色を保って読める。
    /// @note エミッター側の設定なのに調整値として持つ: 電極のエミッターは Attach の
    ///       GetOrAddComponent が実行時に作るため、シーンに居ない状態で Inspector を触っても
    ///       Play 停止で消える。FBZZ_FIELD なら .scene に残るので、ここが唯一の置き場になる。
    ParticleSimulationMode simulationMode = ParticleSimulationMode::Cpu;
    int   maxParticles   = 1200;
    float emitRate       = 200.0f;
    float lifetime       = 2.4f;
    float spawnRadius    = 0.55f;
    float sizeStart      = 0.45f;
    float sizeEnd        = 0.06f;
    float velocitySpread = 0.9f;
    float particleDamping = 0.5f;
    float crackle        = 3.0f;
    /// 芯から外向きの加速度 [m/s^2]。湧き出しの勢い。
    float radialBurst    = 3.0f;
    /// 極を軸にした粒子の周回 [m/s^2]。放電の渦を粒子側でも作る。
    float spin           = 2.0f;
    /// 生まれた瞬間の明るさ倍率。色は極性色のまま、明るさだけが立ち上がる。
    /// オーサリング空間 (sRGB) に掛かるので、リニアでは 2.2 乗で効く。
    float hotCore        = 1.2f;
    /// @note 自発光は ElectricCharge*.mat の [particle] emissive_scale が持つ。
    ///       見た目は素材の性質なので、エミッター側にもここにも置かない。
    /// @}

    /// @name 電極が張る場
    /// @{
    /// 逆極の粒子を吸い込む加速度 [m/s^2]。
    float pullStrength  = 26.0f;
    /// 同極の粒子を押し出す加速度 [m/s^2]。自分の粒子が芯から湧き出して見える。
    float pushStrength  = 10.0f;
    /// 場の到達距離 [m]。0 以下でシーン全体。
    float fieldRadius   = 10.0f;
    float falloffPower  = 1.6f;
    /// 極を軸にした渦。0 で無効。まっすぐ吸い込まれるだけの線を弧に曲げる。
    float swirlStrength = 4.0f;
    /// @}

    /// @name 電極どうしの運動
    /// @{
    /// 逆極を引き・同極を押す加速度 [m/s^2]。最接近距離での値で、そこから 1/d^2 で落ちる。
    float coupling      = 5.0f;
    /// 元の配置へ戻すばね [1/s^2]。0 にすると画面外まで流れていく。
    float homeSpring    = 2.0f;
    float motionDamping = 1.4f;
    /// これ以上は近づかない距離 [m]。逆極どうしが重なって 1 点に潰れるのを防ぐ。
    float minSeparation = 1.6f;
    /// 逆極が接近したとき、芯を合成色へ寄せる強さ [0,1]。0 で通常色。
    float collisionGlow = 0.0f;
    /// 中心のまわりをゆっくり回す角速度 [deg/s]。釣り合った後も画面が止まらないようにする。
    float orbitSpeed    = 8.0f;
    /// @}

    /// @name ゲーム内カーソルへの追従
    /// @{
    /// ゲーム内カーソルの指す画面点へ寄っていく。
    /// @note 奥行きは «置いた位置のまま» 保つ: カーソルは画面上の 1 点しか指さないため、
    ///       カメラへ寄せると粒子まで大きくなり «近づいてきた» に見える。置いた奥行きを
    ///       保てば画面のどこでも大きさが変わらない。
    bool  followCursor   = false;
    /// カーソルへの食いつき [rad/s]。臨界減衰なので、上げても行き過ぎない。
    float followResponse = 12.0f;
    /// カーソルがこの位置より右にあるときだけ追従する。画面幅の割合 (0 = 画面全体 / 0.5 = 右半分)。
    /// @note タイトルのメニューは画面の左に置いてあるため、追従を «文字の無い側» に絞らないと
    ///       項目を選ぶたびに粒子と放電が文字の上へ乗って読めなくなる。
    float followMinX     = 0.0f;

    /// 極の間に走る放電。＋極の rig だけが引き受ける。
    ElectricArcStyle arc;
    /// @}

    /// @name 電荷そのものの見せ方
    /// @{
    /// 芯に立てる ＋ / − の記号。
    /// @note 放電とは別に持つ: 放電は «対» の持ち物で極 1 本だけでは出ない。記号と力線は
    ///       1 個の電荷の持ち物なので、極ごとに持って «そこに電荷がある» ことを示す。
    ElectrodeCoreStyle  core;
    /// 芯から外へ伸びる電気力線。
    ElectrodeFieldStyle field;
    /// @}
};

/// 電極 1 本。Plus/Minus コンポーネントが 1 つずつ値として持つ。
///
/// 組み立てるもの (自 GameObject の子として実行時に作る):
///   Pull  — Attract, 逆極チャンネル。相手の粒子を吸い込む
///   Push  — Repulse, 自極チャンネル。自分の粒子を芯から押し出す
///   Swirl — Vortex,  両チャンネル。吸い込まれる線を弧に曲げる
class ElectrodeRig {
public:
    static void BeginFusion(const Vector3& position)
    {
        s_fusionApproach = false;
        s_fusionPosition = position;
        s_fusionActive = true;
    }

    static void SetFusionApproach(const Vector3& center, float radius)
    {
        s_fusionApproach = true;
        s_fusionCenter = center;
        s_fusionRadius = Max(radius, 0.01f);
    }

    static void ResetFusion()
    {
        s_fusionActive = false;
        s_fusionApproach = false;
        s_fusionPosition = Vector3::ZERO;
    }

    [[nodiscard]] static bool IsFusionActive() { return s_fusionActive; }
    [[nodiscard]] static bool IsFusionApproaching() { return s_fusionApproach; }
    [[nodiscard]] static Vector3 FusionPosition() { return s_fusionPosition; }

    /// OnStart から呼ぶ。エミッターが無ければ作り、力場の子を組み、登録簿へ載せる。
    void Attach(Script& owner, Pole pole, const ElectrodeTuning& tuning);
    /// OnUpdate から呼ぶ。調整値を流し込み、極どうしの運動を 1 ステップ進める。
    void Tick(Script& owner, const ElectrodeTuning& tuning, float dt);
    /// OnDestroy から呼ぶ。登録簿と放電を外す。力場の子は GameObject の破棄に随伴する。
    void Detach(const Script& owner);

    [[nodiscard]] Pole    PoleOf()   const { return m_pole; }
    [[nodiscard]] Vector3 Position() const { return m_position; }

    /// 盤面に居る電極すべて。相手の型を知らずに走査するための窓口。
    [[nodiscard]] static const std::vector<ElectrodeRig*>& All() { return s_rigs; }

private:
    /// @note ポインタでなく EntityID を持つ: AddComponent は型ごとの配列を伸ばすことがあり、
    ///       力場を続けて足すと先に返ったポインタが無効になりうる。ID なら影響を受けない。
    EntityID MakeField(Script& owner, const char* suffix,
                       FlowFieldType type, uint32_t channels) const;
    [[nodiscard]] static FlowFieldSettings* ResolveField(const Script& owner, EntityID id);

    /// 極とシミュレーション経路から既定の .mat を選ぶ。
    [[nodiscard]] static const char* DefaultMaterialPath(Pole pole,
                                                        ParticleSimulationMode mode);
    /// この rig が入れた既定の .mat か。ユーザーが差した素材は上書きしないための判定。
    [[nodiscard]] static bool IsDefaultMaterialPath(std::string_view path);

    /// シミュレーション経路を流し込む。ConfigureEmitter より先に呼ぶこと (素材がこれで決まる)。
    static void ApplySimulationMode(Script& owner, const ElectrodeTuning& tuning);
    void ConfigureEmitter(ParticleEmitterSettings& emitter, const ElectrodeTuning& tuning) const;
    void ApplyFields(const Script& owner, const ElectrodeTuning& tuning) const;
    /// カーソルの指す画面点を、この電極が置かれた奥行きの平面へ起こす。
    /// 追従が切ってあるか followMinX の領域から外れている間は false を返し、
    /// 呼び出し側を «定位置へ戻すばね» の側へ倒す。
    [[nodiscard]] bool CursorTarget(Script& owner, const ElectrodeTuning& tuning,
                                    Vector3& outTarget);
    void Integrate(Script& owner, const ElectrodeTuning& tuning, float dt);
    void UpdateArcs(Script& owner, const ElectrodeTuning& tuning, float dt);
    void UpdateCharge(Script& owner, const ElectrodeTuning& tuning, float dt);
    /// Gpu を要求したのに CPU で回っているときだけ、理由を 1 回言う。
    void WarnGpuFallbackOnce(Script& owner);

    /// 追従に «入る» ときと «抜ける» ときのしきい値の差 (画面幅の割合)。
    /// @note 無いと、境界で手が 1px 揺れただけで行き先が «カーソル» と «定位置» の間で
    ///       毎フレーム入れ替わり極が震える。入りと出をずらせば 1 回の通過で切り替わる。
    static constexpr float kFollowHysteresis = 0.02f;

    static inline std::vector<ElectrodeRig*> s_rigs;
    static inline bool s_fusionActive = false;
    static inline bool s_fusionApproach = false;
    static inline Vector3 s_fusionCenter = Vector3::ZERO;
    static inline float s_fusionRadius = 0.01f;
    static inline Vector3 s_fusionPosition = Vector3::ZERO;

    /// 逆極の相手ごとに 1 束。＋極の rig だけが持つ (UpdateArcs の @note を参照)。
    std::vector<ElectricArcBundle> m_arcs;
    /// 芯の記号と力線。放電と違い極ごとに持つ (ElectrodeTuning::core の @note を参照)。
    ElectrodeCoreGlyph              m_core;
    ElectrodeFieldLines             m_fieldLines;
    /// 力線を曲げる盤面の電荷。毎フレームの再確保を避けるための作業領域。
    std::vector<ElectrodeCharge>    m_charges;
    Pole      m_pole = Pole::Plus;
    EntityID      m_pullId   = EntityID::INVALID;
    EntityID      m_pushId   = EntityID::INVALID;
    EntityID      m_swirlId  = EntityID::INVALID;
    /// GPU 縮退をもう言ったか。CPU へ戻った瞬間に 1 回だけ言うための掛け金。
    bool          m_gpuFallbackWarned = false;
    /// いまカーソルに掴まれているか。followMinX のヒステリシスに使う。
    bool          m_following         = false;
    /// シーンで置かれた位置。m_home と違い周回で動かないので、カーソル追従の奥行きに使う。
    Vector3 m_anchor   = Vector3::ZERO;
    Vector3 m_home     = Vector3::ZERO;
    Vector3 m_position = Vector3::ZERO;
    Vector3 m_velocity = Vector3::ZERO;
};


inline EntityID ElectrodeRig::MakeField(Script& owner, const char* suffix,
                                        FlowFieldType type,
                                        uint32_t channels) const
{
    GameObject* self = owner.scene.Self();
    if (!self) return EntityID::INVALID;

    GameObject& fieldObject = owner.scene.Create(self->name + "_" + suffix);
    const EntityID id = fieldObject.GetID();
    fieldObject.runtimeGenerated   = true;
    fieldObject.transform.position = Vector3::ZERO;
    fieldObject.SetParent(*self);

    /// @note 力場は 1 GameObject へ複数本を持てるが、この rig は «1 オブジェクト = 1 力» で組む。
    ///       3 本を別オブジェクトへ分けるのは、極ごとに channels と位置を独立に動かすため。
    FlowFieldSettings force;
    force.fieldType = type;
    force.channels  = channels;
    /// @note 渦の軸だけ意味を持つ。Attract / Repulse は direction を見ない。
    force.direction = Vector3::UP;
    FlowField& field = fieldObject.AddComponent<FlowField>();
    field.forces = { force };
    return id;
}

inline FlowFieldSettings* ElectrodeRig::ResolveField(const Script& owner, EntityID id)
{
    if (!id.IsValid()) return nullptr;
    GameObject* object = owner.scene.GetGameObject(id);
    FlowField* field = object ? object->GetComponent<FlowField>() : nullptr;
    if (field == nullptr || field->forces.empty()) return nullptr;
    return &field->forces.front();
}

inline const char* ElectrodeRig::DefaultMaterialPath(Pole pole,
                                                    ParticleSimulationMode mode)
{
    if (mode == ParticleSimulationMode::Gpu) {
        return pole == Pole::Minus
            ? "Assets/Materials/Effects/ElectricChargeMinusGPU.mat"
            : "Assets/Materials/Effects/ElectricChargePlusGPU.mat";
    }
    return pole == Pole::Minus
        ? "Assets/Materials/Effects/ElectricChargeMinus.mat"
        : "Assets/Materials/Effects/ElectricChargePlus.mat";
}

inline bool ElectrodeRig::IsDefaultMaterialPath(std::string_view path)
{
    return path == DefaultMaterialPath(Pole::Plus,  ParticleSimulationMode::Cpu)
        || path == DefaultMaterialPath(Pole::Plus,  ParticleSimulationMode::Gpu)
        || path == DefaultMaterialPath(Pole::Minus, ParticleSimulationMode::Cpu)
        || path == DefaultMaterialPath(Pole::Minus, ParticleSimulationMode::Gpu);
}

inline void ElectrodeRig::ApplySimulationMode(Script& owner, const ElectrodeTuning& tuning)
{
    /// @note settings へ直接書かずプロキシを通す: 経路切り替え直後は GPU バッファに前の粒子が
    ///       残る。SetSimulationMode は差分を見て gpuClearPending を立てるため、直接代入だと
    ///       その一手が抜ける。
    owner.particle.SetSimulationMode(tuning.simulationMode);
}

inline void ElectrodeRig::ConfigureEmitter(ParticleEmitterSettings& emitter,
                                           const ElectrodeTuning& tuning) const
{
    /// @note 素材は «極 × シミュレーション経路» で決まる。指定が無いときと、この rig が入れた
    ///       既定のままのときだけ差し替える (エディタで別の .mat を差した選択は残す)。
    /// @note 極ごとに分ける: ParticlePass は materialPath をキーに材質をグローバルへ 1 つだけ
    ///       持つため、同じ .mat を差すと両極の粒が同じ符号 (chargeSign) になる。
    /// @note 経路ごとにも分ける: GPU 経路は StructuredBuffer から粒子を引くため、CPU 用
    ///       シェーダーのまま Gpu にすると縮退の警告も無く粒が 1 つも出ない。
    if (emitter.materialPath.empty() || IsDefaultMaterialPath(emitter.materialPath))
        emitter.materialPath = DefaultMaterialPath(m_pole, emitter.simulationMode);

    emitter.flowFieldChannels = ElectrodeChannelOf(m_pole);
    emitter.receiveFlowFields = true;

    emitter.maxParticles   = (std::max)(tuning.maxParticles, 1);
    emitter.emitRate       = (std::max)(tuning.emitRate, 0.0f);
    emitter.lifetime       = (std::max)(tuning.lifetime, 0.05f);
    emitter.lifetimeRandom = 0.35f;
    /// @note 芯から湧き出させる。Point だと全粒子が同じ 1 点に生まれ、Repulse の向きが
    ///       決まらないまま重なるので、湧き出しの形が出ない。
    emitter.shape          = ParticleEmitterShape::Sphere;
    emitter.sphereRadius   = (std::max)(tuning.spawnRadius, 0.01f);
    emitter.velocitySpread = tuning.velocitySpread;
    emitter.emitVelocity   = Vector3::ZERO;
    emitter.sizeStart      = tuning.sizeStart;
    emitter.sizeEnd        = tuning.sizeEnd;
    emitter.flowCoupling = tuning.particleDamping;
    emitter.SetGravityAcceleration(Vector3::ZERO);

    const Vector4 tint = PoleColor(m_pole);
    emitter.colorStart = tint;
    emitter.colorEnd   = { tint.x, tint.y, tint.z, 0.0f };

    /// @note 寿命に沿った色。全キーが極性色の «比率» を保ち、白いキーは置かない (ここが赤青が
    ///       消える原因だった)。トーンマップは全チャンネルが 1 を超えると比率を潰して白にする
    ///       ため、純白キーは密集直後に画面全体を白へ引きずる。白熱は密度から創発させ、
    ///       比率を保ったまま重ねれば芯だけが飽和し外周は赤/青のまま残る。
    /// @note 1 粒は暗めに抑える: 重なり N 枚で G は N 倍 (リニアで見積もる)。キー RGB は
    ///       sRGB でリニア化すると赤の G=0.12 は 0.12^2.2=0.009。実効倍率 (色×hotCore×Glow×
    ///       シェーダー形) が 0.2 前後なら 20 枚で G=0.04 に留まり 60 枚超の芯だけ白熱するが、
    ///       1 を超えると 10 枚で ACES がチャンネルごとに潰し赤も青も同じ白になる。
    emitter.useColorGradient = true;
    ParticleGradient& gradient = emitter.colorGradient;
    gradient.keyCount      = 4;
    gradient.interpolation = ParticleCurveInterpolation::Linear;
    /// @note Linear 空間で混ぜる: 飽和した赤/青は彩度の高いランプで、Gamma で混ぜると
    ///       中間が濁る。光として足し合わせたいので Linear を選ぶ。
    gradient.colorSpace    = ParticleColorSpace::Linear;
    const float hot = Max(tuning.hotCore, 0.05f);
    gradient.keys[0] = { 0.00f, { tint.x * hot,  tint.y * hot,  tint.z * hot,  1.00f } };
    gradient.keys[1] = { 0.18f, { tint.x,        tint.y,        tint.z,        1.00f } };
    gradient.keys[2] = { 0.55f, { tint.x * 0.6f, tint.y * 0.6f, tint.z * 0.6f, 0.80f } };
    gradient.keys[3] = { 1.00f, { tint.x * 0.2f, tint.y * 0.2f, tint.z * 0.2f, 0.00f } };

    /// @note 生まれた瞬間に立ち上がり、ほぼ最大のまま保ち、最後に畳んで消える。
    /// @note カーブの «1» は大きさの最大ではない (取り違えると絵が反転する): ParticlePass は
    ///       size = sizeStart + (sizeEnd - sizeStart) * curve と評価し、sizeEnd < sizeStart
    ///       のこの設定では 0 が最大・1 が最小になる。
    /// @note «小さい時間» を作らない: 1 粒は記号そのもの (ElectricCharge.hlsl) で、小さいと
    ///       «ぼやけた点» にしか見えない。読める大きさで居る時間を寿命の大半にする。
    emitter.useSizeCurve = true;
    ParticleCurve& size = emitter.sizeCurve;
    size.keyCount      = 4;
    size.interpolation = ParticleCurveInterpolation::Smooth;
    /// @note 生まれは小さい
    size.keys[0] = { 0.00f, 0.85f };
    /// @note すぐ最大まで開く
    size.keys[1] = { 0.12f, 0.00f };
    /// @note ほぼ最大のまま保つ
    size.keys[2] = { 0.75f, 0.10f };
    /// @note 最後に畳んで消える
    size.keys[3] = { 1.00f, 1.00f };

    /// @note 記号は立っていないと読めない。角速度を明示的に止める
    ///       (既定に任せると、あとで «なんとなく回す» 変更が入ったときに符号が転ぶ)。
    emitter.angularVelocityMin = 0.0f;
    emitter.angularVelocityMax = 0.0f;
    emitter.useRotationCurve   = false;

    /// @note 自発光 (emissive_scale) とライティング無効 (lighting_strength = 0) は
    ///       ElectricCharge*.mat の [particle] が持つ。見た目は素材の性質なので、
    ///       スクリプトからは触らない (同じ素材を使う全エミッターで共有される)。
    ///       ブルームのしきい値 (既定 0.7) は 1 粒ではなく重なった芯が越える。

    /// @note 芯から外向き + 極を軸にした周回。力場だけだと粒子が素直に相手へ向かうので、
    ///       湧き出し口のあたりで «巻いてから飛ぶ» 一手間を足す。
    emitter.EnsureLocalForce(FlowFieldType::Source).strength = tuning.radialBurst;
    {
        auto& spin = emitter.EnsureLocalForce(FlowFieldType::Vortex);
        spin.strength  = tuning.spin;
        spin.direction = Vector3::UP;
    }

    /// @note ブレンドは ElectricCharge*.mat の blend_mode (Additive) が決める。
    emitter.sortMode  = ParticleSortMode::None;
    emitter.simulationSpace = ParticleSimulationSpace::World;
    /// @note 電極が動いても粒子が引きずられないよう World。Local だと親が動いた瞬間に
    ///       既に飛んでいる粒子ごと平行移動し、放電が電極に貼り付いて見える。

    /// @note 速度方向へ伸ばさない: 電荷は «点» で、伸ばすと芯が筋へ引き伸ばされ面積が増える
    ///       ぶんだけ明るい画素が重なり白飛びの原因になる。流れの向きは力場と放電が示すので、
    ///       粒そのものは丸のままにする。
    emitter.renderMode = ParticleRenderMode::Billboard;

    auto& crackle = emitter.EnsureLocalForce(FlowFieldType::Curl);
    crackle.strength       = tuning.crackle;
    crackle.noiseFrequency = 1.2f;
    crackle.noiseSpeed     = 2.0f;
}

inline void ElectrodeRig::Attach(Script& owner, Pole pole,
                                 const ElectrodeTuning& tuning)
{
    m_pole = pole;
    m_anchor   = owner.transform.position;
    m_home     = m_anchor;
    m_position = m_home;
    m_velocity = Vector3::ZERO;

    /// @note シーンで作り込んだエミッターがあればそれを使い、無ければ足す。
    ///       見た目 (マテリアル・テクスチャ) はエディタ側の仕事にして、ここは場との結線だけ持つ。
    ParticleEmitter& emitter = owner.scene.GetOrAddComponent<ParticleEmitter>();
    ApplySimulationMode(owner, tuning);
    ConfigureEmitter(emitter.settings, tuning);
    /// @note 再生状態だけは組み立て時に一度決める。毎フレーム書き戻すと Stop() が効かなくなる。
    emitter.settings.loop     = true;
    /// @note 0 = 打ち切らない
    emitter.settings.duration = 0.0f;
    emitter.settings.playing  = true;
    emitter.settings.enabled  = true;
    const uint32_t own      = ElectrodeChannelOf(m_pole);
    const uint32_t opposite = ElectrodeChannelOf(OppositePole(m_pole));
    m_pullId  = MakeField(owner, "Pull",  FlowFieldType::Sink, opposite);
    m_pushId  = MakeField(owner, "Push",  FlowFieldType::Source, own);
    m_swirlId = MakeField(owner, "Swirl", FlowFieldType::Vortex,  kElectrodeChannelBoth);
    ApplyFields(owner, tuning);

    if (std::find(s_rigs.begin(), s_rigs.end(), this) == s_rigs.end())
        s_rigs.push_back(this);
}

inline void ElectrodeRig::Detach(const Script& owner)
{
    s_rigs.erase(std::remove(s_rigs.begin(), s_rigs.end(), this), s_rigs.end());
    /// @note 放電・記号・力線の子はこの極の子ではなくルート直下に居るので、GameObject の破棄に
    ///       随伴しない。明示的に畳まないとシーンに置き去りの帯が残る。
    for (ElectricArcBundle& bundle : m_arcs) bundle.Detach(owner);
    m_arcs.clear();
    m_core.Detach(owner);
    m_fieldLines.Detach(owner);
    m_pullId = m_pushId = m_swirlId = EntityID::INVALID;
}

inline void ElectrodeRig::ApplyFields(const Script& owner, const ElectrodeTuning& tuning) const
{
    const float radius = tuning.fieldRadius;
    const float power  = Max(tuning.falloffPower, 0.001f);
    if (auto* pull = ResolveField(owner, m_pullId)) {
        pull->strength     = tuning.pullStrength;
        pull->radius       = radius;
        pull->falloffPower = power;
    }
    if (auto* push = ResolveField(owner, m_pushId)) {
        push->strength     = tuning.pushStrength;
        push->radius       = radius;
        push->falloffPower = power;
    }
    if (auto* swirl = ResolveField(owner, m_swirlId)) {
        swirl->strength     = tuning.swirlStrength;
        swirl->radius       = radius;
        swirl->falloffPower = power;
        swirl->enabled      = tuning.swirlStrength != 0.0f;
    }
}

inline bool ElectrodeRig::CursorTarget(Script& owner, const ElectrodeTuning& tuning,
                                       Vector3& outTarget)
{
    Vector2 normalized{};
    if (!tuning.followCursor || !GameCursorComponent::NormalizedPoint(owner, normalized)) {
        m_following = false;
        return false;
    }

    /// @note 入るときは followMinX、抜けるときはその手前で判定する (kFollowHysteresis の @note)。
    ///       先に前フレームの値でしきい値を決めてから倒す。m_following は «このフレーム実際に
    ///       掴まれたか» を意味するので、以降の失敗経路でも立てたままにしない。
    const float threshold = m_following ? tuning.followMinX - kFollowHysteresis
                                        : tuning.followMinX;
    m_following = false;
    if (normalized.x < threshold) return false;

    /// @note 控え付きの解決を使う。scene.GetMainCameraObject() を直接呼ぶと、電極 1 本ごとに
    ///       全 GameObject の走査が 1 回増える (WorldPointAtDepth の中でもう 1 回引くため)。
    GameObject* cameraObject = GameCursorComponent::MainCameraObject(owner);
    if (!cameraObject) return false;

    const Vector3 toAnchor = m_anchor - cameraObject->transform.worldPosition;
    const float   depth    = Vector3::Dot(toAnchor, cameraObject->transform.forward);
    /// @note カメラの背後に置かれている
    if (depth <= EPSILON) return false;

    if (!GameCursorComponent::WorldPointAtDepth(owner, depth, outTarget)) return false;
    m_following = true;
    return true;
}

inline void ElectrodeRig::Integrate(Script& owner, const ElectrodeTuning& tuning, float dt)
{
    /// @note 収束の時間軸に反発・周回を加えると、接触する前に融合の時刻だけが来る。
    if ((s_fusionApproach || s_fusionActive) && tuning.collisionGlow > 0.0f) {
        const Vector3 offset = m_anchor - s_fusionCenter;
        const float length = offset.Length();
        const Vector3 direction = length > EPSILON ? offset * (1.0f / length)
            : (m_pole == Pole::Plus ? Vector3::RIGHT : -Vector3::RIGHT);
        m_position = s_fusionActive ? s_fusionPosition
            : s_fusionCenter + direction * s_fusionRadius;
        m_velocity = Vector3::ZERO;
        owner.transform.position = m_position;
        return;
    }
    /// @note 行き先はカーソルか元の配置かのどちらか。
    /// @note 追従には別のばねを立てる: Home Spring は «だいたいこの辺» の緩いばねで、行き先を
    ///       カーソルへ差し替えただけでは指に付いてこない。追従は臨界減衰 (減衰=2ω) にして
    ///       食いつきを 1 値で決める。coupling は足したまま (ずれは ω^2 で割られ画面に出ず、
    ///       相手の極に引かれて «重い» 感じだけが残る)。
    Vector3 cursor = Vector3::ZERO;
    const bool onCursor = CursorTarget(owner, tuning, cursor);

    Vector3 force;
    if (onCursor) {
        const float omega = Clamp(tuning.followResponse, 0.1f, 40.0f);
        force = (cursor - m_position) * (omega * omega) - m_velocity * (2.0f * omega);
    } else {
        /// @note これが無いと、釣り合わない配置のときに極が画面外へ流れ去る。
        force = (m_home - m_position) * tuning.homeSpring;
    }

    const float minGap   = Max(tuning.minSeparation, 0.01f);
    const float minGapSq = minGap * minGap;
    for (const ElectrodeRig* other : s_rigs) {
        if (other == this) continue;

        const Vector3 delta = other->m_position - m_position;
        const float   dist  = delta.Length();
        if (dist <= EPSILON) continue;
        const Vector3 dir = delta * (1.0f / dist);

        /// @note 最接近距離で coupling そのもの、そこから 1/d^2 で落ちる。素の 1/d^2 は
        ///       接触寸前に発散して、ぶつかった瞬間に極が画面外へ弾き飛ばされる。
        const float falloff = minGapSq / Max(dist * dist, minGapSq);
        const float sign    = ArePolesAttracting(m_pole, other->m_pole) ? 1.0f : -1.0f;
        force = force + dir * (sign * tuning.coupling * falloff);

        /// @note 逆極どうしは放っておくと重なって 1 点に潰れる。近すぎるぶんだけ押し戻す。
        if (dist < minGap)
            force = force - dir * ((minGap - dist) * tuning.coupling);
    }

    m_velocity = (m_velocity + force * dt) * Max(0.0f, 1.0f - tuning.motionDamping * dt);
    m_position = m_position + m_velocity * dt;

    /// @note 釣り合った後も画面が静止しないよう、定位置そのものをゆっくり回す。回すのは
    ///       m_home だけで m_position はばね経由で遅れて追従し、極が硬直せず引き合いの
    ///       結果が回転で潰れない。
    /// @note 軸はワールド原点でなく極の重心にする: 原点軸だと電極を端へ寄せただけで各極が
    ///       別の半径で周回し組が引き裂かれる。回したいのは «互いのまわりを回ること» なので
    ///       軸は組の重心に置く。
    if (tuning.orbitSpeed != 0.0f) {
        /// @note 自分は必ず登録済みなので空にはならない。
        Vector3 center = Vector3::ZERO;
        for (const ElectrodeRig* rig : s_rigs) center = center + rig->m_home;
        center = center * (1.0f / static_cast<float>(s_rigs.size()));

        const float step = ToRad(tuning.orbitSpeed * dt);
        const float cosA = std::cos(step);
        const float sinA = std::sin(step);
        /// @note Y 軸まわり。タイトルはカメラを正面に据えるので、横方向の回りが一番読める。
        const Vector3 offset = m_home - center;
        m_home = { center.x + offset.x * cosA - offset.z * sinA,
                   m_home.y,
                   center.z + offset.x * sinA + offset.z * cosA };
    }

    owner.transform.position = m_position;
}

inline void ElectrodeRig::WarnGpuFallbackOnce(Script& owner)
{
    /// @note 言うのは «Gpu を要求したのに CPU で回っている» ときだけ。Cpu 指定 (NotRequested) は
    ///       縮退ではないので黙る。
    const bool fellBack = owner.particle.GetSimulationMode() == ParticleSimulationMode::Gpu
                       && !owner.particle.IsGpuSimulated();
    if (!fellBack) {
        /// @note 直ったら掛け金を戻す: Inspector や DLL リロードで往復させたとき、«直した→
        ///       また落とした» の 2 回目が黙ると直したつもりのまま CPU で回り続ける。
        ///       落ちている状態 1 回につき 1 行、が欲しい粒度。
        m_gpuFallbackWarned = false;
        return;
    }
    if (m_gpuFallbackWarned) return;
    m_gpuFallbackWarned = true;

    /// @note 素材の解決は最初の描画時なので、.mat 由来の理由はここでも 1 フレーム遅れて出る
    ///       (ScriptParticleProxy::GetGpuFallbackReason の NOTE)。毎フレーム見ているので拾える。
    const std::string self = owner.scene.name;
    owner.debug.LogWarning(self + ": particle simulation fell back to CPU ("
                           + owner.particle.GetGpuFallbackField() + "). "
                           + owner.particle.GetGpuFallbackDescription());
}

inline void ElectrodeRig::Tick(Script& owner, const ElectrodeTuning& tuning, float dt)
{
    /// @note 調整値は毎フレーム流し込む。Inspector で触った値がそのまま画面へ出る。
    if (auto* emitter = owner.scene.GetComponent<ParticleEmitter>()) {
        ApplySimulationMode(owner, tuning);
        ConfigureEmitter(emitter->settings, tuning);
    }
    WarnGpuFallbackOnce(owner);
    ApplyFields(owner, tuning);
    Integrate(owner, tuning, dt);
    /// @note 放電は全極の位置が更新された後に張りたいが、極ごとに OnUpdate が回るため
    ///       1 フレーム古い相手位置を使う。放電は毎フレーム形が変わる演出なので、
    ///       1 フレームの遅れは見えない。
    UpdateArcs(owner, tuning, dt);
    UpdateCharge(owner, tuning, dt);
}

inline void ElectrodeRig::UpdateCharge(Script& owner, const ElectrodeTuning& tuning, float dt)
{
    if (s_fusionActive && tuning.collisionGlow > 0.0f) {
        if (auto* emitter = owner.scene.GetComponent<ParticleEmitter>()) {
            emitter->settings.emitRate = 0.0f;
            emitter->settings.clearOnStop = true;
            emitter->settings.playing = false;
        }

        ElectrodeCoreStyle hiddenCore = tuning.core;
        hiddenCore.size = 0.0f;
        m_core.Update(owner, m_pole, m_position, hiddenCore, dt);

        ElectrodeFieldStyle hiddenField = tuning.field;
        hiddenField.lineCount = 0;
        m_fieldLines.Update(owner, m_pole, m_position, {}, hiddenField, dt);
        return;
    }

    /// @note 色の対応は ElectrodePole が唯一の正本。ここで赤青を書き直さない。
    ElectrodeCoreStyle core = tuning.core;
    const Vector4 poleColor = PoleColor(m_pole);
    float collision = 0.0f;
    if (tuning.collisionGlow > 0.0f) {
        const float contact = Max(tuning.minSeparation, 0.01f);
        const float radius = contact * 2.0f;
        for (const ElectrodeRig* other : s_rigs) {
            if (other == this || other->m_pole == m_pole) continue;
            const float distance = (other->m_position - m_position).Length();
            collision = Max(collision, Clamp01((radius - distance)
                                               / Max(radius - contact, 0.01f)));
        }
        collision *= Clamp01(tuning.collisionGlow);
    }
    const Vector4 collisionColor{ 0.12f, 1.0f, 0.38f, 1.0f };
    const Vector4 collisionCore{ 1.8f, 6.0f, 2.4f, 1.0f };
    core.color = poleColor * (1.0f - collision) + collisionColor * collision;
    core.coreColor = core.coreColor * (1.0f - collision) + collisionCore * collision;
    m_core.Update(owner, m_pole, m_position, core, dt);

    if (auto* emitter = owner.scene.GetComponent<ParticleEmitter>()) {
        emitter->settings.colorStart = core.color;
        emitter->settings.colorEnd = { core.color.x, core.color.y, core.color.z, 0.0f };
        /// @note 有効なグラデーションは colorStart/End より優先される。
        auto& gradient = emitter->settings.colorGradient;
        for (uint32_t index = 0; index < gradient.keyCount; ++index) {
            auto& color = gradient.keys[index].color;
            color.x = color.x * (1.0f - collision) + collisionColor.x * collision;
            color.y = color.y * (1.0f - collision) + collisionColor.y * collision;
            color.z = color.z * (1.0f - collision) + collisionColor.z * collision;
        }
    }

    /// @note 力線は盤面の全電荷が作る場をなぞる。相手の型を知らずに済むよう、
    ///       登録簿から «位置と符号» だけを写して渡す。
    m_charges.clear();
    m_charges.reserve(s_rigs.size());
    for (const ElectrodeRig* rig : s_rigs)
        m_charges.push_back({ rig->m_position,
                              rig->m_pole == Pole::Minus ? -1.0f : 1.0f });

    ElectrodeFieldStyle field = tuning.field;
    field.color    = PoleColor(m_pole);
    field.tipColor = PoleColor(OppositePole(m_pole));
    /// @note ＋と−で種を半間隔ずらす。同じ場の同じ族なので、揃えると同じ曲線を 2 度描く
    ///       (ElectrodeFieldStyle::seedStagger の @note を参照)。
    field.seedStagger = m_pole == Pole::Minus ? 0.5f : 0.0f;
    m_fieldLines.Update(owner, m_pole, m_position, m_charges, field, dt);
}

inline void ElectrodeRig::UpdateArcs(Script& owner, const ElectrodeTuning& tuning, float dt)
{
    if (s_fusionActive && tuning.collisionGlow > 0.0f) {
        for (ElectricArcBundle& bundle : m_arcs) bundle.Detach(owner);
        m_arcs.clear();
        return;
    }

    /// @note ＋極だけが持つ: 放電は対の持ち物で、両極が張ると同じ 2 点に 2 束が重なり
    ///       «明るさだけ倍の 1 本» になって負荷だけ倍になる。＋から−へ流れる向きは電流の
    ///       慣習と一致するので、担当を＋に決めるのは恣意的でない。
    if (m_pole != Pole::Plus) {
        if (!m_arcs.empty()) {
            for (ElectricArcBundle& bundle : m_arcs) bundle.Detach(owner);
            m_arcs.clear();
        }
        return;
    }

    std::size_t used = 0;
    for (const ElectrodeRig* other : s_rigs) {
        if (other == this || other->m_pole == m_pole) continue;

        if (used >= m_arcs.size()) {
            m_arcs.emplace_back();
            /// @note 束ごとに違う鍵を渡す。同じ鍵だと 2 本目の束が 1 本目の筋を掴み、
            ///       相手が 2 極以上いる構成で放電が 1 本ぶんしか出なくなる。
            m_arcs.back().SetKey("Electrode" + std::to_string(used));
        }

        /// @note 色の対応は ElectrodePole が唯一の正本。ここで赤青を書き直さない。
        ElectricArcStyle style = tuning.arc;
        style.fromColor = PoleColor(m_pole);
        style.toColor   = PoleColor(other->m_pole);
        m_arcs[used].Update(owner, m_position, other->m_position, style, dt);
        ++used;
    }

    /// @note 相手が減ったぶんを片付ける。残すと消えた極へ向かって放電が伸び続ける。
    while (m_arcs.size() > used) {
        m_arcs.back().Detach(owner);
        m_arcs.pop_back();
    }
}

} // namespace sandbox
