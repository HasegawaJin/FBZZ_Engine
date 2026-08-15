// FBZZ Engine
// VFXRecipeLibrary.hpp | fbzz::editor
// 代表的なエフェクトの層構成 (recipe) と、そこからグラフを組み立てる生成器
// WHY: 新規 .vfx は Entry ノード 1 個から始まる。ところが VFX で最も難しいのは
//      「どの層を、どの順で、どのブレンドで重ねるか」であって、ノードを置く作業ではない。
//      層構成は vfx.guide が AI へ散文で渡していたが、人間側には同じものが無く、
//      Editor では毎回ゼロから積み直していた。
// NOTE: この表が recipe の唯一の正本。vfx.guide (AI) も Recipe ウィザード (人間) も
//       ここを読む。別々に持つと「guide は煙を要求するが生成器は作らない」という
//       食い違いが必ず生まれる (テクスチャ解析・警告と同じ方針)。
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

// 素材の役割と、それを満たすテクスチャ分類 (AnalyzeTexture の classification)。
// vfx.assetSurvey の充足判定もこの表を読む。
struct VFXAssetRoleInfo {
    const char* role;           // asset::kVFXAssetRoles と同じ語彙
    const char* classification; // AnalyzeTexture の classification
    const char* purpose;        // 無いと何が作れないか
    const char* fallback;       // 無い場合の代替案
};

[[nodiscard]] std::span<const VFXAssetRoleInfo> GetVFXAssetRoles();

// 層 1 枚の骨格。素材はロールで指名し、実体はプロジェクトの手持ちから選ぶ。
struct VFXRecipeLayer {
    const char* name;
    asset::VFXNodeType nodeType;
    // 必要な素材ロール ("core" / "body" / "sparks" / "animated")。空なら素材不要。
    const char* assetRole;
    float startDelay;   // Entry からの遅延 [秒]
    float duration;     // ノードの生存時間 [秒]
    int   particleCount; // Particle 層の maxParticles (規模でスケールする)
    int   burstCount;   // >0 なら t=0 の burst、0 なら emitRate による連続放出
    float emitRate;     // burstCount == 0 のときの毎秒放出数
    int   blendMode;    // scene::ParticleBlendMode
    int   sortMode;     // scene::ParticleSortMode
    int   shape;        // scene::ParticleEmitterShape
    float shapeRadius;
    float sizeStart;
    float sizeEnd;
    float lifetime;
    float velocitySpread;
    float gravityY;
    float emissiveScale;
    int   renderPriority; // 小さいほど奥
    bool  softParticles;
    bool  sixWayLighting;
    bool  distortion;
    // 層の役割。グループ枠の note とツールチップへそのまま出す。
    const char* note;
};

struct VFXRecipe {
    const char* name;
    const char* summary;
    // vfx.guide が AI へ返す散文表現。layerSpecs と同じ内容を人が読む形にしたもの。
    const char* layers;
    std::span<const VFXRecipeLayer> layerSpecs;
    bool loopByDefault;
    // 追加要素の既定。Impact 系はシェイク込みが自然、Fire 系はループが自然。
    bool wantsLight;
    bool wantsCameraShake;
};

[[nodiscard]] std::span<const VFXRecipe> GetVFXRecipes();
[[nodiscard]] const VFXRecipe* FindVFXRecipe(std::string_view name);

// recipe が要求する素材ロールの一覧 (重複なし)。Template の requiredRoles を作るのに使う。
[[nodiscard]] std::vector<std::string> CollectRecipeRoles(const VFXRecipe& recipe);

struct VFXRecipeBuildOptions {
    // 規模倍率。S=0.6 / M=1.0 / L=1.6 を想定。サイズ・粒子数・力の強さへ効く。
    float scale = 1.0f;
    bool loop = false;
    bool includeLight = true;
    bool includeCameraShake = false;
    // ロール -> 実在するテクスチャのプロジェクト相対パス。
    // 埋まっていないロールの層はテクスチャ未設定で作る (置いてから割り当てられる)。
    std::unordered_map<std::string, std::string> roleTextures;
    // 生成物の名前。空なら recipe 名。
    std::string graphName;
};

// recipe からグラフを組み立てる。返るグラフは必ず Entry を 1 個持ち、
// 全ての層が Entry から到達可能で、budget は実使用量へ合わせてある。
[[nodiscard]] asset::VFXGraphAsset BuildGraphFromRecipe(const VFXRecipe& recipe,
                                                        const VFXRecipeBuildOptions& options);

} // namespace fbzz::editor
