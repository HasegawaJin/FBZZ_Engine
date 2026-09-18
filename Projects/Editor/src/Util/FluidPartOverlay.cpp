/// @file    FluidPartOverlay.cpp
/// @brief   Fluid Editor のビューポートへ部品を重ねて描き、ハンドルのドラッグをレシピへ書き戻す
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// ハンドルの位置・形の当たり・ドラッグの逆算は 3 か所で同じ幾何を使う。どれか 1 つだけ直すと
/// «描いた位置と掴める位置がずれる» ので、部品ごとの幾何 (円錐の軸・板の軸・輪の向き・カプセルと円柱の芯)
/// は下の小関数に寄せる。
///
/// 向きのドラッグの規則: 画面では xy しか動かせないので、単位ベクトルの z (奥行き成分) を保ったまま
/// xy の向きだけをカーソルへ回す (元の長さも保つ — 長さは意味を持たないので差分を増やさない)。
/// ただし xy がほぼ 0 (画面の真正面を向いている) なら z を捨てて画面内へ倒す。z を保つと掴んでも何も変わらない。

#include <Editor/Util/FluidPartOverlay.hpp>

#include <Fluid/FluidOperatorEval.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace fbzz::editor {
namespace {

using fluid::FluidCollider;
using fluid::FluidForce;
using fluid::FluidRecipe;
using fluid::FluidSource;
using Kind = FluidSelectionKind;

constexpr float kMinPartSize = 0.005f;
constexpr float kCenterHandleRadius = 7.0f;
constexpr float kHandleRadius = 6.0f;
/// 向きのハンドルは向きしか表さないので、部品の大きさによらず画面上で一定の距離に置く。
constexpr float kDirectionHandlePixels = 30.0f;
constexpr float kPickTolerancePixels = 5.0f;
/// 選択中の部品のハンドルは先に並ぶ。後ろのハンドルはこれ以上近くないと勝てない (重なった点で選択が飛ばない)。
constexpr float kHandleTieSlackPixels = 1.0f;
constexpr float kFacingViewPlanar = 0.1f;

constexpr ImU32 kSourceColor = IM_COL32(110, 235, 140, 255);
constexpr ImU32 kForceColor = IM_COL32(255, 170, 60, 255);
constexpr ImU32 kColliderColor = IM_COL32(225, 228, 240, 255);

/// @name 共通の幾何

float EvalTime(const FluidRecipe& recipe, float time)
{
    return (std::max)(recipe.output.warmup, 0.0f) + time;
}

bool IsVisible(const FluidPartVisibility& visible, Kind list, int index)
{
    return !visible || visible(list, index);
}

float PlanarLength(const math::Vector3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y);
}

/// 2D の絵は奥行きを見ないので xy だけで向きを取る。
math::Vector3 PlanarDirection(const math::Vector3& d, const math::Vector3& fallback)
{
    const float length = PlanarLength(d);
    if (length < 1.0e-4f) return fallback;
    return { d.x / length, d.y / length, 0.0f };
}

math::Vector3 Perpendicular(const math::Vector3& axis)
{
    return { -axis.y, axis.x, 0.0f };
}

float DomainLength(const FluidViewMapping& mapping, float pixels)
{
    return mapping.size > 0.0f ? pixels * 2.0f / mapping.size : 0.0f;
}

/// 画面は y 下向き。
ImVec2 AlongScreen(const ImVec2& from, const math::Vector3& domainDirection, float pixels)
{
    return { from.x + domainDirection.x * pixels, from.y - domainDirection.y * pixels };
}

float ScreenDistance(const ImVec2& a, const ImVec2& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

/// PickFluidPart は ImGui の文脈なしでも呼ばれる (テスト) ので、アイコンの大きさはフォントでなく正方形から決める。
float IconPixels(const FluidViewMapping& mapping)
{
    return std::clamp(mapping.size * 0.028f, 8.0f, 14.0f);
}

bool ForceIsPlaced(const FluidForce& force)
{
    using Type = fluid::FluidForceType;
    return force.radius > 0.0f || force.type == Type::Attract || force.type == Type::Repulse
        || force.type == Type::Vortex;
}

/// 領域全体に効く力は置き場所が無いので、左上に並べたアイコンで見せる。描く側と選ぶ側で並びを揃える。
ImVec2 GlobalForceIconCenter(const FluidViewMapping& mapping, int slot)
{
    const float icon = IconPixels(mapping);
    return { mapping.origin.x + icon * (1.6f + 2.6f * static_cast<float>(slot)), mapping.origin.y + icon * 1.6f };
}

int GlobalForceSlot(const FluidRecipe& recipe, int index, const FluidPartVisibility& visible)
{
    int slot = 0;
    for (int i = 0; i < index; ++i) {
        if (IsVisible(visible, Kind::Force, i) && !ForceIsPlaced(recipe.forces[static_cast<std::size_t>(i)])) ++slot;
    }
    return slot;
}

/// 長さ 0 の向きは上向き (FluidSourceWeight の既定) なので画面内として扱う。
bool RingFacesView(const math::Vector3& direction)
{
    return direction.LengthSq() > 1.0e-12f && std::abs(direction.z) >= PlanarLength(direction);
}

struct ConeFrame {
    math::Vector3 axis;
    math::Vector3 across;
    float length = 0.0f;
    float radius = 0.0f;
};

ConeFrame MakeConeFrame(const FluidSource& source)
{
    ConeFrame frame;
    frame.axis = PlanarDirection(source.direction, { 0.0f, 1.0f, 0.0f });
    frame.across = Perpendicular(frame.axis);
    frame.length = (std::max)(source.size.y, 0.0f);
    frame.radius = (std::max)(source.size.x, 0.0f);
    return frame;
}

/// カプセル / 円柱の芯を画面内に倒した枠。size.x = 半径、size.y = 軸方向の長さの半分。
/// 2D の絵は奥行きを見ないので、円錐と同じく軸の xy 成分だけで描く。
struct SegmentFrame {
    math::Vector3 axis;
    math::Vector3 across;
    float half = 0.0f;
    float radius = 0.0f;
};

SegmentFrame MakeSegmentFrame(const math::Vector3& direction, const math::Vector3& size)
{
    SegmentFrame frame;
    frame.axis = PlanarDirection(direction, { 0.0f, 1.0f, 0.0f });
    frame.across = Perpendicular(frame.axis);
    frame.radius = (std::max)(size.x, 0.0f);
    frame.half = (std::max)(size.y, 0.0f);
    return frame;
}

struct Plate {
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    math::Vector3 ax;
    math::Vector3 ay;
};

Plate MakePlate(const FluidSource& source)
{
    Plate plate;
    fluid::FluidTextureSourceBasis(source, plate.right, plate.up, plate.normal);
    plate.ax = plate.right * (std::max)(source.size.x, 0.0f);
    plate.ay = plate.up * (std::max)(source.size.y, 0.0f);
    return plate;
}

bool TextureOffersDirection(const Plate& plate)
{
    return PlanarLength(plate.normal) > kFacingViewPlanar;
}

/// 部品の基準位置・動き・time の姿。
struct PartView {
    const math::Vector3* base = nullptr;
    const fluid::FluidMotion* motion = nullptr;
    math::Vector3 posed;
    bool placed = true;
};

bool ViewPart(const FluidRecipe& recipe, Kind list, int index, float evalTime, PartView& out)
{
    switch (list) {
    case Kind::Source: {
        if (index < 0 || index >= static_cast<int>(recipe.sources.size())) return false;
        const FluidSource& source = recipe.sources[static_cast<std::size_t>(index)];
        out.base = &source.center;
        out.motion = &source.motion;
        out.posed = fluid::PoseFluidSource(source, evalTime).center;
        out.placed = true;
        return true;
    }
    case Kind::Force: {
        if (index < 0 || index >= static_cast<int>(recipe.forces.size())) return false;
        const FluidForce& force = recipe.forces[static_cast<std::size_t>(index)];
        out.base = &force.center;
        out.motion = &force.motion;
        out.posed = fluid::PoseFluidForce(force, evalTime).center;
        out.placed = ForceIsPlaced(force);
        return true;
    }
    case Kind::Collider: {
        if (index < 0 || index >= static_cast<int>(recipe.colliders.size())) return false;
        const FluidCollider& collider = recipe.colliders[static_cast<std::size_t>(index)];
        out.base = &collider.center;
        out.motion = &collider.motion;
        out.posed = fluid::PoseFluidCollider(collider, evalTime).center;
        out.placed = true;
        return true;
    }
    default:
        return false;
    }
}

/// @name ハンドル

void AddHandle(std::vector<FluidPartHandle>& out, Kind list, int index, FluidHandleKind kind, const ImVec2& screen,
               int keyIndex = -1)
{
    FluidPartHandle handle;
    handle.list = list;
    handle.index = index;
    handle.kind = kind;
    handle.keyIndex = keyIndex;
    handle.screen = screen;
    handle.radius = kind == FluidHandleKind::Center ? kCenterHandleRadius : kHandleRadius;
    out.push_back(handle);
}

/// Center は必ず先頭に積む (DrawSelectedHandles が向きの線の根元に使う)。
void CollectPartHandles(std::vector<FluidPartHandle>& out, const FluidViewMapping& mapping, const FluidRecipe& recipe,
                        Kind list, int index, float evalTime, bool full)
{
    PartView view;
    if (!ViewPart(recipe, list, index, evalTime, view) || !view.placed) return;
    const math::Vector3& c = view.posed;
    const ImVec2 cs = mapping.ToScreen(c);
    AddHandle(out, list, index, FluidHandleKind::Center, cs);
    if (!full) return;

    const auto addSize = [&](const math::Vector3& at) {
        AddHandle(out, list, index, FluidHandleKind::Size, mapping.ToScreen(at));
    };
    const auto addDirection = [&](const ImVec2& at) { AddHandle(out, list, index, FluidHandleKind::Direction, at); };
    const math::Vector3 up{ 0.0f, 1.0f, 0.0f };

    if (list == Kind::Source) {
        using Shape = fluid::FluidSourceShape;
        const FluidSource& source = recipe.sources[static_cast<std::size_t>(index)];
        const float sx = (std::max)(source.size.x, 0.0f);
        const float sy = (std::max)(source.size.y, 0.0f);
        switch (source.shape) {
        case Shape::Sphere:
            addSize(c + math::Vector3{ sx, 0.0f, 0.0f });
            break;
        case Shape::Box:
            addSize(c + math::Vector3{ sx, sy, 0.0f });
            break;
        case Shape::Cone: {
            const ConeFrame frame = MakeConeFrame(source);
            const math::Vector3 baseCenter = c + frame.axis * frame.length;
            addSize(baseCenter + frame.across * frame.radius);
            addDirection(mapping.ToScreen(baseCenter));
            break;
        }
        case Shape::Ring:
            if (RingFacesView(source.direction)) {
                addSize(c + math::Vector3{ sx, 0.0f, 0.0f });
            } else {
                const math::Vector3 axis = PlanarDirection(source.direction, up);
                addSize(c + Perpendicular(axis) * sx);
                addDirection(AlongScreen(cs, axis, kDirectionHandlePixels));
            }
            break;
        case Shape::Texture: {
            const Plate plate = MakePlate(source);
            addSize(c + plate.ax + plate.ay);
            if (TextureOffersDirection(plate))
                addDirection(AlongScreen(cs, PlanarDirection(plate.normal, up), kDirectionHandlePixels));
            break;
        }
        case Shape::Capsule:
        case Shape::Cylinder: {
            const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size);
            addSize(c + frame.axis * frame.half + frame.across * frame.radius);
            addDirection(AlongScreen(cs, frame.axis, kDirectionHandlePixels));
            break;
        }
        }
    } else if (list == Kind::Force) {
        const FluidForce& force = recipe.forces[static_cast<std::size_t>(index)];
        if (force.radius > 0.0f) addSize(c + math::Vector3{ force.radius, 0.0f, 0.0f });
        /// @note 2D では渦の軸は常に奥行きなので、向きを掴めるのは風だけ。
        if (force.type == fluid::FluidForceType::Wind)
            addDirection(AlongScreen(cs, PlanarDirection(force.direction, { 1.0f, 0.0f, 0.0f }), kDirectionHandlePixels));
    } else {
        using Shape = fluid::FluidColliderShape;
        const FluidCollider& collider = recipe.colliders[static_cast<std::size_t>(index)];
        const float sx = (std::max)(collider.size.x, 0.0f);
        const float sy = (std::max)(collider.size.y, 0.0f);
        switch (collider.shape) {
        case Shape::Sphere:
            addSize(c + math::Vector3{ sx, 0.0f, 0.0f });
            break;
        case Shape::Box:
            addSize(c + math::Vector3{ sx, sy, 0.0f });
            break;
        case Shape::Plane:
            /// @note 画面の真正面を向いた面 (2D では効かない) も、掴めば画面内へ倒して直せるように出す。
            addDirection(AlongScreen(cs, PlanarDirection(collider.direction, up), kDirectionHandlePixels));
            break;
        case Shape::Capsule:
        case Shape::Cylinder: {
            const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size);
            addSize(c + frame.axis * frame.half + frame.across * frame.radius);
            addDirection(AlongScreen(cs, frame.axis, kDirectionHandlePixels));
            break;
        }
        }
    }

    const std::vector<fluid::FluidMotionKey>& keys = view.motion->keys;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        AddHandle(out, list, index, FluidHandleKind::MotionKey, mapping.ToScreen(*view.base + keys[k].offset),
                  static_cast<int>(k));
    }
}

/// @name ドラッグ

math::Vector3 DraggedDirection(const math::Vector3& original, const math::Vector3& fallback, float dx, float dy)
{
    const float planar = std::sqrt(dx * dx + dy * dy);
    /// @note 中心の上では向きが決まらない。
    if (planar < 1.0e-5f) return original;
    const float length = original.Length();
    const math::Vector3 unit = length > 1.0e-6f ? original / length : fallback;
    const float z = PlanarLength(unit) < kFacingViewPlanar ? 0.0f : unit.z;
    const float inPlane = std::sqrt((std::max)(1.0f - z * z, 0.0f));
    const float scale = length > 1.0e-6f ? length : 1.0f;
    return { dx / planar * inPlane * scale, dy / planar * inPlane * scale, z * scale };
}

void DragHalfExtents(math::Vector3& size, float dx, float dy)
{
    size.x = (std::max)(std::abs(dx), kMinPartSize);
    size.y = (std::max)(std::abs(dy), kMinPartSize);
}

/// 板の角 = center + right × sx + up × sy (xy だけ) を解いて sx / sy を戻す。
void DragPlateSize(FluidSource& source, float dx, float dy)
{
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    fluid::FluidTextureSourceBasis(source, right, up, normal);
    const float det = right.x * up.y - right.y * up.x;
    float a = source.size.x;
    float b = source.size.y;
    if (std::abs(det) > 0.05f) {
        a = (dx * up.y - dy * up.x) / det;
        b = (right.x * dy - right.y * dx) / det;
    } else {
        /// @note 板が真横を向くと画面では線になり、2 軸を分けられない。画面に見えている軸だけ直す。
        const float rr = right.x * right.x + right.y * right.y;
        const float uu = up.x * up.x + up.y * up.y;
        if (rr > 0.0025f) a = (dx * right.x + dy * right.y) / rr;
        if (uu > 0.0025f) b = (dx * up.x + dy * up.y) / uu;
    }
    source.size.x = (std::max)(std::abs(a), kMinPartSize);
    source.size.y = (std::max)(std::abs(b), kMinPartSize);
}

/// Center / MotionKey は部品の種類によらない。処理したら true。
bool ApplyCommonDrag(math::Vector3& center, fluid::FluidMotion& motion, const math::Vector3& posed,
                     const FluidPartHandle& handle, const math::Vector3& target)
{
    if (handle.kind == FluidHandleKind::Center) {
        /// @note 動きのずれは基準の center に依らないので、差だけ足せば今の姿がカーソルに乗る。
        center.x += target.x - posed.x;
        center.y += target.y - posed.y;
        return true;
    }
    if (handle.kind == FluidHandleKind::MotionKey) {
        if (handle.keyIndex >= 0 && handle.keyIndex < static_cast<int>(motion.keys.size())) {
            math::Vector3& offset = motion.keys[static_cast<std::size_t>(handle.keyIndex)].offset;
            offset.x = target.x - center.x;
            offset.y = target.y - center.y;
        }
        return true;
    }
    return false;
}

void ApplySourceShapeDrag(FluidSource& source, FluidHandleKind kind, const math::Vector3& c,
                          const math::Vector3& target)
{
    using Shape = fluid::FluidSourceShape;
    const float dx = target.x - c.x;
    const float dy = target.y - c.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (kind == FluidHandleKind::Size) {
        switch (source.shape) {
        case Shape::Sphere:
        case Shape::Ring:
            source.size.x = (std::max)(distance, kMinPartSize);
            break;
        case Shape::Box:
            DragHalfExtents(source.size, dx, dy);
            break;
        case Shape::Cone: {
            /// @note ハンドルは底の縁。軸方向の成分が長さ、軸に直交する成分が底の半径。
            const ConeFrame frame = MakeConeFrame(source);
            const math::Vector3 v{ dx, dy, 0.0f };
            source.size.y = (std::max)(math::Vector3::Dot(v, frame.axis), kMinPartSize);
            source.size.x = (std::max)(std::abs(math::Vector3::Dot(v, frame.across)), kMinPartSize);
            break;
        }
        case Shape::Texture:
            DragPlateSize(source, dx, dy);
            break;
        case Shape::Capsule:
        case Shape::Cylinder: {
            /// @note ハンドルは «端 + 半径» の角。軸方向の成分が半分の長さ、軸に直交する成分が半径。
            const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size);
            const math::Vector3 v{ dx, dy, 0.0f };
            source.size.y = (std::max)(std::abs(math::Vector3::Dot(v, frame.axis)), kMinPartSize);
            source.size.x = (std::max)(std::abs(math::Vector3::Dot(v, frame.across)), kMinPartSize);
            break;
        }
        }
        return;
    }
    if (kind == FluidHandleKind::Direction) {
        const math::Vector3 fallback = source.shape == Shape::Texture ? math::Vector3{ 0.0f, 0.0f, 1.0f }
                                                                      : math::Vector3{ 0.0f, 1.0f, 0.0f };
        source.direction = DraggedDirection(source.direction, fallback, dx, dy);
        /// @note 円錐の向きのハンドルは底の中心にある。長さもカーソルまでに合わせないと、手を離すと底がカーソルから跳ねる。
        if (source.shape == Shape::Cone && distance > 1.0e-5f) source.size.y = (std::max)(distance, kMinPartSize);
    }
}

void ApplyForceShapeDrag(FluidForce& force, FluidHandleKind kind, const math::Vector3& c, const math::Vector3& target)
{
    const float dx = target.x - c.x;
    const float dy = target.y - c.y;
    if (kind == FluidHandleKind::Size) {
        force.radius = (std::max)(std::sqrt(dx * dx + dy * dy), kMinPartSize);
    } else if (kind == FluidHandleKind::Direction) {
        force.direction = DraggedDirection(force.direction, { 1.0f, 0.0f, 0.0f }, dx, dy);
    }
}

void ApplyColliderShapeDrag(FluidCollider& collider, FluidHandleKind kind, const math::Vector3& c,
                            const math::Vector3& target)
{
    using Shape = fluid::FluidColliderShape;
    const float dx = target.x - c.x;
    const float dy = target.y - c.y;
    if (kind == FluidHandleKind::Size) {
        if (collider.shape == Shape::Sphere)
            collider.size.x = (std::max)(std::sqrt(dx * dx + dy * dy), kMinPartSize);
        else if (collider.shape == Shape::Box)
            DragHalfExtents(collider.size, dx, dy);
        else if (collider.shape == Shape::Capsule || collider.shape == Shape::Cylinder) {
            const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size);
            const math::Vector3 v{ dx, dy, 0.0f };
            collider.size.y = (std::max)(std::abs(math::Vector3::Dot(v, frame.axis)), kMinPartSize);
            collider.size.x = (std::max)(std::abs(math::Vector3::Dot(v, frame.across)), kMinPartSize);
        }
    } else if (kind == FluidHandleKind::Direction) {
        collider.direction = DraggedDirection(collider.direction, { 0.0f, 1.0f, 0.0f }, dx, dy);
    }
}

/// @name 形の当たり (領域座標の xy)

float SegmentDistance(const math::Vector3& p, const math::Vector3& a, const math::Vector3& b)
{
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float lengthSq = abx * abx + aby * aby;
    const float t = lengthSq > 1.0e-12f
        ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / lengthSq, 0.0f, 1.0f)
        : 0.0f;
    const float dx = p.x - (a.x + abx * t);
    const float dy = p.y - (a.y + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

/// 凸四角形の中か、辺の近く。真横を向いた板は面積 0 の線になるので、辺の近さで拾う。
bool QuadContains(const math::Vector3 (&corners)[4], const math::Vector3& p, float tolerance)
{
    float sign = 0.0f;
    bool inside = true;
    for (int i = 0; i < 4; ++i) {
        const math::Vector3& a = corners[i];
        const math::Vector3& b = corners[(i + 1) % 4];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::abs(cross) < 1.0e-9f) continue;
        if (sign == 0.0f) sign = cross;
        else if ((cross > 0.0f) != (sign > 0.0f)) inside = false;
    }
    if (inside && sign != 0.0f) return true;
    for (int i = 0; i < 4; ++i) {
        if (SegmentDistance(p, corners[i], corners[(i + 1) % 4]) <= tolerance) return true;
    }
    return false;
}

bool SourceContains(const FluidSource& source, const math::Vector3& c, const math::Vector3& p, float tolerance)
{
    using Shape = fluid::FluidSourceShape;
    const float dx = p.x - c.x;
    const float dy = p.y - c.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    switch (source.shape) {
    case Shape::Sphere:
        return distance <= sx + tolerance;
    case Shape::Box:
        return std::abs(dx) <= sx + tolerance && std::abs(dy) <= sy + tolerance;
    case Shape::Cone: {
        const ConeFrame frame = MakeConeFrame(source);
        const math::Vector3 v{ dx, dy, 0.0f };
        const float t = math::Vector3::Dot(v, frame.axis);
        if (t < -tolerance || t > frame.length + tolerance) return false;
        const float allowed = frame.length > 1.0e-6f
            ? frame.radius * std::clamp(t, 0.0f, frame.length) / frame.length
            : frame.radius;
        return std::abs(math::Vector3::Dot(v, frame.across)) <= allowed + tolerance;
    }
    case Shape::Ring: {
        if (RingFacesView(source.direction)) return std::abs(distance - sx) <= sy + tolerance;
        const math::Vector3 across = Perpendicular(PlanarDirection(source.direction, { 0.0f, 1.0f, 0.0f }));
        const math::Vector3 a = c + across * sx;
        const math::Vector3 b = c - across * sx;
        const float reach = sy + tolerance;
        return std::hypot(p.x - a.x, p.y - a.y) <= reach || std::hypot(p.x - b.x, p.y - b.y) <= reach;
    }
    case Shape::Texture: {
        const Plate plate = MakePlate(source);
        const math::Vector3 corners[4] = { c - plate.ax - plate.ay, c + plate.ax - plate.ay, c + plate.ax + plate.ay,
                                           c - plate.ax + plate.ay };
        return QuadContains(corners, p, tolerance);
    }
    case Shape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size);
        return SegmentDistance(p, c - frame.axis * frame.half, c + frame.axis * frame.half)
            <= frame.radius + tolerance;
    }
    case Shape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size);
        const math::Vector3 v{ dx, dy, 0.0f };
        return std::abs(math::Vector3::Dot(v, frame.axis)) <= frame.half + tolerance
            && std::abs(math::Vector3::Dot(v, frame.across)) <= frame.radius + tolerance;
    }
    }
    return false;
}

bool ColliderContains(const FluidCollider& collider, const math::Vector3& c, const math::Vector3& p, float tolerance)
{
    using Shape = fluid::FluidColliderShape;
    const float dx = p.x - c.x;
    const float dy = p.y - c.y;
    switch (collider.shape) {
    case Shape::Sphere:
        return std::sqrt(dx * dx + dy * dy) <= (std::max)(collider.size.x, 0.0f) + tolerance;
    case Shape::Box:
        return std::abs(dx) <= (std::max)(collider.size.x, 0.0f) + tolerance
            && std::abs(dy) <= (std::max)(collider.size.y, 0.0f) + tolerance;
    case Shape::Plane: {
        /// @note 固体の側は領域の半分を覆うことがある。そこを掴めると他の部品が選べなくなるので、面の線の近くだけにする。
        const math::Vector3& n = collider.direction;
        if (PlanarLength(n) < 1.0e-4f && std::abs(n.z) > 1.0e-4f) return std::sqrt(dx * dx + dy * dy) <= tolerance * 1.5f;
        const math::Vector3 normal = PlanarDirection(n, { 0.0f, 1.0f, 0.0f });
        return std::abs(dx * normal.x + dy * normal.y) <= tolerance;
    }
    case Shape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size);
        return SegmentDistance(p, c - frame.axis * frame.half, c + frame.axis * frame.half)
            <= frame.radius + tolerance;
    }
    case Shape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size);
        const math::Vector3 v{ dx, dy, 0.0f };
        return std::abs(math::Vector3::Dot(v, frame.axis)) <= frame.half + tolerance
            && std::abs(math::Vector3::Dot(v, frame.across)) <= frame.radius + tolerance;
    }
    }
    return false;
}

/// @name 描画

ImU32 WithAlpha(ImU32 color, int alpha)
{
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

ImU32 Brighten(ImU32 color, float amount)
{
    const auto channel = [color, amount](int shift) {
        const float value = static_cast<float>((color >> shift) & 0xFFu);
        return static_cast<ImU32>(value + (255.0f - value) * amount + 0.5f) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT)
         | (color & IM_COL32_A_MASK);
}

struct PartStyle {
    ImU32 base = 0;
    ImU32 line = 0;
    ImU32 faint = 0;
    float thickness = 1.5f;
};

PartStyle MakeStyle(ImU32 base, bool active, bool selected, int activeAlpha = 230, int inactiveAlpha = 110)
{
    PartStyle style;
    style.base = selected ? Brighten(base, 0.35f) : base;
    /// @note 選択中でも止まっている部品は止まっていると読めるよう、濃さの差は残す。
    style.line = WithAlpha(style.base, selected ? (active ? 255 : 170) : (active ? activeAlpha : inactiveAlpha));
    style.faint = WithAlpha(style.base, selected ? 130 : 70);
    style.thickness = selected ? 2.5f : 1.5f;
    return style;
}

math::Vector3 EvaluateRamp(const fluid::FluidColorRamp& ramp, float t)
{
    const auto& stops = ramp.stops;
    if (t <= stops[0].position) return stops[0].color;
    for (int i = 1; i < fluid::kFluidRampStops; ++i) {
        const fluid::FluidColorStop& a = stops[static_cast<std::size_t>(i) - 1];
        const fluid::FluidColorStop& b = stops[static_cast<std::size_t>(i)];
        if (t <= b.position) {
            const float span = b.position - a.position;
            const float u = span > 1.0e-6f ? (t - a.position) / span : 1.0f;
            return math::Vector3::Lerp(a.color, b.color, u);
        }
    }
    return stops[fluid::kFluidRampStops - 1].color;
}

float LinearToSrgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

ImU32 SourceBaseColor(const fluid::FluidRenderSettings& look, const FluidSource& source)
{
    if (!look.useAlbedoRamp) return kSourceColor;
    /// @note 煤や血のような暗い色でも市松の上で輪郭が消えないよう、白側へ持ち上げてから縁取る。
    const math::Vector3 tint = EvaluateRamp(look.albedoRamp, std::clamp(source.colorKey, 0.0f, 1.0f));
    return ImGui::ColorConvertFloat4ToU32({ 0.35f + 0.65f * LinearToSrgb(tint.x), 0.35f + 0.65f * LinearToSrgb(tint.y),
                                            0.35f + 0.65f * LinearToSrgb(tint.z), 1.0f });
}

void DrawArrow(ImDrawList* drawList, const ImVec2& from, const ImVec2& to, ImU32 color, float thickness)
{
    drawList->AddLine(from, to, color, thickness);
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0e-3f) return;
    const ImVec2 dir{ dx / length, dy / length };
    const float head = (std::min)(length * 0.5f, 6.0f);
    const ImVec2 back{ to.x - dir.x * head, to.y - dir.y * head };
    drawList->AddTriangleFilled(to, { back.x - dir.y * head * 0.5f, back.y + dir.x * head * 0.5f },
                                { back.x + dir.y * head * 0.5f, back.y - dir.x * head * 0.5f }, color);
}

void DrawDiamond(ImDrawList* drawList, const ImVec2& c, float r, ImU32 fill, ImU32 outline)
{
    const ImVec2 top{ c.x, c.y - r };
    const ImVec2 right{ c.x + r, c.y };
    const ImVec2 bottom{ c.x, c.y + r };
    const ImVec2 left{ c.x - r, c.y };
    drawList->AddQuadFilled(top, right, bottom, left, fill);
    if (outline != 0) drawList->AddQuad(top, right, bottom, left, outline, 1.5f);
}

/// カプセル / 円柱の輪郭。rounded なら両端を半円でつなぐ (円柱は平らな端なので長方形)。
void DrawSegmentOutline(ImDrawList* drawList, const FluidViewMapping& mapping, const math::Vector3& c,
                        const SegmentFrame& frame, bool rounded, ImU32 color, float thickness)
{
    const ImVec2 tail = mapping.ToScreen(c - frame.axis * frame.half);
    const ImVec2 head = mapping.ToScreen(c + frame.axis * frame.half);
    /// @note 半径 0 だと輪郭が線に潰れて «置いた場所» が読めないので 1 px は残す。
    const float r = (std::max)(mapping.ToPixels(frame.radius), 1.0f);
    /// @note 画面は y 下向きなので軸の y を反転して角度を取る。
    const float angle = std::atan2(-frame.axis.y, frame.axis.x);
    const ImVec2 side{ -std::sin(angle) * r, std::cos(angle) * r };
    if (!rounded) {
        drawList->AddQuad({ tail.x + side.x, tail.y + side.y }, { head.x + side.x, head.y + side.y },
                          { head.x - side.x, head.y - side.y }, { tail.x - side.x, tail.y - side.y }, color,
                          thickness);
        return;
    }
    constexpr float kHalfPi = 1.5707963f;
    drawList->PathClear();
    drawList->PathArcTo(head, r, angle - kHalfPi, angle + kHalfPi, 16);
    drawList->PathArcTo(tail, r, angle + kHalfPi, angle + kHalfPi * 3.0f, 16);
    drawList->PathStroke(color, ImDrawFlags_Closed, thickness);
}

void DrawMotionPath(ImDrawList* drawList, const FluidViewMapping& mapping, const math::Vector3& base,
                    const fluid::FluidMotion& motion, const PartStyle& style, bool selected)
{
    const std::size_t count =
        (std::min)(motion.keys.size(), static_cast<std::size_t>(fluid::kMaxFluidMotionKeys));
    if (count == 0) return;
    ImVec2 points[fluid::kMaxFluidMotionKeys];
    for (std::size_t k = 0; k < count; ++k) points[k] = mapping.ToScreen(base + motion.keys[k].offset);
    if (count >= 2) drawList->AddPolyline(points, static_cast<int>(count), style.faint, selected ? 1.5f : 1.0f);
    /// @note 選択中の部品のキーはハンドル (大きい菱形) として後で描く。
    if (selected) return;
    for (std::size_t k = 0; k < count; ++k) DrawDiamond(drawList, points[k], 2.5f, style.faint, 0);
}

void DrawSource(ImDrawList* drawList, const FluidViewMapping& mapping, const FluidRecipe& recipe,
                const FluidSource& source, float evalTime, bool selected)
{
    using Shape = fluid::FluidSourceShape;
    /// @note 液体の Duration 0 は «一斉» なので、その瞬間だけ光らせても目で追えない。少しだけ幅を持たせる。
    const bool active = recipe.kind == fluid::FluidKind::Liquid
        ? source.enabled && evalTime >= source.startTime
              && evalTime < source.startTime + (std::max)(source.duration, 0.1f)
        : fluid::FluidSourceEmitting(source, evalTime);
    const PartStyle style = MakeStyle(SourceBaseColor(recipe.render, source), active, selected);
    DrawMotionPath(drawList, mapping, source.center, source.motion, style, selected);

    const math::Vector3 c = fluid::PoseFluidSource(source, evalTime).center;
    const ImVec2 cs = mapping.ToScreen(c);
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    switch (source.shape) {
    case Shape::Sphere:
        drawList->AddCircle(cs, mapping.ToPixels(sx), style.line, 0, style.thickness);
        break;
    case Shape::Box: {
        const float hx = mapping.ToPixels(sx);
        const float hy = mapping.ToPixels(sy);
        drawList->AddRect({ cs.x - hx, cs.y - hy }, { cs.x + hx, cs.y + hy }, style.line, 0.0f, style.thickness);
        break;
    }
    case Shape::Cone: {
        const ConeFrame frame = MakeConeFrame(source);
        const math::Vector3 baseCenter = c + frame.axis * frame.length;
        drawList->AddTriangle(cs, mapping.ToScreen(baseCenter + frame.across * frame.radius),
                              mapping.ToScreen(baseCenter - frame.across * frame.radius), style.line, style.thickness);
        break;
    }
    case Shape::Ring: {
        const float ringRadius = mapping.ToPixels(sx);
        const float tube = mapping.ToPixels(sy);
        if (RingFacesView(source.direction)) {
            /// @note 法線が奥行き寄り: 画面では輪そのもの。
            drawList->AddCircle(cs, ringRadius, style.line, 0, style.thickness);
            drawList->AddCircle(cs, ringRadius + tube, style.faint, 0, 1.0f);
            if (ringRadius - tube > 1.0f) drawList->AddCircle(cs, ringRadius - tube, style.faint, 0, 1.0f);
        } else {
            /// @note 法線が画面内: 輪を真横から見るので、断面の 2 つの塊になる。
            const math::Vector3 across = Perpendicular(PlanarDirection(source.direction, { 0.0f, 1.0f, 0.0f }));
            const ImVec2 a = mapping.ToScreen(c + across * sx);
            const ImVec2 b = mapping.ToScreen(c - across * sx);
            drawList->AddLine(a, b, style.faint, 1.0f);
            drawList->AddCircle(a, (std::max)(tube, 1.5f), style.line, 0, style.thickness);
            drawList->AddCircle(b, (std::max)(tube, 1.5f), style.line, 0, style.thickness);
        }
        break;
    }
    case Shape::Texture: {
        const Plate plate = MakePlate(source);
        const auto corner = [&](const math::Vector3& offset) { return mapping.ToScreen(c + offset); };
        /// @note 板を画面内へ倒すと厚みの側が見えてくる。前後の面も薄く描いて、湧く範囲の奥行きを示す。
        if (PlanarLength(plate.normal) > 0.05f) {
            const math::Vector3 az = plate.normal * (std::max)(source.size.z, 0.0f);
            for (const float s : { -1.0f, 1.0f }) {
                const math::Vector3 o = az * s;
                drawList->AddQuad(corner(o - plate.ax - plate.ay), corner(o + plate.ax - plate.ay),
                                  corner(o + plate.ax + plate.ay), corner(o - plate.ax + plate.ay), style.faint, 1.0f);
            }
        }
        drawList->AddQuad(corner(-plate.ax - plate.ay), corner(plate.ax - plate.ay), corner(plate.ax + plate.ay),
                          corner(-plate.ax + plate.ay), style.line, style.thickness);
        /// @note 画像の上辺を太くして、板の上下 (up 軸の向き) を読めるようにする。
        drawList->AddLine(corner(-plate.ax + plate.ay), corner(plate.ax + plate.ay), style.line, style.thickness + 1.5f);
        break;
    }
    case Shape::Capsule:
    case Shape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size);
        DrawSegmentOutline(drawList, mapping, c, frame, source.shape == Shape::Capsule, style.line, style.thickness);
        /// @note 芯の線分を薄く引いて «どこが軸か» を読めるようにする。
        drawList->AddLine(mapping.ToScreen(c - frame.axis * frame.half), mapping.ToScreen(c + frame.axis * frame.half),
                          style.faint, 1.0f);
        break;
    }
    }
    drawList->AddCircleFilled(cs, 2.0f, style.line);
}

void DrawCollider(ImDrawList* drawList, const FluidViewMapping& mapping, const FluidCollider& collider, float evalTime,
                  bool selected)
{
    using Shape = fluid::FluidColliderShape;
    const bool active = fluid::FluidColliderActive(collider, evalTime);
    const PartStyle style = MakeStyle(kColliderColor, active, selected, 220, 90);
    const ImU32 hatch = WithAlpha(style.base, selected ? 120 : (active ? 90 : 40));
    const float spacing = (std::max)(ImGui::GetFontSize() * 0.45f, 4.0f);
    DrawMotionPath(drawList, mapping, collider.center, collider.motion, style, selected);

    const ImVec2 c = mapping.ToScreen(fluid::PoseFluidCollider(collider, evalTime).center);
    switch (collider.shape) {
    case Shape::Sphere: {
        const float r = mapping.ToPixels((std::max)(collider.size.x, 0.0f));
        /// @note 斜線は円の弦として描く (矩形のクリップでは円に切り抜けない)。
        constexpr float kInvSqrt2 = 0.70710678f;
        for (float t = -r + spacing * 0.5f; t < r; t += spacing) {
            const float h = std::sqrt((std::max)(r * r - t * t, 0.0f));
            const ImVec2 m{ c.x + t * kInvSqrt2, c.y + t * kInvSqrt2 };
            drawList->AddLine({ m.x - h * kInvSqrt2, m.y + h * kInvSqrt2 }, { m.x + h * kInvSqrt2, m.y - h * kInvSqrt2 },
                              hatch, 1.0f);
        }
        drawList->AddCircle(c, r, style.line, 0, style.thickness);
        break;
    }
    case Shape::Box: {
        const float hx = mapping.ToPixels((std::max)(collider.size.x, 0.0f));
        const float hy = mapping.ToPixels((std::max)(collider.size.y, 0.0f));
        const ImVec2 lo{ c.x - hx, c.y - hy };
        const ImVec2 hi{ c.x + hx, c.y + hy };
        const float height = hi.y - lo.y;
        drawList->PushClipRect(lo, hi, true);
        for (float t = spacing * 0.5f; t < (hi.x - lo.x) + height; t += spacing)
            drawList->AddLine({ lo.x + t, lo.y }, { lo.x + t - height, hi.y }, hatch, 1.0f);
        drawList->PopClipRect();
        drawList->AddRect(lo, hi, style.line, 0.0f, style.thickness);
        break;
    }
    case Shape::Plane: {
        const math::Vector3& n = collider.direction;
        if (PlanarLength(n) < 1.0e-4f && std::abs(n.z) > 1.0e-4f) {
            /// @note 法線が奥行き向きだと、2D の断面はどこも面の上 (距離 0) で固体にならない。
            drawList->AddCircle(c, 4.0f, style.line, 0, style.thickness);
            drawList->AddText({ c.x + 7.0f, c.y - ImGui::GetFontSize() * 0.5f }, style.line,
                              "Plane faces view (no effect in 2D)");
            break;
        }
        const math::Vector3 nd = PlanarDirection(n, { 0.0f, 1.0f, 0.0f });
        const ImVec2 normal{ nd.x, -nd.y };
        const ImVec2 along{ -normal.y, normal.x };
        const float reach = mapping.size * 1.5f;
        drawList->AddLine({ c.x - along.x * reach, c.y - along.y * reach }, { c.x + along.x * reach, c.y + along.y * reach },
                          style.line, style.thickness);
        /// @note 固体の側 (法線の反対) へ短い斜線を刻む。
        const float interval = spacing * 2.0f;
        const float tick = spacing * 1.2f;
        const int ticks = static_cast<int>(reach / interval);
        for (int k = -ticks; k <= ticks; ++k) {
            const ImVec2 q{ c.x + along.x * interval * static_cast<float>(k),
                            c.y + along.y * interval * static_cast<float>(k) };
            drawList->AddLine(q, { q.x - (normal.x + along.x) * tick * 0.7f, q.y - (normal.y + along.y) * tick * 0.7f },
                              style.line, 1.0f);
        }
        DrawArrow(drawList, c, { c.x + normal.x * spacing * 3.0f, c.y + normal.y * spacing * 3.0f }, style.line,
                  style.thickness);
        break;
    }
    case Shape::Capsule:
    case Shape::Cylinder: {
        const bool rounded = collider.shape == Shape::Capsule;
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size);
        const math::Vector3 center = fluid::PoseFluidCollider(collider, evalTime).center;
        /// @note 斜線でなく軸に直交する弦にする: 傾いた形は矩形でクリップできず «円の弦» も端が半円 / 平らで場合分けになるが、軸に沿って刻めば両方を 1 本の式 (その位置の半幅) で引ける。
        const float half = mapping.ToPixels(frame.half);
        const float radius = mapping.ToPixels(frame.radius);
        const float reach = rounded ? half + radius : half;
        const ImVec2 along{ frame.axis.x, -frame.axis.y };
        const ImVec2 side{ -along.y, along.x };
        for (float t = -reach + spacing * 0.5f; t < reach; t += spacing) {
            const float over = (std::max)(std::abs(t) - half, 0.0f);
            const float width = std::sqrt((std::max)(radius * radius - over * over, 0.0f));
            if (width <= 0.5f) continue;
            const ImVec2 m{ c.x + along.x * t, c.y + along.y * t };
            drawList->AddLine({ m.x - side.x * width, m.y - side.y * width },
                              { m.x + side.x * width, m.y + side.y * width }, hatch, 1.0f);
        }
        DrawSegmentOutline(drawList, mapping, center, frame, rounded, style.line, style.thickness);
        break;
    }
    }
    drawList->AddCircleFilled(c, 2.0f, style.line);
}

void DrawForce(ImDrawList* drawList, const FluidViewMapping& mapping, const FluidForce& force, float evalTime,
               bool selected, int& globalSlot)
{
    using Type = fluid::FluidForceType;
    const PartStyle style = MakeStyle(kForceColor, fluid::FluidForceActive(force, evalTime), selected);
    const ImU32 color = style.line;
    const float thickness = style.thickness;
    const float icon = IconPixels(mapping);
    ImVec2 c;
    if (ForceIsPlaced(force)) {
        DrawMotionPath(drawList, mapping, force.center, force.motion, style, selected);
        c = mapping.ToScreen(fluid::PoseFluidForce(force, evalTime).center);
        if (force.radius > 0.0f)
            drawList->AddCircle(c, mapping.ToPixels(force.radius), WithAlpha(style.base, selected ? 150 : 80), 0,
                                selected ? 1.5f : 1.0f);
    } else {
        c = GlobalForceIconCenter(mapping, globalSlot++);
        if (selected)
            drawList->AddRect({ c.x - icon * 1.3f, c.y - icon * 1.3f }, { c.x + icon * 1.3f, c.y + icon * 1.3f },
                              style.faint, 3.0f, 1.0f);
    }
    switch (force.type) {
    case Type::Wind: {
        const math::Vector3 dir = PlanarDirection(force.direction, { 1.0f, 0.0f, 0.0f });
        DrawArrow(drawList, AlongScreen(c, dir, -icon), AlongScreen(c, dir, icon), color, thickness);
        break;
    }
    case Type::Attract:
    case Type::Repulse: {
        const bool inward = (force.type == Type::Attract) == (force.strength >= 0.0f);
        static constexpr ImVec2 kDirs[] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f } };
        for (const ImVec2& dir : kDirs) {
            const ImVec2 inner{ c.x + dir.x * icon * 0.35f, c.y + dir.y * icon * 0.35f };
            const ImVec2 outer{ c.x + dir.x * icon * 1.2f, c.y + dir.y * icon * 1.2f };
            if (inward) DrawArrow(drawList, outer, inner, color, thickness);
            else        DrawArrow(drawList, inner, outer, color, thickness);
        }
        break;
    }
    case Type::Vortex: {
        /// @note 2D では軸が常に奥行き (0,0,1) なので、Strength が正なら画面で反時計回り。上端の矢じりで向きを見せる。
        const float radius = icon * 0.9f;
        drawList->AddCircle(c, radius, color, 20, thickness);
        const float sign = force.strength >= 0.0f ? -1.0f : 1.0f;
        const ImVec2 tip{ c.x + sign * icon * 0.45f, c.y - radius };
        drawList->AddTriangleFilled(tip, { c.x, c.y - radius - icon * 0.3f }, { c.x, c.y - radius + icon * 0.3f }, color);
        break;
    }
    case Type::Noise: {
        ImVec2 points[13];
        for (int i = 0; i < 13; ++i) {
            const float t = static_cast<float>(i) / 12.0f;
            points[i] = { c.x - icon + t * icon * 2.0f, c.y + std::sin(t * 6.2831853f * 1.5f) * icon * 0.35f };
        }
        drawList->AddPolyline(points, 13, color, thickness);
        break;
    }
    case Type::Drag:
        drawList->AddRect({ c.x - icon * 0.6f, c.y - icon * 0.6f }, { c.x + icon * 0.6f, c.y + icon * 0.6f }, color, 0.0f,
                          thickness);
        drawList->AddLine({ c.x - icon * 0.35f, c.y }, { c.x + icon * 0.35f, c.y }, color, thickness);
        break;
    }
}

void DrawSelectedHandles(ImDrawList* drawList, const FluidViewMapping& mapping, const FluidRecipe& recipe,
                         float evalTime, const FluidSelection& selection, const FluidPartVisibility& visible)
{
    if (!selection.IsPart() || !IsVisible(visible, selection.kind, selection.index)) return;
    PartView view;
    if (!ViewPart(recipe, selection.kind, selection.index, evalTime, view)) return;
    std::vector<FluidPartHandle> handles;
    CollectPartHandles(handles, mapping, recipe, selection.kind, selection.index, evalTime, true);
    if (handles.empty()) return;

    /// @note 今の時刻に一番近いキーを目立たせる (Timeline のどのキーを触っているかの目安)。
    int nearestKey = -1;
    float nearestGap = FLT_MAX;
    for (std::size_t k = 0; k < view.motion->keys.size(); ++k) {
        const float gap = std::abs(view.motion->keys[k].time - evalTime);
        if (gap < nearestGap) {
            nearestGap = gap;
            nearestKey = static_cast<int>(k);
        }
    }

    constexpr ImU32 kFill = IM_COL32(255, 255, 255, 245);
    constexpr ImU32 kOutline = IM_COL32(20, 20, 24, 230);
    constexpr ImU32 kKeyFill = IM_COL32(200, 200, 210, 230);
    constexpr ImU32 kCurrentKeyFill = IM_COL32(255, 215, 90, 255);
    constexpr float kGlyph = 4.0f;

    for (const FluidPartHandle& handle : handles) {
        if (handle.kind != FluidHandleKind::MotionKey) continue;
        const bool current = handle.keyIndex == nearestKey;
        DrawDiamond(drawList, handle.screen, current ? kGlyph * 1.5f : kGlyph * 1.1f, current ? kCurrentKeyFill : kKeyFill,
                    kOutline);
    }
    const ImVec2 center = handles.front().screen;
    for (const FluidPartHandle& handle : handles) {
        const ImVec2& p = handle.screen;
        switch (handle.kind) {
        case FluidHandleKind::Center:
            drawList->AddCircleFilled(p, kGlyph + 0.5f, kFill);
            drawList->AddCircle(p, kGlyph + 0.5f, kOutline, 0, 1.5f);
            break;
        case FluidHandleKind::Size:
            drawList->AddRectFilled({ p.x - kGlyph, p.y - kGlyph }, { p.x + kGlyph, p.y + kGlyph }, kFill);
            drawList->AddRect({ p.x - kGlyph, p.y - kGlyph }, { p.x + kGlyph, p.y + kGlyph }, kOutline, 0.0f, 1.5f);
            break;
        case FluidHandleKind::Direction:
            drawList->AddLine(center, p, WithAlpha(kFill, 150), 1.0f);
            drawList->AddCircleFilled(p, kGlyph, kFill);
            drawList->AddCircle(p, kGlyph, kOutline, 0, 1.5f);
            break;
        case FluidHandleKind::MotionKey:
            break;
        }
    }
}

} // namespace

void DrawFluidPartOverlays(ImDrawList* drawList, const FluidViewMapping& mapping, const fluid::FluidRecipe& recipe,
                           float time, const FluidSelection& selection, const FluidPartVisibility& visible)
{
    if (drawList == nullptr || mapping.size <= 0.0f) return;
    /// @note ソルバーの時計は warmup を回した後から数えている。部品の姿も同じ時刻で出す。
    const float evalTime = EvalTime(recipe, time);
    const auto isSelected = [&selection](Kind list, int index) {
        return selection.kind == list && selection.index == index;
    };
    drawList->PushClipRect(mapping.origin, { mapping.origin.x + mapping.size, mapping.origin.y + mapping.size }, true);
    for (int i = 0; i < static_cast<int>(recipe.colliders.size()); ++i) {
        if (!IsVisible(visible, Kind::Collider, i)) continue;
        DrawCollider(drawList, mapping, recipe.colliders[static_cast<std::size_t>(i)], evalTime,
                     isSelected(Kind::Collider, i));
    }
    for (int i = 0; i < static_cast<int>(recipe.sources.size()); ++i) {
        if (!IsVisible(visible, Kind::Source, i)) continue;
        DrawSource(drawList, mapping, recipe, recipe.sources[static_cast<std::size_t>(i)], evalTime,
                   isSelected(Kind::Source, i));
    }
    int globalSlot = 0;
    for (int i = 0; i < static_cast<int>(recipe.forces.size()); ++i) {
        if (!IsVisible(visible, Kind::Force, i)) continue;
        DrawForce(drawList, mapping, recipe.forces[static_cast<std::size_t>(i)], evalTime, isSelected(Kind::Force, i),
                  globalSlot);
    }
    DrawSelectedHandles(drawList, mapping, recipe, evalTime, selection, visible);
    drawList->PopClipRect();
}

std::vector<FluidPartHandle> CollectFluidPartHandles(const FluidViewMapping& mapping, const fluid::FluidRecipe& recipe,
                                                     float time, const FluidSelection& selection,
                                                     const FluidPartVisibility& visible)
{
    std::vector<FluidPartHandle> handles;
    const float evalTime = EvalTime(recipe, time);
    /// @note 選択中の部品を先頭に置く (PickFluidPartHandle は近さが並んだら先のものを取る)。
    if (selection.IsPart() && IsVisible(visible, selection.kind, selection.index))
        CollectPartHandles(handles, mapping, recipe, selection.kind, selection.index, evalTime, true);
    const auto collectList = [&](Kind list, int count) {
        for (int i = 0; i < count; ++i) {
            if ((selection.kind == list && selection.index == i) || !IsVisible(visible, list, i)) continue;
            CollectPartHandles(handles, mapping, recipe, list, i, evalTime, false);
        }
    };
    /// @note 残りは上に描かれるもの (力 → 発生源 → 障害物) から並べる。
    collectList(Kind::Force, static_cast<int>(recipe.forces.size()));
    collectList(Kind::Source, static_cast<int>(recipe.sources.size()));
    collectList(Kind::Collider, static_cast<int>(recipe.colliders.size()));
    return handles;
}

const FluidPartHandle* PickFluidPartHandle(const std::vector<FluidPartHandle>& handles, ImVec2 mouse)
{
    const FluidPartHandle* best = nullptr;
    float bestDistance = FLT_MAX;
    for (const FluidPartHandle& handle : handles) {
        const float distance = ScreenDistance(handle.screen, mouse);
        if (distance > handle.radius) continue;
        if (best == nullptr || distance + kHandleTieSlackPixels < bestDistance) {
            best = &handle;
            bestDistance = distance;
        }
    }
    return best;
}

void ApplyFluidHandleDrag(fluid::FluidRecipe& recipe, const FluidPartHandle& handle,
                          const math::Vector3& domainPosition, float time)
{
    const float evalTime = EvalTime(recipe, time);
    const std::size_t index = static_cast<std::size_t>(handle.index);
    switch (handle.list) {
    case Kind::Source: {
        if (handle.index < 0 || index >= recipe.sources.size()) return;
        FluidSource& source = recipe.sources[index];
        const math::Vector3 posed = fluid::PoseFluidSource(source, evalTime).center;
        if (!ApplyCommonDrag(source.center, source.motion, posed, handle, domainPosition))
            ApplySourceShapeDrag(source, handle.kind, posed, domainPosition);
        break;
    }
    case Kind::Force: {
        if (handle.index < 0 || index >= recipe.forces.size()) return;
        FluidForce& force = recipe.forces[index];
        const math::Vector3 posed = fluid::PoseFluidForce(force, evalTime).center;
        if (!ApplyCommonDrag(force.center, force.motion, posed, handle, domainPosition))
            ApplyForceShapeDrag(force, handle.kind, posed, domainPosition);
        break;
    }
    case Kind::Collider: {
        if (handle.index < 0 || index >= recipe.colliders.size()) return;
        FluidCollider& collider = recipe.colliders[index];
        const math::Vector3 posed = fluid::PoseFluidCollider(collider, evalTime).center;
        if (!ApplyCommonDrag(collider.center, collider.motion, posed, handle, domainPosition))
            ApplyColliderShapeDrag(collider, handle.kind, posed, domainPosition);
        break;
    }
    default:
        break;
    }
}

FluidSelection PickFluidPart(const FluidViewMapping& mapping, const fluid::FluidRecipe& recipe, float time,
                             ImVec2 mouse, const FluidPartVisibility& visible)
{
    if (mapping.size <= 0.0f) return FluidSelection{};
    const float evalTime = EvalTime(recipe, time);
    const math::Vector3 p = mapping.ToDomain(mouse);
    const float tolerance = DomainLength(mapping, kPickTolerancePixels);
    const float icon = IconPixels(mapping);

    /// @note 描いた順 (障害物 → 発生源 → 力、各リストは添字順) の逆から当てる = 画面で一番上に見えているもの。
    ///       障害物は大きく下敷きになりやすいので、小さな力のアイコンや発生源が先に取れる。
    for (int i = static_cast<int>(recipe.forces.size()) - 1; i >= 0; --i) {
        if (!IsVisible(visible, Kind::Force, i)) continue;
        const FluidForce& force = recipe.forces[static_cast<std::size_t>(i)];
        if (!ForceIsPlaced(force)) {
            const ImVec2 c = GlobalForceIconCenter(mapping, GlobalForceSlot(recipe, i, visible));
            if (std::abs(mouse.x - c.x) <= icon * 1.3f && std::abs(mouse.y - c.y) <= icon * 1.3f)
                return FluidSelection{ Kind::Force, i };
            continue;
        }
        const math::Vector3 c = fluid::PoseFluidForce(force, evalTime).center;
        const float distance = std::hypot(p.x - c.x, p.y - c.y);
        if (distance <= DomainLength(mapping, icon * 1.2f)
            || (force.radius > 0.0f && std::abs(distance - force.radius) <= tolerance))
            return FluidSelection{ Kind::Force, i };
    }
    for (int i = static_cast<int>(recipe.sources.size()) - 1; i >= 0; --i) {
        if (!IsVisible(visible, Kind::Source, i)) continue;
        const FluidSource& source = recipe.sources[static_cast<std::size_t>(i)];
        if (SourceContains(source, fluid::PoseFluidSource(source, evalTime).center, p, tolerance))
            return FluidSelection{ Kind::Source, i };
    }
    for (int i = static_cast<int>(recipe.colliders.size()) - 1; i >= 0; --i) {
        if (!IsVisible(visible, Kind::Collider, i)) continue;
        const FluidCollider& collider = recipe.colliders[static_cast<std::size_t>(i)];
        if (ColliderContains(collider, fluid::PoseFluidCollider(collider, evalTime).center, p, tolerance))
            return FluidSelection{ Kind::Collider, i };
    }
    return FluidSelection{};
}

} // namespace fbzz::editor
