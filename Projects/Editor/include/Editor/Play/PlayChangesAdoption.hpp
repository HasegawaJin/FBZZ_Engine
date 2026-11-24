/// @file    PlayChangesAdoption.hpp
/// @brief   Selected native authoring values and generated data forests use explicit checked Play adoption.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Editor/Op/EditorOperator.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
namespace fbzz::scene { class Scene; }
namespace fbzz::editor {
/// @note Contains data snapshots and stable identity proofs only; no runtime or GPU/physics ownership crosses Stop.
class PlayChangesAdoptionState {
    struct Impl;
public:
    explicit PlayChangesAdoptionState(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
    static std::shared_ptr<PlayChangesAdoptionState> Begin(scene::Scene& edit, scene::Scene& play, std::string& diagnostic);
    /// @note Only the safe Stop restore admits adoption; cancellation invalidates pending drafts.
    void Finish(bool stopped);
    OpResult Capture(EditorContext& context, const OpData& selection);
    OpResult Review(uint64_t id) const;
    OpResult Prepare(EditorContext& context, uint64_t id);
    OpResult Apply(EditorContext& context, uint64_t id);
    void Discard(uint64_t id);
    void Draw(EditorContext& context);
private:
    std::shared_ptr<Impl> m_impl;
};
void RegisterPlayChangesAdoptionOperators(OperatorRegistry& registry);
void DrawPlayChangesAdoptionReview(EditorContext& context);
} /// @note namespace fbzz::editor
