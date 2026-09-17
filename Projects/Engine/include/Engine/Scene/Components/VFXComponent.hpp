/// @file    VFXComponent.hpp
/// @brief   `.vfx` プレハブのルートに載る再生ヘッド。配下の VFXElement へ時刻を配る。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 旧 VFXGraphComponent は graphPath から DAG を読みノードごとに GameObject を生成していたが、
///       生成物は普通のコンポーネントなので階層をアセット化すれば生成段が不要になる。時刻を進めて
///       配下へ配るだけで、何が置かれているかは知らない。
/// @see Docs/design/vfx-prefab.md
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class Scene;
class GameObject;

struct VFXComponent {
    bool  enabled     = true;
    bool  playOnAwake = true;
    bool  loop        = false;
    float speed       = 1.0f;

    /// 全体の尺 [秒]。0 以下なら配下から自動算出する (全要素の終了時刻の最大値)。
    float duration = 0.0f;

    /// 再生し終えたら GameObject を片付ける。
    /// プール由来 (pooled) ならプールへ返し、そうでなければ Destroy する。
    bool autoDestroy = true;

    /// @name ランタイム (Reflect しない = シーン/プレハブへ保存されない)
    /// @{

    bool  playing     = false;
    bool  initialized = false;
    float time        = 0.0f;
    /// duration が 0 以下のときに配下から算出した実効尺。
    float resolvedDuration = 0.0f;
    /// 配下にループする要素があり、時間では終わらない。autoDestroy は効かない。
    bool endless = false;
    bool  restartRequested = false;
    /// Trigger() で積まれ、次の更新で同名の VFXElement を開始させる。
    std::vector<std::string> pendingTriggers;
    /// エディタのスクラブ用。0 以上なら time をこの値へ固定して評価する。
    float editorScrubTime = -1.0f;
    /// 上の値が書かれたフレーム。
    /// @note 鮮度判定用。書き込みが途絶えたら通常再生へ戻す (スクラブパネルを閉じても VFX が
    ///       固定されたままにならないよう、ParticleEmitter の editorTimeScaleFrame と同じ考え方)。
    std::uint64_t editorScrubFrame = 0;
    /// 入れ子 VFX の再帰ガード。ルートは 0、子ルートは親 + 1。
    int nestingDepth = 0;

    const char* GetTypeName() const { return "VFX"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.FloatRange("speed", speed, 0.0f, 8.0f);
        r.Field("duration", duration);
        r.Field("autoDestroy", autoDestroy);
    }

    void Play()    { playing = true; }
    void Pause()   { playing = false; }
    void Resume()  { playing = true; }
    void Stop()    { playing = false; time = 0.0f; restartRequested = true; }

    /// 頭出しして再生する。プールから取り出した実体を鳴らすときの入口。
    void Restart()
    {
        playing = true;
        time = 0.0f;
        restartRequested = true;
    }

    /// trigger 名を持つ VFXElement を開始させる。
    void Trigger(std::string_view name) { pendingTriggers.emplace_back(name); }
    /// @}
};

/// `.vfx` を 1 発鳴らす。PrefabPool から取り出して位置を合わせ、頭出しまで行う。
/// @note プールの実体は前回の再生状態のまま非アクティブなだけで、Spawn しただけでは鳴らない。
///       「取り出す→起こす→頭出し」を呼び出し側に書かせると必ずどれかが抜ける。
[[nodiscard]] GameObject* SpawnVFX(Scene& scene,
                                   const std::string& vfxPath,
                                   math::Vector3 position,
                                   math::Quaternion rotation);

} // namespace fbzz::scene
