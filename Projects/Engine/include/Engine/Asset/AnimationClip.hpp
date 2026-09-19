/// @file    AnimationClip.hpp
/// @brief   .anim バイナリのランタイム表現。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// AnimatorSystem がサンプリングする読み取り中心のデータ定義。
/// 再生中の時刻やブレンド状態は AnimatorComponent 側に置き、この型には持たせない。
#pragma once
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
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
    double time = 0.0;
    float value = 0.0f;
    float inTangent = 0.0f;
    float outTangent = 0.0f;
};

struct IntKey {
    double time = 0.0;
    int32_t value = 0;
};

struct BoolKey {
    double time = 0.0;
    bool value = false;
};

struct Vector2Key {
    double time = 0.0;
    math::Vector2 value = math::Vector2::ZERO;
};

struct VectorKey {
    double time = 0.0;
    math::Vector3 value = math::Vector3::ZERO;
};

struct QuaternionKey {
    double time = 0.0;
    math::Quaternion value = math::Quaternion::Identity();
};

struct Vector4Key {
    double time = 0.0;
    math::Vector4 value{};
};

struct NodeAnimationTrack {
    /// @brief Animator 所有 GameObject からの相対 Path。
    std::string               targetPath;
    std::string               nodeName;
    AnimInterp                interp = AnimInterp::Linear;
    std::vector<VectorKey>    positions;
    std::vector<QuaternionKey> rotations;
    std::vector<VectorKey>    scales;
};

/// @brief Component / Material / Morph を同じ時間軸から操作する型付き Track。
/// @note Component は componentType+propertyName、Material は materialSlot+propertyName、Morph は meshIndex+propertyName (Morph 名) で解決する。
struct PropertyAnimationTrack {
    AnimTargetType targetType = AnimTargetType::ComponentProperty;
    AnimValueType  valueType = AnimValueType::Float;
    AnimInterp     interp = AnimInterp::Linear;
    std::string    targetPath;
    std::string    componentType;
    std::string    propertyName;
    int32_t        materialSlot = 0;
    int32_t        meshIndex = 0;
    std::vector<FloatKey>   floatKeys;
    std::vector<Vector2Key> vector2Keys;
    std::vector<VectorKey>  vector3Keys;
    std::vector<Vector4Key> vector4Keys;
    std::vector<IntKey>     intKeys;
    std::vector<BoolKey>    boolKeys;
};

/// @brief .anim ファイル内に埋め込まれたアニメーションイベント。
struct AnimEvent {
    double      time       = 0.0;
    std::string name;
    int32_t     intParam   = 0;
    float       floatParam = 0.0f;
};

struct AnimationClip {
    std::string name;
    /// @brief .anim v3 の正規化済み再生長。AnimatorSystem はこの値だけを遷移時間に使う。
    double durationSeconds = 0.0;
    /// @brief キー時刻を tick 空間でサンプリングするための補助値 (durationTicks / ticksPerSecond の組)。
    /// @note key.time は exporter 元の tick 単位を保持するため、秒→tick 変換係数として必要。
    double durationTicks  = 0.0;
    double ticksPerSecond = 30.0;
    float  frameRate       = 30.0f;
    bool   loop            = false;
    bool   hasRootMotion   = false;
    uint32_t rootMotionTrackIndex = UINT32_MAX;
    /// @brief Root Motion の水平移動・高さ・回転を個別に適用できる。
    bool rootMotionApplyXZ = true;
    bool rootMotionApplyY = false;
    bool rootMotionApplyRotation = true;
    /// @brief インポート時に採用したルートモーションノード名。空なら未指定 (インデックスのみ)。
    /// @note トラック配列の組み替えに強い追跡キー。Inspector 提示とランタイムの名前指定オーバーライド突き合わせに使う。
    std::string rootMotionNodeName;

    std::vector<NodeAnimationTrack> tracks;
    std::vector<PropertyAnimationTrack> propertyTracks;
    std::vector<AnimEvent>          events;

    /// @brief Import 時の最適化結果。ランタイムでは展開済み Key を二分探索して評価する (以下 4 フィールドが一群)。
    bool optimized = false;
    float positionError = 0.0001f;
    float rotationErrorDegrees = 0.05f;
    float scaleError = 0.0001f;

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
    if (const size_t helperPos = name.find(helper); helperPos != std::string::npos)
        name = name.substr(0, helperPos);

    if (const size_t pathPos = name.find_last_of("/|"); pathPos != std::string::npos)
        name = name.substr(pathPos + 1);

    if (const size_t namespacePos = name.find_last_of(':'); namespacePos != std::string::npos)
        name = name.substr(namespacePos + 1);

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
        if (c == '_' || c == '-' || c == ' ' || c == '.') continue;
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
    if (key.empty()) return RootMotionNameTier::None;

    /// 専用ノード。DCC 側で明示的に用意されたもの。
    static constexpr std::string_view explicitNames[] = {
        "rootmotion", "motionroot", "rootmotionnode", "fbzzrootmotion", "trajectory",
    };
    for (const auto& candidate : explicitNames)
        if (key == candidate) return RootMotionNameTier::Explicit;

    /// スケルトンのルート相当。Mixamo / Blender / 3ds Max Biped / UE の一般的な命名。
    static constexpr std::string_view skeletalNames[] = {
        "root", "reference", "armature", "skeleton", "cog",
        "hips", "hip", "pelvis", "bip01", "bip001", "bip01pelvis",
    };
    for (const auto& candidate : skeletalNames)
        if (key == candidate) return RootMotionNameTier::Skeletal;

    return RootMotionNameTier::None;
}

/// @brief 名前を明示指定してトラックを引く。完全一致 → 正規化一致の順に探す。
/// @return 見つからなければ UINT32_MAX。
inline uint32_t FindRootMotionTrackIndexByName(const AnimationClip& clip,
                                               std::string_view nodeName)
{
    if (nodeName.empty()) return UINT32_MAX;
    for (size_t i = 0; i < clip.tracks.size(); ++i)
        if (clip.tracks[i].nodeName == nodeName) return static_cast<uint32_t>(i);

    const std::string key = RootMotionNameKey(nodeName);
    for (size_t i = 0; i < clip.tracks.size(); ++i)
        if (RootMotionNameKey(clip.tracks[i].nodeName) == key) return static_cast<uint32_t>(i);

    return UINT32_MAX;
}

/// @brief クリップ内から最も確度の高いルートモーショントラックを推定する。
/// @param skeletonRootName 骨階層のルートノード名。候補名に一致しない独自命名のリグでもこれで拾える。
/// @param allowSkeletalTier false なら専用ノード (Explicit) のみを採用する。
inline uint32_t AutoDetectRootMotionTrackIndex(const AnimationClip& clip,
                                               std::string_view skeletonRootName = {},
                                               bool allowSkeletalTier = true)
{
    uint32_t best = UINT32_MAX;
    RootMotionNameTier bestTier = RootMotionNameTier::None;
    for (size_t i = 0; i < clip.tracks.size(); ++i) {
        const RootMotionNameTier tier = ClassifyRootMotionNodeName(clip.tracks[i].nodeName);
        if (tier == RootMotionNameTier::None) continue;
        if (!allowSkeletalTier && tier != RootMotionNameTier::Explicit) continue;
        if (static_cast<int>(tier) <= static_cast<int>(bestTier)) continue;
        bestTier = tier;
        best = static_cast<uint32_t>(i);
    }
    if (best != UINT32_MAX) return best;

    /// @note 候補名に当たらないリグは、スケルトンのルートノードと同名のトラックを採用する。
    if (allowSkeletalTier && !skeletonRootName.empty())
        return FindRootMotionTrackIndexByName(clip, skeletonRootName);

    return UINT32_MAX;
}

/// @}

} // namespace fbzz::asset
