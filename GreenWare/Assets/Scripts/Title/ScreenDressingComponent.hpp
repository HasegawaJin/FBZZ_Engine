/// @file    ScreenDressingComponent.hpp
/// @brief   UI 画面の下地と題字マテリアルへ «時間» と «画面ごとの覆い» を渡す
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// @note UIConstants に時刻が無く UI パスは時間を配らないため、動く下地や艶は誰かが進める。
/// @note 画面 (Title/Options/StageSelect/Result) ごとにスクリプトを分けない: 同じ «下地+題字+
///       一覧» の組み方なので、分けると艶の間隔を変えるのに 4 ファイルを開くことになり
///       拍が食い違う画面ができる。名前で拾えば違いは «誰を光らせるか» だけになる。
/// @note 覆い (coverage) はここが持つ: 下地 .mat は全画面で共有したいが、Title/Result は
///       背後に磁場グリッドと電極が居て塗り潰すと消える。画面ごとに違うのは覆い具合だけ
///       なので、そこだけ要素ごとの上書きにすれば .mat を画面数ぶん複製せずに済む。
/// @note 題字の出現と放電もここが持つ: 艶と同じ相手 (UITitleSheen.mat) を同じ拍で動かす。
///       別スクリプトに分けると、艶の途中で放電が重なる調停をどこかに書くことになる。
/// @note 艶は一定間隔で 1 回だけ走らせる。常時だと目が休まらないので、静止時間を長く取り
///       «ときどき光を拾った» という見え方にする。
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

    /// @brief 下地の ± 寄せ方を設定する。
    /// @param bias -1 で寒色 (−) だけが残り、+1 で暖色 (＋) だけが残る。
    /// @note 色でなく寄せ方を受け取る: 呼び手 (リザルト) は勝敗しか知らず、極の色は
    ///       ElectrodePole が正本。負けても弱い方の色を消し切らず薄く残す
    ///       (± の対で出来ていることが下地から消えないように)。
    void SetPoleBias(float bias);

private:
    /// @brief 控えた寄せ方を下地へ流す。まだ下地を掴めていなければ何もしない。
    /// @note 要求と適用を分ける: 呼び手 (ResultPresenter) の OnStart がこちらより先に
    ///       走ると下地がまだ解決できておらず、その場で書くと «スクリプトの並び順を
    ///       変えただけで色が戻る» という直しにくい形になる。要求は控え、掴めた時点で流す。
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
    /// @note 画面が出てすぐ光ると «起動エフェクト» に見えて、題字そのものの質感として
    ///       読まれない。1 本目まで少し置く。
    m_sheen = -std::max(sheenInterval, 0.5f) * 0.35f;

    m_backdrop = backdropName.empty() ? nullptr : scene.Find(backdropName);
    if (!m_backdrop && !backdropName.empty()) {
        debug.LogWarning("ScreenDressingComponent: '" + backdropName +
                         "' が見つかりません (下地が既定のまま動きません)");
    }
    /// @note 覆いは動かない値なので開始時に 1 度で足りる。
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
    /// @note 最初の放電は題字が置き切ってから。
    m_crackleTimer = revealDelay + revealSeconds + 1.2f + Rand01() * 1.5f;
    /// @note 1 フレーム目から «まだ集まっていない» で描く。
    for (std::size_t i = 0; i < m_sheenTargets.size(); ++i)
        ui.SetMaterialFloat(m_sheenTargets[i], "reveal", 0.0f);
    if (m_arc) ui.SetMaterialFloat(m_arc, "burst", 0.0f);

    /// @note 先に要求されていた寄せ方をここで流す (要求と適用を分けた理由は ApplyBias)。
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
    /// @note 弱い方は 0.3 まで。消し切らない理由は宣言側の @note。
    const float plus  = t >= 0.0f ? 1.0f : 1.0f + t * 0.7f;
    const float minus = t <= 0.0f ? 1.0f : 1.0f - t * 0.7f;

    /// @note 極の «重み» は色の a として持たせてある (シェーダーが差し込みへ掛ける)。
    ///       rgb は ElectrodePole から引く ─ ここに色を書くと、極の配色を変えたときに
    ///       下地だけ古い色で残る。極が何色かを決めている場所は 1 つでなければならない。
    const Vector4 p = PoleColor(Pole::Plus);
    const Vector4 m = PoleColor(Pole::Minus);
    ui.SetMaterialColor(m_backdrop, "plusColor",  { p.x, p.y, p.z, plus });
    ui.SetMaterialColor(m_backdrop, "minusColor", { m.x, m.y, m.z, minus });
}

inline void ScreenDressingComponent::OnUpdate()
{
    /// @note 開始順で取りこぼした要求をここで拾う。掴めていれば 1 度で終わる。
    ApplyBias();

    /// @note 実時間で進める。UI の画面はヒットストップもスローも掛からず、
    ///       ゲーム時間で進めるとポーズから戻ったときに位相が飛ぶ。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    m_phase += dt * std::max(backdropSpeed, 0.0f);
    if (m_backdrop) ui.SetMaterialFloat(m_backdrop, "phase", m_phase);

    const float interval = std::max(sheenInterval, 0.5f);
    m_sheen += dt;
    if (m_sheen >= interval) m_sheen -= interval;

    /// @note 放電。間隔は ±40% ばらつかせる。
    m_elapsed += dt;
    m_crackleTimer -= dt;
    if (m_crackleTimer <= 0.0f) {
        m_crackle      = std::max(crackleSeconds, 0.05f);
        m_crackleTimer = std::max(crackleInterval, 0.5f) * (0.6f + 0.8f * Rand01());
    }
    /// @note 放電の強さ: 立ち上がりは一瞬、抜けは減衰。
    float crackle = 0.0f;
    if (m_crackle > 0.0f) {
        /// @note 1 → 0
        const float u = m_crackle / std::max(crackleSeconds, 0.05f);
        crackle = crackleStrength * u * u;
        m_crackle -= dt;
    }

    const float sweep = std::max(sheenSeconds, 0.1f);
    for (std::size_t i = 0; i < m_sheenTargets.size(); ++i) {
        /// @note 走っていない間は «矩形の外» で待機させる。シェーダーは 0..1 を左端の外から
        ///       右端の外までへ写すので、範囲外の値を渡せば帯はどこにも掛からない。
        const float delay = sheenStagger * static_cast<float>(i);
        const float t     = (m_sheen - delay) / sweep;
        ui.SetMaterialFloat(m_sheenTargets[i], "sheenPhase", t <= 0.0f || t >= 1.0f ? 1.0f : t);

        /// @note 出現。粒が寄り集まって字になる (UITitleSheen.hlsl の reveal)。
        const float r = std::clamp((m_elapsed - revealDelay - revealStagger * static_cast<float>(i))
                                       / std::max(revealSeconds, 0.05f), 0.0f, 1.0f);
        /// @note OutQuint: 集まり始めは速く、最後の粒だけゆっくり。線形だと «拭き取り» に見える。
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
