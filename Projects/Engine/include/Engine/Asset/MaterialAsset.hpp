/// @file    MaterialAsset.hpp
/// @brief   .mat マテリアルアセットのランタイム表現と TOML 入出力 API。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once
#include <Engine/Asset/ParticleMaterialSettings.hpp>
#include <Engine/Renderer/RenderLayer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Graphics/RayTracing/SurfaceMaterialData.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

/// @note fzmat の render_path フィールドが取れる値。
/// @note "auto"     → 通常の材質。Forward / Deferred はプロジェクト設定に従う
/// @note "particle" → ParticleEmitter 専用。ParticlePass が albedo テクスチャと blendMode を参照する
/// @note "trail"    → TrailComponent / MeshTrailComponent 専用。albedo テクスチャを参照する
/// @note "ui"       → UIImage 専用。b0 が UIConstants になるため他用途の .mat は宣言で弾く
/// @note "decal"    → DecalComponent 専用。頂点入力を持たないため他用途の .mat は宣言で弾く
/// @note Forward/Deferred はここへ持たない。描画経路は RenderSettings::pipeline がプロジェクト
/// @note 全体で決める責務で、Material ごとに上書きするとライティング/GBuffer の前提が混在する。
enum class RenderPath { Auto, Particle, Trail, UI, Decal, PostProcess };

/// @note fzmat の mesh_type フィールドが取れる値。
/// @note Surface シェーダー判定をパス文字列検索から fzmat 宣言へ移し、任意フォルダの
/// @note カスタムシェーダーでも SkinnedMeshRenderer への誤アサイン警告が機能するようにする。
/// @note "surface" → static mesh 専用 (Skinned に割り当てると警告 + SkinnedPBR へ置換)。
/// @note "skinned" → rigged mesh 専用。
/// @note "any"     → 両方可 (カスタムシェーダーのデフォルト)。
enum class MeshType { Any, Surface, Skinned };

/// @note 深度テスト比較関数
enum class DepthTest { Always, Never, Less, LessEqual, Equal, Greater, GreaterEqual, NotEqual };

/// @brief .mat の内容を保持する共有マテリアルデータ。
/// @note GameObject ごとの MaterialComponent に見た目の値を複製せず、同一ファイルを参照する
/// @note 全オブジェクトへ編集結果を即時反映する。
struct MaterialAsset {
    std::string shaderPath;
    renderer::BlendMode blendMode = renderer::BlendMode::OPAQUE_BLEND;
    bool doubleSided  = false;
    bool depthWrite   = true;
    DepthTest depthTest = DepthTest::LessEqual;
    /// @brief 深度バイアス (定数項 / 傾き項)。正で手前へ寄せ、同一平面に重なった面の Z-fighting を片側勝ちにする。
    /// @see renderer::PipelineStateDesc::depthBias
    int32_t depthBias      = 0;
    float   depthBiasSlope = 0.0f;
    int32_t renderQueue = renderer::RenderQueue::GEOMETRY;
    RenderPath renderPath = RenderPath::Auto;
    MeshType   meshType   = MeshType::Any;

    /// @note slot 名 → assets/ 相対パス（元画像 .png/.dds 等を直参照。インポート設定は隣の .meta が担う）
    std::unordered_map<std::string, std::string> textures;

    /// @note シェーダー変数名 → float 要素列。float / float2 / float3 / float4 を同じ形式で保存する。
    /// @note 保存時に ShaderDescriptor のバイトオフセットへ依存させず、ロード後に現在のシェーダーへ名前で束縛する。
    std::unordered_map<std::string, std::vector<float>> params;
    /// @note int/uint/bool は float を経由せず保持する。同名キーは params と排他的に書き込む。
    std::unordered_map<std::string, std::vector<int64_t>> integerParams;

    /// @note シェーダーコンパイル時 define 一覧（例: "USE_NORMAL_MAP", "USE_AO"）
    std::vector<std::string> keywords;

    /// @note [particle] テーブル。render_path = "particle" のときだけ意味を持つ。
    /// @note パーティクルの見た目 (ブレンド以外) の正本。詳細と «なぜ [params] ではないか» は
    /// @note ParticleMaterialSettings.hpp を参照。
    ParticleMaterialSettings particle;
    /// @note [dielectric] は alpha 被覆と独立。未記載の旧材質は光透過を持たない。
    renderer::SolidDielectricSettings dielectric;
};

/// @note TOML から .mat を読み込む。破損・未存在時は false を返し、例外は使わない。
[[nodiscard]] bool LoadMaterialAssetFromFile(std::string_view path, MaterialAsset& outAsset);

/// @note .mat を TOML へ保存する。Editor からの Save とテンプレート生成で共有する。
[[nodiscard]] bool SaveMaterialAssetToFile(std::string_view path, const MaterialAsset& asset);

} /// @note namespace fbzz::asset
