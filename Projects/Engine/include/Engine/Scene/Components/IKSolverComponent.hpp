// FBZZ Engine
// IKSolverComponent.hpp | fbzz::scene
// スケルタルアニメーション後段に適用する解析的 2-Bone IK チェーン設定
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

struct IKChain {
    // 2-Bone IK の 3 ノード。例: Upper -> Lower -> Tip。
    std::string rootBoneName;
    std::string midBoneName;
    std::string tipBoneName;

    EntityID targetEntity = EntityID::INVALID;
    EntityID poleEntity   = EntityID::INVALID;

    // EntityID はロードごとに変わるため、シーンファイルには安定した識別子で保存する。
    // 解決優先順位: GUID (リネーム耐性あり) → 名前 (後方互換フォールバック)
    std::string targetName;
    std::string targetGuid;
    std::string poleName;
    std::string poleGuid;

    // IK のブレンド量。0 は FK 維持、1 は IK 解を完全適用する。
    float weight  = 1.0f;
    bool  enabled = true;

    // IK ゴール距離を「骨長合計 * maxExtension」に制限する。
    // WHY: weight は IK の効きであり、膝ロック回避に使うと IK 全体が弱くなる。
    //      伸展率を別に持つことで weight=1 のまま膝に少し曲がりを残せる。
    float maxExtension = 0.98f;

    // ターゲット位置に加算するワールド空間オフセット。
    // 表示用ターゲットを扱いやすい位置に置いたまま、実際の IK ゴールだけを調整できる。
    math::Vector3 targetOffset = math::Vector3::ZERO;

    // C: Soft IK の減衰量。0 で無効、0.05〜0.15 が実用域。
    // WHY: maxExtension はゴール距離の硬いクランプであり、閾値超過でポッピングが発生する。
    //      指数減衰 (Blender 準拠) で連続的に近似することで、伸び切る手前を滑らかに減速させる。
    float softness = 0.05f;

    // F: Auto Pole — Pole GameObject を配置せずに IKSystem が FK 曲げ方向から
    //    ポール位置を毎フレーム自動計算する。
    // WHY: 膝・肘チェーンの初期設定コストを下げるために追加。
    //      Pole GO を手動配置しなくても FK の曲げ方向 (≒ bind-pose の膝向き) を
    //      そのまま IK に引き継げる。poleEntity が有効なら poleEntity が優先される。
    bool autoPole = false;
    // Auto Pole の優先曲げ方向を Owner ローカル空間で指定する。
    // WHY: Player のように見た目を 180 度回転して使うキャラクターでは、FK 曲げ方向だけを見ると
    //      膝が背面へ折れることがある。Owner 回転込みの方向を指定して、キャラクター向きに追従させる。
    //      ゼロベクトルなら従来通り FK 曲げ方向を使う。
    math::Vector3 autoPoleLocalDirection = math::Vector3::ZERO;
};

struct IKSolverComponent {
    bool enabled = true;
    std::vector<IKChain> chains;

    const char* GetTypeName() const { return "IK Solver"; }
    // WHY: IKChain は可変長配列なので、InspectorPanel 側で直接描画する。
    void Reflect(IReflector& r) {
        r.Field("enabled", enabled);
    }
};

} // namespace fbzz::scene
