// FBZZ Engine
// MaterialAsset.hpp | fbzz::asset
// .mat マテリアルアセットのランタイム表現と TOML 入出力 API
#pragma once
#include <Engine/Renderer/RenderLayer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

// fzmat の render_path フィールドが取れる値。
// WHY: カスタムシェーダーのレンダーパス振り分けをエンジンコード変更なしに制御するため。
//      "auto"     → IsForwardOnlyShader() 名前チェックにフォールバック (従来動作)。
//      "forward"  → 常に Forward パスで描画 (カスタムエフェクト・Unlit 系)。
//      "deferred" → 常に GBuffer Deferred パスで描画 (PBR 系)。
//      "particle" → ParticleEmitter 専用。ParticlePass が albedo テクスチャと blendMode を参照する。
//      "trail"    → TrailComponent / MeshTrailComponent 専用。TrailRenderPass が albedo テクスチャを参照する。
enum class RenderPath { Auto, Deferred, Forward, Particle, Trail };

// fzmat の mesh_type フィールドが取れる値。
// WHY: Surface シェーダー判定をパス文字列検索から fzmat 宣言へ移し、
//      任意フォルダのカスタムシェーダーでも SkinnedMeshRenderer への誤アサイン警告が機能するようにする。
//      "surface" → static mesh 専用 (Skinned に割り当てると警告 + SkinnedPBR フォールバック)。
//      "skinned" → rigged mesh 専用。
//      "any"     → 両方可 (カスタムシェーダーのデフォルト)。
enum class MeshType { Any, Surface, Skinned };

// 深度テスト比較関数
enum class DepthTest { Always, Never, Less, LessEqual, Equal, Greater, GreaterEqual, NotEqual };

// .mat の内容を保持する共有マテリアルデータ。
// WHY: GameObject ごとの MaterialComponent に見た目の値を複製せず、同一ファイルを参照する全オブジェクトへ
//      編集結果を即時反映できるようにする。
struct MaterialAsset {
    std::string shaderPath;
    renderer::BlendMode blendMode = renderer::BlendMode::OPAQUE_BLEND;
    bool doubleSided  = false;
    bool depthWrite   = true;
    DepthTest depthTest = DepthTest::LessEqual;
    int32_t renderQueue = renderer::RenderQueue::GEOMETRY;
    RenderPath renderPath = RenderPath::Auto;
    MeshType   meshType   = MeshType::Any;

    // slot 名 → assets/ 相対パス（.tex descriptor または .png/.dds 直参照、どちらも可）
    std::unordered_map<std::string, std::string> textures;

    // シェーダー変数名 → float 要素列。float / float2 / float3 / float4 を同じ形式で保存する。
    // WHY: 保存時に ShaderDescriptor のバイトオフセットへ依存させず、ロード後に現在のシェーダーへ名前で束縛する。
    std::unordered_map<std::string, std::vector<float>> params;

    // シェーダーコンパイル時 define 一覧（例: "USE_NORMAL_MAP", "USE_AO"）
    std::vector<std::string> keywords;
};

// TOML から .mat を読み込む。破損・未存在時は false を返し、例外は使わない。
[[nodiscard]] bool LoadMaterialAssetFromFile(std::string_view path, MaterialAsset& outAsset);

// .mat を TOML へ保存する。Editor からの Save とテンプレート生成で共有する。
[[nodiscard]] bool SaveMaterialAssetToFile(std::string_view path, const MaterialAsset& asset);

} // namespace fbzz::asset
