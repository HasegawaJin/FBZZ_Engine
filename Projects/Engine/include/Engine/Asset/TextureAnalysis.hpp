/// @file    TextureAnalysis.hpp
/// @brief   VFX 素材テクスチャを解析し、オーサリング設定の根拠になる特徴量を取り出す。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note blendMode / alphaSource / sprite グリッド / softParticles は、アルファの有無・事前乗算・
///       flipbook のコマ割り・発光の芯・縁の硬さを画素から機械的に測れば推測せずに決まる。
/// @note DirectXTex は Engine に閉じるため、解析本体はここに置き Editor と MCP の双方から呼ぶ。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

/// @brief flipbook のコマ割り候補。スコアが低いほど「そのグリッドで切ると境界が自然」。
struct FlipbookGridCandidate {
    int columns = 1;
    int rows = 1;
    float seamScore = 0.0f;  ///< タイル境界をまたぐ画素差の平均。連続した絵を誤って切ると跳ね上がる。
    float uniformity = 0.0f; ///< 全コマが同程度の内容量を持つか [0,1]。1 に近いほど均質 = アトラスらしい。
};

struct TextureAnalysis {
    bool success = false;
    std::string message;      ///< 失敗理由 / 成功時の要約
    std::string resolvedPath; ///< 実際に読んだファイルパス

    int width = 0;
    int height = 0;
    bool powerOfTwo = false;

    /// @name アルファ
    /// hasAlphaChannel はフォーマット上アルファを持つか、alphaIsMeaningful は実際に値のばらつきが
    /// あるか (全画素 1.0 なら実質不透明)。
    /// @{
    bool hasAlphaChannel = false;
    bool alphaIsMeaningful = false;
    float alphaMin = 0.0f;
    float alphaMax = 0.0f;
    float alphaMean = 0.0f;
    /// アルファがほぼ 0 / ほぼ 1 の画素の割合。二値的なマスクかグラデーションかを見る。
    float transparentRatio = 0.0f;
    float opaqueRatio = 0.0f;
    /// 事前乗算済み (RGB <= A が全画素で成り立つ) か。
    /// @note 事前乗算素材を Alpha ブレンドで使うと縁が黒く縁取られ、逆にストレート素材を
    ///       Premultiplied で使うと芯が飛ぶ。見ただけでは判らない。
    bool likelyPremultiplied = false;
    /// @}

    /// @name 輝度・色
    /// @{
    float luminanceMean = 0.0f;
    float luminanceMax = 0.0f;
    float averageColor[3] = { 0.0f, 0.0f, 0.0f }; ///< アルファで重み付けした平均色 (透明部分の色は無視する)。
    float coreHotspot = 0.0f;    ///< 中心付近の輝度が周辺よりどれだけ高いか。発光する芯を持つ素材で大きくなる。
    float saturationMean = 0.0f; ///< 彩度の平均。0 に近いとグレースケール素材 (色は色ゆらぎ/グラデーション側で作る)。
    /// @}

    /// @name 形状
    /// @{
    float coverage = 0.0f;       ///< 不透明部分が占める割合。overdraw の見積もりに使う。
    float radialSymmetry = 0.0f; ///< 中心対称性 [0,1]。1 に近いと放射状の puff / glow。
    float edgeHardness = 0.0f;   ///< 縁のアルファ勾配の平均。大きいほど硬い縁 (切り抜き素材)。
    float seamlessScore = 0.0f;  ///< 左右端・上下端の一致度 [0,1]。1 に近いとタイリング可能。
    /// @}

    /// 上位候補 (最大 4 件)。先頭が最有力。1x1 しか返らなければ単一画像。
    std::vector<FlipbookGridCandidate> flipbookCandidates;

    /// 上の観測から機械的に決まるオーサリング値。AI はそのまま vfx_node_set_field へ流せる (schemaPath と値の組)。
    struct Recommendation {
        std::string schemaPath; ///< 例 "particle.blendMode"
        std::string value;      ///< JSON 表記の値 ("0" / "true" / "\"...\"")
        std::string reason;     ///< なぜそう決まるか (人が読んで検証できる根拠)
    };
    std::vector<Recommendation> recommendations;
    /// 素材の分類 ("glow" / "smoke" / "spark" / "flipbook" / "mask" / "unknown")。
    /// 層構成 (vfx.guide の recipe) のどこへ置く素材かを一言で示す。
    std::string classification;
};

/// @brief テクスチャを 1 枚読み、解析結果を返す。
/// @param maxSampleDimension 解析前に縮小する上限辺長 [px]。4K 素材を原寸で走査すると遅いため、
///        統計量が保たれる範囲で縮めてから見る。flipbook 判定だけはコマ境界が潰れるため原寸で行う。
[[nodiscard]] TextureAnalysis AnalyzeTexture(const std::string& sourcePath,
                                             int maxSampleDimension = 512);

/// @name マテリアル解析
/// パーティクルの見た目 (ブレンド・アルファの取り出し方・フリップブック・歪み・煙・自己影) は
/// .mat (blend_mode と [particle]) が唯一の正本で、ParticleEmitter は「いつどこに出すか」しか
/// 持たない。テクスチャも albedo スロットで一元管理するため、.mat まで読まないと推奨が実際の
/// 描画と噛み合わない。
/// @{

/// @brief .mat とその albedo テクスチャを併せて見た結果。
struct MaterialAnalysis {
    bool success = false;
    std::string message;
    std::string resolvedPath;

    std::string shaderPath;
    std::string blendMode; ///< "Opaque" / "Alpha" / "Additive" / "Premultiplied"
    /// "auto" / "deferred" / "forward" / "particle" / "trail"。particle 以外を ParticleEmitter へ
    /// 割り当てると描画パスが噛み合わない。
    std::string renderPath;
    bool doubleSided = false;
    bool depthWrite = true;
    int renderQueue = 0;

    std::string albedoTexturePath; ///< albedo スロットのテクスチャ。空なら .mat が絵を持たない (色だけのマテリアル)。
    bool hasAlbedoTexture = false;
    TextureAnalysis albedoAnalysis; ///< albedo テクスチャの解析。hasAlbedoTexture が false なら success=false のまま。

    bool blendModeConflictsWithTexture = false; ///< .mat の blendMode とテクスチャの中身から導かれる blendMode が食い違うか。
    std::vector<std::string> findings; ///< 食い違いや設定ミスの説明。空なら健全。
    std::vector<TextureAnalysis::Recommendation> recommendations; ///< ParticleEmitter へ割り当てたときに実際に効く設定。
};

/// @brief .mat を読み、albedo テクスチャがあればそれも解析して突き合わせる。
[[nodiscard]] MaterialAnalysis AnalyzeMaterial(const std::string& materialPath);
/// @}

} // namespace fbzz::asset
