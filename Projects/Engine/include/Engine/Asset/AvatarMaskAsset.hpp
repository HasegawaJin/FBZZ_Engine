/// @file    AvatarMaskAsset.hpp
/// @brief   アニメーションレイヤーが「どのボーンに効くか」を定義する再利用可能アセット (.mask)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// レイヤーごとに対象ボーン集合を切り替えて上半身/下半身を制御する再利用可能アセット。
/// ボーンパスごとに weight (0..1) と blendDepth (分岐点から立ち上げる階層数、腰等の継ぎ目の
/// 折れを防ぐ) を持つ (Unreal の Layered blend per bone の blend depth 相当)。
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

struct Skeleton;

/// @brief ボーン 1 本 (と、任意でその子孫) に対するマスク設定。
struct AvatarMaskEntry {
    /// @brief スケルトンノードのパス (`"Hips/Spine/Spine1"`) または末端ボーン名 (`"Spine1"`)。
    /// @note パス指定は同名ボーン骨格でも一意、名前指定は手書きが楽。AnimationClip のトラックは両形式を持ちうるため、どちらでも一致させる。
    std::string bonePath;
    /// @brief このボーンに適用する最終ウェイト。0 = このレイヤーを効かせない。
    float       weight          = 1.0f;
    /// @brief 子孫ボーンにも同じエントリを適用するか。
    bool        includeChildren = true;
    /// @brief 子孫方向へ weight を立ち上げる階層数。0 なら減衰なし (従来の 0/1 マスク相当)。
    /// @note 1 以上ならこのボーンで weight/(blendDepth+1)、深さ blendDepth で weight に到達する。Spine 等の分岐点をまたぐ継ぎ目が滑らかになる。
    int         blendDepth      = 0;

    bool operator==(const AvatarMaskEntry&) const = default;
};

/// @brief Unity Humanoid 相当の体パーツ。プリセットからエントリ一式を生成するために使う。
enum class HumanoidBodyPart : int {
    Root = 0,   ///< ルート (Root Motion 用)。
    Body,       ///< 腰〜胸。
    Head,       ///< 首・頭。
    LeftArm,    ///< 左肩〜手首。
    RightArm,
    LeftHand,   ///< 左指。
    RightHand,
    LeftLeg,    ///< 左腿〜足。
    RightLeg,
    Count
};

struct AvatarMaskAsset {
    std::string name;
    /// @brief entries に一致しないボーンの既定ウェイト。
    /// @note false (既定) は不一致ボーン=0 (加算的にパーツを選ぶ)。true は不一致ボーン=1 で entries に「除外」を書く。同じ構造で「上半身だけ有効」も「下半身だけ無効」も書ける。
    bool defaultInclude = false;
    std::vector<AvatarMaskEntry> entries;
    /// @brief このマスクを作った元スケルトン。Editor がボーンツリーを再表示するために覚えておく。
    /// @note ランタイムの評価には使わない。
    std::string skeletonSourcePath;
    /// @brief 元 FBX の更新検知用署名。空文字は旧形式または未保存状態を表す。
    std::string skeletonSourceSignature;

    bool operator==(const AvatarMaskAsset&) const = default;
};

[[nodiscard]] bool LoadAvatarMaskAsset(const std::string& path, AvatarMaskAsset& outAsset);
[[nodiscard]] bool SaveAvatarMaskAsset(const std::string& path, const AvatarMaskAsset& asset);

/// @brief bonePath / boneName に対する最終ウェイト 0..1 を返す。
/// @note 複数エントリが一致する場合はより深い (より具体的な) エントリが勝つ。記述順に依存せず「腕全体 0 → 手だけ 1」のような上書きが自然に書ける。
[[nodiscard]] float EvaluateAvatarMaskWeight(
    const AvatarMaskAsset& mask, std::string_view bonePath, std::string_view boneName);

/// @brief blendDepth によるランプ。ルート骨 (depth 0) は weight/(blendDepth+1) から始まり、depth >= blendDepth で weight に到達する。
/// @note 指定した骨自身は weight に届かない、という規則は Inspector の入力欄や .mask のテキストから読み取れないため公開する (Editor がその場でランプを表示するために使う)。
[[nodiscard]] float AvatarMaskRampedWeight(const AvatarMaskEntry& entry, int depth);

/// @brief EvaluateAvatarMaskWeight が内部で行う一致判定の結果。勝者だけでなく全候補を返す。
struct AvatarMaskMatch {
    int   entryIndex  = -1;
    int   depth       = 0;    ///< エントリのルート骨からの階層差。
    float weight      = 0.0f; ///< このエントリ単独で採用された場合の実効ウェイト。
    int   specificity = -1;   ///< 大きいほど優先。同点は先に書かれた方が勝つ。
};

/// @brief bonePath / boneName に一致するエントリを、勝つ順 (specificity 降順) に返す。Editor が「負け続けているエントリ」を見せるために使う。
/// @warning 割り当てを伴うため毎フレーム経路では使わないこと (ランタイムは EvaluateAvatarMaskWeight を使う)。
[[nodiscard]] std::vector<AvatarMaskMatch> MatchAvatarMaskEntries(
    const AvatarMaskAsset& mask, std::string_view bonePath, std::string_view boneName);

/// @brief 体パーツの表示名 (`"Left Arm"` 等)。Editor UI とログに使う。
[[nodiscard]] const char* HumanoidBodyPartName(HumanoidBodyPart part);

/// @brief 体パーツに属するボーンを見分けるための小文字トークン列を返す。
/// @note FBX のボーン命名は DCC ごとにばらつく (LeftArm / L_Arm / mixamorig:LeftArm) ため、完全一致ではなく「小文字化した名前がこのトークンを含むか」で判定する。
[[nodiscard]] const std::vector<std::string>& HumanoidBonePatterns(HumanoidBodyPart part);

/// @brief ボーン名がその体パーツに属しそうか推定する。Editor のプリセット生成に使う。
[[nodiscard]] bool BoneNameMatchesBodyPart(std::string_view boneName, HumanoidBodyPart part);

/// @brief ボーン名からもっとも当てはまる体パーツを推定する。
/// @return 判定できなければ Count。
[[nodiscard]] HumanoidBodyPart GuessBodyPartForBone(std::string_view boneName);

/// @brief SkeletonNode の親を辿って、Inspector とランタイムで共有できる完全パスを構築する。
/// @note ノード名だけでは同名ボーンを区別できず、マスクが別ボーンへ誤適用されうる。
[[nodiscard]] std::string BuildSkeletonNodePath(const Skeleton& skeleton, int nodeIndex);

/// @brief 保存前に、親ルールと同じ結果になる冗長な子ルールを取り除く。
/// @note ツリーで大量のボーンを選択しても、.mask は最小限の階層ルールとして保つ。
void CompressAvatarMaskEntries(AvatarMaskAsset& mask);

} // namespace fbzz::asset
