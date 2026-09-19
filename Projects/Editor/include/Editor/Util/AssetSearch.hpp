/// @file    AssetSearch.hpp
/// @brief   アセット検索の共通基盤 — 索引・差分更新・スコア付きマッチング。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 走査対象/除外規則を 1 箇所に定め、索引をキャッシュして AssetFileWatcher の差分で更新し、
///       一致判定とスコア付けを共通化する (以前は検索実装が画面ごとに分かれ挙動が食い違っていた)。
#pragma once
#include <Editor/AssetFileWatcher.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

struct AssetSearchEntry {
    std::string absolutePath;  ///< OS の絶対パス
    std::string relativePath;  ///< プロジェクトルートからの相対パス ('/' 区切り)
    std::string filename;      ///< 拡張子込みのファイル名
    std::string extension;     ///< 小文字の拡張子 (".mat")。拡張子なしは空文字列
};

struct AssetSearchHit {
    const AssetSearchEntry* entry = nullptr;
    int score = 0;  ///< 大きいほど良い一致
};

class AssetSearch {
public:
    /// @name 索引
    /// @{

    /// 走査対象のプロジェクトルートを設定する。
    /// 同じルートが再設定された場合は何もしない (毎フレーム呼んでも安全)。
    static void SetProjectRoot(const std::string& projectRoot);

    /// 索引を作り直す。ファイル数に比例したコストがかかるため、
    /// 通常は SetProjectRoot と ApplyFileEvents に任せ、明示的な更新操作でのみ呼ぶ。
    static void Rebuild();

    /// AssetFileWatcher のイベントで索引を差分更新する。
    /// @note 数千ファイルの再走査は数百 ms かかる。保存のたびに全再走査すると詰まるため差分にする。
    static void ApplyFileEvents(std::span<const AssetFileWatcher::FileEvent> events);

    [[nodiscard]] static const std::vector<AssetSearchEntry>& Entries();
    [[nodiscard]] static std::size_t Count();
    [[nodiscard]] static const std::string& ProjectRoot();
    /// @}

    /// @name 検索
    /// @{

    /// query に一致するアセットをスコア降順で返す。
    /// query が空の場合は拡張子フィルタだけを適用し、パス順で返す。
    /// extensions が空なら拡張子で絞らない。要素は小文字のドット付き (".mat")。
    [[nodiscard]] static std::vector<AssetSearchHit> Query(
        std::string_view query,
        std::span<const std::string> extensions = {},
        std::size_t maxResults = 200);

    /// ".mat,.hlsl" 形式のフィルタ文字列を配列へ分解する (小文字化・ドット補完込み)。
    [[nodiscard]] static std::vector<std::string> ParseExtensionFilter(std::string_view csv);
    /// @}

    /// @name 単体マッチ
    /// @{

    /// text が query にどれだけ一致するかを返す。0 は不一致。
    /// 索引を経由しない呼び出し (既に手元にリストがある場合) でも
    /// 同じ並び順を再現できるよう公開する。
    ///
    /// スコア規則 (大きいほど上位):
    ///   完全一致 > 前方一致 > 部分一致 (前方にあるほど高い) > 部分列一致 (曖昧)
    /// @note 部分列一致まで拾うのは、"ppvol" で "PostProcessVolume.fzdata" のように、
    ///       正確な綴りを覚えていなくても辿り着けるようにするため。
    [[nodiscard]] static int Match(std::string_view text, std::string_view query);
    /// @}
};

} // namespace fbzz::editor
