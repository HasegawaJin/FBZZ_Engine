/// @file    EditorBusDispatcher.hpp
/// @brief   Editor Command Bus の中核。NDJSON 要求1行を受け、Scene / UndoStack / Renderer を操作して応答1行を返す。
/// @author  Hasegawa Jin
/// @date    2026-07-20
/// @note    Query は副作用なし、Command は UndoStack 経由で Undo 可能に適用し、dryRun は変更せず試算のみ返す。
/// @see     Docs/design/ai-verification-loop.md «6. バスの分割»
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor { struct EditorContext; }
namespace fbzz::editor::ai::bus { class BusHandlerTable; struct BusState; }

namespace fbzz::editor::ai {

class EditorBusDispatcher {
public:
    explicit EditorBusDispatcher(editor::EditorContext& context);
    ~EditorBusDispatcher();
    EditorBusDispatcher(const EditorBusDispatcher&) = delete;
    EditorBusDispatcher& operator=(const EditorBusDispatcher&) = delete;

    /// @brief viewport.capture が読み戻す Scene View RT。EditorApp が drain 直前に毎フレーム渡す。
    void SetSceneViewportRT(renderer::ResourceHandle<renderer::RenderTargetTag> rt);
    /// @brief Playtest の画像検証用に、同じフレームの Game View RT も受け取る。
    void SetGameViewportRT(renderer::ResourceHandle<renderer::RenderTargetTag> rt);

    /// @brief 要求1行 (NDJSON) → 応答1行 (改行なし)。メインスレッドで呼ぶ。
    /// @return 空行には空文字。壊れた要求にも id 空のエラー応答を返す。
    std::string Handle(const std::string& requestLine);

    /// @brief 登録済みの型名。MCP 契約との突き合わせテストが読む。
    [[nodiscard]] std::vector<std::string> RegisteredTypes() const;

private:
    editor::EditorContext& m_context;
    std::unique_ptr<bus::BusHandlerTable> m_table;
    std::unique_ptr<bus::BusState> m_state;
};

} // namespace fbzz::editor::ai
