/// @file    AssetMaintenancePanel.hpp
/// @brief   GUID 重複を検出して振り直すアセット保守パネル。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// Tools > "Asset Maintenance..." から開く。View > Panels には表示しない。
///
/// 修復規則: 同じ guid を名乗る実体のうち、.meta が «いちばん古い» 1 つを残し、
/// 後から増えた側に新しい guid を振る。既存の "guid:" 参照は衝突中も古い側へ
/// 解決されているため、この向きなら参照の解決先が今日と変わらない。
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

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    // 同じ guid を名乗る実体の 1 まとまり。paths は .meta の更新時刻の昇順で、
    // 先頭が «残す側»。
    struct ConflictGroup {
        struct Entry {
            std::string path;       // アセット絶対パス
            std::string relative;   // 表示用のプロジェクト相対パス
            std::string metaTime;   // .meta の更新時刻 (表示用)
            bool        exists = true;
        };
        std::string        guid;
        std::vector<Entry> entries;
    };

    // AssetDatabase の記録から表示用のグループを作り直す。
    void Rescan();
    // group の先頭以外に新しい guid を振る。戻り値は振り直した件数。
    int  FixGroup(const ConflictGroup& group);

    std::vector<ConflictGroup> m_groups;
    bool        m_scanned = false;   // 一度も Rescan していない間は空表示と区別する
    std::string m_lastResult;        // 直近の修復結果 (件数と内訳)
};

} // namespace fbzz::editor
