/// @file    VfxManagerComponent.hpp
/// @brief   単発 VFX を「借りて返す」枠へ載せ、極性の色と敵の寸法を差し込んでから鳴らす
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY 1 箇所へ束ねるか:
///   .vfx を鳴らしたい側 (銃・敵・盤面) は 3 つに散っている。各自が prefab を置くと、
///   「どの演出がどれだけ画面に出ているか」を誰も知らない状態になる。集束で 5 体同時に
///   潰れる場面 (7.9) がある以上、同時発火の抑制は必ずどこかに要る。
///   呼び出し側は「何が起きたか」だけを言い、枠の管理はここが持つ。
///
/// WHY 1 発ぶんの値をスクリプトのフィールドへ書くか (Docs/design/vfx-prefab.md §6):
///   .vfx がプレファブになり、公開パラメーターは «ルートに載せたスクリプトの公開
///   フィールド» になった。旧実装は文字列 (schemaPath) でノードのフィールドを指しており、
///   綴りを間違えても保存も検証も通っていた。型が効く形にすると、光の強さを変えるつもりで
///   パーティクルの色へ書く事故が構造上起こらなくなる。
///
/// WHY GameObject を作り捨てにしないか:
///   演出は 1 秒に何度も出る。そのたびに Create / Destroy すると、シーンの GameObject 数が
///   戦闘の激しさに比例して上下し、EntityID を握っている側の参照が揺さぶられる。
///   .vfx の «種類ごと» にリングを持ち、寝ている枠を起こして使い回す。
///   PolarityRingComponent が環に対して取っているのと同じ形。
///
/// WHY 種類ごとにリングを分けるか:
///   1 本のリングを共有すると、4 秒残る衝突の焦げ跡が、その間に 8 回鳴った付与の演出に
///   押し出されて途中で消える。寿命の桁が違うものを同じ順番待ちに入れてはいけない。
///
/// WHY 画面演出 (ヒットストップ・カメラ揺れ・振動・フラッシュ) を持たないか:
///   それは ImpactFeedbackManagerComponent の担当で、1 つの出来事に対する配分を
///   そこが持っている。.vfx 側にも ScreenEffect / CameraShake / TimeScale ノードは
///   置いていない (理由は FX_IMP_Explosion.vfx のヘッダー)。ここは «その場所に
///   何が見えるか» だけを受け持ち、«画面がどう反応するか» には触らない。
#pragma once

#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Vfx/BeamScorchVfxComponent.hpp>
#include <Scripts/Vfx/ChargeVfxComponent.hpp>
#include <Scripts/Vfx/ImpactVfxComponent.hpp>
#include <Scripts/Vfx/LaunchVfxComponent.hpp>
#include <Scripts/Vfx/NeutralizeVfxComponent.hpp>
#include <Scripts/Vfx/RunDustVfxComponent.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// ── 既定のアセット ───────────────────────────────────────────────────────────
// WHY 既定を持たせるか: Inspector を 1 つも触っていないシーンでも演出が出る状態にする。
//     未割り当てで «何も起きない» が既定だと、演出が消えたときに「壊れた」のか
//     「そもそも差していない」のかを絵から区別できない。差し替えは Inspector が優先する。
inline constexpr const char* kVfxChargePath      = "Assets/VFX/Game/FX_POL_Charge.vfx";
inline constexpr const char* kVfxNeutralizePath  = "Assets/VFX/Game/FX_POL_Neutralize.vfx";
inline constexpr const char* kVfxLaunchPath      = "Assets/VFX/Game/FX_ATR_Launch.vfx";
inline constexpr const char* kVfxImpactPath      = "Assets/VFX/Game/FX_IMP_Explosion.vfx";
inline constexpr const char* kVfxBeamScorchPath  = "Assets/VFX/Game/FX_BEAM_Scorch.vfx";
inline constexpr const char* kVfxRunDustPath     = "Assets/VFX/Game/FX_PLR_RunDust.vfx";
inline constexpr const char* kVfxGroundDustPath  = "Assets/VFX/Game/FX_BOSS_ShockDust.vfx";

// FX_POL_Charge の Symbol Material へ差し込む ＋ / − の記号 (12.3)。
inline constexpr const char* kMatSymbolPlus  = "Assets/Materials/Effects/FX_SymPlus_Additive.mat";
inline constexpr const char* kMatSymbolMinus = "Assets/Materials/Effects/FX_SymMinus_Additive.mat";

// 床で起きる演出 (土煙・床を割る爆発) を回すリングの札。
// 同じ .vfx を «盤面の衝突» と «床» で共有しても、枠は別々になる。
inline constexpr const char* kGroundPool = "Ground";

// FX_IMP_Explosion の Ground Mark。柱・壁への叩きつけ (7.5) だけ «ひび» へ差し替える。
inline constexpr const char* kMarkScorch = "Assets/VFX/Textures/T_Scorch_Decal.png";
inline constexpr const char* kMarkCrack  = "Assets/VFX/Textures/T_Crack_Decal.png";

// コライダーを持たない対象に使う体の寸法。Mite 相当の大きさ。
inline constexpr float kFallbackBodyTop    = 1.2f;
inline constexpr float kFallbackBodyRadius = 0.6f;

class VfxManagerComponent : public Script {
    FBZZ_SCRIPT(VfxManagerComponent)

public:
    FBZZ_GROUP("Graphs")
    FBZZ_ASSET_FIELD(VFXRef, chargeVfx, "Charge (12.1)")
    FBZZ_TOOLTIP("極性が乗った瞬間。未割り当てなら FX_POL_Charge.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, neutralizeVfx, "Neutralize (12.4)")
    FBZZ_TOOLTIP("中和した瞬間。未割り当てなら FX_POL_Neutralize.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, launchVfx, "Launch (7.3)")
    FBZZ_TOOLTIP("溜めを終えて撃ち出される瞬間。未割り当てなら FX_ATR_Launch.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, impactVfx, "Impact (12.6)")
    FBZZ_TOOLTIP("衝突の爆発。未割り当てなら FX_IMP_Explosion.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, beamScorchVfx, "Beam Scorch (6.2)")
    FBZZ_TOOLTIP("ビームが地形を焼いた点。未割り当てなら FX_BEAM_Scorch.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, runDustVfx, "Run Dust")
    FBZZ_TOOLTIP("走っている足が着いた点。未割り当てなら FX_PLR_RunDust.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, groundDustVfx, "Ground Dust")
    FBZZ_TOOLTIP("ボスの重量が床へ掛かった点。未割り当てなら FX_BOSS_ShockDust.vfx を使う")

    FBZZ_GROUP("Pool")
    FBZZ_FIELD_RANGE_INT(int, slotsPerEffect, 8, "Slots Per Effect", 1, 32)
    FBZZ_TOOLTIP("1 種類あたりの同時再生数。超えたら最も古い 1 発を頭出しし直して奪う")

    FBZZ_GROUP("Impact (12.6)")
    // 12.6 の「多重衝突の処理」。集束で同時に潰れるほど 1 発ずつを弱めないと、
    // 光源が重なって画面が白へ抜け、どの極どうしがぶつかったのか読めなくなる。
    FBZZ_FIELD_RANGE(float, blastLightMin, 11.0f, "Blast Light (weak)", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, blastLightMax, 30.0f, "Blast Light (strong)", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, sparkPowerMin, 5.0f, "Spark Power (weak)", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, sparkPowerMax, 20.0f, "Spark Power (strong)", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE_INT(int, lightFalloffCount, 3, "Light Falloff Count", 1, 16)
    FBZZ_TOOLTIP("同フレームに何発目までを明るく出すか。これを超えた分は光源を落とす")
    FBZZ_FIELD_RANGE_INT(int, groundBlastSlots, 14, "Ground Blast Slots", 1, 32)
    FBZZ_TOOLTIP("床を割る爆発 (衝撃波) 専用の枠数。衝突の枠とは別に持つ。"
                 "1 発が焦げ跡まで含めて 4 秒あるので、上げるほど «同時に燃えている» "
                 "数がそのまま増える ─ 撒く間隔より先にここを疑うこと")

    FBZZ_GROUP("Beam Scorch (6.2)")
    // なぞりは 1 本の線を引く操作なので、焼け跡は «列» で置かれる。1 点ぶんを
    // 強くすると線全体が壁を白く塗る。1 点は控えめにして、量で見せる。
    FBZZ_FIELD_RANGE(float, emberRate, 26.0f, "Ember Rate", 0.0f, 60.0f)
    FBZZ_TOOLTIP("焼けた点 1 つから出る火の粉の量 [個/秒]")

    FBZZ_GROUP("Run Dust")
    // 最高速では 1 秒に 7 回近く鳴る。1 発を強くすると足元が煙で埋まって
    // プレイヤー自身が見えなくなるので、量ではなく «速さで変わる» ことで見せる。
    FBZZ_FIELD_COLOR(runDustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.4f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。極性色は乗せない (12.2)。アルファが煙の濃さ")
    FBZZ_FIELD_RANGE(float, runDustKickMin, 1.0f, "Kick (walk pace)", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, runDustKickMax, 2.6f, "Kick (top speed)", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, runDustSizeMin, 0.45f, "Puff Size (walk pace)", 0.1f, 3.0f)
    FBZZ_FIELD_RANGE(float, runDustSizeMax, 0.9f, "Puff Size (top speed)", 0.1f, 3.0f)

    // WHY 走行の土煙と数値を分けるか:
    //   同じ .vfx を大きくして共用すると、ボスの重さに合わせた瞬間にプレイヤーの足元まで
    //   煙まみれになる。鳴る頻度も «1 秒に 7 回» と «衝撃波で数十発» で桁が違うので、
    //   枠の数もここだけ別に持つ (種類が違えばリングも別になる)。
    FBZZ_GROUP("Ground Dust")
    FBZZ_FIELD_RANGE_INT(int, groundDustSlots, 56, "Slots", 1, 96)
    FBZZ_TOOLTIP("同時に生きていられる土煙の数。衝撃波の輪はこの数の煙で描かれる。"
                 "«1 枚の点数 x (1 発の寿命 ÷ 輪の間隔)» を下回ると、外周へ届く前に"
                 "内側の輪が欠け始める ─ 撒く側を密にしたら必ずここも上げること")
    FBZZ_FIELD_RANGE(float, groundDustSize, 2.4f, "Puff Size", 0.2f, 8.0f)
    FBZZ_TOOLTIP("1 発が最後に広がる大きさ [m]。Scale 1.0 のときの値")
    FBZZ_FIELD_RANGE(float, groundDustKick, 4.2f, "Kick", 0.0f, 16.0f)
    FBZZ_TOOLTIP("煙と砂粒を押し出す速さ [m/s]。衝撃波では波の速さに近づけると、"
                 "煙が前縁に貼り付いて «押し寄せている» に見える")
    FBZZ_FIELD_RANGE(float, groundDustGrit, 1.4f, "Grit Spread", 0.0f, 4.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugSlots, 0, "Live Slots")
    FBZZ_FIELD_READ_ONLY(std::string, debugLast, "", "Last Effect")

    // 盤面に «点いている光» が何個あるか。
    //
    // WHY VFX の管理者が数えるか: 光を実行時に増やしうるのは «演出» だけで、
    //     その大半 (爆発・集束・中和・打ち上げ) の実体はここが回している枠。
    //     どこかが片付け損ねたときに最初に膨らむのもここなので、数える場所も同じにする。
    //
    // WHY 上限を «256» ではなく警告のしきい値で見るか: 統合配列の上限は 256 だが、
    //     クラスタ 1 つに入るのは 64。密集して 64 を越えた時点で «そこだけ暗くなる»
    //     という、原因の見えない壊れ方をする。膨らみ始めを名指しで拾う。
    FBZZ_GROUP("Light Census")
    FBZZ_FIELD(bool, censusLights, true, "Count Lights")
    FBZZ_FIELD_RANGE_INT(int, censusWarnAt, 48, "Warn At", 8, 256)
    FBZZ_TOOLTIP("点いている光がこの数を越えたら、内訳を 1 度だけ名指しで報告する")
    FBZZ_FIELD_READ_ONLY(int, debugLightsLit, 0, "Lights (lit)")
    FBZZ_FIELD_READ_ONLY(int, debugLightsTotal, 0, "Lights (total)")

    [[nodiscard]] static VfxManagerComponent* Instance() { return s_instance; }

    /// 無極から極性が乗った瞬間 (12.1 / 12.3)。輪と記号を体の寸法へ合わせる。
    void PlayCharge(GameObject& target, Polarity polarity);
    /// 逆極を当てて中和した瞬間 (12.4)。
    void PlayNeutralize(GameObject& target);
    /// 溜めを終えて撃ち出された瞬間 (7.3 ②)。flightDirection は飛ぶ向き。
    void PlayLaunch(const Vector3& origin, const Vector3& flightDirection,
                    Polarity polarity, float speed);
    /// 衝突 (12.6)。strength01 は衝突速度の強さ、againstAnchor は柱・壁へ叩きつけたか。
    void PlayImpact(const Vector3& point, Polarity polarity, float strength01,
                    bool againstAnchor);
    /// 床が砕けた爆発。見た目は衝突と同じで、枠のリングだけが別。
    ///
    /// WHY 分けるか: 衝撃波は 1 回で十数発撒く。衝突と同じリングに入れると、波が
    ///     走っているあいだ «本当に敵がぶつかった» 爆発が全部押し出される。
    void PlayGroundBlast(const Vector3& point, Polarity polarity, float strength01);
    /// ビームが地形を焼いた点 (6.2)。normal は焼けた面の法線、searSize は焦げの直径。
    ///
    /// WHY 衝突と分けるか: 衝突は «盤面が動いた» 出来事で、焼け跡は «何も起きなかった»
    ///     ことの表示 (地形には極性が乗らない)。同じ枠に入れると、壁をなぞっただけで
    ///     衝突の枠が押し出され、本当にぶつかった爆発が途中で消える。
    void PlayBeamScorch(const Vector3& point, const Vector3& normal, Polarity polarity,
                        float searSize);
    /// 走っている足が床に着いた点。moveDirection は進んでいる向き、
    /// strength01 は最高速に対する今の速さ。
    ///
    /// WHY 他の 5 つと違い «盤面の出来事» ではないのにここへ置くか:
    ///     同時発火の抑制が要る理由は同じで、むしろ足元が最も頻繁に鳴る。呼ぶ側 (移動) に
    ///     枠を持たせると、走っているあいだじゅう自前のリングを回すことになる。
    void PlayRunDust(const Vector3& footPoint, const Vector3& moveDirection, float strength01);
    /// 重いものが床へ掛かって舞う土煙。outward は煙が流れていく «向き»、
    /// scale は 1.0 で Ground Dust の既定の大きさ。
    ///
    /// WHY 走行の土煙と別の口にするか:
    ///   走行のそれは «足が後ろへ掻いた» 跡なので、渡した進行方向の逆へ吹く。こちらは
    ///   «床が押し退けられた» 側なので、渡した向きそのものへ吹く。同じ関数で兼ねると、
    ///   呼ぶ側が «どちらの意味で渡すのか» を毎回思い出さないといけなくなる。
    void PlayGroundDust(const Vector3& point, const Vector3& outward, float strength01,
                        float scale = 1.0f);

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

private:
    /// .vfx 1 種類ぶんの枠のリング。枠の実体はその .vfx プレファブのインスタンス。
    struct Pool {
        std::string vfxPath;
        /// 同じ .vfx を «別の用途» として分けて回すための札。空なら本来の使い道。
        ///
        /// WHY 要るか: 衝撃波は 1 回で十数発の爆発を撒く。衝突 (12.6) と同じリングを
        ///     使うと、波が走っているあいだ本当にぶつかった爆発がすべて押し出される。
        ///     出どころが違えば «込み合い» も別に数えるべきで、それは枠を分けること。
        std::string tag;
        std::vector<EntityRef> slots;
        int next = 0;
        /// この種類だけの枠数。0 なら slotsPerEffect に従う。
        ///
        /// WHY 種類ごとに変えられるようにするか: 枠の数は «1 発の寿命 × 鳴る頻度» で
        ///     決まる量で、衝突の爆発 (数発) と衝撃波の土煙 (数十発) では桁が違う。
        ///     全体を土煙に合わせて増やすと、一度も込み合わない演出まで枠を抱える。
        int capacity = 0;
    };

    static inline VfxManagerComponent* s_instance = nullptr;

    [[nodiscard]] std::string PathOf(const VFXRef& reference, const char* fallback) const;
    [[nodiscard]] Pool& PoolFor(const std::string& vfxPath, const char* poolTag, int capacity);
    /// 爆発 1 発。PlayImpact と PlayGroundBlast の違いは «どのリングで回すか» だけ。
    void Blast(const Vector3& point, Polarity polarity, float strength01, bool againstAnchor,
               const char* poolTag, int capacity, const char* debugName);
    [[nodiscard]] GameObject* AcquireSlot(Pool& pool);
    /// 枠を 1 つ確保し、位置と回転を合わせて返す。まだ鳴らさない。
    ///
    /// WHY 鳴らす前に返すか: 1 発ぶんの値 (極性の色・体の寸法・衝突の強さ) は
    ///     呼び出し側にしか無く、種類ごとに型が違う。値を書き込んでから Restart する
    ///     必要があるので、«確保» と «発火» を分ける。
    /// @param poolTag 空でなければ、同じ .vfx を別のリングで回す。
    /// @param capacity 0 でこの種類の枠数を slotsPerEffect に任せる。
    [[nodiscard]] GameObject* Prepare(const std::string& vfxPath, const Vector3& position,
                                      const Quaternion& rotation, const char* debugName,
                                      const char* poolTag = "", int capacity = 0);
    /// 頭出しして鳴らす。Prepare で得た枠へ値を書いた後に呼ぶ。
    static void Fire(GameObject& root);
    void ReleaseSlots();
    /// 同フレーム何発目か。12.6 の「それ以外は減衰させる」をここで数える。
    [[nodiscard]] int NextImpactIndexThisFrame();

    std::vector<Pool> m_pools;
    std::uint64_t m_impactFrame = 0;
    int           m_impactsThisFrame = 0;
    /// 光が膨らんだことを 1 度だけ言うための札。
    bool          m_warnedLights = false;
};

FBZZ_REFLECT(VfxManagerComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void VfxManagerComponent::OnUpdate()
{
    if (!censusLights) return;

    // 30 フレームに 1 回で足りる。光が «徐々に増える» 不具合を見つけるのが目的で、
    // 1 フレームの正確さは要らない (毎フレーム全走査すると数える側が重い)。
    if ((Time::frameCount % 30) != 0) return;

    // WHY LightComponent を型で引けるか: FindObjectsOfType<T> は Script 派生でない型を
    //     ECS の GetEntities<T> へ流す (Script.hpp の分岐)。IBoss のような
    //     «横断インターフェース» とは経路が違うので、こちらは素直に引ける。
    const std::vector<GameObject*> lights = scene.FindObjectsOfType<LightComponent>();
    debugLightsTotal = static_cast<int>(lights.size());

    int lit = 0;
    for (GameObject* object : lights) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* light = object->GetComponent<LightComponent>();
        if (light && light->enabled) ++lit;
    }
    debugLightsLit = lit;

    if (m_warnedLights || lit <= std::max(censusWarnAt, 8)) return;
    m_warnedLights = true;

    // 内訳は «名前の頭» で束ねる。実行時に増える光はどれも «元の名前 + 連番» で
    // 作られるので、頭を見れば «どのスクリプトが» まで一息で分かる。
    std::vector<std::pair<std::string, int>> byPrefix;
    for (GameObject* object : lights) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* light = object->GetComponent<LightComponent>();
        if (!light || !light->enabled) continue;

        std::string prefix = object->name;
        const std::size_t cut = prefix.find_last_of("_0123456789");
        if (cut != std::string::npos && cut > 0) prefix = prefix.substr(0, cut);

        auto found = std::find_if(byPrefix.begin(), byPrefix.end(),
                                  [&](const auto& e) { return e.first == prefix; });
        if (found == byPrefix.end()) byPrefix.emplace_back(prefix, 1);
        else ++found->second;
    }
    std::sort(byPrefix.begin(), byPrefix.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::string report;
    for (std::size_t i = 0; i < byPrefix.size() && i < 6; ++i)
        report += (i ? ", " : "") + byPrefix[i].first + " x" +
                  std::to_string(byPrefix[i].second);

    debug.LogWarning("VfxManagerComponent: " + std::to_string(lit) +
                     " lights are lit (clusters hold 64). Breakdown: " + report +
                     ". Anything that grows run after run is not being released.");
}

inline void VfxManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("VfxManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;
    // 前回 Play / DLL リロードの枠はここへ来る前に OnDestroy で畳まれている。
    // 畳めていない経路 (Play 中の再読込) が残っても、AcquireSlot が名前で拾い直す。
    m_pools.clear();
    m_impactFrame      = 0;
    m_impactsThisFrame = 0;
    debugSlots = 0;
    m_warnedLights = false;
    debugLast.clear();
}

inline void VfxManagerComponent::OnDestroy()
{
    ReleaseSlots();
    if (s_instance == this) s_instance = nullptr;
}

inline void VfxManagerComponent::ReleaseSlots()
{
    // 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。持ち主が畳む。
    for (Pool& pool : m_pools) {
        for (EntityRef& slot : pool.slots)
            if (GameObject* object = slot.Resolve(scene)) scene.Destroy(*object);
        pool.slots.clear();
    }
    m_pools.clear();
    debugSlots = 0;
    m_warnedLights = false;
}

inline std::string VfxManagerComponent::PathOf(const VFXRef& reference, const char* fallback) const
{
    std::string path = reference.ResolvePath();
    return path.empty() ? std::string(fallback) : path;
}

inline VfxManagerComponent::Pool& VfxManagerComponent::PoolFor(const std::string& vfxPath,
                                                               const char* poolTag, int capacity)
{
    const std::string tag = poolTag ? poolTag : "";
    const auto found = std::find_if(m_pools.begin(), m_pools.end(),
        [&](const Pool& pool) { return pool.vfxPath == vfxPath && pool.tag == tag; });
    if (found != m_pools.end()) {
        // 枠数は «この種類が何発同時に鳴るか» なので、Inspector で動かせば次の 1 発から効く。
        found->capacity = capacity;
        return *found;
    }

    m_pools.push_back(Pool{ vfxPath, tag, {}, 0, capacity });
    return m_pools.back();
}

inline GameObject* VfxManagerComponent::AcquireSlot(Pool& pool)
{
    const int capacity = pool.capacity > 0 ? std::clamp(pool.capacity, 1, 96)
                                           : std::clamp(slotsPerEffect, 1, 32);

    // 上限に届くまでは «使うときに 1 つ足す»。開幕に capacity 個並べると、一度も
    // 鳴らない演出のぶんまで Hierarchy に空の枠が残り、シーンを覗いたときに邪魔になる。
    if (static_cast<int>(pool.slots.size()) < capacity) {
        // WHY 名前で拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
        //     EntityRef は空に戻る。一方で枠の GameObject は Scene 側に残っているため、
        //     拾わずに作ると、リロードのたびに枠が capacity 個ずつ増えていく。
        const std::size_t stemStart = pool.vfxPath.find_last_of("/\\") + 1;
        const std::size_t stemEnd   = pool.vfxPath.find_last_of('.');
        const std::string stem = pool.vfxPath.substr(
            stemStart, stemEnd == std::string::npos ? std::string::npos : stemEnd - stemStart);
        // 札の付いた枠は別の名前で置く。同名だと «同じ .vfx を 2 つのリングで回す»
        // 構成で、片方が作った枠をもう片方が拾い上げて共有してしまう。
        const std::string suffix = pool.tag.empty() ? std::string{} : "_" + pool.tag;
        const std::string name = "VFX_" + stem + suffix + "_" +
                                 std::to_string(pool.slots.size());

        GameObject* object = scene.Find(name);
        if (!object) {
            // .vfx はプレファブなので «展開して置く» のが生成そのもの。
            // scene.Spawn は PrefabPool 経由なので、2 度目以降はファイル読み込みを通らない。
            object = scene.Spawn(pool.vfxPath, Vector3::ZERO, Quaternion::Identity());
            if (!object) return nullptr;
            object->name = name;
            object->runtimeGenerated = true;
        }

        // WHY autoDestroy を降ろすか: 枠の寿命はこのマネージャーが持つ。
        //     VFXSystem に畳ませると、鳴り終わった枠がプールへ返ってしまい、
        //     こちらが握っている EntityRef が «別の演出として貸し出された実体» を指す。
        if (auto* vfx = object->GetComponent<VFXComponent>()) {
            vfx->autoDestroy = false;
            vfx->playOnAwake = false;
            vfx->loop        = false;
        }

        pool.slots.push_back(EntityRef{ object->GetID() });
        ++debugSlots;
        return object;
    }

    // 埋まっているので最も古い枠を奪う。next は常に «次に使う = 最も古い» を指す。
    EntityRef& slot = pool.slots[static_cast<std::size_t>(pool.next)];
    pool.next = (pool.next + 1) % static_cast<int>(pool.slots.size());
    return slot.Resolve(scene);
}

inline GameObject* VfxManagerComponent::Prepare(const std::string& vfxPath,
                                                const Vector3& position,
                                                const Quaternion& rotation,
                                                const char* debugName,
                                                const char* poolTag, int capacity)
{
    if (!enabled || vfxPath.empty()) return nullptr;

    GameObject* object = AcquireSlot(PoolFor(vfxPath, poolTag, capacity));
    if (!object) return nullptr;

    // 枠はルートに置いてある (親が居ないのでローカル = ワールド)。
    // 親に付けると、持ち主が動いた瞬間に既に出ている粒ごと引きずられる。
    object->transform.position      = position;
    object->transform.worldPosition = position;
    object->transform.rotation      = rotation;
    object->transform.worldRotation = rotation;
    object->SetActive(true);

    debugLast = debugName;
    return object;
}

inline void VfxManagerComponent::Fire(GameObject& root)
{
    // WHY 作り直しが要らなくなったか: 旧実装はパラメーターがノード生成時に 1 度だけ
    //     焼き込まれる作りだったため、値を変えるには毎回 reload が必要だった。
    //     いまは値の行き先が実体のコンポーネントそのものなので、書いて頭出しすれば済む。
    if (auto* vfx = root.GetComponent<VFXComponent>()) vfx->Restart();
}

inline int VfxManagerComponent::NextImpactIndexThisFrame()
{
    if (m_impactFrame != Time::frameCount) {
        m_impactFrame      = Time::frameCount;
        m_impactsThisFrame = 0;
    }
    return m_impactsThisFrame++;
}

inline void VfxManagerComponent::PlayCharge(GameObject& target, Polarity polarity)
{
    if (polarity == Polarity::None) return;

    const bodybounds::Extents body = bodybounds::Of(target);
    const float top    = body.measured ? body.top    : kFallbackBodyTop;
    const float radius = body.measured ? body.radius : kFallbackBodyRadius;

    Vector3 center = target.transform.worldPosition;
    center.y += top * 0.5f;

    GameObject* root = Prepare(PathOf(chargeVfx, kVfxChargePath), center,
                               Quaternion::Identity(), "Charge");
    if (!root) return;

    if (auto* params = root->GetScript<ChargeVfxComponent>()) {
        params->polarityColor  = PolarityColor(polarity);
        params->symbolMaterial = polarity == Polarity::Plus ? kMatSymbolPlus : kMatSymbolMinus;
        // 記号は頭の «上» に置く。体の中心を基準にしているので、上端までの残り半分に
        // 余白を足した高さになる。めり込むと 12.3 の «記号で読める» が成立しない。
        params->symbolOffset = { 0.0f, top * 0.5f + 0.55f, 0.0f };
        // 輪は体より一回り大きく。体に埋まると «包んだ» ではなく «光っただけ» に見える。
        params->haloSize = std::max(radius, 0.2f) * 3.2f;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayNeutralize(GameObject& target)
{
    const bodybounds::Extents body = bodybounds::Of(target);
    const float top    = body.measured ? body.top    : kFallbackBodyTop;
    const float radius = body.measured ? body.radius : kFallbackBodyRadius;

    Vector3 center = target.transform.worldPosition;
    center.y += top * 0.5f;

    GameObject* root = Prepare(PathOf(neutralizeVfx, kVfxNeutralizePath), center,
                               Quaternion::Identity(), "Neutralize");
    if (!root) return;

    if (auto* params = root->GetScript<NeutralizeVfxComponent>()) {
        params->symbolOffset = { 0.0f, top * 0.5f + 0.55f, 0.0f };
        // 体の外から吸い込ませる。内側から始めると «畳んだ» 動きが体に隠れる。
        params->collapseRadius = std::max(radius, 0.2f) * 1.8f;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayLaunch(const Vector3& origin, const Vector3& flightDirection,
                                            Polarity polarity, float speed)
{
    // 尾は «飛んだ向きの逆» へ置き去りにする。Cone はローカル +Z へ吹くので、
    // 進行方向の逆を向く回転を渡す。
    const Vector3 wake = (-flightDirection).NormalizedOr(Vector3::FORWARD);
    // LookRotation は forward と up が平行だと基底を作れない。真上・真下へ飛ぶ経路でだけ
    // 姿勢が飛ぶので、そこだけ up を替える。
    const Vector3 up = Abs(Vector3::Dot(wake, Vector3::UP)) > 0.99f
        ? Vector3::FORWARD : Vector3::UP;

    GameObject* root = Prepare(PathOf(launchVfx, kVfxLaunchPath), origin,
                               Quaternion::LookRotation(wake, up), "Launch");
    if (!root) return;

    if (auto* params = root->GetScript<LaunchVfxComponent>()) {
        params->polarityColor = PolarityColor(polarity);
        // 7.3 は距離と impactSeconds から速度を逆算する。遠くから引かれた個体ほど速いので、
        // 尾もそのぶん伸びる。等倍だと尾が本体を追い越して «前へ飛んだ» ように見える。
        params->wakeSpeed = std::clamp(speed * 0.5f, 2.0f, 22.0f);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayImpact(const Vector3& point, Polarity polarity,
                                            float strength01, bool againstAnchor)
{
    Blast(point, polarity, strength01, againstAnchor, /*poolTag=*/"", /*capacity=*/0,
          againstAnchor ? "AnchorImpact" : "EnemyImpact");
}

inline void VfxManagerComponent::PlayGroundBlast(const Vector3& point, Polarity polarity,
                                                 float strength01)
{
    // 床が砕けた爆発なので跡は «ひび»。焦げにすると、波が通った床が焼けたことになる。
    Blast(point, polarity, strength01, /*againstAnchor=*/true, kGroundPool,
          groundBlastSlots, "GroundBlast");
}

inline void VfxManagerComponent::Blast(const Vector3& point, Polarity polarity, float strength01,
                                       bool againstAnchor, const char* poolTag, int capacity,
                                       const char* debugName)
{
    const float strength = Clamp01(strength01);
    const int   index    = NextImpactIndexThisFrame();

    // 12.6 が「多重衝突では減衰させる」と言っているのはヒットストップの話だが、光源にも
    // 同じ問題がある。集束で 5 体同時に潰れると 5 つの光源が近い場所で重なり、画面が
    // 白へ抜けて、12.2 が守ろうとしている赤青の区別がその瞬間だけ消える。
    //
    // WHY «最も強い 1 発» ではなく «先着» を明るくするか: 演出は衝突を受け取ったその場で
    //     鳴らすので、このフレームにあと何発来るかを知る術が無い。1 フレーム貯めてから
    //     並べ替えれば «最も強い 1 発» を選べるが、そのぶん爆発が 1 フレーム遅れて、
    //     衝突音とヒットストップだけが先に来る。手触りは遅延の方に強く出る。
    const float crowdFade = index < std::max(lightFalloffCount, 1)
        ? 1.0f : 1.0f / static_cast<float>(index - lightFalloffCount + 2);

    GameObject* root = Prepare(PathOf(impactVfx, kVfxImpactPath), point, Quaternion::Identity(),
                               debugName, poolTag, capacity);
    if (!root) return;

    if (auto* params = root->GetScript<ImpactVfxComponent>()) {
        // 無極で衝突する経路 (極を使い切った後の余韻・壁への流れ弾) では無彩色へ落ちる。
        params->polarityColor = PolarityColor(polarity);
        params->sparkPower    = Lerp(sparkPowerMin, sparkPowerMax, strength);
        params->blastLight    = Lerp(blastLightMin, blastLightMax, strength) * crowdFade;
        // 叩きつけ (7.5 の「敵 ↔ 柱・壁」/ 床を割る爆発) の跡は焦げではなく «ひび»。
        params->groundMark = againstAnchor ? kMarkCrack : kMarkScorch;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayBeamScorch(const Vector3& point, const Vector3& normal,
                                                Polarity polarity, float searSize)
{
    // 火花も煙も面の «外» へ吹かせる。Cone はローカル +Z へ吹くので、法線を向く回転を渡す。
    const Vector3 axis = normal.NormalizedOr(Vector3::UP);
    // LookRotation は forward と up が平行だと基底を作れない。床と天井でだけ姿勢が
    // 飛ぶので、そこだけ up を替える。
    const Vector3 up = Abs(Vector3::Dot(axis, Vector3::UP)) > 0.99f
        ? Vector3::FORWARD : Vector3::UP;

    // 面から少しだけ浮かせる。面上ちょうどに置くと、粒の半分が受け面へ潜って
    // 焼け跡が «欠けた円» になる。
    GameObject* root = Prepare(PathOf(beamScorchVfx, kVfxBeamScorchPath), point + axis * 0.03f,
                               Quaternion::LookRotation(axis, up), "BeamScorch");
    if (!root) return;

    if (auto* params = root->GetScript<BeamScorchVfxComponent>()) {
        // 極性色が乗るのは電弧の層だけ (12.2 / VFX/Game/README.md)。火花と煙は熱の色のまま。
        params->polarityColor = PolarityColor(polarity);
        params->emberRate     = std::max(emberRate, 0.0f);
        // 光る範囲を焦げの大きさへ合わせる。ずれると «焦げの外側が光っている» ように見える。
        params->searSize = std::clamp(searSize, 0.05f, 2.0f);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayRunDust(const Vector3& footPoint,
                                             const Vector3& moveDirection, float strength01)
{
    const float strength = Clamp01(strength01);

    // 土煙は «足が後ろへ掻いた» 側へ残る。層の Cone はローカル +Z へ吹くので、
    // 進行方向の逆を向く回転を渡す。水平へ倒してから正規化するので、
    // LookRotation の基底が上向きと平行になる経路は無い。
    Vector3 wake = -moveDirection;
    wake.y = 0.0f;
    wake = wake.NormalizedOr(Vector3::FORWARD);

    GameObject* root = Prepare(PathOf(runDustVfx, kVfxRunDustPath), footPoint,
                               Quaternion::LookRotation(wake, Vector3::UP), "RunDust");
    if (!root) return;

    if (auto* params = root->GetScript<RunDustVfxComponent>()) {
        params->dustColor = runDustColor;
        params->kickSpeed = Lerp(runDustKickMin, runDustKickMax, strength);
        params->puffSize  = Lerp(runDustSizeMin, runDustSizeMax, strength);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayGroundDust(const Vector3& point, const Vector3& outward,
                                                float strength01, float scale)
{
    const float strength = Clamp01(strength01);
    const float size     = std::max(scale, 0.05f);

    // 層の Cone はローカル +Z へ吹く。押し退けられた側 (outward) をそのまま向かせる。
    Vector3 flow{ outward.x, 0.0f, outward.z };
    flow = flow.NormalizedOr(Vector3::FORWARD);

    GameObject* root = Prepare(PathOf(groundDustVfx, kVfxGroundDustPath), point,
                               Quaternion::LookRotation(flow, Vector3::UP), "GroundDust",
                               kGroundPool, groundDustSlots);
    if (!root) return;

    if (auto* params = root->GetScript<RunDustVfxComponent>()) {
        params->dustColor = runDustColor;
        // WHY 強さで «0 まで» 落とさないか: 弱く踏んだ 1 歩でも床は鳴っている。
        //     0 に近づけると «歩いているのに何も出ない» フレームができ、
        //     出たり出なかったりする方が抜けとして目立つ。
        params->puffSize  = std::max(groundDustSize, 0.05f) * size * Lerp(0.62f, 1.0f, strength);
        params->kickSpeed = std::max(groundDustKick, 0.0f) * size * Lerp(0.45f, 1.0f, strength);
        params->gritPower = std::max(groundDustGrit, 0.0f);
        params->Apply();
    }
    Fire(*root);
}

} // namespace sandbox
