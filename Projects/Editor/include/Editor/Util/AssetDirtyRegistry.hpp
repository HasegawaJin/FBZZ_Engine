// FBZZ Engine
// AssetDirtyRegistry.hpp | fbzz::editor
// Unreal 方式の未保存アセット中央レジストリ
// WHY: シーン (SceneDirtyTracker) と同様に、マテリアル・AnimatorController など
//      各アセットファイルの編集状態を一元管理し、Save All / 終了時確認を実現する。
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace fbzz::editor {

struct DirtyAsset {
    std::string path;                 // 正規化済み絶対パス
    std::string displayPath;          // UI 表示用の相対パス (Assets/... 形式)
    std::string typeLabel;            // アイコン用ラベル ("MAT", "CTRL" 等)
    std::function<bool()> saveFunc;   // 呼ぶだけで保存 → true=成功
};

// アセットファイルの dirty 状態を管理するスレッドローカルな静的レジストリ。
// WHY: Editor パネルは全て同一スレッド (main render thread) で動作するため
//      static メンバーで十分。スレッドセーフ保護は不要。
class AssetDirtyRegistry {
public:
    // パスを登録 (既登録の場合は saveFunc を上書き更新)
    static void Register(const std::string& absPath,
                         const std::string& displayPath,
                         const std::string& typeLabel,
                         std::function<bool()> saveFunc);

    // 保存完了後 / 変更破棄後に呼ぶ
    static void MarkClean(const std::string& absPath);

    static bool IsDirty(const std::string& absPath);
    static bool HasAny();
    static const std::vector<DirtyAsset>& GetAll();

    // 1 件だけ保存する。未登録 (= dirty でない) なら false。
    // WHY: 「今開いている Animator Controller だけ保存する」を、パネルの Save ボタンと
    //      同じ saveFunc を通して行うため。別経路で書き出すと、レイアウト情報の付与や
    //      保存後のシーン側 Animator への反映といった手順が片方だけ抜ける。
    static bool Save(const std::string& absPath);

    // 全エントリを保存する。失敗件数を返す。
    static int SaveAll();

    // 全エントリをクリア (破棄)
    static void DiscardAll();

private:
    static std::vector<DirtyAsset> s_dirty;
};

} // namespace fbzz::editor
