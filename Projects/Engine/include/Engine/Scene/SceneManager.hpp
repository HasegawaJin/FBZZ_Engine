/// @file    SceneManager.hpp
/// @brief   シーン遷移とアクティブ Scene 管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note LoadScene 要求を保持し、フレーム境界で安全に切り替える。
/// @note Scene の所有は manager が持ち、利用側は非所有参照で扱う。
#pragma once
#include <Engine/Api.hpp>
#include "Scene.hpp"
#include "Engine/Asset/AssetLeaseSet.hpp"
#include "Engine/Core/Scheduler/SystemScheduler.hpp"
#include <Physics/World.hpp>

namespace fbzz::audio { class AudioManager; }
#include <functional>
#include <memory>
#include <string_view>
#include <string>
#include <unordered_map>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class SceneManager {
public:
    FBZZ_ENGINE_API SceneManager();

    using SceneFactory = std::function<std::unique_ptr<Scene>()>;

    FBZZ_ENGINE_API void Register(const std::string& name, SceneFactory factory);

    /// @note .fbzz ファイルからシーンを登録する。ロードは LoadScene 呼び出し時に行う
    FBZZ_ENGINE_API void RegisterFromFile(const std::string& name, const std::string& path,
                          renderer::ResourceManager& resources);

    /// @brief 次フレームの先頭でシーンを切り替える。未登録の名前なら要求を受け付けず false を返す (エラーはログへ)。
    /// @note 遷移はフェードアウトの後に呼ばれることが多く、失敗を黙って捨てると「暗転したまま何も起きない」形で気付かれずに止まる。
    FBZZ_ENGINE_API bool LoadScene(const std::string& name);

    /// @note 外部所有シーンをバインドする。null を渡すと LoadScene で作成した m_active を使用する
    FBZZ_ENGINE_API void SetScene(Scene* scene);
    /// @note ホストが描画先を明示する。別 Manager の生成・破棄で更新先を切り替えない。
    FBZZ_ENGINE_API void SetResourceManager(renderer::ResourceManager* resources);
    [[nodiscard]] FBZZ_ENGINE_API renderer::ResourceManager* GetResourceManager() const;

    /// @brief 外部所有 Scene と Manager 所有 Scene を両方破棄し、遷移要求も取り消す。
    /// @note 外部 Scene と Manager 所有 Scene の両方に残る Script を同じ操作で破棄する。
    FBZZ_ENGINE_API void ClearScenes();

    /// @brief LoadScene で作った Manager 所有 Scene だけを破棄し、保留中の遷移要求も捨てる。
    /// @note Editor は Stop で外部 Scene へ戻るが、遷移先の Scene は誰も Update しないまま Script ごと生き残る。保留要求も残すと、Stop の次フレームに外部バインドがもう一度外れて編集シーンが動かなくなる。
    FBZZ_ENGINE_API void ReleaseOwnedScene();

    /// @note 最後の LoadScene で切り替えた登録名。外部 Scene をバインドし直すと空に戻る。
    [[nodiscard]] const std::string& ActiveSceneName() const { return m_activeName; }

    /// @name シーン単位の先読みと利用権
    /// @see Docs/design/asset-streaming.md «シーン切り替え・停止・復旧»
    /// @{
    /// @brief RegisterFromFile で登録したシーンが参照するテクスチャ・モデル・マテリアルを非同期で読み始める。
    /// @return ファイル登録でない (本文が無い) シーンなら false。
    /// @note 切り替えは待たない。GetScenePrefetchProgress が揃ってから LoadScene すれば同期ロードが起きない。
    FBZZ_ENGINE_API bool PrefetchScene(const std::string& name);
    [[nodiscard]] FBZZ_ENGINE_API asset::AssetLeaseSet::Progress GetScenePrefetchProgress(const std::string& name) const;
    /// @brief 先読みの利用権を手放す。現在のシーンの束は手放さない。
    FBZZ_ENGINE_API void CancelScenePrefetch(const std::string& name);
    /// @}

    /// @note 固定タイムステップの Hz (デフォルト 60)
    FBZZ_ENGINE_API void SetPhysicsHz(int hz);

    /// @note false のとき EditorOnly System のみ実行する (エディタ停止中)
    FBZZ_ENGINE_API void SetSimulating(bool simulating);
    /// @note Play セッションが続いているか。Pause 中も true のまま (simulating だけ false になる)。
    /// @note 呼ばない場合は simulating と同じ扱いになる (Standalone / プレビュー用途)。
    FBZZ_ENGINE_API void SetPlaying(bool playing);

    /// @note AudioSystem に渡す AudioManager を設定する（null を渡すと AudioSystem はスキップ）
    FBZZ_ENGINE_API void SetAudioManager(audio::AudioManager* audioManager);

    /// @note true のとき accumulator を無視して physics を 1 回だけ実行する (frame step 用)
    FBZZ_ENGINE_API void SetSingleStep(bool singleStep);

    /// @note Phase PreScript 〜 Cleanup
    /// @note RenderSystem はゲームループ側から BeginFrame/EndFrame の間に直接呼ぶこと
    FBZZ_ENGINE_API void Update(float dt, physics::World& world);

    /// @note Phase LateUpdate のみ
    FBZZ_ENGINE_API void LateUpdate(float dt, physics::World& world);

    FBZZ_ENGINE_API Scene* GetActive();

    /// @note 名前で System インスタンスを取得（エディタ統合用）。
    [[nodiscard]] ISystem* FindSystem(std::string_view name) const {
        return m_scheduler.FindSystem(name);
    }

private:
    FBZZ_ENGINE_API Scene* CurrentScene() const;
    FBZZ_ENGINE_API void BuildScheduler();
    /// @brief シーン本文から参照を拾って利用権の束を作る。既にあれば優先度だけ上げる。
    FBZZ_ENGINE_API bool AcquireSceneLeases(const std::string& name, asset::AssetPriority priority);

    std::unordered_map<std::string, SceneFactory> m_factories;
    /// @note RegisterFromFile の本文の場所。先読みで参照を拾うのに使う。
    std::unordered_map<std::string, std::string>  m_scenePaths;
    /// @note シーンごとの利用権。切り替えが成功したら旧シーンの束だけを手放す (共有物は他の束が持ち続ける)。
    std::unordered_map<std::string, asset::AssetLeaseSet> m_sceneLeases;
    std::unique_ptr<Scene>                        m_active;
    std::string                                   m_pendingLoad;
    std::string                                   m_activeName;
    Scene*                                        m_externalScene  = nullptr;
    uint64_t m_resourceOwner = 0;
    bool                                          m_simulating     = true;
    bool                                          m_playing        = false;
    /// @note 開始前に一度だけstatic bakeを検証し、runtime中の非追従TargetPoint移動を再Bake要求へ変えない。
    uint64_t m_staticBakeValidationScene = 0;
    bool m_staticBakeValidationSucceeded = true;
    audio::AudioManager*                          m_audioManager   = nullptr;

    SystemScheduler m_scheduler;
};

} /// @note namespace fbzz::scene
