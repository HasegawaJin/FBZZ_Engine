/// @file    AnimationClip.hpp
/// @brief   .anim バイナリのランタイム表現。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// AnimatorSystem がサンプリングする読み取り中心のデータ定義。
/// 再生中の時刻やブレンド状態は AnimatorComponent 側に置き、この型には持たせない。
#pragma once
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

enum class AnimInterp : uint8_t { Step = 0, Linear = 1, Cubic = 2 };

/// @brief AnimationClip が書き換える値の型。
/// @note DLL 境界や Component 実装型を Clip に露出せず、型安全な Property Binding を構築する。
enum class AnimValueType : uint8_t {
    Float = 0,
    Vector2,
    Vector3,
    Vector4,
    Color,
    Int,
    Bool
};

/// @brief Property Track の適用先。Component と Material は同じ値 Track を共有する。
enum class AnimTargetType : uint8_t {
    ComponentProperty = 0,
    MaterialProperty,
    MorphWeight
};

struct FloatKey {
    double time = 0.0; ///< キーを評価するトラック上の時刻。AnimationClip では tick、Sequence では秒。
    float value = 0.0f; ///< この時刻に記録したキーフレームの値。
    float inTangent = 0.0f; ///< Cubic 補間で手前の区間からこのキーへ入る傾き。
    float outTangent = 0.0f; ///< Cubic 補間でこのキーから次の区間へ出る傾き。
};

struct IntKey {
    double time = 0.0; ///< キーを評価するトラック上の時刻。AnimationClip では tick、Sequence では秒。
    int32_t value = 0; ///< この時刻に記録したキーフレームの値。
};

struct BoolKey {
    double time = 0.0; ///< キーを評価するトラック上の時刻。AnimationClip では tick、Sequence では秒。
    bool value = false; ///< この時刻に記録したキーフレームの値。
};

struct Vector2Key {
    double time = 0.0; ///< キーを評価するトラック上の時刻。AnimationClip では tick、Sequence では秒。
    math::Vector2 value = math::Vector2::ZERO; ///< この時刻に記録したキーフレームの値。
};

struct VectorKey {
    double time = 0.0; ///< キーを評価するトラック上の時刻。AnimationClip では tick、Sequence では秒。
    math::Vector3 value = math::Vector3::ZERO; ///< この時刻に記録したキーフレームの値。
};

struct QuaternionKey {
    double time = 0.0; ///< キーを評価するトラック上の時刻。AnimationClip では tick、Sequence では秒。
    math::Quaternion value = math::Quaternion::Identity(); ///< この時刻に記録したキーフレームの値。
};

struct Vector4Key {
    double time = 0.0; ///< キーを評価するトラック上の時刻。AnimationClip では tick、Sequence では秒。
    math::Vector4 value{}; ///< この時刻に記録したキーフレームの値。
};

struct NodeAnimationTrack {
    std::string               targetPath; ///< Animator 所有 GameObject からの相対 Path。
    std::string               nodeName; ///< トラックが駆動する骨格ノードの名前。
    AnimInterp                interp = AnimInterp::Linear; ///< 前後のキー間で値を補う補間方式。
    std::vector<VectorKey>    positions; ///< 対象ノードのローカル位置キー。時刻順に格納する。
    std::vector<QuaternionKey> rotations; ///< 対象ノードのローカル回転キー。時刻順に格納する。
    std::vector<VectorKey>    scales; ///< 対象ノードの軸別ローカル倍率キー。時刻順に格納する。
};

/// @brief Component / Material / Morph を同じ時間軸から操作する型付き Track。
/// @note Component は componentType+propertyName、Material は materialSlot+propertyName、Morph は meshIndex+propertyName (Morph 名) で解決する。
struct PropertyAnimationTrack {
    AnimTargetType targetType = AnimTargetType::ComponentProperty; ///< トラックが書き込むコンポーネント・材質・モーフの分類。
    AnimValueType  valueType = AnimValueType::Float; ///< サンプリング時に参照するキー配列と適用先の値型。
    AnimInterp     interp = AnimInterp::Linear; ///< 前後のキー間で値を補う補間方式。
    std::string    targetPath; ///< Animator 所有個体から書き込み先を解決する相対階層パス。
    std::string    componentType; ///< コンポーネントに書き込むときに解決する登録型名。
    std::string    propertyName; ///< 変更するフィールド、材質変数、またはモーフの名前。
    int32_t        materialSlot = 0; ///< 材質を駆動する場合の対象メッシュ内の材質スロット番号。
    int32_t        meshIndex = 0; ///< モーフを駆動するときに対象とするメッシュの番号。
    std::vector<FloatKey>   floatKeys; ///< スカラー浮動小数点型の時系列キー。
    std::vector<Vector2Key> vector2Keys; ///< 2 成分ベクトル型の時系列キー。
    std::vector<VectorKey>  vector3Keys; ///< 3 成分ベクトル型の時系列キー。
    std::vector<Vector4Key> vector4Keys; ///< 4 成分ベクトル・色型の時系列キー。
    std::vector<IntKey>     intKeys; ///< 整数型の時系列キー。
    std::vector<BoolKey>    boolKeys; ///< 真偽値型の時系列キー。
};

/// @brief .anim ファイル内に埋め込まれたアニメーションイベント。
struct AnimEvent {
    double      time       = 0.0; ///< イベントを発行するクリップ上の時刻 [tick]。
    std::string name; ///< イベント配送時にリスナーを照合する名前。
    int32_t     intParam   = 0; ///< イベントと一緒に配送する整数引数。
    float       floatParam = 0.0f; ///< イベントと一緒に配送する浮動小数点引数。
};

struct AnimationClip {
    std::string name; ///< ファイルやモデル内でクリップを識別する名前。
    double durationSeconds = 0.0; ///< .anim v3 の正規化済み再生長。AnimatorSystem はこの値だけを遷移時間に使う。
    /// @brief キー時刻を tick 空間でサンプリングするための補助値 (durationTicks / ticksPerSecond の組)。
    /// @note key.time は exporter 元の tick 単位を保持するため、秒→tick 変換係数として必要。
    double durationTicks  = 0.0; ///< キーが使う tick 時間軸での再生区間の長さ。
    double ticksPerSecond = 30.0; ///< 経過秒をキーの時刻へ変換する 1 秒あたりの tick 数。
    float  frameRate       = 30.0f; ///< 制作・表示で使うクリップのフレームレート [frame/s]。
    bool   loop            = false; ///< 再生が末尾に達したときにクリップ冒頭へ戻すか。
    bool   hasRootMotion   = false; ///< ルートノードから個体の移動・回転を取り出す設定があるか。
    uint32_t rootMotionTrackIndex = UINT32_MAX; ///< ルートモーションに使う tracks の添字。UINT32_MAX は未解決。
    bool rootMotionApplyXZ = true; ///< Root Motion の水平移動・高さ・回転を個別に適用できる。
    bool rootMotionApplyY = false; ///< ルートモーションの高さ方向を個体へ反映するか。
    bool rootMotionApplyRotation = true; ///< ルートモーションの回転を個体へ反映するか。
    /// @brief インポート時に採用したルートモーションノード名。空なら未指定 (インデックスのみ)。
    /// @note トラック配列の組み替えに強い追跡キー。Inspector 提示とランタイムの名前指定オーバーライド突き合わせに使う。
    std::string rootMotionNodeName; ///< ルートモーションの元ノード名。トラックの配列順が変わった際にも照合に使う。

    std::vector<NodeAnimationTrack> tracks; ///< 骨格・ノードの位置、回転、倍率を駆動するトラック列。
    std::vector<PropertyAnimationTrack> propertyTracks; ///< コンポーネント・材質・モーフを駆動するトラック列。
    std::vector<AnimEvent>          events; ///< 再生区間を横切ったときに発行するイベント列。

    bool optimized = false; ///< Import 時の最適化結果。ランタイムでは展開済み Key を二分探索して評価する (以下 4 フィールドが一群)。
    float positionError = 0.0001f; ///< キー削減時に許容する位置誤差 [m]。
    float rotationErrorDegrees = 0.05f; ///< キー削減時に許容する姿勢差の角度 [deg]。
    float scaleError = 0.0001f; ///< キー削減時に許容する倍率の誤差。

    /// @brief 最新仕様の正規化済み再生秒数を返す。
    double GetDurationSeconds() const {
        return durationSeconds;
    }
};

/// @name ルートモーションノード名の解決
/// @note 候補リスト + 段階付けで判定する。Editor インポーターと Engine ランタイムで同じ規則を共有する。
/// @{

/// @brief FBX チャンネル名の揺れ (namespace・パス区切り・Assimp の補助ノード suffix) を吸収する。
/// @note 例: `"Armature|mixamorig:Hips_$AssimpFbx$_Translation"` → `"Hips"`。
inline std::string CanonicalNodeName(std::string_view rawName)
{
    std::string name(rawName);
    std::replace(name.begin(), name.end(), '\\', '/');

    static constexpr std::string_view helper = "_$AssimpFbx$_";
    if (const size_t helperPos = name.find(helper); helperPos != std::string::npos) {
        name = name.substr(0, helperPos);
    }

    if (const size_t pathPos = name.find_last_of("/|"); pathPos != std::string::npos) {
        name = name.substr(pathPos + 1);
    }

    if (const size_t namespacePos = name.find_last_of(':'); namespacePos != std::string::npos) {
        name = name.substr(namespacePos + 1);
    }

    return name;
}

/// @brief CanonicalNodeName の結果を小文字化し、区切り記号を除いた比較用キーを返す。
/// @note `"Root_Motion"` / `"root motion"` / `"RootMotion"` を同一視するために使う。
inline std::string RootMotionNameKey(std::string_view rawName)
{
    std::string name = CanonicalNodeName(rawName);
    std::string key;
    key.reserve(name.size());
    for (const char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            continue;
        }
        key.push_back(static_cast<char>(std::tolower(uc)));
    }
    return key;
}

/// @brief ルートモーション候補としての確度。数値が大きいほど確実。
enum class RootMotionNameTier : int {
    None     = 0,
    /// 専用ノードであることが明確な名前。
    Explicit = 2,
    /// スケルトンのルート相当としてよく使われる名前。自動検出でのみ採用する。
    Skeletal = 1,
};

inline RootMotionNameTier ClassifyRootMotionNodeName(std::string_view rawName)
{
    const std::string key = RootMotionNameKey(rawName);
    if (key.empty()) {
        return RootMotionNameTier::None;
    }

    // 専用ノード。DCC 側で明示的に用意されたもの。
    static constexpr std::string_view explicitNames[] = {
        "rootmotion", "motionroot", "rootmotionnode", "fbzzrootmotion", "trajectory",
    };
    for (const auto& candidate : explicitNames)
        if (key == candidate) {
            return RootMotionNameTier::Explicit;
        }

    // スケルトンのルート相当。Mixamo / Blender / 3ds Max Biped / UE の一般的な命名。
    static constexpr std::string_view skeletalNames[] = {
        "root", "reference", "armature", "skeleton", "cog",
        "hips", "hip", "pelvis", "bip01", "bip001", "bip01pelvis",
    };
    for (const auto& candidate : skeletalNames)
        if (key == candidate) {
            return RootMotionNameTier::Skeletal;
        }

    return RootMotionNameTier::None;
}

/// @brief 名前を明示指定してトラックを引く。完全一致 → 正規化一致の順に探す。
/// @return 見つからなければ UINT32_MAX。
inline uint32_t FindRootMotionTrackIndexByName(const AnimationClip& clip,
                                               std::string_view nodeName)
{
    if (nodeName.empty()) {
        return UINT32_MAX;
    }
    for (size_t i = 0; i < clip.tracks.size(); ++i)
        if (clip.tracks[i].nodeName == nodeName) {
            return static_cast<uint32_t>(i);
        }

    const std::string key = RootMotionNameKey(nodeName);
    for (size_t i = 0; i < clip.tracks.size(); ++i)
        if (RootMotionNameKey(clip.tracks[i].nodeName) == key) {
            return static_cast<uint32_t>(i);
        }

    return UINT32_MAX;
}

/// @brief クリップ内から最も確度の高いルートモーショントラックを推定する。
/// @param[in] skeletonRootName 骨階層のルートノード名。候補名に一致しない独自命名のリグでもこれで拾える。
/// @param[in] allowSkeletalTier false なら専用ノード (Explicit) のみを採用する。
inline uint32_t AutoDetectRootMotionTrackIndex(const AnimationClip& clip,
                                               std::string_view skeletonRootName = {},
                                               bool allowSkeletalTier = true)
{
    uint32_t best = UINT32_MAX;
    RootMotionNameTier bestTier = RootMotionNameTier::None;
    for (size_t i = 0; i < clip.tracks.size(); ++i) {
        const RootMotionNameTier tier = ClassifyRootMotionNodeName(clip.tracks[i].nodeName);
        if (tier == RootMotionNameTier::None) {
            continue;
        }
        if (!allowSkeletalTier && tier != RootMotionNameTier::Explicit) {
            continue;
        }
        if (static_cast<int>(tier) <= static_cast<int>(bestTier)) {
            continue;
        }
        bestTier = tier;
        best = static_cast<uint32_t>(i);
    }
    if (best != UINT32_MAX) {
        return best;
    }

    // 候補名に当たらないリグは、スケルトンのルートノードと同名のトラックを採用する。
    if (allowSkeletalTier && !skeletonRootName.empty()) {
        return FindRootMotionTrackIndexByName(clip, skeletonRootName);
    }

    return UINT32_MAX;
}

/// @}

} // namespace fbzz::asset
