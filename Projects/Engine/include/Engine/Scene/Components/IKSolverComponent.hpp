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
    // 2-Bone IK の 3 ノード。例: UpLeg -> Leg -> Foot。
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

    // 足チェーン向けに地面スナップでターゲット位置だけを補正する。
    // WHY: 接地法線を TipBone や toe 子孫の回転へ直接伝播させない。
    bool useGroundSnap = true;

    // ターゲット位置に加算するワールド空間オフセット。
    // 表示用 FootTarget を扱いやすい位置に置いたまま、実際の IK ゴールだけを調整できる。
    math::Vector3 targetOffset = math::Vector3::ZERO;

    // C: Soft IK の減衰量。0 で無効、0.05〜0.15 が実用域。
    // WHY: maxExtension はゴール距離の硬いクランプであり、閾値超過でポッピングが発生する。
    //      指数減衰 (Blender 準拠) で連続的に近似することで、伸び切る手前を滑らかに減速させる。
    float softness = 0.05f;

    // D: このチェーンを骨盤高さ補正 (IKSolverComponent::hipBoneName) の計算対象とする。
    // WHY: 腕など骨盤と連結しないチェーンまで補正対象に含めると補正量が不正確になる。
    bool isLeg = false;

    // 斜面での足首傾き補正に使う、足裏が向いているボーンローカル軸。
    // ゼロベクトルのとき補正を行わない (デフォルト)。
    // WHY: 足ボーンのローカル軸方向はリグごとに異なるため汎用軸は仮定できない。
    //      ユーザーがリグを見て正しい軸を指定する必要がある。
    //      例: Mixamo 系 = {0,-1,0} / Blender Z-up 系 = {0,0,-1}
    math::Vector3 footNormalAxis = math::Vector3::ZERO;

    // E: 地面レイキャストの感度パラメータ。脚長に対する比率で指定する。
    // rayUpRatio   : 足 FK 位置からレイ開始点を上にずらす量 (脚長倍率)。
    //                大きいほど段差の「上り坂」をより高い位置から拾える。
    // rayDownRatio : 下向きレイの全長 (脚長倍率)。
    //                大きいほど足元が深く沈んだ地形や「下り坂」まで追従する。
    // footSurfaceOffset : 接地ヒット点から法線方向に浮かせる距離 (ワールド単位)。
    //                     足首ジョイントが足裏より内側にあるため必要。小さいほど地面に密着する。
    // WHY: 固定定数では脚長・カメラ距離・ステージ段差高さがリグごとに異なるため調整不能。
    //      チェーン単位でパラメータを持つことで、片足だけ感度を変えるといった柔軟な運用が可能。
    float rayUpRatio        = 0.4f;
    float rayDownRatio      = 1.3f;
    float footSurfaceOffset = 0.06f;

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

    // D: 骨盤ボーン名。空のとき Hip 高さ補正を行わない。
    // WHY: 複数の脚チェーンの接地オフセット平均から骨盤を垂直移動させ、
    //      両脚が IK 可動域内に収まるよう先行補正する。
    std::string hipBoneName;
    // 平均脚長に対する骨盤移動量の上限比率。
    // WHY: リグごとに許容できる移動量が異なるため、固定値ではなく調整可能にする。
    float hipMaxOffsetRatio = 0.4f;

    const char* GetTypeName() const { return "IK Solver"; }
    // WHY: IKChain は可変長配列なので、InspectorPanel 側で直接描画する。
    void Reflect(IReflector& r) {
        r.Field("enabled", enabled);
        r.Field("hipBoneName", hipBoneName);
        r.Field("hipMaxOffsetRatio", hipMaxOffsetRatio);
    }
};

} // namespace fbzz::scene
