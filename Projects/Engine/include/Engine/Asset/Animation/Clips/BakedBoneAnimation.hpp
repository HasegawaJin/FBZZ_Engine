/// @file    BakedBoneAnimation.hpp
/// @brief   検証付きの骨格クリップのベイク、個体ごとの再生時刻と CPU 代替パレット。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Api.hpp>

#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::asset {
struct AnimationClip;
struct Skeleton;

enum class BoneBakeStatus : uint8_t {
    OK, INVALID_SKELETON, INVALID_CLIP, UNSUPPORTED_TRACK,
    UNSUPPORTED_SIDE_EFFECT, INVALID_SETTINGS, CAPACITY_EXCEEDED,
    INVALID_DATA, INCOMPATIBLE_SKELETON, INCOMPATIBLE_LOD, STALE_SOURCE
};

/// @note BakedBoneReplay.cs.hlsl とレイアウトを共有する。初回の比較に量子化が影響しないよう 32 ビット浮動小数点を使う。
struct BakedBoneTransform {
    math::Vector4 translation{0.0f, 0.0f, 0.0f, 0.0f}; ///< ベイクしたノードの親基準位置。xyz を使用する。
    math::Vector4 rotation{0.0f, 0.0f, 0.0f, 1.0f}; ///< ベイクしたノードの親基準回転。xyzw 順のクォータニオン。
    math::Vector4 scale{1.0f, 1.0f, 1.0f, 0.0f}; ///< ベイクしたノードの親基準の軸別倍率。xyz を使用する。
};
static_assert(sizeof(BakedBoneTransform) == 48);

struct BakedBoneNode {
    int32_t parent = -1; ///< ベイク済み nodes 内の親ノードの添字。-1 はルート。
    int32_t bone = -1; ///< 対応する bones の添字。-1 はスキニング出力のないノード。
    uint32_t padding[2]{}; ///< GPU と共有する構造体のバイト配置を揃える予約領域。
};
static_assert(sizeof(BakedBoneNode) == 16);

struct BakedBoneBinding {
    math::Matrix4 offset = math::Matrix4::Identity(); ///< バインド時にモデル空間から対応する骨空間へ変換する行列。
    uint32_t node = 0; ///< この出力骨に対応するベイク済み nodes の添字。
    uint32_t padding[3]{}; ///< GPU と共有する構造体のバイト配置を揃える予約領域。
};
static_assert(sizeof(BakedBoneBinding) == 80);

/// @note 両端点を保持するため、最終区間で最後の内部サンプルからフレーム 0 へ直接補間しない。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/AnimToTexture/UAnimToTextureDataAsset ボーンアニメーションのデータとサンプルレート。
struct BakedBoneAnimation {
    static constexpr uint32_t FORMAT_VERSION = 1; ///< ベイククリップを読み書きするときに照合する形式の版。
    uint64_t skeletonFingerprint = 0; ///< ベイク元の階層・バインド姿勢・逆バインドを検証する内容ハッシュ。
    uint64_t clipFingerprint = 0; ///< ベイク元のクリップ内容と再生条件を検証するハッシュ。
    double durationSeconds = 0.0; ///< クリップの先頭から末尾サンプルまでの時間 [s]。
    float sampleRate = 60.0f; ///< ベイクに用いる 1 秒あたりの姿勢サンプル数。
    float sourceMeshRadius = 0.0f; ///< ベイク時の変形前頂点を収めるモデル空間原点からの最大半径 [m]。
    float conservativeRadius = 0.0f; ///< 全ベイク姿勢を覆うカリング用の保守的な半径 [m]。
    uint32_t frameCount = 0; ///< 先頭と末尾を含む姿勢サンプルフレーム数。
    uint32_t root = 0; ///< ベイク済み nodes 内のルートノードの添字。
    bool loop = false; ///< クリップ区間外の再生時刻を折り返すか。
    math::Matrix4 rootInverse = math::Matrix4::Identity(); ///< 評価した骨格変換をモデル側の基準へ戻すルートの逆変換。
    std::vector<BakedBoneNode> nodes; ///< 親ノードと出力骨の対応を保持するベイク済み骨格。
    std::vector<BakedBoneBinding> bones; ///< 出力パレットの各骨が参照するノードと逆バインドの列。
    std::vector<BakedBoneTransform> samples; ///< フレームごとに全ノード分を並べたローカル TRS の列。
};

/// @note 各行の時刻は個体ごとに持つ。不変サンプルと骨格の対応付けは種別とクリップの組で共有する。
struct BakedBoneFrame {
    uint32_t first = 0; ///< 再生時刻を挟む 2 フレームのうち、手前のサンプル番号。
    uint32_t second = 0; ///< 再生時刻を挟む 2 フレームのうち、奥のサンプル番号。
    float alpha = 0.0f; ///< first から second へ補間する割合 [0, 1]。
    uint32_t padding = 0; ///< GPU と共有する構造体のバイト配置を揃える予約領域。
};
static_assert(sizeof(BakedBoneFrame) == 16);

struct BoneBakeSettings {
    float sampleRate = 60.0f; ///< ベイクに用いる 1 秒あたりの姿勢サンプル数。
    float meshRadius = 0.0f; ///< 対象の全メッシュ LOD の入力頂点について、モデル空間原点からの距離の最大値。
    uint32_t maxFrames = 65536; ///< メモリ負荷を抑えるためベイクを受け付ける最大フレーム数。
    uint64_t maxSampleBytes = 256ull * 1024ull * 1024ull; ///< ノード姿勢の時系列サンプルに許可する最大容量 [byte]。
};

/// @note Animator ごとに一時作業領域を再利用する。評価に失敗しても公開済みの出力は変更しない。
struct BakedBoneWorkspace {
    std::vector<uint32_t> order; ///< 親の変換を先に合成するために並べたノードの添字。
    std::vector<uint8_t> visited; ///< 階層の探索状態を nodes と同じ添字で保持する作業配列。
    std::vector<math::Matrix4> globals, matrices;
};

/// @return 失敗時は出力を変更しない。未対応のトラックは通常の Animator で再生する。
FBZZ_ENGINE_API BoneBakeStatus BakeBoneAnimation(const Skeleton& skeleton, const AnimationClip& clip,
                                const BoneBakeSettings& settings, BakedBoneAnimation& output);

/// @note 内容キーは階層、バインド TRS、逆バインド行列、クリップのキー・時刻・ループ方針を含む。資産ポインターはキーに使わない。
FBZZ_ENGINE_API uint64_t BoneBakeSkeletonFingerprint(const Skeleton& skeleton);
FBZZ_ENGINE_API uint64_t BoneBakeClipFingerprint(const AnimationClip& clip);

FBZZ_ENGINE_API BoneBakeStatus ValidateBakedBoneAnimation(const BakedBoneAnimation& bake);
FBZZ_ENGINE_API BoneBakeStatus ValidateBoneBakeSource(const BakedBoneAnimation& bake, const Skeleton& skeleton,
                                    const AnimationClip& clip, float lodMeshRadius);

/// @note 2 クリップの境界は各ノードの最大値を合成してから階層へ伝播させる。ブレンドした階層を max(radiusA, radiusB) では包めない。
FBZZ_ENGINE_API BoneBakeStatus BoneBakeBlendRadius(const BakedBoneAnimation& bake, const BakedBoneAnimation& target,
                                  float& output);

/// @param[in] elapsedSeconds 個体ごとの倍率適用前の経過秒。位相はクリップ時間の秒単位で、速度倍率の適用後に加える。
/// @return 非有限値の時刻は拒否する。有限値はループなら正負両方向に折り返し、非ループなら範囲内に丸める。
FBZZ_ENGINE_API BoneBakeStatus ResolveBakedBoneFrame(const BakedBoneAnimation& bake, double elapsedSeconds,
                                   double speed, double phaseSeconds, bool loop,
                                   BakedBoneFrame& output);

/// @note 階層を合成する前に、最短弧の Slerp を使ってローカル TRS を評価する。行列は線形補間しない。
/// @note 任意のブレンド先は GPU カーネルと同じ骨格とローカルの 2 クリップブレンドを使う。失敗時は出力を変更しない。
FBZZ_ENGINE_API BoneBakeStatus EvaluateBakedBoneAnimation(const BakedBoneAnimation& bake, const BakedBoneFrame& frame,
                                        std::vector<math::Matrix4>& palette,
                                        std::vector<math::Matrix4>& nodeGlobals,
                                        const BakedBoneAnimation* target = nullptr,
                                        const BakedBoneFrame* targetFrame = nullptr,
                                        float blendWeight = 0.0f,
                                        BakedBoneWorkspace* workspace = nullptr);

/// @note .bba は FBX から派生したキャッシュ。リトルエンディアンで、版番号・長さ・チェックサムを検証する。
/// @see https://github.com/microsoft/DirectXTex/wiki/DirectXTex DDS は浮動小数点画像を格納できる。構造化アセットでは骨格やキャッシュのメタデータも保持する。
FBZZ_ENGINE_API BoneBakeStatus EncodeBoneBake(const BakedBoneAnimation& bake, std::vector<std::byte>& output);
FBZZ_ENGINE_API BoneBakeStatus DecodeBoneBake(std::span<const std::byte> bytes, BakedBoneAnimation& output);
}
