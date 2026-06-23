// FBZZ Engine
// HdriLoader.hpp | fbzz::editor
// .hdr / .exr HDRI ファイルを float RGBA ピクセル配列として読み込むローダー
//
// .hdr : stb_image の stbi_loadf() を使用 (既存 Stb サードパーティ)
// .exr : TinyEXR の LoadEXR() を使用 (ThirdParty/TinyExr/tinyexr.h)
//
// 読み込んだピクセルは RGBA float (4ch) として正規化される。
// IblBaker に float* を渡す前にこのクラスで読み込む。
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::editor {

// HDRI の生ピクセルデータ。IblBaker::Bake() に渡すための一時データ。
// デストラクタが stb_image_free / free を呼ぶためライフタイムに注意。
struct HdriPixels {
    // pixels は常に RGBA float (4 ch) として返される。
    // デリーターで正しい解放関数を呼ぶ。
    std::unique_ptr<float[], void(*)(void*)> data{ nullptr, nullptr };
    uint32_t width    = 0;
    uint32_t height   = 0;

    bool IsValid() const { return data && width > 0 && height > 0; }

    // IblBakeInput::pixels に渡す raw ポインター
    const float* Pixels() const { return data.get(); }
};

class HdriLoader {
public:
    // absPath の拡張子 (.hdr / .exr) に応じて適切なローダーを自動選択する。
    // 失敗時は IsValid() == false の HdriPixels を返す。
    [[nodiscard]] static HdriPixels Load(const std::string& absPath);
};

} // namespace fbzz::editor
