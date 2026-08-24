// FBZZ Engine
// ScriptUIProxy.hpp | fbzz::scene
// Script から UI コンポーネントを操作するショートハンド
#pragma once

#include <Math/Vector4.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

// UIButton の当たり状態。IsHovered() / IsPressed() の戻り値と 1 対 1 で対応する。
enum class UIButtonPhase {
    Normal,
    Hovered,
    Pressed,
    Disabled,   // enabled == false または isInteractable == false
};

struct ScriptUIProxy {
    Script* script = nullptr;

    // ── ボタン入力の取得 ──────────────────────────────────────────────────────
    // WHY ポーリングにするか: コールバック登録にすると、スクリプト DLL がリロードされた
    //     瞬間に UIButton 側へ登録済みの std::function が解放済みコードを指す。
    //     UISystem が立てる 1 フレーム限定フラグを読むだけなら寿命問題が起きない。
    //
    // WHY 1 フレーム遅れるか: UISystem はレンダーパスから呼ばれるため、フレーム N の描画で
    //     立ったフラグをフレーム N+1 の OnUpdate() が読む。フラグは次の UISystem 呼び出しまで
    //     保持されるので、取りこぼしも二重発火も起きない。
    [[nodiscard]] bool WasClicked() const;
    [[nodiscard]] bool WasHoverEnter() const;
    [[nodiscard]] bool WasHoverExit() const;
    [[nodiscard]] bool IsHovered() const;
    [[nodiscard]] bool IsPressed() const;
    [[nodiscard]] bool IsInteractable() const;
    [[nodiscard]] UIButtonPhase GetButtonPhase() const;

    // GameObject ターゲット版。ボタンが子要素や別 Canvas にある構成 (メニュー画面を
    // 1 つのスクリプトで捌く場合) はこちらを使う。
    [[nodiscard]] bool WasClicked(GameObject* go) const;
    [[nodiscard]] bool WasHoverEnter(GameObject* go) const;
    [[nodiscard]] bool WasHoverExit(GameObject* go) const;
    [[nodiscard]] bool IsHovered(GameObject* go) const;
    [[nodiscard]] bool IsPressed(GameObject* go) const;
    [[nodiscard]] bool IsInteractable(GameObject* go) const;
    [[nodiscard]] UIButtonPhase GetButtonPhase(GameObject* go) const;

    void SetButtonInteractable(bool v) const;
    void SetButtonInteractable(GameObject* go, bool v) const;
    void SetImageColor(const math::Vector4& color) const;
    void SetText(std::string_view text) const;
    // UIText の色。SetImageColor は UIImage しか触らないため、文字だけを
    // 状態に応じて明滅させたい HUD には別の入口が要る。
    void SetTextColor(const math::Vector4& color) const;
    void SetCanvasSortOrder(int order) const;

    /// 直近のフレームで UI が解決したマウス位置 (Canvas 空間、原点は左上)。
    ///
    /// WHY input.GetMousePosition() で足りないか: あちらはビューポート内のピクセルで、
    ///     Canvas Scaler や renderMode を通していない。要素の矩形と直接比べられるのは
    ///     こちらだけ。スライダーのドラッグのように「掴んだ位置」を要素の座標系で
    ///     測る操作で使う。
    /// NOTE: シーンで最初に見つかった Canvas の値。UI が 1 度も回っていなければ (0,0)。
    ///       ポインターを差し替えていても、ここは常に OS のマウスを返す
    ///       (カーソルを動かす側が生の値を要るため)。
    [[nodiscard]] math::Vector2 GetCanvasMousePosition() const;

    /// @name ゲーム内カーソル
    /// UI の当たり判定に使う座標と押下状態を、OS のマウスから差し替える。
    ///
    /// WHY これだけで足りるか: UIButton / UISlider / UIToggle の判定は
    ///     「Canvas 空間の座標 1 つと押下状態」しか見ていない。入口を差し替えれば、
    ///     マウスとパッドの両対応をウィジェット側に 1 行も書かずに済む。
    /// NOTE: **毎フレーム呼ぶこと。** 1 フレーム途切れると OS のマウスへ戻る。
    ///       置きっぱなしにできる作りだと、カーソルを持つ側が消えた後 (シーン遷移や
    ///       Play 停止) に古い座標が残り、UI が触れない状態の原因が追えなくなる。
    ///@{
    void SetPointer(const math::Vector2& canvasPosition, bool pressed) const;
    void ClearPointer() const;
    [[nodiscard]] bool IsPointerOverridden() const;
    ///@}
    // スプライトシートのピクセル矩形 (x,y,w,h) をテクスチャサイズ (texW,texH) で正規化し
    // UIImage の uvMin / uvMax へ書き込む。
    void SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const;
    /// UIImage の絵を差し替える。画像パスのほか、Sprite サブアセット参照
    /// ("Assets/UI/Atlas.png::sprite::<id>") をそのまま渡せる。
    /// 切り出し矩形・9-slice の余白・タイル寸法は .meta から自動で解決される。
    void SetImageTexture(std::string_view path) const;
    // 自 GO の UIImage 塗り潰し量 [0,1] を設定する (体力ゲージ等)。
    void SetImageFillAmount(float amount) const;

    // ── GameObject ターゲット版 ───────────────────────────────────────────────
    // 体力バーのように UI が別 GO (HUD キャンバスや子要素) にある場合、
    // スクリプトが保持する GameObject* を直接指定して UI を更新する。
    void SetImageColor(GameObject* go, const math::Vector4& color) const;
    void SetImageFillAmount(GameObject* go, float amount) const;
    void SetImageTexture(GameObject* go, std::string_view path) const;
    void SetText(GameObject* go, std::string_view text) const;
    void SetTextColor(GameObject* go, const math::Vector4& color) const;
    void SetImageEnabled(GameObject* go, bool enabled) const;
    void SetTextEnabled(GameObject* go, bool enabled) const;

    // 現在値の読み取り。ゲージを「今の値から目標値へ」補間する、点滅の基準色を
    // オーサリング値のまま使う、といった演出は、Script 側に控えを持たずここから読む。
    // 対象コンポーネントが無い場合は 0 / 白 / 空文字列 / false を返す。
    [[nodiscard]] float         GetImageFillAmount() const;
    [[nodiscard]] math::Vector4 GetImageColor() const;
    [[nodiscard]] std::string   GetText() const;
    [[nodiscard]] math::Vector4 GetTextColor() const;
    [[nodiscard]] bool          IsImageEnabled() const;
    [[nodiscard]] bool          IsTextEnabled() const;

    [[nodiscard]] float         GetImageFillAmount(GameObject* go) const;
    [[nodiscard]] math::Vector4 GetImageColor(GameObject* go) const;
    [[nodiscard]] std::string   GetText(GameObject* go) const;
    [[nodiscard]] math::Vector4 GetTextColor(GameObject* go) const;
    [[nodiscard]] bool          IsImageEnabled(GameObject* go) const;
    [[nodiscard]] bool          IsTextEnabled(GameObject* go) const;

    // ── UI マテリアル (UIImage.materialPath) ────────────────────────────────
    //
    // WHY 「自分」版を持たないか:
    //   UI 要素 1 つ 1 つにスクリプトを付ける構成にすると、HUD が 20 要素あれば
    //   20 個の Script インスタンスが毎フレーム回る。実際の使い方は
    //   「1 つの HUD スクリプトが複数の要素を指して値を流す」なので、
    //   対象を引数で受ける形だけを出す。
    //
    // WHY 共有 .mat ではなく要素ごとの上書きになるか:
    //   .mat は参照する全要素が共有する実体で、そこへ書くと 1 本のゲージを
    //   動かしたつもりが同じ .mat の全ゲージへ波及する。ここで設定した値は
    //   その UIImage の描画にだけ乗り、シーンにも保存されない。
    //
    // param 名はシェーダーの MaterialConstants に宣言した変数名そのまま。
    // 綴りが違っても失敗しない (存在しない変数は無視される) ので、
    // 効かないときはまずシェーダーの変数名と突き合わせること。
    void SetMaterial(GameObject* go, std::string_view materialPath) const;
    void SetMaterialFloat(GameObject* go, std::string_view param, float value) const;
    void SetMaterialVector2(GameObject* go, std::string_view param, float x, float y) const;
    void SetMaterialVector4(GameObject* go, std::string_view param,
                            const math::Vector4& value) const;
    void SetMaterialColor(GameObject* go, std::string_view param,
                          const math::Vector4& color) const;
    /// スロット名は "albedo" / "normal" / "tex5" 等 (.mat の [textures] と同じ)。
    void SetMaterialTexture(GameObject* go, std::string_view slot,
                            std::string_view texturePath) const;
    /// 1 つだけ共有 .mat の値へ戻す。
    void ClearMaterialOverride(GameObject* go, std::string_view param) const;
    /// この要素の上書きを全て捨て、共有 .mat そのままの見た目へ戻す。
    void ClearMaterialOverrides(GameObject* go) const;
    [[nodiscard]] bool HasMaterialOverride(GameObject* go, std::string_view param) const;
};

} // namespace fbzz::scene
