/// @file    AppIconWriter.hpp
/// @brief   配布 exe のアイコンリソースを画像から差し替える。
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// @note PNG 等を Windows のアイコンリソース (RT_GROUP_ICON / RT_ICON) に変換し、
///       パッケージング済みの exe へ直接書き込む。.rc に持たせるとゲーム側の再リンクが要るため。
#pragma once
#include <filesystem>
#include <string>

namespace fbzz::editor {

class AppIconWriter {
public:
    /// 書き込むアイコングループのリソース ID。
    /// @note 101 固定。Engine/src/Core/Platform/Window.cpp の kDefaultApplicationIconId と対応する。
    ///       ずらすと Explorer には出てもウィンドウ (タイトルバー / Alt+Tab) は既定アイコンのまま残る。
    static constexpr int kIconResourceId = 101;

    /// 画像の素性。Build Settings の事前チェックが exe に触れずに読む。
    struct SourceInfo {
        int  width  = 0;
        int  height = 0;
        bool isIco  = false; ///< .ico は変換せず、中の各サイズをそのまま焼く
    };

    /// 画像を読めるかとサイズだけを確かめる (デコードはしない)。
    static bool Inspect(const std::filesystem::path& imagePath,
                        SourceInfo& out,
                        std::string& outError);

    /// exe のアイコンを差し替える。対象の exe が実行中でないこと。
    /// @param imagePath PNG / JPEG / TGA / BMP / ICO
    /// @param exePath   書き換える exe
    /// @param outError  失敗理由 (ビルドログへそのまま出す)
    static bool Apply(const std::filesystem::path& imagePath,
                      const std::filesystem::path& exePath,
                      std::string& outError);
};

} // namespace fbzz::editor
