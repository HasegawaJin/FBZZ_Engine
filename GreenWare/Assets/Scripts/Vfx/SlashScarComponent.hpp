/// @file    SlashScarComponent.hpp
/// @brief   斬った面へ残る斬撃痕。当たった一撃ごとにデカールを 1 枚置く
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持ち、
/// BladeComponent へ注入する (他の Player モジュールと同じ形)。
///
/// WHY 要るか:
///   当たったことは止め (ヒットストップ)・音・刃の白熱で伝えているが、どれも
///   斬った «瞬間» にしか無い。振り抜いてしまえば画面は当たる前と同じで、
///   «自分がどこを何回斬ったか» が盤面に一切残らない。斬った線が体に残れば、
///   崩し (break-parry) がどこまで進んだかを HP バー以外からも読める。
///
/// WHY デカールか (パーティクルでも軌跡でもなく):
///   痕は «相手の面に貼り付いている» ことに意味がある。粒でも板でも、相手が動いた
///   瞬間に空中へ取り残されて «その場に散らかった何か» になる。デカールは投影なので、
///   凹凸のある部位の上でも面に沿って曲がる。
///
/// WHY 枠を使い回すか:
///   斬撃は 1 秒に何度も当たる。1 発ごとに GameObject を作ると、連撃 1 セットで
///   十数個の実体がルートへ積み上がり、消すのを 1 度でも取りこぼすと残り続ける。
///   固定数の枠を «いちばん古いものから奪う» 形にすれば、上限が構造で決まる。
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

inline constexpr const char* kSlashScarMaterialPath =
    "Assets/Materials/Decal/DecalSlashScar.mat";

class SlashScarComponent : public Script {
    FBZZ_SCRIPT(SlashScarComponent)

public:
    FBZZ_GROUP("Slash Scar")
    FBZZ_ASSET_FIELD(MaterialRef, scarMaterial, "Material")
    FBZZ_TOOLTIP("未割り当てなら DecalSlashScar.mat を使う")

    FBZZ_FIELD_RANGE(float, scarLength, 1.6f, "長さ", 0.2f, 6.0f)
    FBZZ_TOOLTIP("斬った向きへの傷の長さ [m]。斬撃の射程ではなく «刃が触れた幅»")
    FBZZ_FIELD_RANGE(float, scarWidthMeters, 0.55f, "幅", 0.05f, 3.0f)
    FBZZ_TOOLTIP("傷に直交する側の箱の大きさ [m]。細くしすぎると、少しでも面が"
                 "傾いている部位で投影が外れて傷が消える")
    FBZZ_FIELD_RANGE(float, scarDepth, 1.2f, "奥行き", 0.1f, 6.0f)
    FBZZ_TOOLTIP("投影の奥行き [m]。浅いと «面から浮いた部位» に載らず、"
                 "深いと後ろに居る別の部位にも同じ傷が写る")

    FBZZ_FIELD_RANGE(float, scarSeconds, 2.6f, "寿命", 0.2f, 20.0f)
    FBZZ_TOOLTIP("傷が消えるまで [秒]。長く残すほど «何回斬ったか» が読めるが、"
                 "残しすぎると体が傷だらけになって 1 発ごとの差が消える")
    FBZZ_FIELD_RANGE(float, scarCoolSeconds, 0.35f, "Cool", 0.0f, 3.0f)
    FBZZ_TOOLTIP("縁の熱が引くまで [秒]。ここが «今つけた傷» と «さっきの傷» を"
                 "分ける唯一の手掛かり")
    FBZZ_FIELD_RANGE(float, scarFadeStart, 0.55f, "Fade Start", 0.0f, 1.0f)
    FBZZ_TOOLTIP("寿命のどこから薄れ始めるか [0,1]。それまでは濃さを保つ ─ "
                 "最初から薄れると «付いた瞬間がいちばん濃い» が出ない")

    FBZZ_FIELD_RANGE_INT(int, scarSlots, 10, "スロット", 1, 32)
    FBZZ_TOOLTIP("同時に出せる傷の数。埋まると最も古い 1 枚を奪う")

    FBZZ_FIELD_READ_ONLY(int, debugLiveScars, 0, "Live Scars")

    /// 斬った痕を 1 枚置く。
    ///   point     … 斬れた点 (ワールド)
    ///   direction … 刃が走った向き (水平で可)。傷の長さ方向になる
    ///   facing    … 傷を «どちらから» 貼るか。斬った本人から点への向きを渡す ─
    ///               面の法線が取れないので、見ている側から投影するのが最も外れない
    ///   scale     … 大きさの倍率 (締めと溜め斬りで上げる)
    void Play(const Vector3& point, const Vector3& direction, const Vector3& facing,
              float scale);

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

private:
    /// 出ている傷 1 枚ぶん。
    struct Slot {
        EntityRef ref;
        float     elapsed = 0.0f;
        float     seed    = 0.0f;
        bool      live    = false;
    };

    /// 空いている枠を返す。全部埋まっていれば最も古い 1 枚を奪う。
    [[nodiscard]] Slot* AcquireSlot();
    [[nodiscard]] GameObject* EnsureObject(Slot& slot, int index);
    void ReleaseSlots();

    [[nodiscard]] std::string MaterialPath() const
    {
        std::string path = scarMaterial.ResolvePath();
        return path.empty() ? std::string(kSlashScarMaterialPath) : path;
    }

    std::vector<Slot> m_slots;
    /// 1 枚ごとの種を進める番号。乱数を引かないのは、同じ振りが毎回同じ絵になる方が
    /// 調整中に «今の変更で何が変わったか» を見分けやすいため。
    int m_serial = 0;
};

FBZZ_REFLECT(SlashScarComponent)


inline void SlashScarComponent::OnStart()
{
    m_slots.clear();
    m_serial = 0;
    debugLiveScars = 0;
}

inline void SlashScarComponent::OnDestroy()
{
    ReleaseSlots();
}

inline void SlashScarComponent::ReleaseSlots()
{
    // 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。持ち主が畳む。
    for (Slot& slot : m_slots)
        if (GameObject* object = slot.ref.Resolve(scene)) scene.Destroy(*object);
    m_slots.clear();
    debugLiveScars = 0;
}

inline SlashScarComponent::Slot* SlashScarComponent::AcquireSlot()
{
    const int capacity = std::clamp(scarSlots, 1, 32);

    for (Slot& slot : m_slots)
        if (!slot.live) return &slot;

    if (static_cast<int>(m_slots.size()) < capacity) {
        m_slots.push_back(Slot{});
        return &m_slots.back();
    }

    // 埋まっている。最も長く出ている 1 枚を奪う ─ いちばん薄くなっているので、
    // 消え方が飛んでも目に留まりにくい。
    Slot* oldest = &m_slots.front();
    for (Slot& slot : m_slots)
        if (slot.elapsed > oldest->elapsed) oldest = &slot;
    return oldest;
}

inline GameObject* SlashScarComponent::EnsureObject(Slot& slot, int index)
{
    if (GameObject* existing = slot.ref.Resolve(scene)) return existing;

    const std::string name = "SlashScar_" + std::to_string(index);
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方で枠の GameObject は Scene 側に残っているため、
    //     拾わずに作るとリロードのたびに枠が 1 枚ずつ増えていく。
    GameObject* object = scene.Find(name);
    if (!object) {
        // WHY 斬った相手の子にしないか: 追従させたくなるのは分かるが、部位は
        //     スキンドメッシュのノードで、transform が恒等のまま骨だけが動く
        //     («スキンド描画ノードの座標» の罠)。子にしても付いてこないうえ、
        //     ボスが倒れた瞬間に傷だけ原点へ飛ぶ。ワールドへ置き切る。
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        object = &created;
    }
    slot.ref = EntityRef{ object->GetID() };

    // Create / AddComponent はシーンの配列を伸ばしうる。設定は必ず ID から引き直した
    // 個体へ入れる ─ 作った直後のポインタは、次の Create で無効になりうる。
    GameObject* scar = slot.ref.Resolve(scene);
    if (!scar) return nullptr;

    auto* decal = scar->GetComponent<DecalComponent>();
    if (!decal) decal = &scar->AddComponent<DecalComponent>();
    decal->materialPath = MaterialPath();

    return slot.ref.Resolve(scene);
}

inline void SlashScarComponent::Play(const Vector3& point, const Vector3& direction,
                                     const Vector3& facing, float scale)
{
    if (!enabled) return;

    Slot* slot = AcquireSlot();
    if (!slot) return;
    const int index = static_cast<int>(slot - m_slots.data());

    GameObject* scar = EnsureObject(*slot, index);
    if (!scar) return;

    // ── 箱の向き ────────────────────────────────────────────────────────────
    // デカールの投影軸はローカル +Y (DecalScorch と同じ規約)。傷の長さは +X、
    // 直交する側が +Z になるよう、3 本を直交化して組む。
    //
    // WHY 面の法線を使わないか: 判定 (ResolveHit) は «扇の中に居るか» しか見て
    //     いないので、当たった «面» は誰も知らない。斬った本人から点への向きを
    //     投影軸にすれば、少なくともカメラに見えている側へは必ず貼れる ─
    //     見えていない裏面へ正確に貼っても、絵には 1 画素も出ない。
    const Vector3 down = facing.NormalizedOr(Vector3{ 0.0f, -1.0f, 0.0f });
    Vector3       cut  = direction.NormalizedOr(Vector3::FORWARD);

    // 投影軸と斬った向きが並ぶと «傷の長さ方向» が決まらない (正面から真っ直ぐ
    // 突いた形)。そのときだけ、投影軸に直交する適当な向きへ倒す。
    cut = (cut - down * Vector3::Dot(cut, down));
    if (cut.LengthSq() < 0.05f * 0.05f)
        cut = Vector3::Cross(Vector3::UP, down);
    cut = cut.NormalizedOr(Vector3::RIGHT);

    // ローカル +Y を down (投影軸)、+X を cut (傷の長さ) に向ける。
    //
    // WHY LookRotation へ渡す «前» を作り直すか: LookRotation はローカル +Z を
    //     forward、+Y を up に向ける。基底は X = Y × Z なので、+X を cut にしたければ
    //     down × Z = cut を満たす Z、すなわち Z = cut × down を渡すことになる。
    const Quaternion rotation =
        Quaternion::LookRotation(Vector3::Cross(cut, down), down).Normalized();

    scar->transform.position      = point;
    scar->transform.worldPosition = point;
    scar->transform.rotation      = rotation;
    scar->transform.worldRotation = rotation;

    const float size = Max(scale, 0.05f);
    scar->transform.scale = Vector3{ Max(scarLength, 0.05f) * size,
                                     Max(scarDepth, 0.05f),
                                     Max(scarWidthMeters, 0.05f) * size };
    scar->SetActive(true);

    slot->elapsed = 0.0f;
    slot->live    = true;
    // 枠の番号ではなく «何枚目か» で散らす。枠は使い回すので、番号だけだと
    // 同じ枠に来た傷が毎回同じ形になる。
    slot->seed    = static_cast<float>(++m_serial) * 0.37f;
}

inline void SlashScarComponent::OnUpdate()
{
    if (!enabled) return;

    // WHY Time::deltaTime か: ヒットストップで画面が止まっている間は傷も止まる。
    //     止まった絵の中で熱だけが引くと、止めが «斬った瞬間» から外れる。
    const float dt   = Max(Time::deltaTime, 0.0f);
    const float span = Max(scarSeconds, 0.05f);

    int live = 0;
    for (Slot& slot : m_slots) {
        if (!slot.live) continue;

        GameObject* scar = slot.ref.Resolve(scene);
        if (!scar) {
            slot.live = false;
            continue;
        }

        slot.elapsed += dt;
        if (slot.elapsed >= span) {
            slot.live = false;
            scar->SetActive(false);
            continue;
        }
        ++live;

        auto* decal = scar->GetComponent<DecalComponent>();
        if (!decal) continue;

        // 熱 (速い) と 濃さ (遅い) の 2 本。桁が違うので 1 本には畳めない ─
        // 熱に合わせると傷が一瞬で消え、濃さに合わせると «いつまでも熱い» になる。
        const float cooled = scarCoolSeconds > 0.0f
            ? Clamp01(slot.elapsed / scarCoolSeconds) : 1.0f;
        const float life01 = Clamp01(slot.elapsed / span);
        // 付いた瞬間がいちばん濃い。最初から薄れると «斬った» の手応えが出ない。
        const float open = 1.0f - Clamp01((life01 - Clamp01(scarFadeStart))
                                        / Max(1.0f - Clamp01(scarFadeStart), 1.0e-3f));

        decal->materialParamOverrides["cooled"]   = { cooled };
        decal->materialParamOverrides["coverage"] = { open };
        decal->materialParamOverrides["seed"]     = { slot.seed };
    }

    debugLiveScars = live;
}

} // namespace sandbox
