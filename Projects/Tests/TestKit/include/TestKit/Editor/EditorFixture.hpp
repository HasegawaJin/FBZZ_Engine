/// @file    EditorFixture.hpp
/// @brief   EditorContext を必要な分だけ組み立てる、Editor テスト向けの土台。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// EditorContext は «エディターの今» をまとめて持つ大きな構造体で、実運用では
/// EditorApp が全フィールドを埋める。テストからそれを再現するのは現実的ではないし、
/// 再現してしまうと «何が効いているのか» が読めないテストになる。
///
/// ここでは逆に «既定は何も繋がっていない» を出発点にし、テストが必要な物だけを
/// 明示的に繋ぐ。Play 中にシーンが差し替わる谷間 (activeScene == nullptr) は
/// 実際に起こる状態なので、それを既定にしておく方が事故を見つけやすい。
#pragma once

#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>

#include <filesystem>
#include <memory>

namespace fbzz::testkit {

class EditorFixture : public EngineFixture {
protected:
    void SetUp() override;
    void TearDown() override;

    /// 何も繋がっていない文脈。activeScene は nullptr のまま。
    [[nodiscard]] editor::EditorContext& Context() { return m_context; }

    /// シーンを 1 つ作って activeScene へ繋ぐ。呼ばない限り繋がらない。
    scene::Scene& AttachScene();

    /// 繋いだシーン。AttachScene() を呼んでいなければ nullptr。
    [[nodiscard]] scene::Scene* AttachedScene() { return m_scene.get(); }

    /// Play 中のシーン遷移で起きる «谷間» を作る。
    /// シーンは生かしたまま、文脈からの参照だけを切る。
    void DetachScene();

    /// このテスト専用の作業ディレクトリ。SceneIO の一時ファイルもここへ向いている。
    [[nodiscard]] const std::filesystem::path& ProjectRoot() const { return m_temp.Path(); }

    /// ProjectRoot 配下のパスを組み立てる (ファイルはまだ存在しない)。
    [[nodiscard]] std::filesystem::path File(const std::string& name) const
    {
        return m_temp.File(name);
    }

private:
    editor::EditorContext          m_context{};
    std::unique_ptr<scene::Scene>  m_scene;
    TempDir                        m_temp{"editor"};
};

} // namespace fbzz::testkit
