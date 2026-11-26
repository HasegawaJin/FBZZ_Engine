/// @file    IblBakePanel.hpp
/// @brief   HDRI (.hdr / .exr) → IBL アセット (.ibl + 4 つの DDS) をベイクするエディターパネル。
/// @author  Hasegawa Jin
/// @date    2026-06-23/// @note Tools > "IBL Baker..." から開く。View > Panels には表示しない。/// @note ベイクフロー:
/// @note HdriLoader::Load()  → float RGBA ピクセル
/// @note IRenderer::CreateIblBaker() → IIblBaker
/// @note IIblBaker::Bake() → 4 DDS + .ibl バイナリ記述子
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Graphics/Renderer/View/Lighting/IIblBaker.hpp>
#include <array>
#include <atomic>
#include <future>
#include <string>

namespace fbzz::editor {

class IblBakePanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "IBL Baker"; }
    const char* GetViewMenuName()      const override { return "IBL Baker"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }
    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnUpdate(EditorContext& ctx, float dt) override;
    void OnShutdown() override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    std::array<char, 512> m_hdriPath  = {};  ///< ソース HDRI の絶対パス
    std::array<char, 512> m_outputDir = {};  ///< 出力先ディレクトリ絶対パス
    std::array<char, 128> m_baseName  = {};  ///< 出力ファイルのベース名 (例: "sky")

    int m_envCubemapSize  = 2048; ///< ベイク設定 (デフォルト値は IblBakeInput のデフォルトに合わせる)
    int m_irradianceSize  = 32; ///< 拡散照明の irradiance キューブマップの一面の幅 [px]。
    int m_prefilteredSize = 512; ///< 鏡面照明用の事前フィルター済みキューブマップの最高解像度の幅 [px]。
    int m_prefilteredMips = 5; ///< 鏡面照明の粗さに対応する事前フィルター済みキューブマップのミップ数。
    int m_brdfLutSize     = 256; ///< BRDF 積分結果を保存する正方形 LUT の幅 [px]。
    int m_sampleCount     = 1024; ///< IBL の積分処理で使うサンプル数。

    enum class Status { Idle, Baking, Done, Error, Cancelled };
    bool m_bakeRequested = false; ///< 次の更新でベイクを開始する要求。
    int m_job = 0; ///< ベイクジョブ番号用の予備フィールド。現在の完了判定には m_bake を使う。
    Status      m_status = Status::Idle; ///< IBL ベイクの待機、実行中、終端の状態。
    std::string m_statusMsg; ///< IBL ベイクの結果やエラーを表示する文字列。

    struct BakeResult {
        renderer::IblBakeOutput output; ///< ベイクした irradiance、prefiltered map、BRDF LUT の結果。
        std::string message; ///< 実行完了時にパネルへ返す診断または結果メッセージ。
        bool success = false; ///< 要求した IBL のベイクに成功したか。
    };
    std::atomic<int> m_progress{ 0 }; ///< -1 は中止要求。90% で DDS 出力を確定するため、それ以降は中止を受け付けない。
    std::future<BakeResult> m_bake; ///< バックグラウンドで実行したベイクの結果を受け取る future。
    static constexpr int SAVE_PROGRESS = 90;

    void StartBake(EditorContext& ctx);
    void FinishBake(EditorContext& ctx, const BakeResult& result);
    bool CancelBake();
};

} // namespace fbzz::editor
