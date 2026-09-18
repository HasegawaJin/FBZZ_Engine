/// @file    PresentationComponents.hpp
/// @brief   2Dスプライト、線、ビルボード、投影表示に使う汎用描画Component。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

/// CPU で組み直すメッシュを 2 枚持ち、書くたびに入れ替える置き場。
/// PresentationSystem が Sprite / Line の帯を焼くのに使う (シーンへは保存しない)。
/// @note 2 枚必要: DX12 は BeginFrame が 2 フレーム前のフェンスまでしか待たないため、1 枚だと
///       GPU が読んでいる最中の頂点を上書きする (DX11 は WRITE_DISCARD で無害、DX12 でのみ壊れる)。
/// @note 毎フレーム作り直さない: 以前は Release+CreateBuffer を焼くたびに行い、電極 61 本で
///       15ms 掛かっていた。unique_ptr 単独所有にして、複製時は「まだ焼いていない」状態から
///       始める (次フレームで焼き直す)。所有者は常に 1 つなので shared_ptr の参照カウントは不要。
struct DoubleBufferedMesh {
    std::unique_ptr<renderer::Mesh> slots[2];
    /// 入力が変わっていないかの判定に使う。変わらなければ書き直さない。
    std::size_t signature = 0;
    /// 今 slots のどちらを描いているか。
    std::uint32_t current = 0;

    DoubleBufferedMesh() = default;
    ~DoubleBufferedMesh() = default;
    DoubleBufferedMesh(DoubleBufferedMesh&&) noexcept = default;
    DoubleBufferedMesh& operator=(DoubleBufferedMesh&&) noexcept = default;

    /// @note コピーで中身を連れていかないのは、Component の複製・プレファブ展開が値コピーのため。
    ///       GPU バッファまで写すと 2 つの GameObject が同じバッファへ交互に書き、片方が畳まれた
    ///       時点でもう片方が消えたバッファを描く。複製先は «まだ焼いていない» 状態から始める。
    DoubleBufferedMesh(const DoubleBufferedMesh&) {}
    DoubleBufferedMesh& operator=(const DoubleBufferedMesh&) { return *this; }

    [[nodiscard]] renderer::Mesh* Current() const { return slots[current].get(); }
    [[nodiscard]] bool HasMesh() const { return static_cast<bool>(slots[current]); }
};

enum class SpriteDrawMode : int { Simple = 0, Sliced = 1, Tiled = 2 };

struct SpriteRendererComponent {
    bool enabled = true;
    std::string spritePath;
    std::string materialPath = "Assets/Materials/Fallback/SpriteFallback.mat";
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector2 size = { 1.0f, 1.0f };
    math::Vector2 pivot = { 0.5f, 0.5f };
    /// size と pivot を Sprite の .meta から取る (Pixels Per Unit と Pivot)。
    /// @note 既定 false: 既存シーンの size / pivot は手で決めた値で、素材由来の値へ勝手に
    ///       置き換えると全部の見た目が変わる。
    /// @note 1 回きりのボタンではなく常時追従にする: Sprite Editor で Pivot を直したら、
    ///       その絵を使う全オブジェクトが直ってほしいため。
    bool useSpriteNativeSize = false;
    SpriteDrawMode drawMode = SpriteDrawMode::Simple;
    int sortingLayer = 0;
    int orderInLayer = 0;
    bool flipX = false;
    bool flipY = false;
    bool receiveLighting = false;
    DoubleBufferedMesh runtimeMesh;

    const char* GetTypeName() const { return "Sprite Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("spritePath", spritePath);
        r.Field("materialPath", materialPath);
        r.ColorField("color", color);
        r.Field("useSpriteNativeSize", useSpriteNativeSize);
        r.Tooltip("Sprite の Pixels Per Unit と Pivot から size / pivot を毎フレーム求めます");
        r.FieldIf("size", size, !useSpriteNativeSize);
        r.FieldIf("pivot", pivot, !useSpriteNativeSize);
        int mode = static_cast<int>(drawMode);
        static constexpr const char* MODES[] = { "Simple", "Sliced", "Tiled" };
        r.Enum("drawMode", mode, MODES);
        drawMode = static_cast<SpriteDrawMode>(mode < 0 || mode > 2 ? 0 : mode);
        r.Field("sortingLayer", sortingLayer);
        r.Field("orderInLayer", orderInLayer);
        r.Field("flipX", flipX);
        r.Field("flipY", flipY);
        r.Field("receiveLighting", receiveLighting);
    }
};

struct SortingGroupComponent {
    bool enabled = true;
    int sortingLayer = 0;
    int orderInLayer = 0;

    const char* GetTypeName() const { return "Sorting Group"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("sortingLayer", sortingLayer);
        r.Field("orderInLayer", orderInLayer);
    }
};

enum class LineSpace : int { Local = 0, World = 1 };

/// 点列を «どんな断面で» 押し出すか。
/// @note Tube を足したのは、Ribbon (板 1 枚をカメラへ向ける) だと «太さのあるもの» (レーザー筒・
///       パイプ) が視線が寄ると潰れ、地形へ突き刺さる端が板の切り口として出て刺さって見えないため。
/// @note Ribbon も残すのは、軌跡・放電・UI の線は «常にこちらを向く» ことが読みやすさそのもので、
///       Tube にすると細い線ほど画面上で消えるため。用途が違うので選択式にする。
enum class LineShape : int { Ribbon = 0, Tube = 1 };

struct LineRendererComponent {
    bool enabled = true;
    std::vector<math::Vector3> points;
    std::string materialPath = "Assets/Materials/Fallback/VFXMeshFallback.mat";
    math::Vector4 startColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 endColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    float startWidth = 0.05f;
    float endWidth = 0.05f;
    LineSpace space = LineSpace::Local;
    bool loop = false;
    /// Ribbon のときだけ意味を持つ。Tube は形が視点に依存しない。
    bool billboard = true;
    LineShape shape = LineShape::Ribbon;
    /// Tube の円周分割数。小さいと角が見え、大きくしても遠目には変わらない。
    int radialSegments = 8;
    int sortingLayer = 0;
    int orderInLayer = 0;
    DoubleBufferedMesh runtimeMesh;

    const char* GetTypeName() const { return "Line Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.ListField("points", points);
        r.Field("materialPath", materialPath);
        r.ColorField("startColor", startColor);
        r.ColorField("endColor", endColor);
        r.Field("startWidth", startWidth);
        r.Field("endWidth", endWidth);
        int value = static_cast<int>(space);
        static constexpr const char* SPACES[] = { "Local", "World" };
        r.Enum("space", value, SPACES);
        space = static_cast<LineSpace>(value < 0 || value > 1 ? 0 : value);
        r.Field("loop", loop);
        r.Field("billboard", billboard);
        int shapeValue = static_cast<int>(shape);
        static constexpr const char* SHAPES[] = { "Ribbon", "Tube" };
        r.Enum("shape", shapeValue, SHAPES);
        shape = static_cast<LineShape>(shapeValue < 0 || shapeValue > 1 ? 0 : shapeValue);
        r.Field("radialSegments", radialSegments);
        r.Field("sortingLayer", sortingLayer);
        r.Field("orderInLayer", orderInLayer);
    }
};

enum class BillboardMode : int { FaceCamera = 0, YAxisOnly = 1, MatchCamera = 2 };

struct BillboardComponent {
    bool enabled = true;
    BillboardMode mode = BillboardMode::FaceCamera;
    bool reverseForward = false;

    const char* GetTypeName() const { return "Billboard"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int value = static_cast<int>(mode);
        static constexpr const char* MODES[] = { "Face Camera", "Y Axis Only", "Match Camera" };
        r.Enum("mode", value, MODES);
        mode = static_cast<BillboardMode>(value < 0 || value > 2 ? 0 : value);
        r.Field("reverseForward", reverseForward);
    }
};

enum class ProjectorShape : int { Box = 0, Perspective = 1 };

struct ProjectorComponent {
    bool enabled = true;
    ProjectorShape shape = ProjectorShape::Box;
    /// render_path = "decal" の .mat はシェーダーごと DecalComponent へ渡る。
    /// それ以外の .mat は albedo / normal / emissive のテクスチャだけを抜き出して
    /// 組み込みデカール描画へ載せる (旧来の Projector 設定を壊さないため)。
    std::string materialPath;
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    float fieldOfView = 45.0f;
    float nearClip = 0.01f;
    float farClip = 10.0f;
    int receiverLayerMask = -1;
    std::string runtimeLoadedMaterialPath;

    const char* GetTypeName() const { return "Projector"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int value = static_cast<int>(shape);
        static constexpr const char* SHAPES[] = { "Box", "Perspective" };
        r.Enum("shape", value, SHAPES);
        shape = static_cast<ProjectorShape>(value < 0 || value > 1 ? 0 : value);
        r.Field("materialPath", materialPath);
        r.Tooltip("投影するマテリアル (.mat)。render_path = \"decal\" ならシェーダーごと使い、"
                  "それ以外は albedo / normal / emissive のテクスチャだけを取り出します");
        r.ColorField("color", color);
        r.FloatRange("fieldOfView", fieldOfView, 1.0f, 179.0f);
        r.Field("nearClip", nearClip);
        r.Field("farClip", farClip);
        r.Field("receiverLayerMask", receiverLayerMask);
    }
};

} // namespace fbzz::scene
