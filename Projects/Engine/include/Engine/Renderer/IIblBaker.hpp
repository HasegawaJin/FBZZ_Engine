/// @file    IIblBaker.hpp
/// @brief   IBL ベイク処理の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// 責務:
/// HDRI float ピクセルデータを受け取り、4 枚の DDS ファイルを outputDir に生成する。
/// 処理は GPU Compute Shader で行う。Runtime は生成済み DDS を LoadTexture で読むだけ。
///
/// 取得方法:
/// IRenderer::CreateIblBaker(compiledShadersDir) で DX11 実装を得る。
/// compiledShadersDir は Assets/Shaders/compiled/ の絶対パス。
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::renderer {

// ベイクの入力パラメーター
struct IblBakeInput {
    // RGBA float ピクセル配列 (stb_image stbi_loadf / TinyEXR LoadEXR の出力をそのまま渡す)
    const float* pixels    = nullptr;
    uint32_t     equirectW = 0;
    uint32_t     equirectH = 0;

    // 出力品質設定
    uint32_t envCubemapSize      = 2048; // Environment Cubemap の 1 辺 (px)
    uint32_t irradianceSize      = 32;   // Irradiance Cubemap の 1 辺 (px) — 32 で十分
    uint32_t prefilteredSize     = 512;  // Prefiltered Cubemap mip0 の 1 辺 (px)
    uint32_t prefilteredMipCount = 5;    // Prefiltered mip 数 (roughness 0→1 を均等分割)
    uint32_t sampleCount         = 1024; // PrefilteredEnvMap.cs の GGX IS サンプル数
    uint32_t brdfLutSize         = 256;  // BRDF LUT の 1 辺 (px)

    // コンパイル済みシェーダー (.cso) ディレクトリの絶対パス
    // 例: "C:/proj/Assets/Shaders/compiled/"
    std::string compiledShadersDir;
};

// ベイク結果 — 生成された DDS ファイルの絶対パス
struct IblBakeOutput {
    std::string envCubemapPath;     // *_env.dds
    std::string irradiancePath;     // *_irr.dds
    std::string prefilteredPath;    // *_prefilter.dds
    std::string brdfLutPath;        // *_brdf.dds
    uint32_t    prefilteredMipCount = 0;
};

// IBL ベイク処理の抽象インターフェース
// DX11 実装 (DX11IblBaker) は IRenderer::CreateIblBaker() で生成する。
class IIblBaker {
public:
    virtual ~IIblBaker() = default;

    // HDRI → DDS × 4 のベイク処理
    // outputDir : DDS ファイルを書き出すディレクトリ (存在しない場合は生成する)
    // baseName  : ファイル名の共通プレフィックス (例: "sky" → sky_env.dds 等)
    // 失敗時は false を返す。例外は投げない。
    [[nodiscard]] virtual bool Bake(
        const IblBakeInput& input,
        const std::string&  outputDir,
        const std::string&  baseName,
        IblBakeOutput&      output) = 0;
};

} // namespace fbzz::renderer
