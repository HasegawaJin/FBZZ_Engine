/// @file    TitleFieldGridComponent.hpp
/// @brief   タイトル背景の磁場グリッドへ、電極の位置と時間を毎フレーム送る。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// 見た目の値は 1 つも持たない。格子の間隔も歪みの深さも FieldGrid.mat が正本で、
/// ここが送るのは「マテリアルには置けないもの」だけ:
///   - 電極の位置と極性 — ElectrodeRig がカーソル追従で毎フレーム動かす
///   - 時間             — ジオメトリ描画では PostProcConstants (b6) の time が束縛されない
///   - 極性色           — [[ElectrodePole]] が正本。.mat へ写すと 12.2 の配色が二重管理になる
///
/// @note 電極を型で探さない: 型で引くと 2 回走査したうえ «どちらが先か» でスロット番号が
///       入れ替わる。ElectrodeRig::All() は両極が 1 本の配列へ登録される窓口で順番が変わらない。
/// @note ドームはワールドの一部でなく «画面を埋める代理» で、床は視線と平面の交点から作る
///       (FieldGrid.hlsl)。中心がカメラに一致していれば半径・位置は絵に効かない。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Title/ElectrodeRig.hpp>
#include <Scripts/Utils/GameCursorComponent.hpp>
#include <Scripts/Title/ElectrodePole.hpp>
#include <cmath>
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
    FBZZ_GROUP("デバッグ")
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

    /// @note 色自体は変わらないが、Play/Stop の往復や .mat 再読み込みで per-instance の
    ///       上書きが落ちることがあるため毎フレーム送り直す。
    instance.SetVector4(kFieldGridPlusId,  kColorRight);
    instance.SetVector4(kFieldGridMinusId, kColorLeft);

    if (scene.name == "Load") {
        float collision = 0.0f;
        const auto& rigs = ElectrodeRig::All();
        for (const ElectrodeRig* left : rigs) {
            for (const ElectrodeRig* right : rigs) {
                if (!left || !right || left == right || left->PoleOf() == right->PoleOf()) continue;
                const float distance = (left->Position() - right->Position()).Length();
                collision = Max(collision, Clamp01((2.0f - distance) / 1.2f));
            }
        }
        const Vector4 green{ 0.12f, 1.0f, 0.38f, 1.0f };
        const float pulse = 0.5f + 0.5f * std::sin(time.UnscaledTime() * 5.0f);
        const float accent = collision * (0.72f + pulse * 0.28f);
        instance.SetVector4(kFieldGridPlusId,
                            kColorRight * (1.0f - accent) + green * accent);
        instance.SetVector4(kFieldGridMinusId,
                            kColorLeft * (1.0f - accent) + green * accent);
        instance.SetFloat(MaterialPropertyId("intensity"), 1.0f + accent * 0.45f);
        instance.SetFloat(MaterialPropertyId("warpStrength"), 1.6f + accent * 1.4f);
        instance.SetFloat(MaterialPropertyId("ringStrength"), 0.3f + accent * 0.9f);
    }

    /// @note phase は輪の進み (r - phase * speed) にしか使わないため巻き取らない。
    ///       ハッシュへ渡す Beam の phase と違い、大きくしても縞へ潰れる場所が無い。
    instance.SetFloat(kFieldGridPhaseId, time.UnscaledTime());

    const auto& rigs = ElectrodeRig::All();
    int active = 0;
    for (int slot = 0; slot < kSlots; ++slot) {
        const ElectrodeRig* rig = static_cast<std::size_t>(slot) < rigs.size()
            ? rigs[static_cast<std::size_t>(slot)]
            : nullptr;

        /// @note w = 0 のスロットは歪みも光も出さない。空きを «無効» として使えるので、
        ///       電極が 4 本に満たなくても分岐が要らない。
        Vector4 packed{ 0.0f, 0.0f, 0.0f, 0.0f };
        if (rig) {
            const Vector3 position = rig->Position();
            packed = { position.x, position.y, position.z,
                       rig->PoleOf() == Pole::Plus ? 1.0f : -1.0f };
            ++active;
        }
        instance.SetVector4(kFieldGridElectrodeIds[slot], packed);
    }
    debugElectrodes = active;
}

} // namespace sandbox
