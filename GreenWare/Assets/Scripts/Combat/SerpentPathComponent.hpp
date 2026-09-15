/// @file    SerpentPathComponent.hpp
/// @brief   口から口へ渡る 1 本の経路。地上の円弧を床下のリンクで数珠繋ぎに持つ
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 経路が必ず円弧か (boss-serpent.md「検算」):
///   同じ «弦と弧長» を満たす形のうち最大曲率が最小になるのは曲率一定の円弧で、
///   正弦の山だと曲がりが頂上へ集中して 2 倍近く折れる (内→内で 30.2度 対 58.9度)。
///   装甲の限界は «1 関節あたり 12 度» なので、どこか 1 箇所でも超えれば破綻する ─
///   均す形を選ぶしかない。
///
/// WHY 経路を «張り替え» ずに継ぎ足すか:
///   1 本の弧だけを持って口から口へ張り替えると、渡り終えた瞬間に鎖の弧長が 0 へ戻る。
///   胴は «頭の弧長 − 累積» で置いているので、これは 24 m の胴が 1 フレームで
///   前後ひっくり返るということ ─ しかも張り替えてよいのは «全身が床下に入りきってから»
///   なので、蛇が盤面から完全に消える時間が毎回できる。
///   弧を継ぎ足して s を伸ばし続ければ、頭が次の口から出てくる頃に尾はまだ前の弧に居る。
///   出入りが «糸を通す» 形になり、跳ぶ瞬間も消える瞬間も無くなる。
///
/// WHY 口と口を繋ぐ床下リンクが要るか:
///   継ぎ足すには «入った口から出る口まで» の床下の道が要る。端の接線を延ばすだけでは
///   道にならない (次の口へ着かない)。ここでは口での接線を保ったまま繋ぐ 3 次曲線を
///   1 本張り、弧長の表を作って «長さで» 引けるようにしてある。
///
/// WHY リンクを浅く保つか:
///   胴が途切れずに見えているのは «リンクが胴より短いあいだ» だけ。深さ 6.2 m で
///   寝かせる端の延長 (Tail Depth) と同じ作りにすると片道 15 m を超え、渡っている
///   最中に全身が床下へ入る。リンクは «床スラブを抜けるだけ» の深さで足りる。
///
/// WHY エンジンと Blender で同じ式を持つか:
///   `serpent_arena.py` の arch() / route() / at() が «この寸法で胴が渡れるか» を
///   検算している。式が 2 通りあると、検算が通っているのに実機で折れる。
///   片方を直したらもう片方も直すこと (検算しているのは地上の弧の側)。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentPathComponent : public Script {
    FBZZ_SCRIPT(SerpentPathComponent)

public:
    FBZZ_GROUP("受付時間")
    // 渡れる間隔には上下の窓がある。狭いと折れ角が上限を超える。
    //
    // WHY 上限が «露出長より短く» なければならないか (企画の 11.5 m から下げた理由):
    //   弧長は露出長 (11.2 m) で固定なので、弦がそれに近づくほど弧は伸びきって直線になる。
    //   実測した口の組のうち 11.39 m の 4 組 (I2-O1 / I3-O5 / I5-O6 / I6-O10) は
    //   弦の方が弧より長く、丸めで «山 0 m・半径 56000 m» ─ つまり床に寝た棒になっていた。
    //   «渡っている» に見える最低限の山 (約 2 m) を残すには弦を露出長の 9 割あたりで切る。
    //   10.5 m で切っても各口に 2〜4 本の渡り先が残る (グラフは連結のまま)。
    FBZZ_FIELD_RANGE(float, minChord, 8.0f, "Min Chord", 1.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, maxChord, 10.5f, "Max Chord", 1.0f, 40.0f)
    FBZZ_TOOLTIP("胴が渡れる穴の間隔。外れた組を渡そうとしたら名指しで警告する。"
                 "上限は露出長 (SerpentAiComponent の Exposed) より必ず短くすること")
    FBZZ_FIELD(bool, warnOutOfWindow, true, "Warn Out Of Window")

    FBZZ_GROUP("Underground")
    // 経路の «端» ─ 尾の先と、まだ次を決めていない頭の先。
    //
    // WHY 接線のまま延ばさないか: 弦 8 m の経路は口での接線が水平から 78 度もあり、
    //     接線のまま 11.2 m 延ばすと胴は y = -11 m まで潜る。床下スラブ (ARENA_Pit) は
    //     -7.4 m にあるので、開いた口を覗くと胴がその床を突き抜けているのが見える。
    FBZZ_FIELD_RANGE(float, undergroundDepth, 6.2f, "Tail Depth", 0.5f, 20.0f)
    FBZZ_TOOLTIP("経路の端で水平になるまでに降りる深さ。床下スラブ (-7.4 m) より浅くすること")
    FBZZ_FIELD_RANGE(float, linkDepth, 2.6f, "Link Depth", 0.5f, 7.0f)
    FBZZ_TOOLTIP("口から口へ潜って渡るときの深さ。深いほど床下の道が長くなり、"
                 "渡っている最中に胴が全部床下へ入ってしまう。"
                 "床スラブ (厚み 0.6 m) と胴の太さ (半径 0.62 m) を抜ける分があれば足りる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, drawPath, false, "Draw Path")
    FBZZ_FIELD_READ_ONLY(std::string, debugRoute, "-", "経路")
    FBZZ_FIELD_READ_ONLY(float, debugChord, 0.0f, "Chord")
    FBZZ_FIELD_READ_ONLY(float, debugApex, 0.0f, "Apex")
    FBZZ_FIELD_READ_ONLY(float, debugRadius, 0.0f, "半径")
    FBZZ_FIELD_READ_ONLY(float, debugLink, 0.0f, "Link Length")
    FBZZ_FIELD_READ_ONLY(int, debugPieces, 0, "Pieces")

    /// 経路を張り直す。s = 0 が from の口。
    /// @param exposedMeters 地上に出す弧の長さ。弦より短い値は弦へ丸める。
    void BeginRoute(const std::string& from, const std::string& to,
                    const Vector3& fromCenter, const Vector3& toCenter, float exposedMeters);
    /// 今の終端の口から床下を通って exit の口まで潜り、そこから to へ弧を継ぎ足す。
    /// @ret 足せたら true。
    bool AppendRoute(const std::string& exitHole, const Vector3& exitCenter,
                     const std::string& to, const Vector3& toCenter, float exposedMeters);
    /// 現在の終端から床上だけを走る弧を継ぎ足す。穴を開けずに地上移動を続けるために使う。
    bool AppendSurfaceRoute(const std::string& from, const Vector3& fromCenter,
                            const std::string& to, const Vector3& toCenter, float exposedMeters);
    /// 経路を持っているか。張る前は At() が原点を返すだけになる。
    [[nodiscard]] bool Valid() const { return !m_pieces.empty(); }

    /// 最後の弧の «弧長» だけを差し替える。口 (弦) は動かさない。@ret 直せたら true。
    ///
    /// WHY これがせり上がりになるか: 弦が固定のまま弧を伸ばすと、伸びたぶんは
    ///     まるごと «山の高さ» になる (弧長 11.2 m で山 3.3 m、16 m で山 5.6 m)。
    ///     胴は弧長で置いてあるので、経路を伸ばすだけで蛇が持ち上がる。
    ///     半径は弧と一緒に育つので、折れ角は 11 度前後のまま増えない。
    bool ReshapeLast(float exposedMeters);

    /// 弧長 s の位置。両端の外は床下へ寝かせながら延ばす。
    [[nodiscard]] Vector3 At(float s) const;
    /// 弧長 s での進行方向 (正規化済み)。
    [[nodiscard]] Vector3 Tangent(float s) const;

    /// s が地上に出ているか。極を乗せられるのも斬れるのも出ている節だけ。
    [[nodiscard]] bool OnSurface(float s) const;
    /// s を含む地上の弧の中での位置 [0,1] と、その弧の半径。床下なら false。
    bool SurfaceLocal(float s, float* out01, float* outRadius) const;

    /// 最後に継いだ地上の弧の始まり / 終わり [m]。
    [[nodiscard]] float LastArcStart() const;
    [[nodiscard]] float LastArcEnd() const;
    /// 最後の弧の «構える» 位置。ratio は弧の中での割合。
    [[nodiscard]] float HoldArc(float ratio) const
    {
        const float start = LastArcStart();
        return start + (LastArcEnd() - start) * Clamp01(ratio);
    }

    /// 尾より後ろの弧とリンクを捨てる。s は詰めない (float の精度は 1 戦分では余る)。
    void PruneBefore(float keep);

    /// 最後の弧の長さ [m]。
    [[nodiscard]] float LastArcLength() const { return LastArcEnd() - LastArcStart(); }
    /// 最後の弧の 2 口の中点 (床面)。叩きつけの着弾はここで見る。
    [[nodiscard]] Vector3 LastArcGroundCenter() const;
    /// 点を最後の弧へ落としたときの弧長。頭が «プレイヤーの正面» へ滑るのに使う。
    ///
    /// WHY 弦へ落とすだけで足りるか: 弧は 2 口を通る垂直な面の中にあるので、
    ///     床への影はちょうど 2 口を結ぶ線分になる。上下の分は頭の狙いが持つ。
    [[nodiscard]] float ProjectOnLastArc(const Vector3& point) const;
    /// 最後の弧の山の高さ [m]。
    [[nodiscard]] float ApexHeight() const;
    /// 最後の弧の半径。関節 1 つの折れ角は (関節長 / 半径) で出る。
    [[nodiscard]] float Radius() const;
    /// 半径 radius の弧を関節長 jointMeters で辿るときの 1 関節あたりの折れ角 [deg]。
    [[nodiscard]] static float BendPerJointDegrees(float radius, float jointMeters)
    {
        return radius > EPSILON ? ToDeg(jointMeters / radius) : 0.0f;
    }

    [[nodiscard]] const std::string& From() const;
    [[nodiscard]] const std::string& To() const;

    void OnUpdate() override;

private:
    /// 地上の円弧 1 本、または口と口を繋ぐ床下のリンク 1 本。
    struct Piece {
        bool        surface = true;
        float       start   = 0.0f;   ///< 経路全体での弧長
        float       length  = 0.0f;

        // 地上の弧
        std::string from;
        std::string to;
        Vector3     a{};              ///< 出る口 (床面)
        Vector3     b{};              ///< 入る口 (床面)
        Vector3     u{ 0.0f, 0.0f, 1.0f };  ///< a → b の水平単位ベクトル
        Vector3     center{};         ///< 円の中心。床より下にある
        float       radius = 0.0f;
        float       theta  = 0.0f;    ///< 半開角 [rad]
        float       apex   = 0.0f;

        // 床下のリンク。3 次曲線を等分割し、弧長で引けるようにしてある。
        std::vector<Vector3> samples;
        std::vector<float>   arcs;
    };

    static constexpr int kLinkSamples = 48;

    /// sin(θ)/θ = ratio を満たす θ を二分法で解く。ratio は (0,1)。
    ///
    /// WHY 閉じた形で解かないか: sin(θ)/θ = c に初等関数の逆は無い。二分法は
    ///     単調な区間 (0,π) で 40 回も回せば float の精度に収まる。
    [[nodiscard]] static float SolveHalfAngle(float ratio);

    /// 2 口から地上の弧を組む。@ret 組めたら true。
    /// @param warn 窓を外れた組を名指しするか。せり上がりのように毎フレーム組み直す
    ///             ときは切る ─ 同じ警告が 60 回/秒 出てログが読めなくなる。
    bool ShapeArc(Piece& piece, const std::string& from, const std::string& to,
                  const Vector3& a, const Vector3& b, float exposedMeters, bool warn = true);
    /// prev の入る口から next の出る口までを床下で繋ぐ。
    ///
    /// WHY 3 次曲線か: 口での向きは弧が既に決めていて (下向き 48〜78 度)、
    ///     そこを外すと胴が口の縁を舐める。両端の «点と向き» を固定して繋げる
    ///     一番低い次数がエルミートの 3 次で、深さは接線の長さだけで決まる
    ///     (最深 = 0.25 · 接線長 · sinθ なので、欲しい深さから逆に解ける)。
    void BuildLink(Piece& link, const Piece& prev, const Piece& next);

    [[nodiscard]] static Vector3 ArcAt(const Piece& piece, float local);
    [[nodiscard]] static Vector3 ArcTangent(const Piece& piece, float local);
    static void LinkSample(const Piece& piece, float local, Vector3* outPosition,
                           Vector3* outTangent);

    /// 口から床下へ u [m] 進んだ位置と向き。経路の «端» を延ばすのに使う。
    ///
    /// 口での接線 (水平から θ だけ下向き) から水平へ、半径 R2 の円弧で寝かせる。
    /// 深さ d で水平になる条件から R2 = d / (1 - cosθ)。曲がりきった後は水平に直進。
    /// @param mouth 出入口 (床面)
    /// @param hDir  床下へ抜けていく側の水平単位ベクトル
    void Underground(float u, const Vector3& mouth, const Vector3& hDir, float theta,
                     Vector3* outPosition, Vector3* outTangent) const;

    [[nodiscard]] const Piece* PieceAt(float s) const;

    std::vector<Piece> m_pieces;
};

FBZZ_REFLECT(SerpentPathComponent)

inline float SerpentPathComponent::SolveHalfAngle(float ratio)
{
    const float target = Clamp(ratio, 0.001f, 0.9999f);
    float low  = 1.0e-4f;
    float high = PI - 1.0e-4f;
    for (int i = 0; i < 48; ++i) {
        const float mid = (low + high) * 0.5f;
        // sin(θ)/θ は (0, π) で単調に減る。
        if (std::sin(mid) / mid > target) low = mid;
        else                              high = mid;
    }
    return (low + high) * 0.5f;
}

inline bool SerpentPathComponent::ShapeArc(Piece& piece, const std::string& from,
                                           const std::string& to, const Vector3& a,
                                           const Vector3& b, float exposedMeters, bool warn)
{
    piece.surface = true;
    piece.from    = from;
    piece.to      = to;
    piece.a       = a;
    piece.b       = b;

    const Vector3 delta{ b.x - a.x, 0.0f, b.z - a.z };
    const float   chord = delta.Length();
    if (chord < 0.5f) {
        if (warn)
            debug.LogWarning("SerpentPathComponent: route " + from + " -> " + to +
                             " has no length (the two holes are at the same place).");
        return false;
    }
    piece.u = delta / chord;

    if (warn && warnOutOfWindow && (chord < minChord || chord > maxChord)) {
        // 狭いと折れすぎ、広いと胴が床に埋まる。どちらも «なんとなく変» にしか
        // 見えないので、渡る前に名指しで言う。
        debug.LogWarning("SerpentPathComponent: route " + from + " -> " + to + " spans " +
                         std::to_string(chord) + " m, outside the " + std::to_string(minChord) +
                         "-" + std::to_string(maxChord) +
                         " m window that the body can arch over.");
    }

    // 弧は弦より必ず長い。等しいと直線になり、山が立たない。
    piece.length = Max(exposedMeters, chord * 1.02f);
    piece.theta  = SolveHalfAngle(chord / piece.length);
    piece.radius = piece.length / (2.0f * Max(piece.theta, 1.0e-4f));

    // 中心は弦の中点から «下» へ R·cosθ。山は上へ R(1-cosθ)。
    const Vector3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    piece.center = mid - Vector3::UP * (piece.radius * std::cos(piece.theta));
    piece.apex   = piece.radius * (1.0f - std::cos(piece.theta));

    debugChord  = chord;
    debugApex   = piece.apex;
    debugRadius = piece.radius;
    return true;
}

inline void SerpentPathComponent::BuildLink(Piece& link, const Piece& prev, const Piece& next)
{
    link.surface = false;

    const Vector3 p0 = prev.b;                        // 潜る口
    const Vector3 p1 = next.a;                        // 出る口
    const Vector3 t0 = ArcTangent(prev, prev.length); // 下向き
    const Vector3 t1 = ArcTangent(next, 0.0f);        // 上向き

    // 最深は 0.25 · scale · sinθ。欲しい深さから接線の長さを逆に出す。
    const float sinAvg = Max((std::sin(prev.theta) + std::sin(next.theta)) * 0.5f, 0.05f);
    const float scale  = 4.0f * Max(linkDepth, 0.2f) / sinAvg;

    link.samples.resize(kLinkSamples + 1);
    link.arcs.resize(kLinkSamples + 1);
    for (int i = 0; i <= kLinkSamples; ++i) {
        const float t   = static_cast<float>(i) / static_cast<float>(kLinkSamples);
        const float t2  = t * t;
        const float t3  = t2 * t;
        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;
        link.samples[static_cast<std::size_t>(i)] =
            p0 * h00 + t0 * (scale * h10) + p1 * h01 + t1 * (scale * h11);
    }

    link.arcs[0] = 0.0f;
    for (int i = 1; i <= kLinkSamples; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        link.arcs[k] = link.arcs[k - 1] + (link.samples[k] - link.samples[k - 1]).Length();
    }
    link.length = link.arcs[static_cast<std::size_t>(kLinkSamples)];
    debugLink   = link.length;
}

inline void SerpentPathComponent::BeginRoute(const std::string& from, const std::string& to,
                                             const Vector3& fromCenter, const Vector3& toCenter,
                                             float exposedMeters)
{
    m_pieces.clear();

    Piece arc;
    arc.start = 0.0f;
    if (!ShapeArc(arc, from, to, fromCenter, toCenter, exposedMeters)) return;

    m_pieces.push_back(std::move(arc));
    debugRoute  = from + " -> " + to;
    debugPieces = static_cast<int>(m_pieces.size());
}

inline bool SerpentPathComponent::AppendRoute(const std::string& exitHole,
                                              const Vector3& exitCenter, const std::string& to,
                                              const Vector3& toCenter, float exposedMeters)
{
    if (m_pieces.empty()) return false;

    const Piece& tail = m_pieces.back();
    Piece        arc;
    if (!ShapeArc(arc, exitHole, to, exitCenter, toCenter, exposedMeters)) return false;

    Piece link;
    BuildLink(link, tail, arc);
    link.start = tail.start + tail.length;
    arc.start  = link.start + link.length;

    m_pieces.push_back(std::move(link));
    m_pieces.push_back(std::move(arc));
    debugRoute  = m_pieces.back().from + " -> " + m_pieces.back().to;
    debugPieces = static_cast<int>(m_pieces.size());
    return true;
}

inline bool SerpentPathComponent::AppendSurfaceRoute(const std::string& from,
                                                     const Vector3& fromCenter,
                                                     const std::string& to,
                                                     const Vector3& toCenter,
                                                     float exposedMeters)
{
    if (m_pieces.empty()) return false;

    Piece arc;
    if (!ShapeArc(arc, from, to, fromCenter, toCenter, exposedMeters)) return false;
    arc.start = m_pieces.back().start + m_pieces.back().length;
    m_pieces.push_back(std::move(arc));
    debugRoute  = from + " -> " + to;
    debugPieces = static_cast<int>(m_pieces.size());
    return true;
}

inline bool SerpentPathComponent::ReshapeLast(float exposedMeters)
{
    if (m_pieces.empty()) return false;

    // 自分の値を引数に渡すことになるので、先に控えてから組み直す。
    Piece&            piece = m_pieces.back();
    const std::string from  = piece.from;
    const std::string to    = piece.to;
    const Vector3     a     = piece.a;
    const Vector3     b     = piece.b;
    const float       start = piece.start;

    if (!ShapeArc(piece, from, to, a, b, exposedMeters, false)) return false;
    piece.start = start;
    return true;
}

inline void SerpentPathComponent::PruneBefore(float keep)
{
    // 先頭は必ず地上の弧にしておく ─ 経路より手前は «その弧の口から床下へ» 延ばすので、
    // リンクが先頭に来ると尾の先が置けなくなる。弧とリンクを 2 つ 1 組で捨てる。
    while (m_pieces.size() >= 3 && m_pieces[1].start + m_pieces[1].length < keep)
        m_pieces.erase(m_pieces.begin(), m_pieces.begin() + 2);
    debugPieces = static_cast<int>(m_pieces.size());
}

inline Vector3 SerpentPathComponent::ArcAt(const Piece& piece, float local)
{
    const float phi = -piece.theta + local / Max(piece.radius, 1.0e-4f);
    return piece.center + (Vector3::UP * std::cos(phi) + piece.u * std::sin(phi)) * piece.radius;
}

inline Vector3 SerpentPathComponent::ArcTangent(const Piece& piece, float local)
{
    const float phi = -piece.theta + local / Max(piece.radius, 1.0e-4f);
    return (piece.u * std::cos(phi) - Vector3::UP * std::sin(phi))
        .NormalizedOr(Vector3{ 0.0f, 0.0f, 1.0f });
}

inline void SerpentPathComponent::LinkSample(const Piece& piece, float local,
                                             Vector3* outPosition, Vector3* outTangent)
{
    const int count = static_cast<int>(piece.samples.size());
    if (count < 2) {
        if (outPosition) *outPosition = piece.samples.empty() ? Vector3::ZERO : piece.samples[0];
        if (outTangent)  *outTangent  = Vector3{ 0.0f, 0.0f, 1.0f };
        return;
    }

    const float s = Clamp(local, 0.0f, piece.length);
    const auto  it = std::upper_bound(piece.arcs.begin(), piece.arcs.end(), s);
    int index = static_cast<int>(it - piece.arcs.begin()) - 1;
    index = std::clamp(index, 0, count - 2);

    const std::size_t k    = static_cast<std::size_t>(index);
    const float       span = Max(piece.arcs[k + 1] - piece.arcs[k], 1.0e-5f);
    const float       f    = Clamp01((s - piece.arcs[k]) / span);
    const Vector3     step = piece.samples[k + 1] - piece.samples[k];

    if (outPosition) *outPosition = piece.samples[k] + step * f;
    if (outTangent)  *outTangent  = step.NormalizedOr(Vector3{ 0.0f, 0.0f, 1.0f });
}

inline void SerpentPathComponent::Underground(float u, const Vector3& mouth, const Vector3& hDir,
                                              float theta, Vector3* outPosition,
                                              Vector3* outTangent) const
{
    // ほぼ直線の経路では口の接線が水平に近い。寝かせる余地が無いのでそのまま延ばす。
    if (theta < 0.05f) {
        const Vector3 dir = (hDir - Vector3::UP * std::tan(theta)).NormalizedOr(hDir);
        if (outPosition) *outPosition = mouth + dir * u;
        if (outTangent)  *outTangent  = dir;
        return;
    }

    const float radius = Max(undergroundDepth, 0.1f) / (1.0f - std::cos(theta));
    const float turn   = Min(u / radius, theta);
    const float rest   = Max(u - radius * theta, 0.0f);

    const float h = radius * (std::sin(theta) - std::sin(theta - turn)) + rest;
    const float y = radius * (std::cos(theta) - std::cos(theta - turn));

    if (outPosition) *outPosition = mouth + hDir * h + Vector3::UP * y;
    if (outTangent)
        *outTangent = (hDir * std::cos(theta - turn) - Vector3::UP * std::sin(theta - turn))
                          .NormalizedOr(hDir);
}

inline const SerpentPathComponent::Piece* SerpentPathComponent::PieceAt(float s) const
{
    for (const Piece& piece : m_pieces)
        if (s >= piece.start && s <= piece.start + piece.length) return &piece;
    return nullptr;
}

inline Vector3 SerpentPathComponent::At(float s) const
{
    if (m_pieces.empty()) return Vector3::ZERO;

    const Piece& first = m_pieces.front();
    if (s < first.start) {
        Vector3 out{};
        Underground(first.start - s, first.a, -first.u, first.theta, &out, nullptr);
        return out;
    }
    const Piece& last = m_pieces.back();
    const float  end  = last.start + last.length;
    if (s > end) {
        Vector3 out{};
        Underground(s - end, last.b, last.u, last.theta, &out, nullptr);
        return out;
    }

    const Piece* piece = PieceAt(s);
    if (!piece) return Vector3::ZERO;
    if (piece->surface) return ArcAt(*piece, s - piece->start);

    Vector3 out{};
    LinkSample(*piece, s - piece->start, &out, nullptr);
    return out;
}

inline Vector3 SerpentPathComponent::Tangent(float s) const
{
    if (m_pieces.empty()) return Vector3{ 0.0f, 0.0f, 1.0f };

    const Piece& first = m_pieces.front();
    // 床下でも向きは進行方向で返す。手前側は «来た方向» なので符号を反転する。
    if (s < first.start) {
        Vector3 out{};
        Underground(first.start - s, first.a, -first.u, first.theta, nullptr, &out);
        return -out;
    }
    const Piece& last = m_pieces.back();
    const float  end  = last.start + last.length;
    if (s > end) {
        Vector3 out{};
        Underground(s - end, last.b, last.u, last.theta, nullptr, &out);
        return out;
    }

    const Piece* piece = PieceAt(s);
    if (!piece) return Vector3{ 0.0f, 0.0f, 1.0f };
    if (piece->surface) return ArcTangent(*piece, s - piece->start);

    Vector3 out{};
    LinkSample(*piece, s - piece->start, nullptr, &out);
    return out;
}

inline bool SerpentPathComponent::OnSurface(float s) const
{
    const Piece* piece = PieceAt(s);
    return piece && piece->surface;
}

inline bool SerpentPathComponent::SurfaceLocal(float s, float* out01, float* outRadius) const
{
    const Piece* piece = PieceAt(s);
    if (!piece || !piece->surface) return false;
    if (out01)     *out01     = Clamp01((s - piece->start) / Max(piece->length, 0.01f));
    if (outRadius) *outRadius = piece->radius;
    return true;
}

inline float SerpentPathComponent::LastArcStart() const
{
    return m_pieces.empty() ? 0.0f : m_pieces.back().start;
}

inline float SerpentPathComponent::LastArcEnd() const
{
    return m_pieces.empty() ? 0.0f : m_pieces.back().start + m_pieces.back().length;
}

inline Vector3 SerpentPathComponent::LastArcGroundCenter() const
{
    if (m_pieces.empty()) return Vector3::ZERO;
    const Piece& piece = m_pieces.back();
    return (piece.a + piece.b) * 0.5f;
}

inline float SerpentPathComponent::ProjectOnLastArc(const Vector3& point) const
{
    if (m_pieces.empty()) return 0.0f;
    const Piece& piece = m_pieces.back();

    const Vector3 chord{ piece.b.x - piece.a.x, 0.0f, piece.b.z - piece.a.z };
    const float   length = chord.Length();
    if (length < EPSILON) return piece.start;

    const Vector3 toPoint{ point.x - piece.a.x, 0.0f, point.z - piece.a.z };
    const float   t = Clamp01(Vector3::Dot(toPoint, piece.u) / length);
    return piece.start + piece.length * t;
}

inline float SerpentPathComponent::ApexHeight() const
{
    return m_pieces.empty() ? 0.0f : m_pieces.back().apex;
}

inline float SerpentPathComponent::Radius() const
{
    return m_pieces.empty() ? 0.0f : m_pieces.back().radius;
}

inline const std::string& SerpentPathComponent::From() const
{
    static const std::string kNone;
    return m_pieces.empty() ? kNone : m_pieces.back().from;
}

inline const std::string& SerpentPathComponent::To() const
{
    static const std::string kNone;
    return m_pieces.empty() ? kNone : m_pieces.back().to;
}

inline void SerpentPathComponent::OnUpdate()
{
    if (!drawPath || m_pieces.empty()) return;

    constexpr int kSteps = 32;
    for (const Piece& piece : m_pieces) {
        // 床下のリンクも引く。«どこを通って次の口へ行くか» はここでしか見えない。
        const Vector4 color = piece.surface ? Vector4{ 0.25f, 0.85f, 1.0f, 1.0f }
                                            : Vector4{ 0.35f, 0.40f, 0.55f, 1.0f };
        for (int i = 0; i < kSteps; ++i) {
            const float a = piece.start + piece.length * static_cast<float>(i) / kSteps;
            const float b = piece.start + piece.length * static_cast<float>(i + 1) / kSteps;
            debug.DrawLine(At(a), At(b), color);
        }
    }
}

} // namespace sandbox
