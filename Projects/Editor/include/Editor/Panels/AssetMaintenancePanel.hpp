/// @file    AssetMaintenancePanel.hpp
/// @brief   GUID 重複を検出して振り直すアセット保守パネル。
/// @author  Hasegawa Jin
/// @date    2026-09-10/// @note Tools > "Asset Maintenance..." から開く。View > Panels には表示しない。/// @note 修復規則: 同じ guid を名乗る実体のうち、.meta が «いちばん古い» 1 つを残し、
/// @note 後から増えた側に新しい guid を振る。既存の "guid:" 参照は衝突中も古い側へ
/// @note 解決されているため、この向きなら参照の解決先が今日と変わらない。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

class AssetMaintenancePanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Asset Maintenance"; }
    const char* GetViewMenuName()      const override { return "Asset Maintenance"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }
    void OnRegisterOperators(OperatorRegistry& registry) override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// @note 同じ guid を名乗る実体の 1 まとまり。paths は .meta の更新時刻の昇順で、
    /// @note 先頭が «残す側»。
    struct ConflictGroup {
        struct Entry {
            std::string path;       ///< @note アセット絶対パス
            std::string relative;   ///< @note 表示用のプロジェクト相対パス
            std::string metaTime;   ///< @note .meta の更新時刻 (表示用)
            bool        exists = true;
        };
        std::string        guid;
        std::vector<Entry> entries;
    };

    /// @note AssetDatabase の記録から表示用のグループを作り直す。
    void Rescan();
    /// @note group の先頭以外に新しい guid を振る。戻り値は振り直した件数。
    int  FixGroup(const ConflictGroup& group);

    std::vector<ConflictGroup> m_groups;
    bool        m_scanned = false;   ///< @note 一度も Rescan していない間は空表示と区別する
    std::string m_lastResult;        ///< @note 直近の修復結果 (件数と内訳)
};

} /// @note namespace fbzz::editor
