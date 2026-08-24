// FBZZ Engine
// TextureAnalysis.hpp | fbzz::asset
// VFX 素材テクスチャを解析し、オーサリング設定の根拠になる特徴量を取り出す
//
// WHY: エフェクトの見た目を決める設定 (blendMode / alphaSource / sprite グリッド /
//      softParticles) は、どれも「その素材がどういう絵か」で正解が変わる。
//      これまで素材はパス文字列でしかなく、中身を見る手段が「エディターで開いて目視」
//      しか無かった。AI に至っては目視すらできないため、ファイル名から推測して
//      blendMode を決め、プレビュー画像が変になってから初めて気づく、という
//      収束しない反復に陥っていた。
//
//      画素を数えれば機械的に決まる項目は多い:
//        - アルファチャンネルが実データを持つか (無ければ alphaSource=Luminance が必須)
//        - RGB > A の画素があるか (= 事前乗算済み。Premultiplied 以外では黒枠が出る)
//        - タイル境界の不連続から flipbook のコマ割り
//        - 中心に輝度のピークがあるか (発光する芯 = Additive 向き)
//        - 縁のアルファ勾配 (硬い縁なら softParticles が効く)
//      これらを返せば、設定は推測ではなく観測から決まる。
//
// NOTE: DirectXTex は Engine に閉じているため、解析本体はここに置き
//       Editor と MCP の双方から呼ぶ (FlipbookMotionVectors と同じ方針)。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

// flipbook のコマ割り候補。スコアが低いほど「そのグリッドで切ると境界が自然」。
struct FlipbookGridCandidate {
    int columns = 1;
    int rows = 1;
    // タイル境界をまたぐ画素差の平均。連続した絵を誤って切ると跳ね上がる。
    float seamScore = 0.0f;
    // 全コマが同程度の内容量を持つか [0,1]。1 に近いほど均質 = アトラスらしい。
    float uniformity = 0.0f;
};

struct TextureAnalysis {
    bool success = false;
    std::string message;      // 失敗理由 / 成功時の要約
    std::string resolvedPath; // 実際に読んだファイルパス

    int width = 0;
    int height = 0;
    bool powerOfTwo = false;

    // ── アルファ ──
    // hasAlphaChannel: フォーマット上アルファを持つか
    // alphaIsMeaningful: 実際に値のばらつきがあるか (全画素 1.0 なら実質不透明)
    bool hasAlphaChannel = false;
    bool alphaIsMeaningful = false;
    float alphaMin = 0.0f;
    float alphaMax = 0.0f;
    float alphaMean = 0.0f;
    // アルファがほぼ 0 / ほぼ 1 の画素の割合。二値的なマスクかグラデーションかを見る。
    float transparentRatio = 0.0f;
    float opaqueRatio = 0.0f;
    // 事前乗算済み (RGB <= A が全画素で成り立つ) か。
    // WHY: 事前乗算素材を Alpha ブレンドで使うと縁が黒く縁取られ、
    //      逆にストレート素材を Premultiplied で使うと芯が飛ぶ。見ただけでは判らない。
    bool likelyPremultiplied = false;

    // ── 輝度・色 ──
    float luminanceMean = 0.0f;
    float luminanceMax = 0.0f;
    // アルファで重み付けした平均色。素材そのものの色味 (透明部分の色は無視する)。
    float averageColor[3] = { 0.0f, 0.0f, 0.0f };
    // 中心付近の輝度が周辺よりどれだけ高いか。発光する芯を持つ素材で大きくなる。
    float coreHotspot = 0.0f;
    // 彩度の平均。0 に近いとグレースケール素材で、色は色ゆらぎ/グラデーション側で作る。
    float saturationMean = 0.0f;

    // ── 形状 ──
    // 不透明部分が占める割合。overdraw の見積もりに使う。
    float coverage = 0.0f;
    // 中心対称性 [0,1]。1 に近いと放射状の puff / glow。
    float radialSymmetry = 0.0f;
    // 縁のアルファ勾配の平均。大きいほど硬い縁 (切り抜き素材)。
    float edgeHardness = 0.0f;
    // 左右端・上下端の一致度 [0,1]。1 に近いとタイリング可能。
    float seamlessScore = 0.0f;

    // ── flipbook ──
    // 上位候補 (最大 4 件)。先頭が最有力。1x1 しか返らなければ単一画像。
    std::vector<FlipbookGridCandidate> flipbookCandidates;

    // ── 推奨設定 ──
    // 上の観測から機械的に決まるオーサリング値。AI はこれをそのまま
    // vfx_node_set_field へ流せる (schemaPath と値の組)。
    struct Recommendation {
        std::string schemaPath; // 例 "particle.blendMode"
        std::string value;      // JSON 表記の値 ("0" / "true" / "\"...\"")
        std::string reason;     // なぜそう決まるか (人が読んで検証できる根拠)
    };
    std::vector<Recommendation> recommendations;
    // 素材の分類 ("glow" / "smoke" / "spark" / "flipbook" / "mask" / "unknown")。
    // 層構成 (vfx.guide の recipe) のどこへ置く素材かを一言で示す。
    std::string classification;
};

// テクスチャを 1 枚読み、解析結果を返す。
// maxSampleDimension: 解析前に縮小する上限辺長 [px]。4K 素材をそのまま走査すると
//   エディター操作としては遅すぎるため、統計量が保たれる範囲で縮めてから見る。
//   flipbook 判定だけは縮小するとコマ境界が潰れるので原寸で行う。
[[nodiscard]] TextureAnalysis AnalyzeTexture(const std::string& sourcePath,
                                             int maxSampleDimension = 512);

// ── マテリアル解析 ──
//
// WHY: パーティクルの «見た目» — ブレンド・アルファの取り出し方・フリップブック・歪み・
//      煙・自己影 — は .mat (blend_mode と [particle]) が唯一の正本で、ParticleEmitter は
//      «いつどこに出すか» しか持たない。さらにテクスチャも .mat の albedo スロットで
//      一元管理するため、テクスチャ単体の解析だけでは推奨が実際の描画と噛み合わない。
//      .mat まで読んで初めて「この素材をこの設定で描くと何が起きるか」が言える。

// .mat とその albedo テクスチャを併せて見た結果。
struct MaterialAnalysis {
    bool success = false;
    std::string message;
    std::string resolvedPath;

    std::string shaderPath;
    // "Opaque" / "Alpha" / "Additive" / "Premultiplied"。
    std::string blendMode;
    // "auto" / "deferred" / "forward" / "particle" / "trail"。
    // particle 以外を ParticleEmitter へ割り当てると描画パスが噛み合わない。
    std::string renderPath;
    bool doubleSided = false;
    bool depthWrite = true;
    int renderQueue = 0;

    // albedo スロットのテクスチャ。空なら .mat が絵を持たない (色だけのマテリアル)。
    std::string albedoTexturePath;
    bool hasAlbedoTexture = false;
    // albedo テクスチャの解析。hasAlbedoTexture が false なら success=false のまま。
    TextureAnalysis albedoAnalysis;

    // ── 突き合わせの結果 ──
    // .mat の blendMode と、テクスチャの中身から導かれる blendMode が食い違うか。
    bool blendModeConflictsWithTexture = false;
    // 食い違いや設定ミスの説明。空なら健全。
    std::vector<std::string> findings;
    // ParticleEmitter へ割り当てたときに実際に効く設定 (AI と Inspector の推奨の正本)。
    std::vector<TextureAnalysis::Recommendation> recommendations;
};

// .mat を読み、albedo テクスチャがあればそれも解析して突き合わせる。
[[nodiscard]] MaterialAnalysis AnalyzeMaterial(const std::string& materialPath);

} // namespace fbzz::asset
