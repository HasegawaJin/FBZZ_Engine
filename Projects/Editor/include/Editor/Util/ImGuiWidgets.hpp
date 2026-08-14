// FBZZ Engine
// ImGuiWidgets.hpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット集
#pragma once
#include <imgui.h>
#include <algorithm>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <cmath>
#include <string>

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor::widgets {

// 画像パス選択欄で共通利用する、ImageImporter 対応拡張子の検索フィルター。
// WHY: 呼び出し側ごとの列挙漏れにより、読み込める画像がピッカーに表示されない状態を防ぐ。
inline constexpr const char* kTextureAssetFilter =
    ".fztex,.png,.jpg,.jpeg,.tga,.dds,.bmp,.hdr,.exr";

// std::string を直接編集する InputText。
// WHY ここに置くか: 元は VFXEditorUiCommon にあり、VFX 以外のパネルが使うには
//     VFX のヘッダを引く必要があった。文字列編集はどのパネルでも要る汎用部品なので、
//     ウィジェット層へ移して VFX 側は転送するだけにする。
// @return true if the value changed
bool InputString(const char* label, std::string& value, std::size_t capacity = 512);

// Vector3 の DragFloat3 (ラベル幅を統一)
bool DragVec3(const char* label, math::Vector3& v, float speed = 0.1f,
              float min = 0.0f, float max = 0.0f);

// RGB カラーピッカー (Vector3 を [0,1] で扱う)
bool ColorEdit3(const char* label, math::Vector3& color);

// RGBA カラーピッカー (Vector4 を [0,1] で扱う)
inline bool ColorEdit4(const char* label, math::Vector4& color) {
    float v[4] = { color.x, color.y, color.z, color.w };
    if (ImGui::ColorEdit4(label, v)) {
        color = { v[0], v[1], v[2], v[3] };
        return true;
    }
    return false;
}

// アセットのサムネイル用テクスチャ ID を解決する。画像なら本体、.mat なら albedo を返す。
// 解決できない (画像でない / 見つからない) 場合は nullptr。
//
// WHY: 「このノードはどの素材を使っているか」をパス文字列だけで判断させると、
//      名前が似た素材を取り違える。絵を出せば一目で分かる。
//      ResourceManager 側でも GPU ロードはキャッシュされるが、パス解決と .mat の
//      albedo 探索は毎フレームやるには重いので、ここでも結果を覚える。
// NOTE: ResourceManager の resetVersion が変わったらキャッシュ全体を捨てる
//       (デバイスリセット後は古いテクスチャ ID が無効になるため)。
// @param relativePath projectRoot 相対のアセットパス
[[nodiscard]] void* ResolveAssetThumbnail(const std::string& relativePath,
                                          renderer::ResourceManager* resources,
                                          renderer::IImGuiRenderer* imguiRenderer);

// このプロジェクトの ImGui は ImTextureID を ImU64 として扱う。void* からの変換を 1 か所に置く。
[[nodiscard]] ImTextureID ToImTextureID(void* ptr);

// セクションヘッダー (太字テキスト + 区切り線)
void SectionHeader(const char* label);

// 色付きテキスト
void ColoredText(const char* text, ImVec4 color);

// 読み取り専用テキストフィールド
void ReadOnlyText(const char* label, const char* text);

// レンジ付き数値フィールド: スライダー (ゲージ) + 編集可能な数値入力ボックスを 1 行に並べる。
// WHY: SliderFloat 単体は正確な値入力がしづらく、DragFloat 単体は範囲内の量感が掴めない。
//      ゲージで量感とドラッグ操作を、右の入力ボックスで正確なタイプ入力を同時に満たす。
//      スクリプトの FBZZ_FIELD_RANGE (ImGuiReflector::FloatRange) から共通で使う。
// ImGui::SliderFloat の差し替え先として使えるよう、ラベルは右側に描く ("##" 始まりで非表示)。
// @param tooltip 非 nullptr なら、ゲージ / 入力ボックス / ラベルのどこをホバーしても表示する。
//        呼び出し側の ImGui::IsItemHovered() では最後のアイテムしか拾えないため、
//        説明の出し方はウィジェット側に持たせている。
// @return true if value changed
bool RangeField(const char* label, float& value, float min, float max,
                const char* fmt = "%.3f", const char* tooltip = nullptr);

// ─── 数値調整をやりやすくするための共通部品 ───────────────────────────────
// WHY: Inspector は「値をいくつにするか」を延々と試す場所なので、1 回の操作コストが
//      そのまま作業時間に乗る。掴みやすさ (どの成分か即座に分かる) と刻みの妥当さ
//      (小さい値でも大きい値でも同じ手応え) をウィジェット側で担保する。

// 値の大きさに応じたドラッグ刻みを返す。
// WHY: 固定 0.1 刻みだと、0〜1 のブレンド率では粗すぎて狙った値に止められず、
//      逆に数百 m の距離では細かすぎて目的の値まで何度もドラッグし直すことになる。
//      現在値の 1% を目安にし、下限 0.01 (0 から抜け出せる) / 上限 1.0 で挟む。
[[nodiscard]] inline float AdaptiveDragSpeed(float value)
{
    const float magnitude = std::abs(value) * 0.01f;
    return (std::min)(1.0f, (std::max)(0.01f, magnitude));
}

// 軸ごとに色分けした多成分ドラッグ入力。
//   X [1.234]  Y [0.000]  Z [-2.500]
// WHY: DragFloat3 は 3 つの数値が同じ見た目で並ぶため「どれが Z か」を毎回数え直す
//      必要があり、隣の成分を掴む誤操作も起きやすい。頭文字を各成分へ付けると
//      視線だけで対象を特定でき、Position / Rotation / Scale の往復が速くなる。
// NOTE: 頭文字は枠の外 (左隣) に軸色で描く。枠内に入れると数値と一緒に中央寄せされ、
//       桁数によって数値の左端が成分ごとにずれてしまうため。幅が足りない狭い
//       Inspector では頭文字を落とし、代わりに枠内へ軸色のマーカーを出す。
// NOTE: 成分ごとに独立した DragFloat なので、Ctrl+Click の直接入力・Alt (微調整) /
//       Shift (粗調整) といった ImGui 標準操作はそのまま使える。
//       BeginGroup で囲んでいるため、ImGui::DragFloat3 と同様に IsItemActivated() /
//       IsItemDeactivatedAfterEdit() が全成分ぶんまとめて機能する (Undo 追跡が壊れない)。
// @param id  "##" 始まりならラベル非表示。それ以外は DragFloat3 と同じく右側へ描くので、
//            既存の ImGui::DragFloat2/3/4 呼び出しをそのまま差し替えられる。
// @param count 成分数 (1..4 / それぞれ X Y Z W として描く)
// @param speed <= 0 を渡すと成分ごとに AdaptiveDragSpeed() を使う
// @return true if any component changed
bool DragAxes(const char* id, float* values, int count,
              float speed = 0.1f, float min = 0.0f, float max = 0.0f,
              const char* fmt = "%.3f");

inline bool DragAxes(const char* id, math::Vector3& v, float speed = 0.1f,
                     float min = 0.0f, float max = 0.0f, const char* fmt = "%.3f")
{
    float arr[3] = { v.x, v.y, v.z };
    if (!DragAxes(id, arr, 3, speed, min, max, fmt)) return false;
    v = { arr[0], arr[1], arr[2] };
    return true;
}

// 等比リンク付きのスケール入力。[鍵] [▌X] [▌Y] [▌Z]
// WHY: 均一スケールは Inspector で最も頻繁に触る値だが、3 成分を手で揃えると
//      桁がずれて「なぜか潰れたモデル」になりやすい。鍵を閉じている間は
//      掴んだ成分の変化率を他成分へそのまま掛けて比率を保つ。
// @param uniform リンク状態 (呼び出し側が保持する。ボタン押下でトグルされる)
// @return true if the vector changed
bool DragScaleAxes(const char* id, math::Vector3& scale, bool& uniform,
                   float speed = 0.01f);

// ラベルが列幅に収まらない場合だけ末尾を "..." に省略し、ホバーで全文を出す。
// WHY: Inspector のラベル列は幅が固定なので、長いフィールド名は値ウィジェットへ
//      食い込んで行のレイアウトを崩していた。省略 + ツールチップなら情報は失われない。
void LabelEllipsis(const char* text, float maxWidth);

// プロパティ 1 行ぶんのホバー地色を、行の中身より先に敷くためのスコープ。
// WHY: 縦に数十行続く Inspector では「左のラベル」と「右の値」の対応を目で追いづらく、
//      1 行ずれた値を触ってしまう。カーソル下の行だけ淡く塗れば対応が一目で分かる。
// NOTE: 縞模様 (ゼブラ) は採用していない。Inspector は Reflector 生成の行と手書きの行が
//       混在しており、片方だけに恒久的な縞が付くと不具合のように見えるため。
//       ホバーは一時的な表示なので、付いていない行があっても違和感が出ない。
// HOW: ImGui は「これから描く行の高さ」を事前に知らないため、前フレームの実測高さを
//      ImGuiStorage に覚えて背景を先に描く。行高はフレーム間で安定するので、
//      初回フレームだけ 1 行ぶんの既定高さで代用すれば見た目の破綻は起きない。
struct PropertyRowScope {
    ImGuiID key = 0;      // 行高を覚える ImGuiStorage のキー
    float   top = 0.0f;   // 行の開始 Y (スクリーン座標)
};
[[nodiscard]] PropertyRowScope BeginPropertyRow();
void EndPropertyRow(const PropertyRowScope& row);

// プロパティ行で値ウィジェットを開始する X (= ラベル列の幅、ウィンドウローカル)。
// WHY: Reflector が自動生成する行と、手書きの行 (Transform など) で列位置が違うと
//      同じ Inspector の中で値の左端が段違いになり、目で追いづらい。1 か所で決める。
// WHY 上下でクランプするか: 割合だけで決めると、狭いドックでは Position のような
//      3 成分ベクトルに残る幅が足りず数値が欠け、逆に広げたときはラベルの右へ
//      無駄な余白だけが伸びる。「名前が読める最小」と「値に残す幅」を両端で押さえる。
[[nodiscard]] inline float PropertyLabelColumnWidth()
{
    const float font  = ImGui::GetFontSize();
    const float ratio = ImGui::GetWindowWidth() * 0.34f;
    return (std::min)((std::max)(ratio, font * 5.0f), font * 9.0f);
}

// 手書きパネル用のプロパティ行。「地色 + 省略付きラベル + 列合わせ済みの値」を 1 回で用意する。
// 直後に値ウィジェットを 1 つ描き、必ず EndPropertyField() で閉じる。
// (ImGuiReflector は永続キーで PushID する必要があるため、同じ構成を自前で組んでいる)
[[nodiscard]] PropertyRowScope BeginPropertyField(const char* label);
void EndPropertyField(const PropertyRowScope& row);

// ─── カード表現 ───────────────────────────────────────────────────────────
// WHY: Inspector は 10 枚以上のコンポーネントが縦に積まれる画面なので、
//      「どこからどこまでが 1 つのコンポーネントか」が見えないと、値を追うたびに
//      名前を読み直すことになる。ヘッダーと本文を 1 枚のカードとして囲い、
//      左端にカテゴリ色の帯を通すことで、スクロール中でも色と塊で目的地を拾える。

struct ComponentHeaderResult {
    bool   open           = false;  // 本文を描くか (折り畳み状態)
    bool   enabledChanged = false;  // 有効チェックが操作された
    bool   menuClicked    = false;  // ⋯ を押した / ヘッダーを右クリックした
    ImVec2 rectMin{};               // ヘッダー矩形 (呼び出し側の追加描画用)
    ImVec2 rectMax{};
};

// コンポーネントカードのヘッダー。
//   [帯] [▼] [✓] Name ................................... [⋯]
// @param label   表示名 (ImGui ID もこの文字列から作るので、同じ親の中で一意にすること)
// @param accent  左帯の色 (カテゴリ色)
// @param enabled 有効チェックを出す場合の参照先。nullptr ならチェックを描かない
//                (BoneComponent のように常に有効な補助コンポーネント用)
ComponentHeaderResult ComponentHeader(const char* label, ImU32 accent,
                                      bool* enabled, bool defaultOpen = true);

// カード本文のスコープ。淡い地色とヘッダーから続く左帯を敷き、中身を一段字下げする。
// HOW: PropertyRowScope と同じく、ImGui は「これから描く中身の高さ」を事前に知れないため
//      前フレームの実測高さを ImGuiStorage に覚えて地色を先に描く。
// NOTE: 横位置はヘッダーの実測矩形から取る。ContentRegion / WorkRect から独自に計算すると、
//       スクロールバーの有無やインデントの扱いの違いでヘッダーと本文の左右端がずれる。
struct ComponentBodyScope {
    ImGuiID key    = 0;
    float   top    = 0.0f;
    float   left   = 0.0f;
    float   right  = 0.0f;
    float   indent = 0.0f;
    bool    active = false;
};
[[nodiscard]] ComponentBodyScope BeginComponentBody(const ComponentHeaderResult& header, ImU32 accent);
void EndComponentBody(const ComponentBodyScope& body);

// 汎用カード (GameObject ヘッダーなど、コンポーネント以外のまとまりを囲う)。
// 中身を描く前に呼び、必ず EndCard() で閉じる。
[[nodiscard]] ComponentBodyScope BeginCard();
void EndCard(const ComponentBodyScope& card);

// ─── 参照スロット (GameObject / Prefab / DataAsset) ───────────────────────
// WHY: 参照フィールドは「今なにが入っているか」「ここへ落とせるか」「型が合っているか」の
//      3 つを同時に読めないと使えない。素の Button ではどれも表現できず、
//      ドラッグ中にどこへ落とせるのかも分からなかった。状態を枠と色で持たせる。

enum class ReferenceSlotState {
    Empty,     // 未割り当て
    Assigned,  // 割り当て済み
    Invalid,   // 割り当て済みだが型が合わない / 参照先が見つからない
};

// スロット本体を描く。呼び出し後は「直前のアイテム」が本体なので、
// 続けて BeginDragDropTarget() / SetTooltip() をそのまま付けられる。
// @param dropActive 受理できるペイロードをドラッグ中なら true (枠をアクセント色で光らせる)
// @param trailing   本体の右に並べるボタン数 (幅の予約に使う)
// @return 本体がクリックされたか
bool BeginReferenceSlot(const char* id, const char* text, ReferenceSlotState state,
                        bool dropActive, int trailing);

struct ReferenceSlotButtons {
    bool pick  = false;  // ◎ 一覧から選ぶ
    bool clear = false;  // × 参照を外す
};
// 本体の右へ ◎ と × を並べる。グリフはフォントに依存しないよう自前描画。
ReferenceSlotButtons EndReferenceSlot(bool showPick = true, bool showClear = true);

// アセットパス入力フィールド。"..." ボタンで projectRoot/ 以下を検索できるモーダルを開く。
// filterExts: カンマ区切り拡張子 ".mat,.hlsl" (空 = すべてのファイル)
// @return true if path was changed (InputText 編集 / drag-drop / picker 選択のいずれか)
bool AssetPathField(const char* label, std::string& path,
                    const char* filterExts,
                    const std::string& projectRoot);

// AssetPathField + ロードコールバック付き版。
// WHY: パス変更時に必ずアセット再ロードが必要なパターン (MeshRenderer/SkinnedMesh 等) の
//      if (AssetPathField(...)) { reload(); } ボイラープレートを排除する。
// @return true if path was changed (AssetPathField と同じ)
template<typename Fn>
inline bool AssetPathFieldWithLoad(const char* label, std::string& path,
                                   const char* filterExts,
                                   const std::string& projectRoot,
                                   Fn&& onLoad)
{
    if (AssetPathField(label, path, filterExts, projectRoot)) {
        std::forward<Fn>(onLoad)();
        return true;
    }
    return false;
}

// InspectorPanel の OnRenderContent 先頭で毎フレーム 1 回だけ呼ぶ。
// resources / imgui を渡すとピッカーに Unity 風のサムネイル可視化が有効になる
// (画像はテクスチャプレビュー、.mat はアルベドのテクスチャ/色スウォッチ)。null でもリスト表示は動く。
void DrawAssetPickerModal(renderer::ResourceManager* resources = nullptr,
                          renderer::IImGuiRenderer* imgui = nullptr);

// アセット検索ピッカーを任意の文字列ターゲットに対して開く (AssetPathField の "..." と同じ実体)。
// WHY: 独自描画のアセットスロット (参照ボタン形式) からも「パス検索」を使えるようにするための公開口。
//      選択時に target へ正規化済み相対パスが書き込まれる。描画は DrawAssetPickerModal() が担う。
//   filterExts: カンマ区切り拡張子 (".prefab" / ".fzdata" 等、空ですべて)
//   anchorPos : ポップアップを出す画面座標 (通常は呼び出し元ボタンの直下)
void OpenAssetPicker(std::string& target, const char* filterExts,
                     const std::string& projectRoot, ImVec2 anchorPos);

// 直前のアイテムを ASSET_PATH ドラッグ＆ドロップの受け皿にする共通ヘルパー。
// WHY: BeginDragDropTarget / AcceptDragDropPayload("ASSET_PATH") / Normalize / End の
//      定型がパス欄やリスト行に散在していたため集約する。ドロップ後の処理 (拡張子除去・
//      リロード等の特殊挙動) は呼び出し側に委ねるので、既存挙動を保ったまま重複だけ消せる。
// filterExts が指定された場合は手入力欄と同じ拡張子制約をドロップにも適用する。
// @return true if an asset path was dropped (outPath に正規化済みパスを格納)
bool AcceptAssetPathDrop(std::string& outPath, const char* filterExts = nullptr);

// Quaternion → オイラー角 (度, YXZ 順)。InspectorPanel と ImGuiReflector で共用
// YXZ 内因順: X(Pitch) が中間角で ±90° 制約、Y(Yaw) は ±180° 任意範囲。
inline math::Vector3 QuatToEulerDeg(const math::Quaternion& q)
{
    constexpr float DEG = 180.0f / 3.14159265f;
    const math::Matrix4 m = math::Matrix4::Rotate(q);

    // R = Ry * Rx * Rz: R[1][2] = -sin(X)
    float x = std::asin((std::max)(-1.0f, (std::min)(1.0f, -m.m[1][2])));
    float y = 0.0f;
    float z = 0.0f;

    if (std::abs(std::cos(x)) > 1e-6f) {
        // R[0][2] = sin(Y)*cos(X), R[2][2] = cos(Y)*cos(X)
        y = std::atan2(m.m[0][2], m.m[2][2]);
        // R[1][0] = cos(X)*sin(Z), R[1][1] = cos(X)*cos(Z)
        z = std::atan2(m.m[1][0], m.m[1][1]);
    } else {
        // ジンバルロック (X ≈ ±90°): Z=0 とし Y を復元
        y = std::atan2(-m.m[2][0], m.m[0][0]);
    }

    return { x * DEG, y * DEG, z * DEG };
}

// オイラー角 (度, YXZ 順) → Quaternion
inline math::Quaternion EulerDegToQuat(const math::Vector3& deg)
{
    constexpr float RAD = 3.14159265f / 180.0f;
    return math::Quaternion::FromEuler({ deg.x * RAD, deg.y * RAD, deg.z * RAD });
}

inline float SanitizeEulerDeg(float value)
{
    if (std::abs(value) < 0.0001f) return 0.0f;
    return value;
}

inline bool DragQuatEuler3(const char* label, math::Quaternion& q, float speed = 0.5f)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    const ImGuiID initKey = id + 1;
    const ImGuiID eulerXKey = id + 2;
    const ImGuiID eulerYKey = id + 3;
    const ImGuiID eulerZKey = id + 4;
    const ImGuiID quatXKey = id + 5;
    const ImGuiID quatYKey = id + 6;
    const ImGuiID quatZKey = id + 7;
    const ImGuiID quatWKey = id + 8;

    const bool initialized = storage->GetBool(initKey, false);
    const math::Quaternion normalized = q.Normalized();
    const float prevX = storage->GetFloat(quatXKey, normalized.x);
    const float prevY = storage->GetFloat(quatYKey, normalized.y);
    const float prevZ = storage->GetFloat(quatZKey, normalized.z);
    const float prevW = storage->GetFloat(quatWKey, normalized.w);
    const bool quaternionChanged = !initialized ||
        std::abs(prevX - normalized.x) > 0.0001f ||
        std::abs(prevY - normalized.y) > 0.0001f ||
        std::abs(prevZ - normalized.z) > 0.0001f ||
        std::abs(prevW - normalized.w) > 0.0001f;

    if (quaternionChanged) {
        const math::Vector3 euler = QuatToEulerDeg(normalized);
        storage->SetFloat(eulerXKey, SanitizeEulerDeg(euler.x));
        storage->SetFloat(eulerYKey, SanitizeEulerDeg(euler.y));
        storage->SetFloat(eulerZKey, SanitizeEulerDeg(euler.z));
        storage->SetFloat(quatXKey, normalized.x);
        storage->SetFloat(quatYKey, normalized.y);
        storage->SetFloat(quatZKey, normalized.z);
        storage->SetFloat(quatWKey, normalized.w);
        storage->SetBool(initKey, true);
    }

    float values[3] = {
        storage->GetFloat(eulerXKey, 0.0f),
        storage->GetFloat(eulerYKey, 0.0f),
        storage->GetFloat(eulerZKey, 0.0f)
    };

    // 軸色付きの成分入力を使う。回転はどの軸を触っているかの誤認が最も痛い値なので、
    // Pitch(X) / Yaw(Y) / Roll(Z) を色で区別できるようにしておく。
    if (!DragAxes(label, values, 3, speed, 0.0f, 0.0f, "%.1f")) return false;

    values[0] = SanitizeEulerDeg(values[0]);
    values[1] = SanitizeEulerDeg(values[1]);
    values[2] = SanitizeEulerDeg(values[2]);
    q = EulerDegToQuat({ values[0], values[1], values[2] }).Normalized();

    storage->SetFloat(eulerXKey, values[0]);
    storage->SetFloat(eulerYKey, values[1]);
    storage->SetFloat(eulerZKey, values[2]);
    storage->SetFloat(quatXKey, q.x);
    storage->SetFloat(quatYKey, q.y);
    storage->SetFloat(quatZKey, q.z);
    storage->SetFloat(quatWKey, q.w);
    storage->SetBool(initKey, true);
    return true;
}

} // namespace fbzz::editor::widgets
