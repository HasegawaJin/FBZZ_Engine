// FBZZ Engine
// AnimationClip.hpp | fbzz::asset
// .anim バイナリのランタイム表現
// AnimatorSystem がサンプリングする読み取り中心のデータ定義。
// 再生中の時刻やブレンド状態は AnimatorComponent 側に置き、この型には持たせない。
#pragma once
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

enum class AnimInterp : uint8_t { Step = 0, Linear = 1, Cubic = 2 };

struct VectorKey {
    double time = 0.0;
    math::Vector3 value = math::Vector3::ZERO;
};

struct QuaternionKey {
    double time = 0.0;
    math::Quaternion value = math::Quaternion::Identity();
};

struct NodeAnimationTrack {
    std::string               nodeName;
    AnimInterp                interp = AnimInterp::Linear;
    std::vector<VectorKey>    positions;
    std::vector<QuaternionKey> rotations;
    std::vector<VectorKey>    scales;
};

// .anim ファイル内に埋め込まれたアニメーションイベント
struct AnimEvent {
    double      time       = 0.0;
    std::string name;
    int32_t     intParam   = 0;
    float       floatParam = 0.0f;
};

struct AnimationClip {
    std::string name;
    // durationTicks / ticksPerSecond: .fbx/.asset 経由ロード時の旧フィールド（後方互換）
    double durationTicks  = 0.0;
    double ticksPerSecond = 30.0;
    // durationSeconds: .anim バイナリローダーが直接設定する（優先）
    double durationSeconds = -1.0; // -1 = 未設定（durationTicks/ticksPerSecond から計算）
    float  frameRate       = 30.0f;
    bool   loop            = false;
    bool   hasRootMotion   = false;
    uint32_t rootMotionTrackIndex = UINT32_MAX;

    std::vector<NodeAnimationTrack> tracks;
    std::vector<AnimEvent>          events;

    // AnimatorSystem 用: 実際の再生秒数を返す
    double GetDurationSeconds() const {
        if (durationSeconds >= 0.0) return durationSeconds;
        const double tps = ticksPerSecond > 0.0 ? ticksPerSecond : 30.0;
        return durationTicks / tps;
    }
};

} // namespace fbzz::asset
