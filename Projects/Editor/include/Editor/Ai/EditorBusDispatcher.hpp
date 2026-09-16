/// @file    EditorBusDispatcher.hpp
/// @brief   Editor Command Bus の中核。NDJSON 要求1行を受け、Scene / UndoStack / Renderer を操作して応答1行を返す。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// 設計 (WHY):
/// NamedPipeServer から渡される要求を「メインスレッド上で」実処理へ写像する唯一の場所。
/// Query は副作用なし、Command は UndoStack 経由で Undo 可能に適用し、dryRun は変更せず試算のみ返す。
/// protocol/型の検証と失敗時の error 応答生成もここで完結し、例外は投げない (プロジェクト規約)。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <string>
#include <vector>

namespace fbzz::editor { struct EditorContext; }

namespace fbzz::editor::ai {

class EditorBusDispatcher {
public:
    explicit EditorBusDispatcher(editor::EditorContext& context);

    // viewport.capture が読み戻す Scene View RT を毎フレーム更新する (EditorApp が drain 直前に呼ぶ)。
    void SetSceneViewportRT(renderer::ResourceHandle<renderer::RenderTargetTag> rt) { m_sceneViewportRT = rt; }
    // Playtestの画像検証用に、実際のGame View RTも同じフレームで受け取る。
    void SetGameViewportRT(renderer::ResourceHandle<renderer::RenderTargetTag> rt) { m_gameViewportRT = rt; }
    // VFX Editor の装飾なし専用 Preview RT。AI は Scene View の UI/選択輪郭を含まない画を評価する。

    // 要求1行 (NDJSON) → 応答1行 (改行なし)。NamedPipeServer::DrainRequests の handler に渡す。
    // 相関不能な壊れた要求には空文字を返す (送信側は timeout で回復)。
    std::string Handle(const std::string& requestLine);

private:
    editor::EditorContext& m_context;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT{};
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT{};

    // 直前に vfx.previewMetrics が測ったフレームの輝度。次の呼び出しで「動いているか」を出すために保持する。
    // WHY: 動きは 1 枚では測れず、2 枚を突き合わせるしかない。MCP 側は PNG しか受け取らないので
    //      比較はここでしかできない。連続スクラブすると自然に「前の時刻との差」になる。
    std::vector<float> m_previousPreviewLuminance;
    std::string m_previousPreviewKey;   // 比較対象を取り違えないよう path + view + サイズで識別する
};

} // namespace fbzz::editor::ai
