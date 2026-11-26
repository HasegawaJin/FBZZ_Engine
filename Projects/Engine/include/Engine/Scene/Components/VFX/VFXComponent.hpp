/// @file    VFXComponent.hpp
/// @brief   `.vfx` プレハブのルートに載る再生ヘッド。配下の VFXElement へ時刻を配る。
/// @author  Hasegawa Jin
/// @date    2026-08-26

/// @note 旧 VFXGraphComponent は graphPath から DAG を読みノードごとに GameObject を生成していたが、
/// @note 生成物は普通のコンポーネントなので階層をアセット化すれば生成段が不要になる。時刻を進めて
/// @note 配下へ配るだけで、何が置かれているかは知らない。
/// @see Docs/design/vfx-prefab.md
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class Scene;
class GameObject;

struct VFXComponent {
    bool  enabled     = true; ///< この VFX インスタンスの再生と配下の効果の更新を有効にする。
    bool  playOnAwake = true; ///< 初回の設定反映時に再生を自動で開始する。
    bool  loop        = false; ///< 再生区間を終えたら配下のエフェクトを頭出しして繰り返す。
    float speed       = 1.0f; ///< 実際の経過時間に掛けるこの VFX の再生速度倍率。

    float duration = 0.0f; ///< 全体の尺 [秒]。0 以下なら配下から自動算出する (全要素の終了時刻の最大値)。

    /// @note 再生し終えたら GameObject を片付ける。
    /// @note プール由来 (pooled) ならプールへ返し、そうでなければ Destroy する。
    bool autoDestroy = true;

    /// @name ランタイム (Reflect しない = シーン/プレハブへ保存されない)
    /// @{

    bool  playing     = false; ///< この VFX が現在時刻を進めて再生中か。
    bool  initialized = false; ///< このインスタンスの最初の状態を子のエフェクトへ反映済み。
    float time        = 0.0f; ///< ルート VFX の再生位置（秒）。配下の生存窓の基準時刻。
    float resolvedDuration = 0.0f; ///< duration が 0 以下のときに配下から算出した実効尺。
    bool endless = false; ///< 配下にループする要素があり、時間では終わらない。autoDestroy は効かない。
    bool  restartRequested = false; ///< 次の更新でルート時刻と配下の再生状態を先頭に戻す。
    std::vector<std::string> pendingTriggers; ///< Trigger() で積まれ、次の更新で同名の VFXElement を開始させる。
    float editorScrubTime = -1.0f; ///< エディタのスクラブ用。0 以上なら time をこの値へ固定して評価する。
    /// @note 上の値が書かれたフレーム。
    /// @note 鮮度判定用。書き込みが途絶えたら通常再生へ戻す (スクラブパネルを閉じても VFX が
    /// @note 固定されたままにならないよう、ParticleEmitter の editorTimeScaleFrame と同じ考え方)。
    std::uint64_t editorScrubFrame = 0;
    int nestingDepth = 0; ///< 入れ子 VFX の再帰ガード。ルートは 0、子ルートは親 + 1。

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

    /// @note 頭出しして再生する。プールから取り出した実体を鳴らすときの入口。
    void Restart()
    {
        playing = true;
        time = 0.0f;
        restartRequested = true;
    }

    /// @note trigger 名を持つ VFXElement を開始させる。
    void Trigger(std::string_view name) { pendingTriggers.emplace_back(name); }
    /// @}
};

/// @note `.vfx` を 1 発鳴らす。PrefabPool から取り出して位置を合わせ、頭出しまで行う。
/// @note プールの実体は前回の再生状態のまま非アクティブなだけで、Spawn しただけでは鳴らない。
/// @note 「取り出す→起こす→頭出し」を呼び出し側に書かせると必ずどれかが抜ける。
[[nodiscard]] FBZZ_ENGINE_API GameObject* SpawnVFX(Scene& scene,
                                   const std::string& vfxPath,
                                   math::Vector3 position,
                                   math::Quaternion rotation);

}
