/// @file    TitleFieldGridComponent.hpp
/// @brief   タイトル背景の磁場グリッドへ、電極の位置と時間を毎フレーム送る。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// 見た目の値は 1 つも持たない。格子の間隔も歪みの深さも FieldGrid.mat が正本で、
/// ここが送るのは「マテリアルには置けないもの」だけ:
///   - 電極の位置と極性 — ElectrodeRig がカーソル追従で毎フレーム動かす
///   - 時間             — ジオメトリ描画では PostProcConstants (b6) の time が束縛されない
///   - 極性色           — [[PolarityTypes]] が正本。.mat へ写すと 12.2 の配色が二重管理になる
///
/// WHY 電極を型で探さないか:
///   ＋と−は別のコンポーネントなので、型で引くと 2 回走査したうえ «どちらが先か» で
///   スロット番号が入れ替わる。ElectrodeRig::All() は両極が 1 本の配列へ登録される
///   窓口で、盤面に何本居ても順番が変わらない。
///
/// WHY ドームをカメラへ貼り付けるか:
///   球はワールドの一部ではなく «画面を埋めるための代理» で、床は視線と平面の交点から
///   作られる (FieldGrid.hlsl)。中心がカメラに一致してさえいれば半径も位置も絵に効かない。
///   シーンでカメラを動かしたときに背景が破綻しないよう、位置合わせをここへ閉じる。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Title/ElectrodeRig.hpp>
#include <Scripts/Utils/GameCursorComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <cstddef>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

inline constexpr MaterialPropertyId kFieldGridPhaseId { "phase" };
inline constexpr MaterialPropertyId kFieldGridPlusId  { "plusColor" };
inline constexpr MaterialPropertyId kFieldGridMinusId { "minusColor" };
inline constexpr MaterialPropertyId kFieldGridElectrodeIds[] = {
    MaterialPropertyId{ "electrode0" },
    MaterialPropertyId{ "electrode1" },
    MaterialPropertyId{ "electrode2" },
    MaterialPropertyId{ "electrode3" },
};

class TitleFieldGridComponent : public Script {
    FBZZ_SCRIPT(TitleFieldGridComponent)

public:
    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugElectrodes, 0, "Electrodes")
    FBZZ_TOOLTIP("いま格子を歪ませている電極の数。0 のままなら送り先が届いていない "
                 "(FieldGrid.mat が付いていないか、電極がまだ OnStart を通っていない)")

    void OnUpdate() override;

private:
    /// FieldGrid.hlsl が持つ electrode スロットの数。
    static constexpr int kSlots =
        static_cast<int>(sizeof(kFieldGridElectrodeIds) / sizeof(kFieldGridElectrodeIds[0]));
};

FBZZ_REFLECT(TitleFieldGridComponent)

inline void TitleFieldGridComponent::OnUpdate()
{
    if (GameObject* cameraObject = GameCursorComponent::MainCameraObject(*this))
        transform.position = cameraObject->transform.worldPosition;

    const MaterialInstance instance = material.Instance();
    if (!instance.IsValid()) return;

    // WHY 毎フレーム送り直すか: 色は変わらないが、Play/Stop の往復や .mat の再読み込みで
    //     per-instance の上書きが落ちる。書き込み 2 回で «落ちたら戻る» が保証できる。
    instance.SetVector4(kFieldGridPlusId,  kColorPlus);
    instance.SetVector4(kFieldGridMinusId, kColorMinus);

    // WHY 巻き取らないか: phase は輪の進み (r - phase * speed) にしか入らない。
    //     ハッシュへ渡す Beam の phase と違い、大きくしても縞へ潰れる場所が無い。
    instance.SetFloat(kFieldGridPhaseId, time.UnscaledTime());

    const auto& rigs = ElectrodeRig::All();
    int active = 0;
    for (int slot = 0; slot < kSlots; ++slot) {
        const ElectrodeRig* rig = static_cast<std::size_t>(slot) < rigs.size()
            ? rigs[static_cast<std::size_t>(slot)]
            : nullptr;

        // w = 0 のスロットは歪みも光も出さない。空きを «無効» として使えるので、
        // 電極が 4 本に満たなくても分岐が要らない。
        Vector4 packed{ 0.0f, 0.0f, 0.0f, 0.0f };
        if (rig) {
            const Vector3 position = rig->Position();
            packed = { position.x, position.y, position.z,
                       rig->Pole() == Polarity::Plus ? 1.0f : -1.0f };
            ++active;
        }
        instance.SetVector4(kFieldGridElectrodeIds[slot], packed);
    }
    debugElectrodes = active;
}

} // namespace sandbox
