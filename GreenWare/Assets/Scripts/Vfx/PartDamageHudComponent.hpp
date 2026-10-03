/// @file    PartDamageHudComponent.hpp
/// @brief   斬った部位に追従する耐久バー 1 本と、その場へ跳ねるダメージ数値
/// @author  Hasegawa Jin
/// @date    2026-09-08

/// @note バーは 1 本だけ。脚 4 本・節 28 の全てに出すと画面が計器で埋まるため、
/// @note 最後に斬った部位へ 1 本が乗り移る形にする。部位に追従させる (脚は踏みつけで
/// @note 跳ね上がり、節は床下へ潜るため、固定すると空中に取り残される)。
/// @note 数値は跳ねさせる (出た瞬間に大きく、すぐ縮み、弧を描いて散る)。同じ部位への
/// @note 連続ヒットは新しい数字を足さず、出ている数字へ積み上げてもう一度跳ねさせる。
/// @note 演出の枠を再利用し、連撃中の生成負荷とオブジェクト数を制限する。
#pragma once

#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class PartDamageHudComponent : public Script {
    FBZZ_SCRIPT(PartDamageHudComponent)

public:
    FBZZ_GROUP("Bar")
    FBZZ_FIELD(bool, showBar, true, "Show Bar")
    FBZZ_FIELD_RANGE(float, barWidth, 1.40f, "幅", 0.1f, 6.0f)
    FBZZ_TOOLTIP("バーの幅 [m]")
    FBZZ_FIELD_RANGE(float, barHeight, 0.14f, "高さ", 0.02f, 1.0f)
    FBZZ_FIELD_RANGE(float, barRise, 0.70f, "立ち上がり", 0.0f, 4.0f)
    FBZZ_TOOLTIP("部位の中心から何 m 上に出すか")
    FBZZ_FIELD_RANGE(float, barHold, 2.6f, "保持", 0.0f, 10.0f)
    FBZZ_TOOLTIP("最後に斬ってから出しておく長さ [秒]")
    FBZZ_FIELD_RANGE(float, barFade, 0.45f, "フェード", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, barLerp, 18.0f, "Follow", 1.0f, 60.0f)
    FBZZ_TOOLTIP("減りの追従の速さ。大きいほど即座に減る。小さいと «削れていく» が見える")
    FBZZ_FIELD_COLOR(barBackColor, (Vector4{ 0.05f, 0.055f, 0.07f, 0.80f }), "Back")
    FBZZ_FIELD_COLOR(barFillColor, (Vector4{ 0.94f, 0.86f, 0.42f, 1.0f }), "Fill")
    FBZZ_FIELD_COLOR(barLowColor, (Vector4{ 0.95f, 0.35f, 0.28f, 1.0f }), "Fill (low)")
    FBZZ_TOOLTIP("残りが lowAt を切ったときの色")
    FBZZ_FIELD_RANGE(float, lowAt, 0.34f, "Low At", 0.0f, 1.0f)
    FBZZ_FIELD_COLOR(barChipColor, (Vector4{ 1.0f, 0.98f, 0.92f, 0.9f }), "Chip")
    FBZZ_TOOLTIP("«たった今削れた分» を白で残す帯。減りが目で追える")

    FBZZ_GROUP("Number")
    FBZZ_FIELD_RANGE_INT(int, numberSlots, 8, "スロット", 1, 32)
    FBZZ_FIELD_FILE(fontFile, "guid:39a356816786e26c3103cb7c0c00195a|Assets/UI/Font/GW-Mono-Regular.ttf",
                    "Font", ".ttf")
    FBZZ_FIELD_RANGE(float, fontSize, 44.0f, "サイズ", 6.0f, 240.0f)
    FBZZ_TOOLTIP("基準の字の大きさ。ここから «重い一撃 x» と «積み上げ» と «弾け» が掛かるので、"
                 "5 段目の締めでは実効 100 を超える")
    FBZZ_FIELD_RANGE(float, heavyScale, 1.5f, "Heavy x", 1.0f, 3.0f)
    FBZZ_TOOLTIP("締めと溜め斬りの倍率。«重い一撃» を字の大きさで言う")
    FBZZ_FIELD_RANGE(float, popScale, 1.85f, "Pop x", 1.0f, 4.0f)
    FBZZ_TOOLTIP("出た瞬間の倍率。ここから popSeconds で 1 倍へ縮む ─ この «弾け» が手応えの正体")
    FBZZ_FIELD_RANGE(float, popSeconds, 0.13f, "はじけ", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, riseSpeed, 2.6f, "上昇速度", 0.0f, 12.0f)
    FBZZ_TOOLTIP("出た瞬間の上向きの初速 [m/s]。重力で減速して弧を描く")
    FBZZ_FIELD_RANGE(float, spreadSpeed, 1.5f, "Spread Speed", 0.0f, 8.0f)
    FBZZ_TOOLTIP("横へ散る初速 [m/s]。左右交互に飛ばして連撃の数字が重ならないようにする")
    FBZZ_FIELD_RANGE(float, gravity, 5.0f, "Gravity", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, numberHold, 0.75f, "保持", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, numberFade, 0.55f, "フェード", 0.05f, 3.0f)
    /// @note ⚠ Hold + Fade は Stacking の Window より長く保つこと。短いと数字が «積み上げの
    /// @note 窓が閉じる前に» 消えてしまい、繋いでいるのに別の数字が出る。
    /// @note 既定は 0.75 + 0.55 = 1.30 秒 > Window 1.20 秒。
    FBZZ_FIELD_COLOR(numberColor, (Vector4{ 1.0f, 0.98f, 0.92f, 1.0f }), "Color")
    FBZZ_FIELD_COLOR(numberHeavyColor, (Vector4{ 1.0f, 0.80f, 0.30f, 1.0f }), "Color (heavy)")
    FBZZ_FIELD_COLOR(numberHotColor, (Vector4{ 1.0f, 0.45f, 0.20f, 1.0f }), "Color (stacked)")
    FBZZ_TOOLTIP("積み上がった数字が寄っていく色。連撃が育つほど熱くなる")
    FBZZ_FIELD_RANGE_INT(int, hotAt, 200, "Hot At", 1, 5000)
    FBZZ_TOOLTIP("この積算値で完全に Hot 色になる")

    FBZZ_GROUP("Stacking")
    FBZZ_FIELD_RANGE(float, stackWindow, 1.20f, "受付時間", 0.0f, 5.0f)
    FBZZ_TOOLTIP("前の一撃からここまでなら同じ数字へ積み上げる [秒]。0 で毎回別の数字。"
                 "連撃の段間は 0.3〜0.5 秒なので、1 セット繋いでいる間は必ず積み上がる")
    FBZZ_FIELD_RANGE(float, stackGrow, 0.10f, "成長", 0.0f, 1.0f)
    FBZZ_TOOLTIP("積み上げ 1 回ごとに字が大きくなる割合。5 段で 1.5 倍まで育つ")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugLive, 0, "Live Numbers")
    FBZZ_FIELD_READ_ONLY(std::string, debugBarPart, "-", "Bar Part")

    [[nodiscard]] static PartDamageHudComponent* Instance() { return s_instance; }

    /// @note 叩いた部位へ、耐久バーを乗せ替えてダメージ数値を跳ねさせる。
    /// @param part    叩いた部位の GameObject。バーはこれに追従する
    /// @param amount  与えたダメージ。0 以下なら数字を出さない
    /// @param ratio   残り耐久 [0,1]。負ならバーを出さない (耐久を持たない部位)
    /// @param heavy   締め / 溜め斬り
    void Show(GameObject* part, int amount, float ratio, bool heavy);

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }

private:
    static inline PartDamageHudComponent* s_instance = nullptr;

    /// @note 跳ねる数字 1 つ。
    struct Number {
        EntityRef canvas;
        EntityRef text;
        /// @note 積み上げのキー。同じ部位の続けての一撃はこれで拾う。
        EntityRef part;
        Vector3   origin   = Vector3::ZERO;
        Vector3   velocity = Vector3::ZERO;
        float     age      = -1.0f;   ///< @note 負なら空き
        int       total    = 0;
        float     pop      = 0.0f;    ///< @note 1 で出た瞬間。popSeconds で 0 へ
        float     grow     = 1.0f;    ///< @note 積み上げで育った倍率
        bool      heavy    = false;
    };
    std::vector<Number> m_numbers;
    int  m_serial = 0;                ///< @note 横へ散らす向きの交互

    /// @note 追従するバー 1 本ぶん。
    struct Bar {
        EntityRef canvas;
        EntityRef back;
        EntityRef fill;
        EntityRef chip;               ///< @note たった今削れた分 (白)
        EntityRef part;               ///< @note 追従先
        float     age      = -1.0f;   ///< @note 負なら消えている
        float     target   = 1.0f;    ///< @note 実際の残り
        float     shown    = 1.0f;    ///< @note 表示中 (target へ寄る)
    };
    Bar m_bar;

    void Build();
    [[nodiscard]] Number* Take();
    void DriveNumbers(float dt);
    void DriveBar(float dt);
    void HideNumber(Number& number) const;
    void HideBar() const;

    /// @note Canvas のピクセルとワールドの換算。BossLegHealthBarComponent と同じ 100px = 1m。
    static constexpr float kPixelsPerUnit = 100.0f;
};

FBZZ_REFLECT(PartDamageHudComponent)

inline void PartDamageHudComponent::OnStart()
{
    s_instance = this;
    Build();
}

inline void PartDamageHudComponent::Build()
{
    const int count = std::clamp(numberSlots, 1, 32);
    if (!scene.CanCreate(static_cast<std::size_t>(count) * 2 + 4)) return;
    std::vector<Number> numbers(static_cast<std::size_t>(count));
    Bar bar{};
    const auto abandon = [&]() {
        for (const Number& number : numbers) {
            if (GameObject* object = number.text.Resolve(scene)) scene.Destroy(*object);
            if (GameObject* object = number.canvas.Resolve(scene)) scene.Destroy(*object);
        }
        if (GameObject* object = bar.back.Resolve(scene)) scene.Destroy(*object);
        if (GameObject* object = bar.chip.Resolve(scene)) scene.Destroy(*object);
        if (GameObject* object = bar.fill.Resolve(scene)) scene.Destroy(*object);
        if (GameObject* object = bar.canvas.Resolve(scene)) scene.Destroy(*object);
    };

    /// @note 生成をすべて終えてから親を設定し、完成した UI だけを公開する。
    for (int i = 0; i < count; ++i) {
        GameObject* canvasObject = scene.Create("PartDamageNum_" + std::to_string(i));
        if (!canvasObject) { abandon(); return; }
        canvasObject->runtimeGenerated = true;
        UICanvas& canvas  = canvasObject->AddComponent<UICanvas>();
        canvas.renderMode = UIRenderMode::WorldSpace;
        canvas.faceCamera = true;
        canvas.worldScale = 1.0f / kPixelsPerUnit;
        numbers[static_cast<std::size_t>(i)].canvas = EntityRef{ canvasObject->GetID() };
    }
    {
        GameObject* canvasObject = scene.Create("PartDamageBar");
        if (!canvasObject) { abandon(); return; }
        canvasObject->runtimeGenerated = true;
        UICanvas& canvas  = canvasObject->AddComponent<UICanvas>();
        canvas.renderMode = UIRenderMode::WorldSpace;
        canvas.faceCamera = true;
        canvas.worldScale = 1.0f / kPixelsPerUnit;
        bar.canvas = EntityRef{ canvasObject->GetID() };
    }

    for (int i = 0; i < count; ++i) {
        GameObject* textObject = scene.Create("PartDamageNum_Text");
        if (!textObject) { abandon(); return; }
        textObject->runtimeGenerated = true;
        UIText& text       = textObject->AddComponent<UIText>();
        text.sortOrder     = 2;
        text.fontPath      = fontFile;
        text.fontSize      = fontSize;
        text.align         = TextAlign::Center;
        text.letterSpacing = 1.0f;
        numbers[static_cast<std::size_t>(i)].text = EntityRef{ textObject->GetID() };
    }
    {
        GameObject* backObject = scene.Create("PartDamageBar_Back");
        if (!backObject) { abandon(); return; }
        backObject->runtimeGenerated = true;
        backObject->AddComponent<UIImage>().sortOrder = 0;
        bar.back = EntityRef{ backObject->GetID() };

        /// @note 削れた分の白。fill の «先» に残して、減りが目で追えるようにする。
        GameObject* chipObject = scene.Create("PartDamageBar_Chip");
        if (!chipObject) { abandon(); return; }
        chipObject->runtimeGenerated = true;
        chipObject->AddComponent<UIImage>().sortOrder = 1;
        bar.chip = EntityRef{ chipObject->GetID() };

        GameObject* fillObject = scene.Create("PartDamageBar_Fill");
        if (!fillObject) { abandon(); return; }
        fillObject->runtimeGenerated = true;
        fillObject->AddComponent<UIImage>().sortOrder = 2;
        bar.fill = EntityRef{ fillObject->GetID() };
    }

    for (Number& number : numbers) {
        GameObject* canvasObject = number.canvas.Resolve(scene);
        GameObject* text = number.text.Resolve(scene);
        if (!canvasObject || !text) continue;
        text->SetParent(*canvasObject);
        HideNumber(number);
    }
    if (GameObject* canvasObject = bar.canvas.Resolve(scene)) {
        if (GameObject* back = bar.back.Resolve(scene)) back->SetParent(*canvasObject);
        if (GameObject* chip = bar.chip.Resolve(scene)) chip->SetParent(*canvasObject);
        if (GameObject* fill = bar.fill.Resolve(scene)) fill->SetParent(*canvasObject);
    }
    m_numbers = std::move(numbers);
    m_bar = bar;
    HideBar();

    if (fontFile.empty())
        debug.LogWarning("PartDamageHud: Font が空です。ダメージ数値は出ません。");
}

inline PartDamageHudComponent::Number* PartDamageHudComponent::Take()
{
    Number* oldest = nullptr;
    for (Number& number : m_numbers) {
        if (number.age < 0.0f) return &number;
        if (!oldest || number.age > oldest->age) oldest = &number;
    }
    return oldest;
}

inline void PartDamageHudComponent::Show(GameObject* part, int amount, float ratio, bool heavy)
{
    /// @note バーを «乗り移らせる»
    if (showBar && ratio >= 0.0f && part) {
        const bool samePart = m_bar.part.Resolve(scene) == part;
        if (!samePart) {
            /// @note 別の部位へ乗り移ったら、前の部位の削れ跡を引きずらない。最初の 1 撃は
            /// @note 白帯が出ない代わりに «その部位の今» から始まる。
            m_bar.shown = Clamp01(ratio);
            m_bar.part  = EntityRef{ part->GetID() };
        }
        m_bar.target = Clamp01(ratio);
        m_bar.age    = 0.0f;
        debugBarPart = part->name;
    }

    if (amount <= 0 || fontFile.empty() || m_numbers.empty()) return;

    /// @note 同じ部位への続けての一撃は «積み上げ»
    if (part && stackWindow > 0.0f) {
        for (Number& number : m_numbers) {
            if (number.age < 0.0f || number.age > stackWindow) continue;
            if (number.part.Resolve(scene) != part) continue;

            number.total += amount;
            number.pop    = 1.0f;
            number.grow   = std::min(number.grow + std::max(stackGrow, 0.0f), 2.0f);
            number.heavy  = number.heavy || heavy;

            /// @note ⚠ age を «巻き戻す» のではなく、その場から撃ち直す。
            /// @note age は寿命であると同時に弧の時刻でもあるので、引き算すると数字が
            /// @note 弧を逆走して下へ戻る ─ 積み上げるたびに «跳ね返って落ちる» 妙な動きになる。
            /// @note 叩かれた部位の «今» の位置から新しい弧で撃ち直せば、連撃のあいだ
            /// @note 数字は刃の近くに居続け、1 撃ごとに «もう一度弾ける»。
            number.age      = 0.0f;
            number.origin   = part->transform.worldPosition;
            number.velocity = { (m_serial++ % 2 == 0) ? spreadSpeed : -spreadSpeed,
                                riseSpeed, 0.0f };
            if (GameObject* text = number.text.Resolve(scene)) {
                text->SetActive(true);
                ui.SetText(text, std::to_string(number.total));
            }
            return;
        }
    }

    /// @note 新しい数字
    Number* number = Take();
    if (!number) return;

    const float side = (m_serial++ % 2 == 0) ? 1.0f : -1.0f;

    number->part     = part ? EntityRef{ part->GetID() } : EntityRef{};
    number->origin   = part ? part->transform.worldPosition : Vector3::ZERO;
    number->velocity = { side * spreadSpeed, riseSpeed, 0.0f };
    number->age      = 0.0f;
    number->total    = amount;
    number->pop      = 1.0f;
    number->grow     = 1.0f;
    number->heavy    = heavy;

    if (GameObject* text = number->text.Resolve(scene)) {
        text->SetActive(true);
        ui.SetText(text, std::to_string(amount));
    }
}

inline void PartDamageHudComponent::OnLateUpdate()
{
    /// @note LateUpdate で行う。部位は Animator が動かすため、Update だと 1 フレーム前の
    /// @note 位置に出て、速い薙ぎで数字とバーだけが取り残される。
    const float dt = std::max(Time::deltaTime, 0.0f);
    DriveNumbers(dt);
    DriveBar(dt);
}

inline void PartDamageHudComponent::DriveNumbers(float dt)
{
    const float hold = std::max(numberHold, 0.0f);
    const float fade = std::max(numberFade, 0.05f);
    const float life = hold + fade;

    int live = 0;
    for (Number& number : m_numbers) {
        if (number.age < 0.0f) continue;
        number.age += dt;
        if (number.age >= life) { number.age = -1.0f; HideNumber(number); continue; }
        ++live;

        GameObject* canvasObject = number.canvas.Resolve(scene);
        GameObject* text = number.text.Resolve(scene);
        if (!canvasObject || !text) continue;

        /// @note 弧。初速で跳ね上げて重力で落とす ─ 等速で上がるより «弾かれた» に見える。
        const float t = number.age;
        const Vector3 offset{ number.velocity.x * t,
                              number.velocity.y * t - 0.5f * gravity * t * t,
                              0.0f };
        /// @note 積み上げで別の部位へ移った数字は、出た場所に置いていく (追従させない) ─
        /// @note 数字が部位に貼り付くと、跳ねているのか脚が動いているのか読めない。
        const Vector3 at = number.origin + offset;
        canvasObject->transform.position      = at;
        canvasObject->transform.worldPosition = at;

        /// @note 弾け。出た瞬間 popScale 倍、popSeconds で 1 倍へ。2 乗で落とすので
        /// @note «最初のひと目だけ大きい» になる ─ 線形だと «ゆっくり縮んだ» に見える。
        number.pop = std::max(number.pop - dt / std::max(popSeconds, 0.01f), 0.0f);
        const float pop  = 1.0f + (std::max(popScale, 1.0f) - 1.0f) * number.pop * number.pop;
        const float size = std::max(fontSize, 1.0f)
                         * (number.heavy ? std::max(heavyScale, 1.0f) : 1.0f)
                         * number.grow * pop;

        if (auto* canvas = canvasObject->GetComponent<UICanvas>()) {
            canvas->canvasWidth  = size * 6.0f;
            canvas->canvasHeight = size * 2.0f;
        }
        text->transform.position = Vector3::ZERO;
        if (auto* t2 = text->GetComponent<UIText>()) t2->fontSize = size;

        /// @note 積み上がるほど熱い色へ。連撃が育っていることが色でも出る。
        const Vector4 base = number.heavy ? numberHeavyColor : numberColor;
        const float   heat = Clamp01(static_cast<float>(number.total)
                                     / static_cast<float>(std::max(hotAt, 1)));
        const float alpha = number.age <= hold ? 1.0f
                                               : Clamp01(1.0f - (number.age - hold) / fade);
        ui.SetTextColor(text, { Lerp(base.x, numberHotColor.x, heat),
                                Lerp(base.y, numberHotColor.y, heat),
                                Lerp(base.z, numberHotColor.z, heat),
                                base.w * alpha });
    }
    debugLive = live;
}

inline void PartDamageHudComponent::DriveBar(float dt)
{
    if (m_bar.age < 0.0f) return;

    GameObject* part = m_bar.part.Resolve(scene);
    GameObject* canvasObject = m_bar.canvas.Resolve(scene);
    GameObject* back = m_bar.back.Resolve(scene);
    GameObject* chip = m_bar.chip.Resolve(scene);
    GameObject* fill = m_bar.fill.Resolve(scene);
    /// @note 追従先が消えた (脚がもげた) ら畳む。空中に取り残さない。
    if (!part || !part->activeInHierarchy() || !canvasObject || !back || !chip || !fill) {
        m_bar.age = -1.0f;
        debugBarPart = "-";
        HideBar();
        return;
    }

    m_bar.age += dt;
    const float hold = std::max(barHold, 0.0f);
    const float fade = std::max(barFade, 0.05f);
    if (m_bar.age >= hold + fade) { m_bar.age = -1.0f; HideBar(); debugBarPart = "-"; return; }

    const float alpha = m_bar.age <= hold ? 1.0f
                                          : Clamp01(1.0f - (m_bar.age - hold) / fade);
    /// @note 表示は実値へ «寄る»。一瞬で減らすと «削れた» が 1 フレームで終わる。
    m_bar.shown = Lerp(m_bar.shown, m_bar.target,
                       Clamp01(std::max(barLerp, 1.0f) * dt));

    const float width  = std::max(barWidth, 0.05f) * kPixelsPerUnit;
    const float height = std::max(barHeight, 0.02f) * kPixelsPerUnit;

    if (auto* canvas = canvasObject->GetComponent<UICanvas>()) {
        canvas->canvasWidth  = width;
        canvas->canvasHeight = height;
    }
    const Vector3 anchor = part->transform.worldPosition + Vector3{ 0.0f, barRise, 0.0f };
    canvasObject->transform.position      = anchor;
    canvasObject->transform.worldPosition = anchor;

    back->SetActive(true);
    chip->SetActive(true);
    fill->SetActive(true);

    back->transform.position = Vector3::ZERO;
    back->transform.scale    = { width, height, 1.0f };
    ui.SetImageColor(back, { barBackColor.x, barBackColor.y, barBackColor.z,
                             barBackColor.w * alpha });

    /// @note UI 要素の localScale.xy は倍率ではなく Canvas ピクセル単位の幅・高さ。左詰め。
    const auto place = [&](GameObject* go, float ratio, const Vector4& color) {
        const float w = width * Clamp01(ratio);
        go->transform.position = { -(width - w) * 0.5f, 0.0f, 0.0f };
        go->transform.scale    = { std::max(w, 1.0f), height, 1.0f };
        ui.SetImageColor(go, { color.x, color.y, color.z, color.w * alpha });
    };
    /// @note 白い «削れ跡» が実値まで縮み、その上に本体が乗る。
    place(chip, m_bar.shown, barChipColor);
    const Vector4 c = m_bar.target <= Clamp01(lowAt) ? barLowColor : barFillColor;
    place(fill, m_bar.target, c);
}

inline void PartDamageHudComponent::HideNumber(Number& number) const
{
    if (GameObject* text = number.text.Resolve(scene)) text->SetActive(false);
}

inline void PartDamageHudComponent::HideBar() const
{
    if (GameObject* back = m_bar.back.Resolve(scene)) back->SetActive(false);
    if (GameObject* chip = m_bar.chip.Resolve(scene)) chip->SetActive(false);
    if (GameObject* fill = m_bar.fill.Resolve(scene)) fill->SetActive(false);
}

} /// @note namespace sandbox
