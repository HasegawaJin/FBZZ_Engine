/// @file    VFXTimelinePanel.hpp
/// @brief   VFX ルート配下の生存窓を 1 子オブジェクト = 1 トラックで並べ、尺を目で詰める。
/// @author  Hasegawa Jin
/// @date    2026-08-26
/// @note エフェクトの良し悪しは «どの層がいつ立ち上がっていつ消えるか» で決まるが、Inspector は
/// @note       1 オブジェクトずつしか見せず、選択往復だけで時間が溶ける。実行の因果は startDelay に
/// @note       畳んであり (Docs/design/vfx-prefab.md §4.3) 配線として描くものは残っていないため、
/// @note       グラフ Canvas ではなく時間軸だけの専用パネルにする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/References/Entity.hpp>

#include <string>
#include <vector>

namespace fbzz::scene { class GameObject; class Scene; struct VFXComponent; }

namespace fbzz::editor {

class VFXTimelinePanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "VFX Timeline"; }
    bool        GetDefaultVisibility() const override { return false; }

    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnUpdate(EditorContext& ctx, float dt) override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// @note 1 行ぶん。窓の値がどのコンポーネントに載っているかを併せて持つ。
    /// @note 時間の正本は 1 オブジェクトにつき 1 箇所で、ParticleEmitter は自前の
    /// @note       startDelay/duration、それ以外は VFXElement にある (§4.1)。書き戻し先を行ごとに
    /// @note       決めておかないと、掴んで動かした値が «読んだ場所と違うところ» へ入る。
    struct Track {
        scene::EntityID entity{}; ///< タイムラインの行が編集するエフェクトの EntityID。
        std::string     label; ///< タイムラインの行に表示するエフェクト名。
        int             depth = 0; ///< 行を字下げするための、VFX ルートからの階層の深さ。
        bool            isGroup = false;    ///< Transform だけの層。窓を持たない
        bool            fromEmitter = false; ///< true: ParticleEmitter / false: VFXElement
        float           start = 0.0f; ///< エフェクトのタイムライン上の開始時刻 [s]。
        float           duration = 0.0f; ///< エフェクトを再生する時間 [s]。
        bool            loop = false; ///< このエフェクトの再生を繰り返すか。
        std::string     trigger;            ///< 空でなければ時間ではなく発火で始まる
    };

    void CollectTracks(scene::Scene& scene, scene::GameObject& root, int depth);
    /// @note 掴んで動かした窓を書き戻す。Undo は呼び出し側が包む。
    void ApplyTrack(scene::Scene& scene, const Track& track, float start, float duration) const;

    [[nodiscard]] scene::GameObject* ResolveRoot(EditorContext& ctx) const;

    std::vector<Track> m_tracks; ///< 現在の VFX ルートから集めたタイムライン行。
    float m_scrubTime = -1.0f; ///< スクラブ中の時刻 [秒]。負なら通常再生。
    float m_apiScrubTime = -1.0f; ///< Editor API が要求したタイムラインの再生位置 [s]。未指定なら負。
    std::string m_apiRoot; ///< API のスクラブ要求に対応する VFX ルートの instanceId。
    std::string m_apiProject; ///< API のスクラブ要求を受けたプロジェクトのルート。
    scene::EntityID m_dragEntity{}; ///< ドラッグ開始時の値。1 フレームごとに加算すると誤差が積もるため元値から作り直す。
    float           m_dragStart = 0.0f; ///< ドラッグ開始時のエフェクトの開始時刻 [s]。
    float           m_dragDuration = 0.0f; ///< ドラッグ開始時のエフェクトの長さ [s]。
    int             m_dragEdge = 0; ///< -1: 左端 / 0: 帯全体 / +1: 右端
    /// @note 直近フレームで書き込んだ値。離した瞬間に Undo を組むとき、
    /// @note m_tracks (フレーム頭の写し) は既に古いのでこちらを正とする。
    float           m_dragResultStart = 0.0f; ///< 現在のドラッグ結果が指定するエフェクトの開始時刻 [s]。
    float           m_dragResultDuration = 0.0f; ///< 現在のドラッグ結果が指定するエフェクトの長さ [s]。
};

} // namespace fbzz::editor
