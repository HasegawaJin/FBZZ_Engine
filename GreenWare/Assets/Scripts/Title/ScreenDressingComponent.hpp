/// @file    ScreenDressingComponent.hpp
/// @brief   UI 画面の下地と題字マテリアルへ «時間» と «画面ごとの覆い» を渡す
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY スクリプトが要るか:
///   UI シェーダーの定数バッファ (UIConstants) には時刻が無い。UI は «今の状態を
///   描く» ものなので、パスが時間を配る作りになっていない。動く下地や艶を作るなら、
///   進める側を誰かが持つしかない。
///
/// WHY 画面ごとのスクリプトへ分けないか:
///   Title / Options / StageSelect / Result は «下地 + 題字 + 一覧» という同じ
///   組み方をしている。画面ごとに飾りのスクリプトを持つと、艶の間隔を変えるのに
///   4 ファイルを開くことになり、必ずどれか 1 枚だけ拍が違う画面ができる。
///   名前で拾う形にしておけば、シーンごとの違いは «誰を光らせるか» だけになる。
///
/// WHY 覆い (coverage) をここが持つか:
///   下地の .mat は全画面で共有したい (同じ画面に見せたい) が、Title と Result は
///   下地の後ろに磁場グリッドと電極が居て、塗り潰すと盤面ごと消える。
///   共有したいのは色と走査線で、画面ごとに違うのは覆い具合だけ ─ その 1 つだけを
///   要素ごとの上書きとして持てば、.mat を画面の数だけ複製せずに済む。
///
/// WHY 題字の出現と放電もここが持つか (2026-09-07):
///   艶と同じ相手 (UITitleSheen.mat の要素) を、同じ拍で動かしたい。出現 (reveal) は
///   画面に入ってからの 1 回、放電 (crackle) は数秒に 1 回 0.3 秒。別のスクリプトに
///   分けると、艶の途中で放電が重なって白飛びする、といった調停をどこかで書く
///   ことになる。同じ相手を触る値は 1 か所から出す。
///
/// WHY 艶を一定間隔で 1 回だけ走らせるか:
///   ずっと走らせていると «常に何かが動いている画面» になり、目が休まらない。
///   タイトルもリザルトも長く見る画面なので、静止している時間の方を長く取る。
///   間隔を空けて 1 本走らせると «ときどき光を拾った» という静かな見え方になる。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Title/ElectrodePole.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ScreenDressingComponent : public Script {
    FBZZ_SCRIPT(ScreenDressingComponent)

public:
    FBZZ_GROUP("Backdrop")
    FBZZ_FIELD(std::string, backdropName, "Backdrop", "Backdrop")
    FBZZ_TOOLTIP("UITitleBackdrop.mat を割り当てた UIImage。空にすると下地を触らない")
    FBZZ_FIELD_RANGE(float, backdropCoverage, 1.0f, "Coverage", 0.0f, 1.0f)
    FBZZ_TOOLTIP("下地がどれだけ後ろを覆うか。後ろに 3D が居る画面 (Title / Result) は "
                 "0.5 前後、何も無い画面 (Options / StageSelect) は 1.0")
    FBZZ_FIELD_RANGE(float, backdropSpeed, 1.0f, "速さ", 0.0f, 4.0f)
    FBZZ_TOOLTIP("呼吸と走査線の流れの速さ。0 で止まる (静止画として見たいときに)")

    FBZZ_GROUP("Sheen")
    FBZZ_LIST_FIELD(std::string, sheenTargets, "対象")
    FBZZ_TOOLTIP("UITitleSheen.mat を割り当てた UIImage の名前。上から順に光る")
    FBZZ_FIELD_RANGE(float, sheenInterval, 6.5f, "間隔", 0.5f, 30.0f)
    FBZZ_TOOLTIP("艶が 1 本走ってから次までの秒数。短いと «光り続けている» になる")
    FBZZ_FIELD_RANGE(float, sheenSeconds, 1.1f, "薙ぎ", 0.1f, 5.0f)
    FBZZ_TOOLTIP("1 本が左端から右端まで走りきる秒数")
    FBZZ_FIELD_RANGE(float, sheenStagger, 0.13f, "のけぞり", 0.0f, 1.0f)
    FBZZ_TOOLTIP("1 枚ごとにずらす秒数。0 にすると全部が同時に光り、"
                 "並んだ何枚かが «1 枚の板» に見える")

    FBZZ_GROUP("Reveal")
    FBZZ_FIELD_RANGE(float, revealDelay, 0.20f, "遅延", 0.0f, 3.0f)
    FBZZ_TOOLTIP("画面に入ってから題字が集まり始めるまで")
    FBZZ_FIELD_RANGE(float, revealSeconds, 0.9f, "継続時間", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, revealStagger, 0.12f, "のけぞり", 0.0f, 1.0f)

    FBZZ_GROUP("はぜる音")
    FBZZ_FIELD_RANGE(float, crackleInterval, 5.5f, "間隔", 0.5f, 30.0f)
    FBZZ_TOOLTIP("縁の放電の間隔 [秒]。±40% ばらつく (等間隔だと «点滅» に読まれる)")
    FBZZ_FIELD_RANGE(float, crackleSeconds, 0.32f, "継続時間", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, crackleStrength, 0.85f, "強度", 0.0f, 1.0f)
    FBZZ_FIELD(std::string, arcName, "Logo_Arc", "Arc")
    FBZZ_TOOLTIP("UIElectricLine.mat を割り当てた UIImage。放電と同時に下線を稲妻が走る。空で無し")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugPhase, 0.0f, "位相")
    FBZZ_FIELD_READ_ONLY(float, debugSheen, 0.0f, "Sheen")
    FBZZ_FIELD_READ_ONLY(int, debugTargets, 0, "Targets Found")

    void OnStart() override;
    void OnUpdate() override;

    /// 下地の ± の寄せ方。-1 で寒色 (−) だけが残り、+1 で暖色 (＋) だけが残る。
    ///
    /// WHY 色そのものではなく «寄せ方» を受け取るか:
    ///   呼ぶ側 (リザルト) が知っているのは «勝ったか負けたか» で、極の色が何色かは
    ///   知らないし知る必要も無い。色を渡させると、極の色を変えたときにリザルトの
    ///   コードまで直すことになる。ここが持つのは配色、あちらが持つのは意味。
    ///
    /// WHY 消し切らないか (弱い方を 0 にしないか):
    ///   片方を消すと «1 色の背景» になり、この作品が ± の対で出来ていることが
    ///   下地から消える。負けても ＋ は薄く残す。
    void SetPoleBias(float bias);

private:
    /// 控えた寄せ方を下地へ流す。まだ下地を掴めていなければ何もしない。
    ///
    /// WHY 要求と適用を分けるか: 呼ぶ側 (ResultPresenter) の OnStart がこちらより
    ///     先に走ると、その時点で下地はまだ解決できていない。その場で書こうとすると
    ///     «スクリプトの並び順を変えただけで下地の色が戻る» という直しにくい形になる。
    ///     要求は控えておいて、掴めた時点で流す。
    void ApplyBias();

    /// 下地の位相。呼吸と走査線が共有する。
    float m_phase = 0.0f;
    /// 要求された ± の寄せ方と、まだ流していないかどうか。
    float m_bias      = 0.0f;
    bool  m_biasDirty = true;
    /// 艶の周期の中の位置 [秒]。0 で «今まさに走り始めた»。
    float m_sheen = 0.0f;

    GameObject*              m_backdrop = nullptr;
    GameObject*              m_arc      = nullptr;
    std::vector<GameObject*> m_sheenTargets;
    float m_elapsed      = 0.0f;   ///< 画面に入ってからの秒数 (出現用)
    float m_crackleTimer = 0.0f;   ///< 次の放電までの秒数
    float m_crackle      = 0.0f;   ///< 放電の残り時間 [秒]。0 で消灯
    std::uint32_t m_seed = 0x2545F491u;
    /// 決定的な乱数 (0..1)。放電の間隔をばらつかせる。
    float Rand01()
    {
        m_seed ^= m_seed << 13; m_seed ^= m_seed >> 17; m_seed ^= m_seed << 5;
        return static_cast<float>(m_seed & 0xFFFFFFu) / 16777216.0f;
    }
};

FBZZ_REFLECT(ScreenDressingComponent)

inline void ScreenDressingComponent::OnStart()
{
    m_phase = 0.0f;
    // 画面が出てすぐ光ると «起動エフェクト» に見えて、題字そのものの質感として
    // 読まれない。1 本目まで少し置く。
    m_sheen = -std::max(sheenInterval, 0.5f) * 0.35f;

    m_backdrop = backdropName.empty() ? nullptr : scene.Find(backdropName);
    if (!m_backdrop && !backdropName.empty()) {
        debug.LogWarning("ScreenDressingComponent: '" + backdropName +
                         "' が見つかりません (下地が既定のまま動きません)");
    }
    // 覆いは動かない値なので開始時に 1 度で足りる。
    if (m_backdrop)
        ui.SetMaterialFloat(m_backdrop, "coverage", std::clamp(backdropCoverage, 0.0f, 1.0f));

    m_sheenTargets.clear();
    m_sheenTargets.reserve(sheenTargets.size());
    for (const std::string& name : sheenTargets) {
        if (name.empty()) continue;
        GameObject* object = scene.Find(name);
        if (!object) {
            debug.LogWarning("ScreenDressingComponent: 艶を掛ける '" + name +
                             "' が見つかりません");
            continue;
        }
        m_sheenTargets.push_back(object);
    }
    debugTargets = static_cast<int>(m_sheenTargets.size());

    m_arc = arcName.empty() ? nullptr : scene.Find(arcName);
    m_elapsed      = 0.0f;
    m_crackle      = 0.0f;
    // 最初の放電は題字が置き切ってから。
    m_crackleTimer = revealDelay + revealSeconds + 1.2f + Rand01() * 1.5f;
    // 1 フレーム目から «まだ集まっていない» で描く。
    for (std::size_t i = 0; i < m_sheenTargets.size(); ++i)
        ui.SetMaterialFloat(m_sheenTargets[i], "reveal", 0.0f);
    if (m_arc) ui.SetMaterialFloat(m_arc, "burst", 0.0f);

    // 先に要求されていた寄せ方をここで流す (要求と適用を分けた理由は ApplyBias)。
    ApplyBias();
}

inline void ScreenDressingComponent::SetPoleBias(float bias)
{
    m_bias      = std::clamp(bias, -1.0f, 1.0f);
    m_biasDirty = true;
    ApplyBias();
}

inline void ScreenDressingComponent::ApplyBias()
{
    if (!m_backdrop || !m_biasDirty) return;
    m_biasDirty = false;

    const float t = m_bias;
    // 弱い方は 0.3 まで。消し切らない理由は宣言側の WHY。
    const float plus  = t >= 0.0f ? 1.0f : 1.0f + t * 0.7f;
    const float minus = t <= 0.0f ? 1.0f : 1.0f - t * 0.7f;

    // 極の «重み» は色の a として持たせてある (シェーダーが差し込みへ掛ける)。
    // rgb は ElectrodePole から引く ─ ここに色を書くと、極の配色を変えたときに
    // 下地だけ古い色で残る。極が何色かを決めている場所は 1 つでなければならない。
    const Vector4 p = PoleColor(Pole::Plus);
    const Vector4 m = PoleColor(Pole::Minus);
    ui.SetMaterialColor(m_backdrop, "plusColor",  { p.x, p.y, p.z, plus });
    ui.SetMaterialColor(m_backdrop, "minusColor", { m.x, m.y, m.z, minus });
}

inline void ScreenDressingComponent::OnUpdate()
{
    // 開始順で取りこぼした要求をここで拾う。掴めていれば 1 度で終わる。
    ApplyBias();

    // 実時間で進める。UI の画面はヒットストップもスローも掛からず、
    // ゲーム時間で進めるとポーズから戻ったときに位相が飛ぶ。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    m_phase += dt * std::max(backdropSpeed, 0.0f);
    if (m_backdrop) ui.SetMaterialFloat(m_backdrop, "phase", m_phase);

    const float interval = std::max(sheenInterval, 0.5f);
    m_sheen += dt;
    if (m_sheen >= interval) m_sheen -= interval;

    // 放電。間隔は ±40% ばらつかせる。
    m_elapsed += dt;
    m_crackleTimer -= dt;
    if (m_crackleTimer <= 0.0f) {
        m_crackle      = std::max(crackleSeconds, 0.05f);
        m_crackleTimer = std::max(crackleInterval, 0.5f) * (0.6f + 0.8f * Rand01());
    }
    // 放電の強さ: 立ち上がりは一瞬、抜けは減衰。
    float crackle = 0.0f;
    if (m_crackle > 0.0f) {
        const float u = m_crackle / std::max(crackleSeconds, 0.05f);   // 1 → 0
        crackle = crackleStrength * u * u;
        m_crackle -= dt;
    }

    const float sweep = std::max(sheenSeconds, 0.1f);
    for (std::size_t i = 0; i < m_sheenTargets.size(); ++i) {
        // 走っていない間は «矩形の外» で待機させる。シェーダーは 0..1 を左端の外から
        // 右端の外までへ写すので、範囲外の値を渡せば帯はどこにも掛からない。
        const float delay = sheenStagger * static_cast<float>(i);
        const float t     = (m_sheen - delay) / sweep;
        ui.SetMaterialFloat(m_sheenTargets[i], "sheenPhase", t <= 0.0f || t >= 1.0f ? 1.0f : t);

        // 出現。粒が寄り集まって字になる (UITitleSheen.hlsl の reveal)。
        const float r = std::clamp((m_elapsed - revealDelay - revealStagger * static_cast<float>(i))
                                       / std::max(revealSeconds, 0.05f), 0.0f, 1.0f);
        // OutQuint: 集まり始めは速く、最後の粒だけゆっくり。線形だと «拭き取り» に見える。
        const float e = 1.0f - std::pow(1.0f - r, 5.0f);
        ui.SetMaterialFloat(m_sheenTargets[i], "reveal", e);
        ui.SetMaterialFloat(m_sheenTargets[i], "crackle", crackle);
        ui.SetMaterialFloat(m_sheenTargets[i], "phase", m_phase);
    }
    if (m_arc) {
        ui.SetMaterialFloat(m_arc, "burst", crackle / std::max(crackleStrength, 1.0e-3f));
        ui.SetMaterialFloat(m_arc, "phase", m_phase);
    }

    debugPhase = m_phase;
    debugSheen = m_sheen;
}

} // namespace sandbox
