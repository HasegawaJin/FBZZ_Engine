// FBZZ Engine
// VFXTemplateCatalog.hpp | fbzz::editor
// Assets/VFX/Templates の走査と Template の書き出し
// WHY: 以前は組み込み 5 種を名前でハードコードしていたため、プロジェクト側で
//      Template を増やしてもメニューへ現れず、中身も適用するまで分からなかった。
//      「どこを探すか」と「中身は何か」をサービスとして独立させ、View はメニューを描くだけにする。
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

// Template の適用方法。人間の UI と AI の vfx.template.apply が同じ 3 択を持つ。
// WHY: Merge はコピーなので Template を後から直しても取り込み済みのグラフへ伝播しない。
//      「更新が伝わる基底」が要る場面のために SubGraph 参照を対等な選択肢として置く。
enum class TemplateApplyMode { Replace, Merge, SubGraph };

[[nodiscard]] const char* TemplateApplyModeName(TemplateApplyMode mode);

// Template の出所。同名は先勝ちで解決するが、どれが採用されたかを画面へ出すために持つ。
// WHY: Project / Engine / SDK に同名の Template があると黙って 1 つだけが残る。
//      「直したはずの Template が反映されない」の原因がこれで、表示が無いと辿れない。
enum class TemplateOrigin { Project, Engine, Bundled };

[[nodiscard]] const char* TemplateOriginName(TemplateOrigin origin);

// Template が公開している Variant Set 1 件。適用時に選ばせる。
struct GraphTemplateVariant {
    std::string name;
    int overrideCount = 0;
};

// Template 内の層 (グループ枠) 1 件。部分取り込みの単位。
struct GraphTemplateLayer {
    int groupId = 0;
    std::string title;
    std::string note;
    int nodeCount = 0;
};

// Templates フォルダから発見した .vfx 1件分のカタログエントリ。
struct GraphTemplateEntry {
    std::string name;     // 拡張子を除いたファイル名 (メニュー表示名)
    std::string path;     // 解決済みの実ファイルパス
    std::string summary;  // ノード種別の内訳 ("Particle x3, Light x1" 等)
    // Templates ルートからの相対サブフォルダ。空ならルート直下。
    // WHY: 走査が非再帰だったためカテゴリ分けができず、増えるほど一列のメニューが伸びた。
    std::string category;
    TemplateOrigin origin = TemplateOrigin::Project;
    // .vfx が持つオーサリング メタデータ。カタログの表示と AI の検索が同じ値を読む。
    std::string description;
    std::vector<std::string> tags;
    std::vector<std::string> requiredRoles;
    std::vector<GraphTemplateVariant> variants;
    std::vector<GraphTemplateLayer> layers;
    // 適用しても素材が無い参照。空でなければ「置いても何も出ない」状態になる。
    std::vector<std::string> missingAssets;
    int nodeCount = 0;
    int linkCount = 0;
    int particleBudget = 0; // 取り込むと必要になる粒子数 (合算 budget の判断材料)
    int lightBudget = 0;
    int audioBudget = 0;
    float duration = 0.0f; // スケジュール全長 [秒]。解析できなければ 0
    // サムネイル PNG の実パス。存在しなければ空 (未生成)。
    std::string thumbnailPath;
    bool valid = false;    // 読み込み・検証に成功したか

    // 検索用の連結文字列 (名前 + カテゴリ + tag + 説明 + 内訳)。小文字化済み。
    std::string searchKey;
};

class VFXTemplateCatalog {
public:
    std::vector<GraphTemplateEntry> entries;
    bool scanned = false;
    // 書き出しの結果メッセージ (成功・失敗)。ダイアログがそのまま表示する。
    std::string status;

    // Template を Project > Engine > 実行ファイル同梱 の優先順で走査してカタログ化する。
    // force=false ならキャッシュ済みなら何もしない (メニューを開くたびのディスクI/Oを避ける)。
    // checkAssetReferences=false なら参照切れ検査を省く (AssetManager 未初期化のプロセス用)。
    void Scan(EditorContext& ctx, bool force, bool checkAssetReferences = true);

    // カテゴリ名の一覧 (重複なし・辞書順)。ルート直下は空文字として先頭に入る。
    [[nodiscard]] std::vector<std::string> Categories() const;

    // 小文字化した部分一致でエントリを絞る。空文字なら全件。
    [[nodiscard]] std::vector<const GraphTemplateEntry*> Filter(const std::string& query) const;

    [[nodiscard]] const GraphTemplateEntry* Find(const std::string& nameOrPath) const;

    // 現在のグラフを Assets/VFX/Templates/<category>/<name>.vfx として書き出す。
    // 書き込み先は Project のみ (Engine/SDK 同梱ディレクトリは読み取り専用として扱う)。
    // description / tags は Template のメタデータとしてアセットへ保存する。
    [[nodiscard]] bool SaveAsTemplate(EditorContext& ctx, const asset::VFXGraphAsset& graph,
                                      const std::string& name, const std::string& category,
                                      const std::string& description,
                                      const std::vector<std::string>& tags);
};

// Template のサムネイル PNG の置き場所。.vfx と同じフォルダの .thumbnails/<name>.png
// WHY: Templates フォルダ直下に PNG を置くと .vfx の走査に混ざり、AssetBrowser にも
//      「テンプレートと同じ名前の画像」が並ぶ。隠しフォルダへ分けて所属を明示する。
//      カテゴリごとに .thumbnails を持つので、Template を移動しても対応が崩れない。
[[nodiscard]] std::string TemplateThumbnailPath(const std::string& templateFilePath);

} // namespace fbzz::editor
