// FBZZ Engine
// VFXGraphComponent.hpp | fbzz::scene
// .vfxグラフの再生設定と生成済みノードのランタイム状態を保持する
#pragma once

#include <Engine/Asset/VFXParameter.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset { struct VFXGraphAsset; }

namespace fbzz::scene {

struct VFXRuntimeNodeState {
    int nodeId = 0;
    EntityID entity = EntityID::INVALID;
    float startTime = 0.0f;
    float endTime = 0.0f;
    float scheduledStartTime = 0.0f;
    float scheduledEndTime = 0.0f;
    bool active = false;
    // AnimatedMeshだけが使用する再利用キー。空なら通常どおり破棄する。
    std::string poolKey;
    float basePlaybackSpeed = 1.0f;
};

// parentNodeId で親に指定されたノードの、Transform だけを持つグループ GameObject。
// 詳細は VFXGraphComponent::runtimePivots を参照。
struct VFXRuntimePivot {
    int nodeId = 0;
    EntityID entity = EntityID::INVALID;
};

struct VFXRuntimeLinkState {
    int fromNode = 0;
    int toNode = 0;
    std::uint8_t trigger = 0;
    float delay = 0.0f;
    bool fired = false;
    std::string eventName;
};

struct VFXAnimatedMeshPoolEntry {
    std::string key;
    EntityID entity = EntityID::INVALID;
};

struct VFXGraphComponent {
    bool enabled = true;
    std::string graphPath;
    bool playOnAwake = true;
    bool loop = false;
    float speed = 1.0f;
    // 親階層のAnimatorをgraph timeから直接評価し、VFXとモデルのフレーム差を無くす。
    bool syncParentAnimator = false;
    std::string variant;
    std::vector<asset::VFXParamOverride> parameterOverrides;
    std::string serializedParameterOverrides;

    // ランタイム状態はReflectしないため、シーン保存やInspectorのUndo対象に混入しない。
    bool playing = false;
    bool stopped = false;
    bool initialized = false;
    bool reloadRequested = false;
    bool restartRequested = false;
    float playTime = 0.0f;
    float graphDuration = 0.0f;
    std::string loadedGraphPath;
    std::vector<VFXRuntimeNodeState> runtimeNodes;
    std::vector<VFXRuntimeLinkState> runtimeLinks;
    // 重いSkeleton階層とRendererをGraph再開・再読込のたびに作り直さないためのインスタンスPool。
    std::vector<VFXAnimatedMeshPoolEntry> animatedMeshPool;
    // ScriptVFXProxy::Triggerから受け取り、同名Triggerリンクを次回更新で消費する。
    std::vector<std::string> pendingTriggers;
    // parentNodeId で親に指定されたノードのグループ GameObject (Unity の「親の空オブジェクト」相当)。
    // WHY: ノードの実体はスケジュールに合わせて SetActive を切り替えるため、実体同士を直接
    //      親子にすると「親ノードの時間が終わった瞬間に子も消える」という形で、
    //      空間の入れ子が実行の時間へ漏れ出す。Transform だけを持つ常時 active の
    //      グループを間に挟むことで、位置は継承しつつ時間は独立に保てる。
    //      Mesh ノードの膨張スケールのような実行時エンベロープも子へ波及しない。
    // NOTE: 子を持つノードにだけ作る。runtimeNodes とは別に破棄が要るので独立して持つ。
    //       nodeId を併せ持つのは、ライブ編集で親ノードの Transform が動いたときに
    //       対応するグループだけを引き当てて追従させるため。
    std::vector<VFXRuntimePivot> runtimePivots;
    std::uint64_t editorPreviewFrame = 0;
    float editorScrubTime = -1.0f;
    int nestingDepth = 0;
    bool hasEventLinks = false;
    std::vector<asset::VFXParamOverride> resolvedParameters;
    // ロード済みauthoring graphを実行中だけ共有し、動的値ソースを毎フレーム再評価する。
    // Scene/Prefabへは保存せず、reload/stop時に破棄するランタイムキャッシュ。
    // WHY: 参照専用にすることで、ライブ編集時に authoringGraph をそのまま共有でき、
    //      編集のたびにグラフ全体を複製する必要がなくなる。
    std::shared_ptr<const asset::VFXGraphAsset> runtimeGraph;

    // ── ライブ編集 (VFX Editor 用。シーン保存対象外) ──────────────────────────
    // エディタが保持する未保存グラフ。設定されている間はディスクの .vfx より優先される。
    // WHY: これが無いと「保存しないとプレビューへ反映されない」ため、
    //      値を少し動かすたびに Ctrl+S が必要になり試行錯誤が止まる。
    std::shared_ptr<const asset::VFXGraphAsset> authoringGraph;
    // エディタが編集のたびに加算する版数。system 側は appliedAuthoringRevision と
    // 突き合わせて差分適用する。
    std::uint64_t authoringRevision = 0;
    std::uint64_t appliedAuthoringRevision = 0;
    // 直前の編集が触ったノード。>0 ならそのノードだけ再適用する。-1 = 不明 (全ノード)。
    // WHY: スライダーをドラッグしている間は毎フレーム版数が進む。全ノードへ設定を
    //      写し直すとノード数に比例して重くなるが、実際に変わったのは1ノードだけ。
    int authoringDirtyNodeId = -1;
    // 構造変更 (ノード追加/削除/配線) の適用待ち。絵の切れ目まで持ち越す。
    // WHY: 再構築は生成物を作り直すので、再生中に走らせると見た目が飛ぶうえ重い。
    //      ループ先頭・再生終了・停止中といった切れ目でまとめて消化する。
    bool pendingStructuralRebuild = false;

    const char* GetTypeName() const { return "VFX Graph"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("graphPath", graphPath);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.FloatRange("speed", speed, 0.0f, 8.0f);
        r.Field("syncParentAnimator", syncParentAnimator);
        r.Field("variant", variant);
        serializedParameterOverrides = asset::SerializeVFXOverrides(parameterOverrides);
        r.Field("parameterOverrides", serializedParameterOverrides);
        asset::DeserializeVFXOverrides(serializedParameterOverrides, parameterOverrides);
    }

    void Restart()
    {
        playing = true;
        stopped = false;
        playTime = 0.0f;
        restartRequested = true;
    }

    void Pause() { playing = false; stopped = false; }
    void Resume() { playing = true; stopped = false; }
    void Stop() { playing = false; stopped = true; }
    void Trigger(std::string_view name) { pendingTriggers.emplace_back(name); }
};

} // namespace fbzz::scene
