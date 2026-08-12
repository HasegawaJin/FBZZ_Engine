// FBZZ Engine
// AvatarMaskAsset.hpp | fbzz::asset
// アニメーションレイヤーが「どのボーンに効くか」を定義する再利用可能アセット (.mask)
//
// WHY: 上半身だけ / 下半身だけの制御は、レイヤーごとに対象ボーン集合を切り替えることで実現する。
//      その集合をレイヤー内にインライン保持すると、キャラクターやコントローラーごとに
//      同じボーンパスを手で書き直すことになり、スケルトンを差し替えた瞬間に全滅する。
//      独立アセットにしておけば 1 つ作って全レイヤー・全キャラで使い回せる。
//
// WHAT: ボーンパスごとに weight (0..1) と blendDepth を持つ。blendDepth は
//       「分岐点から下へ何階層かけて weight を立ち上げるか」で、腰などの継ぎ目で
//       ポーズが折れるのを防ぐ (Unreal の Layered blend per bone の blend depth 相当)。
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

// ボーン 1 本 (と、任意でその子孫) に対するマスク設定。
struct AvatarMaskEntry {
    // スケルトンノードのパス ("Hips/Spine/Spine1") または末端ボーン名 ("Spine1")。
    // WHY: パス指定は同名ボーンが複数ある骨格でも一意に刺さる。名前指定は手書きが楽。
    //      AnimationClip のトラックが両形式を持ちうるため、どちらでも一致させる。
    std::string bonePath;
    // このボーンに適用する最終ウェイト。0 = このレイヤーを効かせない。
    float       weight          = 1.0f;
    // 子孫ボーンにも同じエントリを適用するか。
    bool        includeChildren = true;
    // 子孫方向へ weight を立ち上げる階層数。
    // 0        : 減衰なし。エントリ配下は一律 weight (従来の 0/1 マスク相当)。
    // 1 以上   : このボーンで weight/(blendDepth+1)、深さ blendDepth で weight に到達。
    // WHY: 上半身レイヤーを Spine で切ると、Spine が 0 と 1 の境界になって腰が折れる。
    //      数階層かけて滑らかに立ち上げると継ぎ目が見えなくなる。
    int         blendDepth      = 0;

    bool operator==(const AvatarMaskEntry&) const = default;
};

// Unity Humanoid 相当の体パーツ。プリセットからエントリ一式を生成するために使う。
// WHY: 「上半身だけ」を作るのに、ボーンツリーを 1 本ずつチェックさせたくない。
enum class HumanoidBodyPart : int {
    Root = 0,   // ルート (Root Motion 用)
    Body,       // 腰〜胸
    Head,       // 首・頭
    LeftArm,    // 左肩〜手首
    RightArm,
    LeftHand,   // 左指
    RightHand,
    LeftLeg,    // 左腿〜足
    RightLeg,
    Count
};

struct AvatarMaskAsset {
    std::string name;
    // entries に一致しないボーンの既定ウェイト。
    // false (既定): 一致しないボーンは 0 = そのレイヤーが効かない (加算的にパーツを選ぶ)。
    // true        : 一致しないボーンは 1 = 全身に効かせ、entries で「除外」を書く。
    // WHY: 「上半身だけ有効」も「下半身だけ無効」も同じ構造で書けるようにする。
    bool defaultInclude = false;
    std::vector<AvatarMaskEntry> entries;
    // このマスクを作った元スケルトン。Editor がボーンツリーを再表示するために覚えておく。
    // ランタイムの評価には使わない。
    std::string skeletonSourcePath;

    bool operator==(const AvatarMaskAsset&) const = default;
};

[[nodiscard]] bool LoadAvatarMaskAsset(const std::string& path, AvatarMaskAsset& outAsset);
[[nodiscard]] bool SaveAvatarMaskAsset(const std::string& path, const AvatarMaskAsset& asset);

// bonePath / boneName に対する最終ウェイト 0..1 を返す。
// 複数エントリが一致する場合は「より深い (より具体的な) エントリ」が勝つ。
// WHY: 「腕全体 0 → 手だけ 1」のような上書き指定を、記述順に依存せず自然に書けるようにする。
[[nodiscard]] float EvaluateAvatarMaskWeight(
    const AvatarMaskAsset& mask, std::string_view bonePath, std::string_view boneName);

// 体パーツの表示名 ("Left Arm" 等)。Editor UI とログに使う。
[[nodiscard]] const char* HumanoidBodyPartName(HumanoidBodyPart part);

// 体パーツに属するボーンを見分けるための小文字トークン列を返す。
// WHY: FBX のボーン命名は DCC ごとにばらつく (LeftArm / L_Arm / mixamorig:LeftArm)。
//      完全一致ではなく「小文字化した名前がこのトークンを含むか」で判定する。
[[nodiscard]] const std::vector<std::string>& HumanoidBonePatterns(HumanoidBodyPart part);

// ボーン名がその体パーツに属しそうか推定する。Editor のプリセット生成に使う。
[[nodiscard]] bool BoneNameMatchesBodyPart(std::string_view boneName, HumanoidBodyPart part);

// ボーン名からもっとも当てはまる体パーツを推定する。判定できなければ Count を返す。
[[nodiscard]] HumanoidBodyPart GuessBodyPartForBone(std::string_view boneName);

} // namespace fbzz::asset
