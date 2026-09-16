/// @file    EditorFixture.cpp
/// @brief   EditorFixture の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/SceneIO.hpp>

#include <filesystem>
#include <system_error>

namespace fbzz::testkit {

void EditorFixture::SetUp()
{
    EngineFixture::SetUp();
    ASSERT_TRUE(m_temp.IsValid());
    m_context = editor::EditorContext{};
    m_scene.reset();

    // SceneIO::Serialize は «メモリ上の TOML» を返す API に見えて、実際は
    // <projectRoot>/Assets/EditorConfig へ一時ファイルを書いて読み戻している。
    // 向き先を変えないと、テストの作業ディレクトリ次第で書き込みに失敗し、
    // 空文字が返る。シーンの直列化を使う側 (プレハブ保存・変更検知) が
    // «保存できない» ではなく «変更が無い» と誤って見えるので、原因を追いにくい。
    editor::SceneIO::SetProjectRoot(m_temp.Path().generic_string());

    // 置き場所を «実運用と同じ形» まで作っておく。
    // WHY 自前で掘るか: FileSystem::EnsureDirectory は CreateDirectoryW 1 回で、
    //     中間ディレクトリを作らない。実プロジェクトでは Assets/ が既にあるので
    //     表に出ないが、空の一時ディレクトリからだと Assets/EditorConfig を作れず、
    //     直列化が黙って空文字を返す。
    std::error_code ec;
    std::filesystem::create_directories(m_temp.Path() / "Assets" / "EditorConfig", ec);
    ASSERT_FALSE(ec) << "作業ディレクトリを用意できません: " << ec.message();
}

void EditorFixture::TearDown()
{
    // 文脈から先に切る。順序を逆にすると、破棄済みの Scene を指したまま
    // 後片付けが走る経路ができる (実際に RestoreEditorHiding で踏んだ形)。
    m_context.activeScene = nullptr;
    m_context.editScene   = nullptr;
    m_scene.reset();

    // 静的な向き先を既定へ戻す。消える一時ディレクトリを指したまま次のテストへ
    // 持ち越すと、そちらが «書けない» で落ちる。
    editor::SceneIO::SetProjectRoot(".");
    EngineFixture::TearDown();
}

scene::Scene& EditorFixture::AttachScene()
{
    if (!m_scene) m_scene = std::make_unique<scene::Scene>();
    m_context.activeScene = m_scene.get();
    m_context.editScene   = m_scene.get();
    return *m_scene;
}

void EditorFixture::DetachScene()
{
    m_context.activeScene = nullptr;
    m_context.editScene   = nullptr;
}

} // namespace fbzz::testkit
