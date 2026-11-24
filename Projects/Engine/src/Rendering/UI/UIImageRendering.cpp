/// @file    UIImageRendering.cpp
/// @brief   画像の素材解決、9-slice・タイル・塗り潰しと描画の登録。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "UIRendering.hpp"

#include "../../Scene/Systems/UI/UIRuntimeGeometry.hpp"

#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Asset/Texture/TextureAsset.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene::ui_render_detail {

namespace {
using namespace ui_runtime_detail;

/// @note 借用した頂点バッファは拡張できないため、扇形や入れ子マスクによる頂点増加に備える。
constexpr uint32_t IMAGE_VERTEX_CAPACITY = 64;
constexpr float RADIAL_PI = 3.14159265358979323846f;

struct ImageDrawStyle {
    math::Vector4 tint;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    UIMaterialBinding* materialBinding = nullptr;
    const UIImage* materialOverrideSource = nullptr;
};

/// @note vertices の uv は「このセルの中の 0..1」。アトラス上の位置は uvMin/uvMax が持つ。
void SubmitSpriteTriangles(const CanvasDrawContext& draw, const UIVertex2D* vertices, uint32_t vertexCount,
    const math::Vector2& cellSize, const math::Vector2& uvMin, const math::Vector2& uvMax,
    const ImageDrawStyle& style)
{
    if (vertexCount < 3 || vertexCount > IMAGE_VERTEX_CAPACITY) return;

    /// @note この DrawCall 専用の頂点バッファを借りる (UISystemContext::vertexPool 参照)。
    const auto vertexBuffer = draw.context.vertexPool.Acquire(
        draw.resources, IMAGE_VERTEX_CAPACITY, static_cast<uint32_t>(sizeof(UIVertex2D)));
    if (!vertexBuffer.IsValid()) return;
    draw.resources.Update(vertexBuffer, vertices, vertexCount * sizeof(UIVertex2D));

    UIConstantsCB constants{};
    constants.ortho  = draw.canvasToClip;
    constants.color = style.tint;
    constants.uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
    /// @note 9-slice やタイルはセルごとにここへ来るので、セルの寸法がそのまま渡る。
    /// @note 角丸マテリアルを Sliced / Tiled と併用してはいけないのはこのため。
    constants.rect   = { cellSize.x, cellSize.y, 0.0f, 0.0f };
    draw.resources.Update(draw.context.constants, &constants, sizeof(constants));

    renderer::DrawCall drawCall;
    drawCall.vertexBuffer       = vertexBuffer;
    drawCall.shader             = draw.context.shader;
    drawCall.pipelineState      = draw.pipelineState;
    drawCall.constantBuffers[0] = draw.context.constants;
    drawCall.vertexCount        = vertexCount;
    drawCall.layer              = draw.layer;
    drawCall.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    drawCall.textures[0] = style.texture.IsValid() ? style.texture : draw.context.whiteTexture;

    if (style.materialBinding && style.materialBinding->valid) {
        BindUIMaterial(draw, *style.materialBinding, drawCall, 0);
        /// @note 共有 .mat を適用したうえに、この要素だけの差分を重ねる。
        if (style.materialOverrideSource)
            ApplyUIMaterialOverrides(draw.resources, *style.materialBinding, *style.materialOverrideSource, drawCall);
    }
    draw.device.Submit(drawCall, draw.resources);
}

/// @note 凸多角形を 1 枚。いま積まれているクリップ面で削ってから三角形へ開く。
void SubmitClippedPolygon(const CanvasDrawContext& draw, const ClipVertex* polygon, int polygonVertexCount,
    const math::Vector2& cellSize, const math::Vector2& uvMin, const math::Vector2& uvMax,
    const ImageDrawStyle& style)
{
    if (polygonVertexCount < 3) return;

    ClipVertex clippedPolygon[MAX_CLIP_POLYGON_VERTICES];
    const int clippedVertexCount = ClipPolygonToPlanes(draw.context.clipPlanes, polygon, polygonVertexCount,
        clippedPolygon, MAX_CLIP_POLYGON_VERTICES);
    if (clippedVertexCount < 3) return;

    /// @note 扇状に開く。凸多角形なので、どの頂点を軸にしても裏返らない。
    UIVertex2D vertices[IMAGE_VERTEX_CAPACITY];
    uint32_t vertexCount = 0;
    for (int triangleIndex = 1; triangleIndex + 1 < clippedVertexCount; ++triangleIndex) {
        if (vertexCount + 3 > IMAGE_VERTEX_CAPACITY) break;
        const auto& first = clippedPolygon[0];
        const auto& second = clippedPolygon[triangleIndex];
        const auto& third = clippedPolygon[triangleIndex + 1];
        vertices[vertexCount++] = { first.position, first.uv, first.color };
        vertices[vertexCount++] = { second.position, second.uv, second.color };
        vertices[vertexCount++] = { third.position, third.uv, third.color };
    }
    SubmitSpriteTriangles(draw, vertices, vertexCount, cellSize, uvMin, uvMax, style);
}

/// @name 画像セル
/// @note 9-slice もタイルも「元画像の一部を要素の一部へ貼る」の繰り返し。セルへ揃えることで
/// @note 塗り潰しのクリップを 1 か所に書けば Simple / Sliced / Tiled 全部に効く。
/// @note pos は要素ローカル (左上原点・回転前)。回転は描画直前にまとめて掛ける。
struct ImageCell {
    math::Vector2 pos;
    math::Vector2 size;
    math::Vector2 uvMin;
    math::Vector2 uvMax;
};

/// @note 1 要素を描くのに要る、セル間で変わらない値。セル単位の関数へ 14 個の引数を並べると
/// @note 順番違いが型で止まらないので束ねる。
struct ImageDrawContext {
    const CanvasDrawContext&         canvas;
    math::Vector2                    origin;      ///< @note 要素の左上 (Canvas 空間)
    math::Vector2                    elementSize;
    float                            cosRotation;
    float                            sinRotation;
    ImageDrawStyle                   style;
};

/// @note 要素ローカル座標を Canvas 座標へ。回転の中心は常に要素全体の中心。
/// @note セルごとに回すと 9-slice の四隅がばらばらに回って継ぎ目が開く。
math::Vector2 LocalToCanvas(const ImageDrawContext& draw, const math::Vector2& localPosition)
{
    const float centeredX = localPosition.x - draw.elementSize.x * 0.5f;
    const float centeredY = localPosition.y - draw.elementSize.y * 0.5f;
    return { draw.origin.x + draw.elementSize.x * 0.5f + centeredX * draw.cosRotation - centeredY * draw.sinRotation,
             draw.origin.y + draw.elementSize.y * 0.5f + centeredX * draw.sinRotation + centeredY * draw.cosRotation };
}

void SubmitCell(const ImageDrawContext& draw, const ImageCell& cell)
{
    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) return;
    const ClipVertex polygon[4] = {
        { LocalToCanvas(draw, cell.pos), { 0.0f, 0.0f } },
        { LocalToCanvas(draw, { cell.pos.x, cell.pos.y + cell.size.y }), { 0.0f, 1.0f } },
        { LocalToCanvas(draw, { cell.pos.x + cell.size.x, cell.pos.y + cell.size.y }),
          { 1.0f, 1.0f } },
        { LocalToCanvas(draw, { cell.pos.x + cell.size.x, cell.pos.y }), { 1.0f, 0.0f } },
    };
    SubmitClippedPolygon(draw.canvas, polygon, 4, cell.size, cell.uvMin, cell.uvMax, draw.style);
}

/// @note 9-slice の 1 軸ぶんの境界 (4 本) と、対応する UV を作る。
/// @note multiplier が描画側だけを拡縮するので、元画像側と描画側は比例しない。
void BuildSliceAxis(float elementLength, float uvLow, float uvHigh, float texturePixels,
                    float borderLow, float borderHigh, float unitScale,
                    float* outBoundaries, float* outUVs)
{
    const float sourceLength = (uvHigh - uvLow) * texturePixels;
    /// @note 余白どうしが重なると中央が裏返る。まず元画像側で収める。
    const float sourceLeadingBorder  = std::clamp(borderLow, 0.0f, sourceLength * 0.5f);
    const float sourceTrailingBorder = std::clamp(borderHigh, 0.0f, sourceLength - sourceLeadingBorder);
    /// @note 描画側は要素をはみ出せない。狭い要素では余白どうしが先に突き当たる。
    const float targetLeadingBorder  = std::min(sourceLeadingBorder * unitScale, elementLength * 0.5f);
    const float targetTrailingBorder = std::min(sourceTrailingBorder * unitScale, elementLength - targetLeadingBorder);

    outBoundaries[0] = 0.0f;
    outBoundaries[1] = targetLeadingBorder;
    outBoundaries[2] = elementLength - targetTrailingBorder;
    outBoundaries[3] = elementLength;
    outUVs[0] = uvLow;
    outUVs[1] = uvLow + sourceLeadingBorder / texturePixels;
    outUVs[2] = uvHigh - sourceTrailingBorder / texturePixels;
    outUVs[3] = uvHigh;
}

/// @note 1 つの領域を step 間隔で敷き詰め、タイル 1 枚ずつを emit へ渡す。
/// @note step が 0 以下の軸は分割しない (辺の帯を一方向だけタイルするのに使う)。
/// @note 数ピクセルの素材を全画面へ敷くと 1 要素で数万ドローになるので上限を置き、
/// @note 当たったらタイルを引き伸ばす。密度はずれるが隙間が空くよりは意図が伝わる。
constexpr int MAX_TILES_PER_AXIS   = 128;
constexpr int MAX_TILES_PER_REGION = 4096;

template<typename EmitFn>
void EmitTiledRegion(const ImageCell& region, float stepX, float stepY, EmitFn& emit)
{
    if (region.size.x <= 0.0f || region.size.y <= 0.0f) return;

    const auto axisCount = [](float length, float step) {
        if (step <= 0.0f) return 1;
        return std::clamp(static_cast<int>(std::ceil(length / step)), 1, MAX_TILES_PER_AXIS);
    };
    int countX = axisCount(region.size.x, stepX);
    int countY = axisCount(region.size.y, stepY);
    if (countX * countY > MAX_TILES_PER_REGION) {
        /// @note 縦横の比を保ったまま総数を落とす。片方だけ削ると模様の目が伸びる。
        const float shrink = std::sqrt(
            static_cast<float>(countX * countY) / static_cast<float>(MAX_TILES_PER_REGION));
        countX = (std::max)(1, static_cast<int>(static_cast<float>(countX) / shrink));
        countY = (std::max)(1, static_cast<int>(static_cast<float>(countY) / shrink));
    }
    /// @note 上限に当たった軸は「領域を count 等分した幅」まで広げる。当たっていなければ step のまま。
    const float tileWidth = (std::max)(stepX, region.size.x / static_cast<float>(countX));
    const float tileHeight = (std::max)(stepY, region.size.y / static_cast<float>(countY));

    const float uvWidth = region.uvMax.x - region.uvMin.x;
    const float uvHeight = region.uvMax.y - region.uvMin.y;
    for (int row = 0; row < countY; ++row) {
        for (int column = 0; column < countX; ++column) {
            ImageCell tile;
            tile.pos = { region.pos.x + tileWidth * static_cast<float>(column),
                         region.pos.y + tileHeight * static_cast<float>(row) };
            /// @note 端の 1 枚は途中で切れる。切れた分だけ UV も詰める。
            tile.size = {
                (std::min)(tileWidth, region.pos.x + region.size.x - tile.pos.x),
                (std::min)(tileHeight, region.pos.y + region.size.y - tile.pos.y)
            };
            if (tile.size.x <= 0.0f || tile.size.y <= 0.0f) continue;
            tile.uvMin = region.uvMin;
            tile.uvMax = { region.uvMin.x + uvWidth * (tile.size.x / tileWidth),
                           region.uvMin.y + uvHeight * (tile.size.y / tileHeight) };
            emit(tile);
        }
    }
}

/// @note imageType に応じてセルを列挙する。
/// @note Simple … 1 枚
/// @note Sliced … 最大 9 枚 (四隅は原寸、辺と中央は引き伸ばし)
/// @note Tiled  … Border があれば四隅は原寸のまま辺と中央をタイル、無ければ全面をタイル
template<typename EmitFn>
void ForEachImageCell(const UIImage& image, const math::Vector2& size,
                      const math::Vector2& uvMin, const math::Vector2& uvMax,
                      EmitFn&& emit)
{
    const math::Vector2 textureSize = image.resolvedTextureSize;
    const float unitScale = image.UnitScale();
    const bool hasTextureSize = textureSize.x > 0.0f && textureSize.y > 0.0f;
    const bool sliceable = image.imageType == UIImageType::Sliced
                        || image.imageType == UIImageType::Tiled;

    if (sliceable && hasTextureSize) {
        const math::Vector4 border = image.EffectiveBorder();
        /// @note 余白が全部 0 なら格子を作る意味が無い。Sliced は 1 枚、Tiled は全面タイルへ落ちる。
        if (border.x > 0.0f || border.y > 0.0f || border.z > 0.0f || border.w > 0.0f) {
            float x[4], u[4], y[4], v[4];
            BuildSliceAxis(size.x, uvMin.x, uvMax.x, textureSize.x,
                           border.x, border.z, unitScale, x, u);
            BuildSliceAxis(size.y, uvMin.y, uvMax.y, textureSize.y,
                           border.y, border.w, unitScale, y, v);
            /// @note 中央列 / 中央行の元画像での寸法。Tiled のタイル 1 枚の大きさになる。
            const float centerStepX = (u[2] - u[1]) * textureSize.x * unitScale;
            const float centerStepY = (v[2] - v[1]) * textureSize.y * unitScale;
            const bool tiled = image.imageType == UIImageType::Tiled;

            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    const ImageCell cell{
                        { x[column], y[row] },
                        { x[column + 1] - x[column], y[row + 1] - y[row] },
                        { u[column], v[row] },
                        { u[column + 1], v[row + 1] }
                    };
                    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) continue;
                    if (!tiled) { emit(cell); continue; }
                    /// @note 四隅は原寸のまま。伸びる方向だけをタイルへ置き換える。
                    EmitTiledRegion(cell,
                                    column == 1 ? centerStepX : 0.0f,
                                    row    == 1 ? centerStepY : 0.0f,
                                    emit);
                }
            }
            return;
        }
    }

    if (image.imageType == UIImageType::Tiled) {
        const math::Vector2 tile = { image.resolvedSizePixels.x * unitScale,
                                     image.resolvedSizePixels.y * unitScale };
        if (tile.x > 0.0f && tile.y > 0.0f) {
            EmitTiledRegion(ImageCell{ { 0.0f, 0.0f }, size, uvMin, uvMax },
                            tile.x, tile.y, emit);
            return;
        }
    }

    emit(ImageCell{ { 0.0f, 0.0f }, size, uvMin, uvMax });
}

/// @note 塗り潰し (Edge) でセルを削る。何も残らなければ false。

/// @note 要素全体を 1 枚のセルとして渡せば、分割前と同じ「矩形と UV を端から削る」動きになる。
bool ClipCellToEdgeFill(ImageCell& cell, const math::Vector2& elementSize,
                        float amount, UIImageFillOrigin origin)
{
    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) return false;
    const float fillFraction = std::clamp(amount, 0.0f, 1.0f);

    float keepLeft = 0.0f, keepTop = 0.0f;
    float keepRight = elementSize.x, keepBottom = elementSize.y;
    switch (origin) {
    case UIImageFillOrigin::Left:   keepRight  = elementSize.x * fillFraction;          break;
    case UIImageFillOrigin::Right:  keepLeft   = elementSize.x * (1.0f - fillFraction); break;
    case UIImageFillOrigin::Bottom: keepTop    = elementSize.y * (1.0f - fillFraction); break;
    case UIImageFillOrigin::Top:    keepBottom = elementSize.y * fillFraction;          break;
    }

    const float clippedLeft = std::max(cell.pos.x, keepLeft);
    const float clippedRight = std::min(cell.pos.x + cell.size.x, keepRight);
    const float clippedTop = std::max(cell.pos.y, keepTop);
    const float clippedBottom = std::min(cell.pos.y + cell.size.y, keepBottom);
    if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) return false;

    const float uvWidth = cell.uvMax.x - cell.uvMin.x;
    const float uvHeight = cell.uvMax.y - cell.uvMin.y;
    const math::Vector2 uvMin = {
        cell.uvMin.x + uvWidth * ((clippedLeft - cell.pos.x) / cell.size.x),
        cell.uvMin.y + uvHeight * ((clippedTop - cell.pos.y) / cell.size.y)
    };
    const math::Vector2 uvMax = {
        cell.uvMin.x + uvWidth * ((clippedRight - cell.pos.x) / cell.size.x),
        cell.uvMin.y + uvHeight * ((clippedBottom - cell.pos.y) / cell.size.y)
    };
    cell = ImageCell{ { clippedLeft, clippedTop }, { clippedRight - clippedLeft, clippedBottom - clippedTop }, uvMin, uvMax };
    return true;
}

/// @name Radial 塗り潰し
/// @note 180 度以下の扇形は「中心を通る 2 つの半平面の共通部分」なので、セルをその 2 枚で
/// @note 切れば残りがそのまま描く形になる。近似ではないのでどの角度でも輪郭が正確に出る。

/// @note fillOrigin が指す辺の方向を角度で返す。Canvas は y が下向きなので、
/// @note 角度が増える向き = 画面上の時計回りになる。
float FillOriginAngle(UIImageFillOrigin origin)
{
    switch (origin) {
    case UIImageFillOrigin::Right:  return 0.0f;
    case UIImageFillOrigin::Bottom: return RADIAL_PI * 0.5f;
    case UIImageFillOrigin::Left:   return RADIAL_PI;
    case UIImageFillOrigin::Top:    return -RADIAL_PI * 0.5f;
    default: break;
    }
    return 0.0f;
}

/// @note 扇形の定義。amount = 1 でちょうど要素全体を覆う位置と角度になる。
struct RadialSector {
    math::Vector2 apex{};        ///< @note 要素ローカルの回転軸
    float startAngle = 0.0f;     ///< @note fillAmount = 0 のときの縁
    float fullSweep  = 0.0f;     ///< @note amount = 1 での角度 (符号は回転方向)
};

/// @note 90 / 180 は「四半円 / 半円で要素をちょうど覆う」形。中心を軸にしたまま角度だけ
/// @note 絞ると要素の一部しか覆えないので、軸を角 (90) や辺の中点 (180) へ移す。
RadialSector BuildRadialSector(UIImageFillMethod method, UIImageFillOrigin origin,
                               bool clockwise, const math::Vector2& size)
{
    const float direction = clockwise ? 1.0f : -1.0f;
    const UIImageFillOrigin resolved = NormalizeFillOrigin(method, origin);

    RadialSector sector;
    switch (method) {
    case UIImageFillMethod::Radial90: {
        /// @note 角を軸に、そこから伸びる 2 辺の間を四半周する。
        struct Corner { math::Vector2 apex; float baseAngle; };
        const Corner corner = [&]() -> Corner {
            switch (resolved) {
            case UIImageFillOrigin::TopLeft:     return { { 0.0f,   0.0f   },  0.0f };
            case UIImageFillOrigin::TopRight:    return { { size.x, 0.0f   },  RADIAL_PI * 0.5f };
            case UIImageFillOrigin::BottomRight: return { { size.x, size.y },  RADIAL_PI };
            default:                             return { { 0.0f,   size.y }, -RADIAL_PI * 0.5f };
            }
        }();
        sector.apex = corner.apex;
        /// @note 覆う範囲は [baseAngle, baseAngle + 90 度]。どちら端から満ちるかだけが向きで変わる。
        sector.startAngle = clockwise ? corner.baseAngle : corner.baseAngle + RADIAL_PI * 0.5f;
        sector.fullSweep  = direction * RADIAL_PI * 0.5f;
        break;
    }
    case UIImageFillMethod::Radial180: {
        /// @note 辺の中点を軸に、その辺に沿って半周する。内側へ向かう法線を必ず通る。
        struct Edge { math::Vector2 apex; float inwardAngle; };
        const Edge edge = [&]() -> Edge {
            switch (resolved) {
            case UIImageFillOrigin::Top:    return { { size.x * 0.5f, 0.0f   },  RADIAL_PI * 0.5f };
            case UIImageFillOrigin::Left:   return { { 0.0f,   size.y * 0.5f },  0.0f };
            case UIImageFillOrigin::Right:  return { { size.x, size.y * 0.5f },  RADIAL_PI };
            default:                        return { { size.x * 0.5f, size.y }, -RADIAL_PI * 0.5f };
            }
        }();
        sector.apex = edge.apex;
        sector.startAngle = edge.inwardAngle - direction * RADIAL_PI * 0.5f;
        sector.fullSweep  = direction * RADIAL_PI;
        break;
    }
    case UIImageFillMethod::Radial360:
    default:
        sector.apex = { size.x * 0.5f, size.y * 0.5f };
        sector.startAngle = FillOriginAngle(resolved);
        sector.fullSweep  = direction * RADIAL_PI * 2.0f;
        break;
    }
    return sector;
}

void SubmitCellRadial(const ImageDrawContext& draw, const ImageCell& cell,
                      float amount, const RadialSector& sector)
{
    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) return;
    const float signedSweep = std::clamp(amount, 0.0f, 1.0f) * sector.fullSweep;
    const float sweep = std::fabs(signedSweep);
    if (sweep <= 0.0f) return;
    const bool clockwise = signedSweep > 0.0f;

    const ClipVertex polygon[4] = {
        { { cell.pos.x,                cell.pos.y                }, { 0.0f, 0.0f } },
        { { cell.pos.x,                cell.pos.y + cell.size.y  }, { 0.0f, 1.0f } },
        { { cell.pos.x + cell.size.x,  cell.pos.y + cell.size.y  }, { 1.0f, 1.0f } },
        { { cell.pos.x + cell.size.x,  cell.pos.y                }, { 1.0f, 0.0f } },
    };
    /// @note セルが軸を含まなくても、半平面の切り取りは同じ式で効く。
    const math::Vector2 apex = sector.apex;
    const float base = sector.startAngle;
    /// @note 180 度を超える扇形は 2 枚の半平面では表せない。等分して 2 区画に分ける。
    const int sectionCount = sweep > RADIAL_PI ? 2 : 1;
    const float sectionSweep = sweep / static_cast<float>(sectionCount);

    /// @note 隣り合う区画は 1 本の境界を共有する。区画ごとに角度を足し引きすると
    /// @note 丸め誤差で 2 本に割れ、境目に隙間か二重塗りの筋が出る。
    float sectionAngles[3] = {};
    for (int i = 0; i <= sectionCount; ++i) {
        const float offset = sectionSweep * static_cast<float>(i);
        sectionAngles[i] = clockwise ? base + offset : base - offset;
    }

    /// @note 区画ごとに submit する。2 区画をまとめて 1 つの多角形にはできない
    /// @note (合わせると凹むことがある)。区画は最大 2 つなので、ドローは高々 2 本。
    for (int i = 0; i < sectionCount; ++i) {
        /// @note 切り取りは常に「角度の小さい側 → 大きい側」で渡す。
        const float low  = clockwise ? sectionAngles[i]     : sectionAngles[i + 1];
        const float high = clockwise ? sectionAngles[i + 1] : sectionAngles[i];
        const math::Vector2 dirLow  = { std::cosf(low),  std::sinf(low)  };
        const math::Vector2 dirHigh = { std::cosf(high), std::sinf(high) };

        /// @note 内側の条件は cross(dirLow, v) >= 0 かつ cross(dirHigh, v) <= 0。
        /// @note cross(d, v) は dot(v, (-d.y, d.x)) と同じなので、法線 2 本に直せる。
        ClipVertex work[MAX_CLIP_POLYGON_VERTICES];
        ClipVertex sectionPolygon[MAX_CLIP_POLYGON_VERTICES];
        int clippedCount = ClipHalfPlane(polygon, 4, apex, { -dirLow.y, dirLow.x },
                              work, MAX_CLIP_POLYGON_VERTICES);
        clippedCount = ClipHalfPlane(work, clippedCount, apex, { dirHigh.y, -dirHigh.x },
                          sectionPolygon, MAX_CLIP_POLYGON_VERTICES);
        if (clippedCount < 3) continue;

        /// @note 扇形は要素ローカルで解いてある。Mask のクリップは Canvas 空間なので、
        /// @note 渡す前にここで写す (2 つの座標系を 1 つの関数に混ぜない)。
        for (int v = 0; v < clippedCount; ++v)
            sectionPolygon[v].position = LocalToCanvas(draw, sectionPolygon[v].position);

        SubmitClippedPolygon(draw.canvas, sectionPolygon, clippedCount, cell.size,
            cell.uvMin, cell.uvMax, draw.style);
    }
}

/// @note UIButton の状態別スプライトを、同じ GameObject の UIImage へ反映する。
/// @note 差し替えは「今どの状態か」の関数でしかないので描画側で当てる。イベント側で書くと、
/// @note 状態が変わらなかったフレームや Play していない Editor で絵が付かない。
void ApplyButtonSpriteSwap(UIImage& image, const UIButton* button)
{
    /// @note 空欄の状態は差し替えない = UIImage に指定された絵のまま。
    const std::string* spriteOverride = nullptr;
    if (button != nullptr) {
        if (!button->enabled || !button->isInteractable) {
            if (!button->disabledSprite.empty())    spriteOverride = &button->disabledSprite;
            else if (!button->normalSprite.empty()) spriteOverride = &button->normalSprite;
        } else if (button->state == UIButtonState::PRESSED && !button->pressedSprite.empty()) {
            spriteOverride = &button->pressedSprite;
        } else if (button->state == UIButtonState::HOVERED && !button->hoveredSprite.empty()) {
            spriteOverride = &button->hoveredSprite;
        } else if (!button->normalSprite.empty()) {
            spriteOverride = &button->normalSprite;
        }
    }

    if (spriteOverride == nullptr) {
        image.hasTextureOverride = false;
        image.overrideTexturePath.clear();
        return;
    }
    image.hasTextureOverride = true;
    if (image.overrideTexturePath != *spriteOverride) image.overrideTexturePath = *spriteOverride;
}

/// @note texturePath から GPU テクスチャと、Sprite サブアセットの切り出しを解決する。
/// @note テクスチャは非同期に読まれるので要求したフレームには寸法が無く、UV も Border も
/// @note 出せない。1 回きりの解決にするとその状態が固定されて絵が伸びたままになる。
void ResolveImageTexture(UIImage& image, renderer::ResourceManager& resources)
{
    const std::string& textureReference = image.EffectiveTexturePath();
    if (textureReference.empty()) {
        if (image.loadedTexturePath.empty()) return;
        image.texture = {};
        image.loadedTexturePath.clear();
        image.hasResolvedSprite = false;
        image.resolvedSpriteUvMin = { 0.0f, 0.0f };
        image.resolvedSpriteUvMax = { 1.0f, 1.0f };
        image.resolvedSpriteBorder = {};
        image.resolvedTextureSize = {};
        image.resolvedSizePixels = {};
        return;
    }
    uint32_t sourceWidth = 0, sourceHeight = 0;
    image.texture = asset::StreamedTextureResolver::Engine().ResolveGpuWithSourceSize(
        resources, textureReference, sourceWidth, sourceHeight);
    const float width = static_cast<float>(sourceWidth);
    const float height = static_cast<float>(sourceHeight);
    const asset::ResolvedSprite resolvedSprite =
        asset::ResolveSpriteReference(textureReference, width, height);

    image.resolvedTextureSize  = { width, height };
    image.resolvedSizePixels   = resolvedSprite.sizePixels;
    image.resolvedSpriteUvMin  = resolvedSprite.uvMin;
    image.resolvedSpriteUvMax  = resolvedSprite.uvMax;
    image.resolvedSpriteBorder = resolvedSprite.border;
    image.hasResolvedSprite    = resolvedSprite.resolved;
    image.loadedTexturePath    = textureReference;
}

} /// @note namespace

void SubmitSolidQuad(const CanvasDrawContext& draw, const math::Vector2& position, const math::Vector2& size,
    const math::Vector4& color, float rotationZ)
{
    const float halfWidth = size.x * 0.5f;
    const float halfHeight = size.y * 0.5f;
    const float centerX = position.x + halfWidth;
    const float centerY = position.y + halfHeight;
    const float cosRotation = std::cosf(rotationZ);
    const float sinRotation = std::sinf(rotationZ);
    const auto rotateAroundCenter = [&](float localX, float localY) -> math::Vector2 {
        return { centerX + localX * cosRotation - localY * sinRotation,
                 centerY + localX * sinRotation + localY * cosRotation };
    };
    const ClipVertex polygon[4] = {
        { rotateAroundCenter(-halfWidth, -halfHeight), { 0.0f, 0.0f } },
        { rotateAroundCenter(-halfWidth,  halfHeight), { 0.0f, 1.0f } },
        { rotateAroundCenter( halfWidth,  halfHeight), { 1.0f, 1.0f } },
        { rotateAroundCenter( halfWidth, -halfHeight), { 1.0f, 0.0f } },
    };
    SubmitClippedPolygon(draw, polygon, 4, size, { 0.0f, 0.0f }, { 1.0f, 1.0f },
        ImageDrawStyle{ color, draw.context.whiteTexture });
}

void DrawImageElement(const CanvasDrawContext& canvas, GameObject& element, UIImage& image, const UIButton* button,
    const UITransform2D& transform, float groupOpacity, const asset::UIThemeStyle* theme)
{
    ApplyButtonSpriteSwap(image, button);
    ResolveImageTexture(image, canvas.resources);

    const Rect imageRect = RectFromTransform(element, transform);
    math::Vector4 tint = ResolveUIImageColor(image, theme);
    if (button) {
        const auto* navigation = element.GetComponent<UINavigation>();
        tint = ResolveUIButtonImageColor(tint, *button, theme,
            navigation && navigation->enabled && navigation->focused);
    }
    /// @note 群の透明度はここで乗せる。コンポーネントの値は書き換えない ─
    /// @note 書き換えるとフェードの途中で保存したときに薄い色が焼き付く。
    tint.w *= groupOpacity;

    const math::Vector2 uvMin = image.hasResolvedSprite
        ? image.resolvedSpriteUvMin : image.uvMin;
    const math::Vector2 uvMax = image.hasResolvedSprite
        ? image.resolvedSpriteUvMax : image.uvMax;
    const float fillAmount = std::clamp(image.fillAmount, 0.0f, 1.0f);

    if (imageRect.size.x > 0.0f && imageRect.size.y > 0.0f && fillAmount > 0.0f) {
        /// @note 未設定 (圧倒的多数) なら nullptr が返り、以降は従来どおりの経路になる。
        UIMaterialBinding* materialBinding =
            ResolveUIMaterial(canvas.context, canvas.resources, image.materialPath);
        const ImageDrawContext draw{
            canvas,
            imageRect.pos, imageRect.size,
            std::cosf(transform.rotationZ), std::sinf(transform.rotationZ),
            { tint, image.texture, materialBinding, &image }
        };

        /// @note 塗り潰しは描画時にセルを削るだけで、transform.scale もレイアウトも
        /// @note 当たり判定も動かさない (ゲージが減っても押せる範囲は変わらない)。
        const bool usesRadialFill = image.fillMethod != UIImageFillMethod::Edge;
        const RadialSector sector = usesRadialFill
            ? BuildRadialSector(image.fillMethod, image.fillOrigin,
                                image.fillClockwise, imageRect.size)
            : RadialSector{};
        const UIImageFillOrigin edgeOrigin =
            NormalizeFillOrigin(UIImageFillMethod::Edge, image.fillOrigin);
        ForEachImageCell(image, imageRect.size, uvMin, uvMax,
            [&](const ImageCell& cell) {
                if (fillAmount >= 1.0f) {
                    SubmitCell(draw, cell);
                } else if (usesRadialFill) {
                    SubmitCellRadial(draw, cell, fillAmount, sector);
                } else {
                    ImageCell clipped = cell;
                    if (ClipCellToEdgeFill(clipped, imageRect.size, fillAmount, edgeOrigin))
                        SubmitCell(draw, clipped);
                }
            });
    }
}

} /// @note namespace fbzz::scene::ui_render_detail
