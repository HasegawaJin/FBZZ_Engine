/// @file    MeshBuilder.hpp
/// @brief   スクリプトから手続き的にメッシュを組み立てる CPU 側の作業台
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Renderer/Mesh.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::scene {

/// MeshBuilder が積む頂点。GPU へ渡す並びそのままなので変換コピーが要らない。
using MeshVertex = renderer::Vertex;

/// 手続きメッシュの «どこが変わったか»。
///
/// WHY 区別するか: トポロジを変えずに頂点だけ動かす演出 (脈動・波打ち・なびき) では
///      インデックスバッファの中身は 1 バイトも変わらない。毎フレーム上げ直すのは
///      そのぶん丸ごと無駄なので、頂点だけの更新を明示できるようにする。
enum class MeshDirty {
    None,      ///< 変更なし。アップロードしない。
    Vertices,  ///< 頂点だけ変わった。インデックスバッファには触らない。
    All        ///< 頂点数・三角形が変わった。両方を上げ直す。
};

/// 頂点とインデックスを積み上げてメッシュ 1 個ぶんの形を作る。
///
/// GPU リソースには触れない純粋な CPU 側のデータで、アップロードは
/// ScriptMeshProxy::Apply → RuntimeMeshSystem が LateUpdate に行う。
/// スクリプトのフィールドとして持ち続けても、毎フレーム作り直しても構わない。
///
/// 座標系はすべてオブジェクトのローカル空間。ワールド空間で組みたい場合は
/// GameObject を原点に置くか、Transform() で自分で移す。
class MeshBuilder {
public:
    // ── 状態 ──────────────────────────────────────────────────────────────

    void Clear();
    /// 積む量が読めているときに確保しておく。毎フレーム組み直す形では効きが大きい。
    void Reserve(std::size_t vertexCount, std::size_t indexCount);

    [[nodiscard]] bool     Empty()         const { return m_indices.empty(); }
    [[nodiscard]] uint32_t VertexCount()   const { return static_cast<uint32_t>(m_vertices.size()); }
    [[nodiscard]] uint32_t IndexCount()    const { return static_cast<uint32_t>(m_indices.size()); }
    [[nodiscard]] uint32_t TriangleCount() const { return static_cast<uint32_t>(m_indices.size() / 3); }

    /// 積んだ頂点への直接アクセス。トポロジを変えずに頂点だけ動かす演出に使う。
    [[nodiscard]] std::vector<MeshVertex>&       Vertices()       { return m_vertices; }
    [[nodiscard]] const std::vector<MeshVertex>& Vertices() const { return m_vertices; }
    [[nodiscard]] std::vector<uint32_t>&         Indices()        { return m_indices; }
    [[nodiscard]] const std::vector<uint32_t>&   Indices()  const { return m_indices; }

    // ── 手組み ────────────────────────────────────────────────────────────

    /// 頂点を 1 個足してその添字を返す。
    uint32_t AddVertex(const MeshVertex& vertex);
    uint32_t AddVertex(const math::Vector3& position,
                       const math::Vector3& normal,
                       const math::Vector2& uv);
    uint32_t AddVertex(const math::Vector3& position,
                       const math::Vector3& normal,
                       const math::Vector2& uv,
                       const math::Vector4& color);

    /// 時計回りが表 (DirectX 左手系の既定カリング)。
    void AddTriangle(uint32_t a, uint32_t b, uint32_t c);
    /// 四角形を a-b-c / a-c-d の 2 枚に割る。
    void AddQuad(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

    // ── 色 ────────────────────────────────────────────────────────────────

    /// これ以降に積む頂点の既定色。形状ヘルパーもこの色を使う。
    void SetColor(const math::Vector4& color) { m_color = color; }
    [[nodiscard]] math::Vector4 GetColor() const { return m_color; }

    /// 既に積んだ頂点を塗り直す。
    void ColorizeAll(const math::Vector4& color);
    void ColorizeRange(uint32_t firstVertex, uint32_t count, const math::Vector4& color);

    // ── 形状 ──────────────────────────────────────────────────────────────
    // すべて「今ある内容へ足す」。連続して呼べば 1 メッシュに複数の形が入る。

    /// right / up が板の半径ベクトル (長さがそのまま半分の大きさ)。
    void AddQuad(const math::Vector3& center,
                 const math::Vector3& right,
                 const math::Vector3& up);
    /// 分割した板。subdivisions は 1 辺あたりの分割数 (1 で AddQuad と同じ)。
    void AddPlane(const math::Vector3& center,
                  const math::Vector3& right,
                  const math::Vector3& up,
                  int subdivisions);
    void AddBox(const math::Vector3& center, const math::Vector3& halfExtents);
    /// 経緯度分割の球。rings は極から極までの段数。
    void AddSphere(const math::Vector3& center, float radius, int segments = 24, int rings = 16);
    /// 正二十面体を分割した球。極が詰まらないので変形させる用途に向く。
    void AddIcoSphere(const math::Vector3& center, float radius, int subdivisions = 2);
    void AddCylinder(const math::Vector3& baseCenter, const math::Vector3& axis,
                     float radius, float height, int segments = 24, bool capped = true);
    void AddCone(const math::Vector3& baseCenter, const math::Vector3& axis,
                 float radius, float height, int segments = 24, bool capped = true);
    /// height は円柱部分の長さ (全長は height + radius * 2)。
    void AddCapsule(const math::Vector3& center, const math::Vector3& axis,
                    float radius, float height, int segments = 24, int rings = 8);
    void AddTorus(const math::Vector3& center, const math::Vector3& axis,
                  float majorRadius, float minorRadius,
                  int majorSegments = 32, int minorSegments = 12);
    /// 円板。innerRadius > 0 でリングになる (衝撃波の輪)。
    void AddDisc(const math::Vector3& center, const math::Vector3& normal,
                 float radius, int segments = 32, float innerRadius = 0.0f);
    /// 平面上の凸多角形を扇状に張る。points は normal 側から見て時計回りに並べること。
    void AddPolygon(std::span<const math::Vector3> points, const math::Vector3& normal);
    /// 多角形を extrusion ぶん押し出す。capped で上下の蓋を付ける。
    /// points は extrusion 側から見て時計回りに並べること (AddPolygon と同じ規約)。
    void ExtrudePolygon(std::span<const math::Vector3> points,
                        const math::Vector3& extrusion, bool capped = true);
    /// 回転体。profile の x = 軸からの距離、y = 軸方向の位置。
    void AddLathe(std::span<const math::Vector2> profile,
                  const math::Vector3& center, const math::Vector3& axis,
                  int segments = 24);

    /// 折れ線に沿った帯。up が帯の «厚みのない» 面の向きを決める。
    /// widths は points と同数。1 個だけ渡せば全体で一定幅になる。
    void AddRibbon(std::span<const math::Vector3> points,
                   std::span<const float> widths,
                   const math::Vector3& up);
    /// 帯を viewPosition の側へ正対させる。斬撃・ビームはこちらを使う。
    ///
    /// WHY 別関数か: 固定の up で組んだ帯は、その面を真横から見た瞬間に消える。
    ///      視点方向と進行方向の外積で毎フレーム面を作り直せば、どの角度からも
    ///      幅が保たれる (LineRenderer が内部でやっているのと同じこと)。
    void AddRibbonFacing(std::span<const math::Vector3> points,
                         std::span<const float> widths,
                         const math::Vector3& viewPosition);
    /// 折れ線に沿った筒。radii は points と同数、または 1 個。
    void AddTube(std::span<const math::Vector3> points,
                 std::span<const float> radii,
                 int sides = 8, bool capped = false);

    // ── 加工 ──────────────────────────────────────────────────────────────

    void Transform(const math::Matrix4& matrix);
    void TransformRange(uint32_t firstVertex, uint32_t count, const math::Matrix4& matrix);

    /// 別の builder の内容を取り込む。添字は自動でずらす。
    void Append(const MeshBuilder& other);
    void Append(const MeshBuilder& other, const math::Matrix4& matrix);

    /// 面法線を面積重み付きで頂点へ平均する。手組みした後は基本これを呼ぶ。
    void RecalculateNormals();
    /// UV から接線を求める。法線マップを使うマテリアルで要る。
    void RecalculateTangents();
    /// 巻き順と法線を同時に裏返す。
    void FlipFaces();
    /// 法線だけ裏返す (内側から見せる筒など)。
    void FlipNormals();
    /// epsilon 以内の同一位置の頂点をまとめる。滑らかな法線を作る前に使う。
    void Weld(float epsilon = 0.0001f);
    /// 三角形を中点で 4 分割する。levels 回繰り返す。
    void Subdivide(int levels = 1);

private:
    /// 現在の色を乗せて頂点を積む内部用。
    uint32_t Push(const math::Vector3& position, const math::Vector3& normal, const math::Vector2& uv);

    std::vector<MeshVertex> m_vertices;
    std::vector<uint32_t>   m_indices;
    math::Vector4           m_color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

} // namespace fbzz::scene
