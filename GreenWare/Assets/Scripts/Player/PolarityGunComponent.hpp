/// @file PolarityGunComponent.hpp
/// @brief 二丁の Polarity Emitter。左右独立の照射バッテリーで極性だけを塗る
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY ここにダメージが 1 行も無いか:
///   企画書 2 章の前提「銃は敵を倒さない。倒すのは衝突である」。
///   ここに 1 行でもダメージ処理を足すと、企画そのものが別のゲームになる。
///
/// WHY 弾ではなく照射か (6 章):
///   「単発では 5 体溜めるのに 4 秒かかっていた。なぞりなら 1 回のジェスチャーで終わる」。
///   飛翔体は 1 発 1 体しか運べないため、6.1 の「敵 A → B → C をなぞって 3 体同時に
///   帯電させる」が原理的に作れない。線を引く操作にするには、線そのものが判定でなければ
///   ならない。貫通・塗り時間・太さの 3 つは 6.2 の仕様表がそのまま実装になる。
///
/// WHY 同じボタンで長押しとタップを分けるか (6.2):
///   長押し = なぞって塗る / タップ = 一瞬の点付与 = 起爆。7.9 の起爆は
///   「温存した無極の 1 体へ逆極を当てる」1 手なので、なぞりと同じ操作では出せない。
///   押した瞬間からビームは出る (どこを指しているかは常に見えていないと線が引けない)。
///   離すのが速ければ、塗り切っていなくてもその 1 体へ点付与を確定させる。
///
/// WHY ビームの GameObject を先に作って寝かせるか:
///   照射のたびに GameObject を作って捨てると、押すたびに生成・破棄が走り、
///   シーンの GameObject 数が入力に合わせて上下する。左右 2 本ぶんを OnStart で
///   確保しておけば、戦闘中は「寝ているビームを起こして伸ばす」だけになる。
///
/// WHY 円柱メッシュではなく LineRendererComponent か:
///   MeshRenderer::mesh を meshPath から解決しているのは SceneSerializer だけで、
///   ランタイムに meshPath を書いても mesh は nullptr のまま = 一切描画されない。
///   LineRendererComponent なら PresentationSystem (Phase::LateUpdate) が
///   points から毎フレーム メッシュを作って MeshRenderer へ挿してくれる。
///   Script フェーズで両端を書けば同じフレームの描画に乗る。
///   ビルボードなので、どの角度から見ても線の太さが変わらないという利点もある。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/WeaponAnimatorComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/EmitterBattery.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PolarityGunComponent : public Script {
    FBZZ_SCRIPT(PolarityGunComponent)

    // SE はここから鳴らす。無くても照射できるが 12.1 の手応えが丸ごと消える。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    // PlayerComponent が必須 PolarityTuning を注入する。数値を Inspector 側へ複製しない。
    fbzz::Asset<PolarityTuning> tuning{};

    FBZZ_GROUP("Weapons")
    // 銃本体。ここから WeaponAnimatorComponent を引いて照射モーションを再生する。
    // 未設定でも WPN_Pistol_R / WPN_Pistol_L を名前で復旧する。
    FBZZ_REF(GameObject, weaponPlus,  "Weapon (+) / Right")
    FBZZ_REF(GameObject, weaponMinus, "Weapon (-) / Left")

    FBZZ_GROUP("Muzzles (override)")
    // 通常は空でよい。銃側の SOCKET_Muzzle が自動で使われる。
    // WHY それでも残すか: マズルを意図的にずらしたい演出用の逃げ道。
    FBZZ_REF(GameObject, muzzleRight, "Muzzle Right (+)")
    FBZZ_REF(GameObject, muzzleLeft,  "Muzzle Left (-)")

    // WHY 入力の項目を持たないか:
    //   11 章の操作表は「左入力 = 左銃 (−) / 右入力 = 右銃 (＋)」という対応だけを
    //   決めており、それが右クリックなのか RT なのかは別の話。物理入力を Inspector に
    //   持つと、その 1 行のためにゲームパッド対応が原理的に不可能になる。
    //   実際の割り当ては ProjectSettings/Input.inputactions が持つ
    //   (マウス左右 / LT・RT が同じアクションへ束ねてある)。

    FBZZ_GROUP("Feedback (12.1)")
    // 12.1 は「4 週目の調整項目ではなく 1 週目から入れる」と名指ししている。
    // 極性にダメージが無い以上、塗った手応えが無いと開始 1 分で投げられる。
    FBZZ_FIELD_FILE(sfxBeamStart,  "", "SFX Beam Start",  ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxPaint,      "", "SFX Paint",       ".wav,.ogg")
    FBZZ_TOOLTIP("1 体を塗り切った瞬間。なぞりのリズムはこの音で数える")
    FBZZ_FIELD_FILE(sfxNeutralize, "", "SFX Neutralize",  ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxEmpty,      "", "SFX Empty",       ".wav,.ogg")
    FBZZ_TOOLTIP("バッテリーが尽きた瞬間と、空のまま撃とうとしたときの音")

    FBZZ_GROUP("Beam")
    FBZZ_FIELD_FILE(beamMaterial, "Assets/Materials/Fallback/VFXMeshFallback.mat",
                    "Beam Material", ".mat")
    FBZZ_TOOLTIP("ビームに割り当てる .mat。極の色は albedo へ毎フレーム上書きされるため、"
                 "Unlit・両面のものを指す。既定は VFX 用のフォールバック")
    // 12.3「ビームは細い芯 ＋ 淡いグローの 2 層」。芯だけだと線が硬く、
    // グローだけだとどこが中心か読めない。当たり判定の太さとは無関係の見た目の値。
    FBZZ_FIELD_RANGE(float, coreWidth, 0.09f, "Core Width", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, glowWidth, 0.34f, "Glow Width", 0.0f,  3.0f)
    FBZZ_TOOLTIP("0 でグロー層を出さない")
    // 1 を超えた色がブルームに乗る。12.2 の注記どおり、上げすぎると白飛びして
    // 赤と青の区別がつかなくなるので、明るさより彩度を優先する。
    FBZZ_FIELD_RANGE(float, coreBrightness, 2.4f, "Core Brightness", 0.1f, 8.0f)
    FBZZ_FIELD_RANGE(float, glowBrightness, 1.0f, "Glow Brightness", 0.1f, 8.0f)
    FBZZ_FIELD_RANGE(float, glowOpacity, 0.35f, "Glow Opacity", 0.0f, 1.0f)
    FBZZ_TOOLTIP("グロー層の不透明度。芯を透かして見せるため 1 未満にする")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugBeam, false, "Draw Debug Beam")

    // ── HUD が読む窓口 ──────────────────────────────────────────────────
    /// 照射バッテリーの充填率。1 = 満タン / 0 = 空。
    [[nodiscard]] float BatteryOf(Polarity polarity) const;
    /// 今この極で照射を始められるか (6.3 の再点火待ちを含む)。
    [[nodiscard]] bool  CanEmit(Polarity polarity) const;
    /// 今まさに照射しているか。HUD が「減っている最中」を出し分けるために読む。
    [[nodiscard]] bool  IsEmitting(Polarity polarity) const;

    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    void SetController(PlayerControllerComponent* controller) { m_controllerOverride = controller; }
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

    void OnStart()      override;
    // WHY Script ではなく LateScript か: ビームの終点は PlayerAimComponent が引いた線の
    //     先端で、その線はカメラの向きから決まる。照射と同じフェーズに揃えておかないと、
    //     触れて見えた敵と極性が乗る敵が 1 フレームずれる。
    void OnLateUpdate() override;
    void OnDestroy()    override;

private:
    /// 片側のエミッター 1 基。左右で同じ処理を 2 度書かないためにまとめる。
    struct Emitter {
        Polarity       polarity = Polarity::Plus;
        EmitterBattery battery;
        /// 12.3 の 2 層。芯とグローで GameObject を分ける。
        EntityRef core;
        EntityRef glow;
        /// 押している間の経過秒。tapSeconds 以下で離せばタップ = 起爆 (6.2)。
        float heldSeconds = 0.0f;
        bool  held        = false;
        bool  emitting    = false;
        /// この 1 押しで 1 度でも照射できたか。空のまま連打して起爆だけ通ると、
        /// 6.3 の「時間が資源になる」が起爆に対して効かなくなる。
        bool  pressEmitted = false;
        /// 空になった瞬間だけ音を出すための立ち上がり検出。
        bool  wasDepleted = false;
        /// 今ビームを出しているか。消すのは状態が変わったフレームだけでよい。
        bool  beamVisible = false;
    };

    [[nodiscard]] Emitter&       Side(Polarity polarity)
    { return polarity == Polarity::Plus ? m_plus : m_minus; }
    [[nodiscard]] const Emitter& Side(Polarity polarity) const
    { return polarity == Polarity::Plus ? m_plus : m_minus; }

    void TickEmitter(Emitter& emitter, float dt);
    /// 照射 1 フレームぶん。線に触れている対象すべてを塗る (6.2 の貫通)。
    /// contactSeconds はバッテリーから実際に引けた秒数。尽きかけたフレームは
    /// dt より短くなるので、残量がそのまま塗れた量になる。
    void Sweep(Emitter& emitter, float contactSeconds);
    /// タップの一瞬の点付与 = 起爆 (6.2 / 7.9)。
    void Detonate(Emitter& emitter);
    void PlayPaintFeedback(const PolarityResult& result);

    [[nodiscard]] bool InputHeld(Polarity polarity) const;
    /// 銃が手にあって、抜き / 収めも終わっているか。
    ///
    /// WHY IsDrawn() だけでは足りないか: WeaponRigComponent の m_drawn は
    ///     「動作が終わったか」ではなく「銃の親を移し終えたか」で立つ。抜きの後半は
    ///     まだ展開クリップの途中なのに IsDrawn() は true になっているため、
    ///     発射口が畳まれたままレーザーだけが伸びる。IsBusy() はそのために在る。
    [[nodiscard]] bool WeaponsReady() const;
    [[nodiscard]] PlayerAimComponent* Aim() const;

    // ── ビームの見た目 ──────────────────────────────────────────────────
    void BuildBeam(Emitter& emitter);
    /// 銃口から照準の先までへ 2 層の線を張る。
    void ShowBeam(Emitter& emitter, const Vector3& from, const Vector3& to);
    void HideBeam(Emitter& emitter);
    void ReleaseBeam(Emitter& emitter);
    [[nodiscard]] GameObject* BuildBeamPart(const std::string& name, int order);
    /// 1 層ぶんの線を今フレームの両端・太さ・色へ合わせる。
    void PlaceBeamLayer(const EntityRef& ref, const Vector3& from, const Vector3& to,
                        float width, const Vector4& color);
    [[nodiscard]] std::string BeamName(Polarity polarity, const char* layer) const;

    // ── 銃 ──────────────────────────────────────────────────────────────
    [[nodiscard]] Vector3 MuzzlePosition(Polarity polarity) const;
    /// 銃 GameObject と、その上の WeaponAnimatorComponent を引く。
    /// 銃は Draw/Holster で親が張り替わるが GameObject 自体は生き続けるため、
    /// 参照ではなく毎回引き直す。
    [[nodiscard]] GameObject* WeaponObject(Polarity polarity) const;
    [[nodiscard]] WeaponAnimatorComponent* WeaponAnim(Polarity polarity) const;

    Emitter m_plus;
    Emitter m_minus;

    PlayerAimComponent* m_aimOverride = nullptr;
    PlayerControllerComponent* m_controllerOverride = nullptr;
    WeaponRigComponent* m_weaponRig = nullptr;
    // 照準が居ない構成は組み間違いなので、1 度だけ言えば足りる。押している間ずっと
    // 出すと、同じ行で Console が埋まって他の警告が流れる。
    bool m_warnedNoAim = false;
};

FBZZ_REFLECT(PolarityGunComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline float PolarityGunComponent::BatteryOf(Polarity polarity) const
{
    return Side(polarity).battery.Ratio(tuning->batterySeconds);
}

inline bool PolarityGunComponent::CanEmit(Polarity polarity) const
{
    return Side(polarity).battery.CanEmit();
}

inline bool PolarityGunComponent::IsEmitting(Polarity polarity) const
{
    return Side(polarity).emitting;
}

inline PlayerAimComponent* PolarityGunComponent::Aim() const
{
    return m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
}

inline bool PolarityGunComponent::WeaponsReady() const
{
    // WHY 無いときは撃てる扱いにするか: WeaponRigComponent を置かない構成
    //     (デバッグシーンや、抜き差しを作る前の検証) で撃てなくなると、
    //     照射そのものを確かめられなくなる。抑止は「収納機構がある」ときだけ効かせる。
    auto* rig = m_weaponRig ? m_weaponRig : scene.GetScript<WeaponRigComponent>();
    return !rig || (rig->IsDrawn() && !rig->IsBusy());
}

inline void PolarityGunComponent::OnStart()
{
    if (!tuning) {
        debug.LogError("PolarityGunComponent requires PolarityTuning.fzdata "
                       "(PlayerComponent injects it).");
        enabled = false;
        return;
    }

    m_plus  = {};
    m_minus = {};
    m_warnedNoAim = false;
    m_plus.polarity  = Polarity::Plus;
    m_minus.polarity = Polarity::Minus;
    m_plus.battery.Reset(tuning->batterySeconds);
    m_minus.battery.Reset(tuning->batterySeconds);

    BuildBeam(m_plus);
    BuildBeam(m_minus);

    // 7.7 の「破ってはいけない設計上の制約」をここで検算する。
    //     敵の極性持続時間 ＞ 1 本の線を引き切る時間 ＋ 起爆までの間
    // 破っていると起爆する前に最初に塗った敵の極が切れ、ゲームが成立しない。
    // 破綻の仕方が「なんとなく繋がらない」なので、明示的に言わないと気付けない。
    constexpr float kDetonateSecondsEstimate = 0.8f; // 線を引き終えて起爆点へ振り向くまで
    const float shortest = tuning->ShortestDuration();
    if (!tuning->SatisfiesTimingConstraint(shortest, kDetonateSecondsEstimate)) {
        debug.LogError(
            "PolarityTuning breaks the 7.7 timing constraint: shortest duration must "
            "exceed battery seconds + the time it takes to swing onto the detonator. "
            "Combos will not connect.");
    }
}

inline void PolarityGunComponent::OnDestroy()
{
    ReleaseBeam(m_plus);
    ReleaseBeam(m_minus);
}

inline bool PolarityGunComponent::InputHeld(Polarity polarity) const
{
    // 右 = ＋ / 左 = −。左右の入力と左右の銃を一致させる (11 章)。
    // マウス左右と LT / RT はアクション層で同じ名前へ束ねてあるので、
    // ここではどのデバイスで押されたかを知らなくてよい。
    return input.GetAction(polarity == Polarity::Plus ? actions::kEmitPlus
                                                      : actions::kEmitMinus);
}

inline void PolarityGunComponent::OnLateUpdate()
{
    const float dt = Time::deltaTime;
    TickEmitter(m_plus, dt);
    TickEmitter(m_minus, dt);
}

inline void PolarityGunComponent::TickEmitter(Emitter& emitter, float dt)
{
    // 銃を収めている間と、抜き / 収めの最中は照射できない。
    //
    // WHY 空撃ちの音も出さないか: バッテリー切れは「撃とうとしたが出なかった」なので
    //     反応を返す必要があるが、収納中は銃自体が手に無い。音を返すと畳まれた銃が
    //     応答したことになる。収納は HUD のゲージとクロスヘアが消えることで既に
    //     見えているので、ここは黙って何もしないのが正しい。
    if (!WeaponsReady()) {
        // 押していた事実ごと捨てる。残したまま抜き直すと、その最初のフレームが
        // 「離した」と解釈されてタップの起爆が 1 回暴発する。
        emitter.held         = false;
        emitter.heldSeconds  = 0.0f;
        emitter.emitting     = false;
        emitter.pressEmitted = false;
        // 6.3 の回復は非照射時に進む。収めている間も溜まる。
        emitter.battery.Refill(dt, tuning->batterySeconds, tuning->batteryRefillSeconds,
                               tuning->batteryRearmRatio);
        // 立ち上がり検出も追従させる。ここを止めると、収納中に満タンへ戻ったのに
        // 抜いた瞬間だけ「尽きた音」が鳴る。
        emitter.wasDepleted = emitter.battery.depleted;
        HideBeam(emitter);
        return;
    }

    const bool held    = InputHeld(emitter.polarity);
    const bool pressed = held && !emitter.held;
    // 離したフレームに、それがタップだったかを決める。押していた時間しか手がかりが無い。
    const bool tapped  = !held && emitter.held && emitter.heldSeconds <= tuning->tapSeconds;

    emitter.heldSeconds = held ? emitter.heldSeconds + dt : 0.0f;
    emitter.held        = held;
    emitter.emitting    = false;

    if (pressed) {
        emitter.pressEmitted = false;
        if (emitter.battery.CanEmit()) {
            if (!sfxBeamStart.empty()) audio.PlayOneShot(sfxBeamStart);
            // 銃側にも点火を伝える。スライドが動く代わりに発射口が開く。
            if (auto* weapon = WeaponAnim(emitter.polarity)) weapon->PlayFire();
            if (auto* player = m_controllerOverride
                ? m_controllerOverride : scene.GetScript<PlayerControllerComponent>())
                player->PlayFireAnimation(emitter.polarity == Polarity::Plus);
        } else {
            // 空撃ちにも音を返す。無反応だと「入力が拾われていない」のか
            // 「バッテリーが空」なのか区別できず、6.3 のリズムを覚えられない。
            if (!sfxEmpty.empty()) audio.PlayOneShot(sfxEmpty);
            if (auto* weapon = WeaponAnim(emitter.polarity)) weapon->PlayDry();
        }
    }

    if (held) {
        // 6.3「非照射時に回復」。押している間は回復しないので、消費できた秒数が
        // そのまま線を引ける長さになる。
        const float spent = emitter.battery.Drain(dt);
        if (spent > 0.0f) {
            emitter.emitting     = true;
            emitter.pressEmitted = true;
            Sweep(emitter, spent);
        } else {
            HideBeam(emitter);
        }
    } else {
        emitter.battery.Refill(dt, tuning->batterySeconds, tuning->batteryRefillSeconds,
                               tuning->batteryRearmRatio);
        HideBeam(emitter);
    }

    // 尽きた瞬間だけ鳴らす。空の間ずっと鳴らすと、音が状態ではなく背景になる。
    if (emitter.battery.depleted && !emitter.wasDepleted && !sfxEmpty.empty())
        audio.PlayOneShot(sfxEmpty);
    emitter.wasDepleted = emitter.battery.depleted;

    // タップは塗り時間に届かなくても点付与を確定させる (6.2)。
    // WHY 長押しの塗りと重ねて構わないか: 塗り切っていれば 7 章のルール 2 で
    //     延長になるだけで、7.9 の起爆点としての意味は変わらない。
    if (tapped && emitter.pressEmitted) Detonate(emitter);
}

inline void PolarityGunComponent::Sweep(Emitter& emitter, float contactSeconds)
{
    auto* aim = Aim();
    if (!aim || !aim->HasAim()) {
        // 6 章のなぞりはこのゲームの入力そのもの。線が引けない状態で照射できると、
        // 「塗れない」原因がプレイヤーの操作に見えてしまう。
        if (!aim && !m_warnedNoAim) {
            m_warnedNoAim = true;
            debug.LogError("PolarityGunComponent requires PlayerAimComponent "
                           "on the same object.");
        }
        HideBeam(emitter);
        return;
    }

    ShowBeam(emitter, MuzzlePosition(emitter.polarity), aim->AimPoint());

    // 6.2「貫通する。線上の敵すべてに判定が乗る」。手前で止めない。
    for (const BeamContact& contact : aim->Contacts()) {
        PolarityResult result{};
        if (contact.target->Paint(emitter.polarity, contactSeconds, result))
            PlayPaintFeedback(result);
    }
}

inline void PolarityGunComponent::Detonate(Emitter& emitter)
{
    auto* aim = Aim();
    if (!aim) return;

    // 起爆点は線のいちばん手前。7.9 の「線から外して温存した 1 体」がそこに来る。
    auto* target = aim->CurrentPolarityTarget();
    if (!target) return;

    PlayPaintFeedback(target->Apply(emitter.polarity));
}

inline void PolarityGunComponent::PlayPaintFeedback(const PolarityResult& result)
{
    // 12.4 は「中和・延長した瞬間にも明確なフィードバックを返す」を仕様として要求する。
    // 同じ音で済ませると、狙って中和したのか事故だったのかが耳で判別できない。
    switch (result.change) {
    case PolarityChange::Neutralized:
        if (!sfxNeutralize.empty()) audio.PlayOneShot(sfxNeutralize);
        break;
    case PolarityChange::Applied:
    case PolarityChange::Extended:
        if (!sfxPaint.empty()) audio.PlayOneShot(sfxPaint);
        break;
    }
}

// ── ビームの見た目 ────────────────────────────────────────────────────────────

inline std::string PolarityGunComponent::BeamName(Polarity polarity, const char* layer) const
{
    // WHY 持ち主ごとに名前を変えるか: ビームはルートに置くため、名前で拾い直すときに
    //     同名だと 2 人目のプレイヤー (デバッグ用の複製を含む) が 1 人目のビームを奪う。
    GameObject* owner = scene.Self();
    return std::string("PolarityBeam_") + (polarity == Polarity::Plus ? "Plus" : "Minus")
         + "_" + layer + "_" + (owner ? owner->instanceId : std::string{});
}

inline void PolarityGunComponent::BuildBeam(Emitter& emitter)
{
    // 芯を後ろ (order 1) に置いてグロー (order 0) の上へ重ねる。順番が逆だと
    // 淡い層が芯を覆い、どこが線の中心なのか読めなくなる。
    emitter.core = EntityRef{ BuildBeamPart(BeamName(emitter.polarity, "Core"), 1)->GetID() };
    emitter.glow = glowWidth > 0.0f
        ? EntityRef{ BuildBeamPart(BeamName(emitter.polarity, "Glow"), 0)->GetID() }
        : EntityRef{};
    emitter.beamVisible = true; // 直後の HideBeam に確実に畳ませる
    HideBeam(emitter);
}

inline GameObject* PolarityGunComponent::BuildBeamPart(const std::string& name, int order)
{
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方 ビームの GameObject は Scene 側に残っているため、
    //     拾わずに作り直すとリロードのたびに 2 本ずつ増えていく。
    GameObject* existing = scene.Find(name);
    if (!existing) {
        // WHY プレイヤーの子にしないか: 子にするとビームがプレイヤーの移動・回転・スケールを
        //     引き継ぎ、ワールド座標で指定した両端がそのぶん歪む。ルートへ原点で置く。
        GameObject& object = scene.Create(name);
        object.runtimeGenerated = true;
        object.AddComponent<LineRendererComponent>();
        existing = &object;
    }

    // 拾い直した個体にも毎回入れ直す。Inspector で太さや色を触った直後に
    // Play し直しても反映されないと、調整のたびにビームを消して回ることになる。
    auto* line = existing->GetComponent<LineRendererComponent>();
    if (!line) return existing;

    line->materialPath = beamMaterial;
    // ワールド座標で両端を指定する。線の GameObject を動かす必要が無くなり、
    // 位置がマズルと照準だけで決まる。
    line->space     = LineSpace::World;
    line->billboard = true;
    line->loop      = false;
    // 12.6 のエフェクトや UI と競合しない帯に置く。
    line->orderInLayer = order;
    return existing;
}

inline void PolarityGunComponent::PlaceBeamLayer(const EntityRef& ref, const Vector3& from,
                                                 const Vector3& to, float width,
                                                 const Vector4& color)
{
    GameObject* object = ref.Resolve(scene);
    if (!object) return;

    auto* line = object->GetComponent<LineRendererComponent>();
    if (!line) return;

    // 2 点しか使わないので、確保済みの領域へ書き戻して毎フレームの再確保を避ける。
    line->points.resize(2);
    line->points[0] = from;
    line->points[1] = to;
    line->startWidth = width;
    line->endWidth   = width;
    // 描画に効くのは startColor だけ (PresentationSystem がこれを albedo へ流す)。
    // endColor も揃えておかないと、Inspector で見たときに嘘の情報になる。
    line->startColor = color;
    line->endColor   = color;
    line->enabled    = true;
}

inline void PolarityGunComponent::ShowBeam(Emitter& emitter, const Vector3& from,
                                           const Vector3& to)
{
    if ((to - from).LengthSq() <= EPSILON) {
        HideBeam(emitter);
        return;
    }
    emitter.beamVisible = true;

    const Vector4 base = PolarityColor(emitter.polarity);
    PlaceBeamLayer(emitter.core, from, to, coreWidth,
                   { base.x * coreBrightness, base.y * coreBrightness,
                     base.z * coreBrightness, 1.0f });
    if (glowWidth > 0.0f)
        PlaceBeamLayer(emitter.glow, from, to, glowWidth,
                       { base.x * glowBrightness, base.y * glowBrightness,
                         base.z * glowBrightness, glowOpacity });

    if (drawDebugBeam)
        debug.DrawLine(from, to, base);
}

inline void PolarityGunComponent::HideBeam(Emitter& emitter)
{
    if (!emitter.beamVisible) return;
    emitter.beamVisible = false;

    // WHY SetActive ではなく enabled か: PresentationSystem は GameObject の有効・無効を
    //     見ずに全 LineRendererComponent を回す。enabled を落とすと同じ関数の中で
    //     MeshRenderer まで無効にしてくれるので、消し方が 1 箇所に閉じる。
    const auto disable = [this](const EntityRef& ref) {
        if (GameObject* object = ref.Resolve(scene))
            if (auto* line = object->GetComponent<LineRendererComponent>())
                line->enabled = false;
    };
    disable(emitter.core);
    disable(emitter.glow);
}

inline void PolarityGunComponent::ReleaseBeam(Emitter& emitter)
{
    // ルートに置いた以上、プレイヤーと一緒には消えない。持ち主が畳む。
    if (GameObject* core = emitter.core.Resolve(scene)) scene.Destroy(*core);
    if (GameObject* glow = emitter.glow.Resolve(scene)) scene.Destroy(*glow);
    emitter.core = {};
    emitter.glow = {};
}

// ── 銃 ────────────────────────────────────────────────────────────────────────

inline GameObject* PolarityGunComponent::WeaponObject(Polarity polarity) const
{
    const HandSide hand = HandOf(polarity);
    const auto& ref = (polarity == Polarity::Plus) ? weaponPlus : weaponMinus;

    // 参照先が本当にその手の銃かを名前で確かめる (理由は WeaponSockets.hpp の IsWeaponObject)。
    if (GameObject* object = ref.Get(); IsWeaponObject(object, hand))
        return object;
    return scene.Find(WeaponObjectName(hand));
}

inline WeaponAnimatorComponent* PolarityGunComponent::WeaponAnim(Polarity polarity) const
{
    GameObject* weapon = WeaponObject(polarity);
    return weapon ? scene.GetScript<WeaponAnimatorComponent>(weapon) : nullptr;
}

inline Vector3 PolarityGunComponent::MuzzlePosition(Polarity polarity) const
{
    // 1) Inspector で明示的に指定されたマズル (演出上ずらしたい場合)
    const auto& muzzle = (polarity == Polarity::Plus) ? muzzleRight : muzzleLeft;
    if (GameObject* object = muzzle.Get())
        return object->transform.worldPosition;

    // 2) 銃側が解決済みの SOCKET_Muzzle
    // WHY scene.Find("SOCKET_Muzzle") としないか: 同名ソケットが左右の銃に 1 本ずつ
    //     存在するため、グローバル検索ではどちらが返るか GameObject の生成順に依存する。
    //     症状が日替わりになる種類のバグなので、必ず銃の部分木から引く。
    if (auto* weapon = WeaponAnim(polarity); weapon && weapon->HasMuzzleSocket())
        return weapon->MuzzlePosition();

    // 3) 旧シーン救済 (FBX 再インポート前)
    if (GameObject* weapon = WeaponObject(polarity)) {
        const bool right = polarity == Polarity::Plus;
        if (GameObject* socket = FindSocketInSubtree(
                *weapon, kSocketMuzzle,
                right ? kLegacySocketMuzzleR : kLegacySocketMuzzleL))
            return socket->transform.worldPosition;
    }

    // WHY Player 原点へ落とすのを最後にするか: ここへ来ると銃を手へ移した後も
    //     ビームだけが足元から伸びる。見た目が微妙にズレるだけなので気付きにくい。
    return transform.worldPosition;
}

} // namespace sandbox
