/// @file    MaterialPreviewCore.hpp
/// @brief   .mat を 1 枚絵へ焼くための共通コア (AssetBrowser サムネイル / Preview パネル兼用)。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @note AssetBrowser のサムネイルと Inspector / Preview パネルは別実装で、パラメータ別名の解決・
///       テクスチャスロット・照明リグ・Terrain / Water の定数バッファが二重にあり «同じ .mat なのに
///       置き場所で見た目が違う» が起きていた。焼く処理はここ 1 本に閉じ、呼び出し側は «どこに出すか» だけ持つ。
#pragma once
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
struct Mesh;
}

namespace fbzz::editor::matpreview {

/// .mat をどう焼くかの分類。頂点入力と定数バッファの組み合わせが変わる。
/// @note エフェクト系の材質はメッシュ用の頂点レイアウトも定数バッファも共有しない。«とりあえず球へ»
///       流すと不正な IA レイアウトで崩れるため、どの絵をどの形で焼くかをここで決め切る。
enum class Flavor {
    Surface,     ///< 標準の MeshRenderer 材質
    Skinned,     ///< SkinnedMeshRenderer 材質 (ボーン行列を単位で埋める)
    Terrain,     ///< Terrain シェーダー (専用 ObjectCB + レイヤーテクスチャ)
    Water,       ///< Water シェーダー (専用 ObjectCB + 時間アニメ)
    Ui,          ///< UI マテリアル (ortho + UIConstants)
    Particle,    ///< ビルボード粒子 (ParticleVertex + b11)
    Trail,       ///< リボン (TrailVertex + b2 は TrailCB)
    Decal,       ///< 受け面へ投影 (受け面の深度 + b10)
    PostProcess, ///< 全画面 (シーンを焼いてから b5 で通す)
    Unsupported, ///< シェーダーが引けない等、最後の受け皿
    Fiber,       ///< 表面繊維 (土台の単色メッシュ + Shell / Fin / Blade を b5 / b10 付きで重ねる)
};

/// @brief Fiber を焼く描画方式。FiberComponent::m_mode と同じ 4 種。
/// @note Fiber の .mat は方式を持たない (方式はコンポーネント側)。プレビューで切り替えて見比べる。
enum class FiberMode {
    Shell,
    Fin,
    Hybrid,
    Blade,
};

/// プレビューに使う形状。Skinned / Water は同じ形状の専用頂点レイアウト版を使う。
enum class Shape {
    Sphere,
    Cube,
    Plane,
    Quad,
    Cylinder,
    Cone,
    Torus,
    Capsule,
};

/// 表示チャンネル。Shaded 以外は Debug/MaterialChannel.hlsl へ差し替えて焼く。
enum class Channel {
    Shaded,
    Albedo,
    Normal,
    Roughness,
    Metallic,
    Occlusion,
    Emissive,
    Uv,
    Wireframe, ///< 材質のシェーダーのまま RasterizerMode だけ差し替える
};

/// 照明リグのプリセット。
enum class LightPreset {
    Studio,  ///< キー暖色 + 寒色フィル + リム (既定。サムネイルと同一)
    Outdoor, ///< 強い太陽 + 空色アンビエント
    Night,   ///< 低キー + 強いリム。発光やリムライトの確認用
    Flat,    ///< 高アンビエント・陰影ほぼ無し。テクスチャの絵柄を読む用
};

/// 照明リグの向きと露出。
struct Rig {
    LightPreset preset   = LightPreset::Studio;
    float       yaw      = 0.0f; ///< リグ全体の水平回転 [rad]
    float       pitch    = 0.0f; ///< リグ全体の垂直回転 [rad]
    float       exposure = 1.0f; ///< 全ライトとアンビエントに掛かる倍率
};

/// 被写体を見る軌道カメラ。distance はメッシュ境界半径に対する倍率。
///
/// 既定値は AssetBrowser のサムネイルと同じ 3/4 視点。
/// (半径 0.5 の球で position = center + (-1.06, +0.64, -1.45) になる)
struct Orbit {
    float yaw      = -2.51034f;
    float pitch    = 0.342295f;
    float distance = 3.8135f;
};

/// .mat 1 件ぶんの GPU 側キャッシュ。シェーダーを差し替えたら作り直す。
struct GpuData {
    std::string                                                shaderPath;
    renderer::ResourceHandle<renderer::ShaderTag>              shader;
    renderer::ResourceHandle<renderer::ConstantBufferTag>      materialCB;
    std::vector<renderer::ResourceHandle<renderer::TextureTag>> textures;
    std::vector<std::uint8_t>                                  paramData;
};


[[nodiscard]] Flavor      DetectFlavor(const asset::MaterialAsset& material);
/// 3D へ焼けない .mat の種別バッジ ("PARTICLE" / "DECAL" …)。
[[nodiscard]] const char* UnsupportedBadge(const asset::MaterialAsset& material);
/// 色見本に使う 1 色。HDR は明るさだけ畳む。
[[nodiscard]] math::Vector4 SwatchColor(const asset::MaterialAsset& material);
/// フォールバック描画に使うベースカラー。
[[nodiscard]] math::Vector4 AlbedoColor(const asset::MaterialAsset& material);
/// «何の絵か» を代表する 1 枚のテクスチャパス (Assets/ 相対)。無ければ空。
[[nodiscard]] std::string   RepresentativeTexturePath(const asset::MaterialAsset& material);
/// Assets/ 相対パスを ResourceManager が読める実パスへ直す。
[[nodiscard]] std::string   TextureLoadPath(const std::string& path, std::string_view projectRoot);

[[nodiscard]] const char* ShapeLabel(Shape shape);
[[nodiscard]] const char* FiberModeLabel(FiberMode mode);
/// @return シェーダー名 (FiberFin / FiberBlade) から推した方式。それ以外は Shell。
[[nodiscard]] FiberMode   DefaultFiberMode(const asset::MaterialAsset& material);
/// @return サムネイルに使う形状。草 (grassShading >= 0.5) の Fiber は平面、それ以外は球。
[[nodiscard]] Shape       ThumbnailShape(const asset::MaterialAsset& material, Flavor flavor);
[[nodiscard]] const char* ChannelLabel(Channel channel);
[[nodiscard]] const char* LightPresetLabel(LightPreset preset);
/// Terrain / Water / UI はテクスチャスロットの意味が標準と違うため、
/// Shaded と Wireframe しか出せない。
[[nodiscard]] bool ChannelSupported(Flavor flavor, Channel channel);


/// .mat の shaderPath / ShaderDescriptor に合わせて Material CB とテクスチャを埋める。
[[nodiscard]] bool BuildGpuData(GpuData& gpu,
                                const asset::MaterialAsset& material,
                                renderer::ResourceManager& resources,
                                std::string_view projectRoot);

/// シェーダー・CB・テクスチャを捨てて次回作り直させる。
void ResetGpuData(GpuData& gpu, renderer::ResourceManager* resources);

/// 形状と Flavor に対応する頂点レイアウトのメッシュを返す (プロセス内キャッシュ)。
/// Terrain / Water は形状指定を無視して細分割した平面を返す。
/// Ui / Particle / Trail / Decal / PostProcess / Unsupported は nullptr。
/// これらは Render が専用の形 (矩形・ビルボード・リボン・全画面) を内部で持つ。
[[nodiscard]] renderer::Mesh* ShapeMesh(renderer::ResourceManager& resources,
                                        Shape shape,
                                        Flavor flavor);

/// Render が 3D メッシュを要らない Flavor か。呼び出し側の «メッシュが無いから焼けない»
/// 判定に使う。
[[nodiscard]] bool UsesOwnGeometry(Flavor flavor);


struct RenderDesc {
    renderer::ResourceHandle<renderer::RenderTargetTag> target;
    const renderer::Mesh*       mesh     = nullptr;
    Flavor                      flavor   = Flavor::Surface;
    const asset::MaterialAsset* material = nullptr;
    /// null なら fallbackColor / fallbackTexture を Lit.hlsl で焼く (メッシュサムネイル用)。
    const GpuData*              gpu      = nullptr;

    renderer::ResourceHandle<renderer::TextureTag> fallbackTexture;
    math::Vector4 fallbackColor{ 0.74f, 0.78f, 0.84f, 1.0f };

    Channel channel = Channel::Shaded;
    Rig     rig{};
    Orbit   orbit{};

    /// false で RT を消さずに重ね描きする (.fbx のサブメッシュ合成)。
    bool          clear          = true;
    math::Vector3 overrideCenter{};
    float         overrideRadius = -1.0f; ///< < 0 でメッシュ境界を使う
    float         time           = 0.0f;  ///< Water の波・Fiber の突風 [秒]

    /// @name Fiber のときだけ使う
    /// @note 既定値は FiberComponent の既定と同じ。風は +X 方向のワールド [m/s]。
    /// @{
    FiberMode fiberMode        = FiberMode::Shell;
    int       fiberShellCount  = 24;
    float     fiberWind        = 0.0f;
    float     fiberBladeDensity = 400.0f;
    float     fiberBladeWidth  = 0.015f;
    /// @}
};

[[nodiscard]] bool Render(renderer::IRenderer& renderer,
                          renderer::ResourceManager& resources,
                          const RenderDesc& desc);

} // namespace fbzz::editor::matpreview
