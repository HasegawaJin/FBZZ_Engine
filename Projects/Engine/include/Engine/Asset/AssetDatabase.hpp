// FBZZ Engine
// AssetDatabase.hpp | fbzz::asset
// GUID ⇄ アセットパスの双方向インデックス。
//
// 各アセット (ファイルおよびフォルダ) は隣接する "<名前>.meta" サイドカーの
// [meta] guid で恒久 ID を持つ。フォルダの .meta は親ディレクトリに置く
// (例: Assets/Textures/ → Assets/Textures.meta)。

// 参照側 (.mat / .scene / コンポーネント) は "guid:<32hex>" 形式の文字列を保存でき、
// AssetManager::ResolvePath がロード時に実パスへ解決する。
// WHY: パス文字列参照はリネーム・移動で全参照が壊れる。GUID は生成後不変なので、
//      ファイルを動かしてもインデックスの再構築だけで参照が生き続ける。
//
// スレッド規約: Init はメインスレッドで 1 回。以後の参照系 (PathFromGuid 等) は
// 内部 mutex で保護されるため任意スレッドから呼べる。
#pragma once
#include <string>
#include <string_view>

namespace fbzz::asset {

class AssetDatabase {
public:
    // "guid:" 参照プレフィックス。この後ろに 32 桁 hex が続く。
    static constexpr std::string_view kGuidPrefix = "guid:";

    // assetsRoot (例 "C:/proj/Assets/") 配下を走査してインデックスを構築する。
    // .meta が無い / guid の無い対象アセットには .meta を自動生成する (自己修復)。
    // AssetManager::Init から呼ばれる。
    static void Init(const std::string& assetsRoot);
    static void Shutdown();

    // guid (32hex) → アセット絶対パス。未登録なら空文字列。
    [[nodiscard]] static std::string PathFromGuid(const std::string& guid);

    // アセット絶対パス → guid。未登録なら .meta を生成して新 guid を返す。
    // 失敗 (.meta 書き込み不可等) なら空文字列。
    [[nodiscard]] static std::string GuidFromPath(const std::string& absPath);

    // リネーム / 移動フック。guid は不変のままパスの索引だけ付け替える。
    // 呼び出し側 (AssetBrowser) は本体と .meta を両方移動した後に呼ぶこと。
    static void OnAssetMoved(const std::string& oldAbsPath, const std::string& newAbsPath);

    // "guid:xxxx" 形式か判定する (ResolvePath の分岐用)。
    [[nodiscard]] static bool IsGuidRef(std::string_view ref) {
        return ref.rfind(kGuidPrefix, 0) == 0;
    }

    // 32 桁 hex の新規 GUID を生成する (Unity と同形式)。
    [[nodiscard]] static std::string GenerateGuid();

    // この拡張子のアセットは .meta を持つべきか (baked 生成物・.meta 自身は対象外)。
    [[nodiscard]] static bool ShouldHaveMeta(std::string_view lowerExt);

    // このフォルダは .meta ("<フォルダ名>.meta" を隣に置く) を持つべきか。
    // WHY: フォルダも参照対象になりうる (デフォルト保存先、検索スコープ、パック単位)。
    //      パスで覚えるとリネームで壊れるのはファイルと同じなので、guid を持たせる。
    //      引数はフォルダ名のみ (パスではない)。ドット始まり・生成物置き場は除外する。
    [[nodiscard]] static bool ShouldHaveFolderMeta(std::string_view folderName);

    // 登録済みアセット数 (デバッグ / EditorUI 表示用)。
    [[nodiscard]] static size_t Count();

    // Init に渡された Assets ルート (末尾 '/' 付き)。"Assets/..." 相対化に使う。
    [[nodiscard]] static std::string AssetsRoot();
};

} // namespace fbzz::asset
