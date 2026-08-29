/// @file    ImGuiWidgets.hpp
/// @brief   パネル間で共有する ImGui カスタムウィジェット集。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#pragma once
#include <imgui.h>
#include <algorithm>
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <functional>
#include <unordered_set>

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor::widgets {

// 画像パス選択欄で共通利用する、ImageImporter 対応拡張子の検索フィルター。
// WHY: 呼び出し側ごとの列挙漏れにより、読み込める画像がピッカーに表示されない状態を防ぐ。
inline constexpr const char* kTextureAssetFilter =
    ".fztex,.png,.jpg,.jpeg,.tga,.dds,.bmp,.hdr,.exr";

// 音声クリップ選択欄の共通フィルター。
// 正本は Engine 側 (scene::kAudioClipExtensions) — スクリプトの FBZZ_FIELD_AUDIO と
// Editor のピッカーが同じ集合を指す必要があるため、Editor 側では別定義せず参照する。
inline constexpr const char* kAudioClipAssetFilter = scene::kAudioClipExtensions;

// std::string を直接編集する InputText。
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

/// 力場チャンネル (32bit マスク) の編集欄。ParticleForceField と ParticleEmitter が共有する。
/// @return 値が変わったら true。
///
/// WHY ポップアップに畳むか: 既定は全ビット ON で、大半のエミッターは一度も触らない。
///     32 個のチェックボックスを常時並べると、触らない設定が他のモジュールを画面外へ押し出す。
inline bool ForceFieldChannelMask(const char* label, std::uint32_t& mask)
{
    bool changed = false;
    char preview[16];
    if (mask == 0xFFFFFFFFu)    std::snprintf(preview, sizeof(preview), "All");
    else if (mask == 0u)        std::snprintf(preview, sizeof(preview), "None");
    else                        std::snprintf(preview, sizeof(preview), "0x%08X", mask);

    ImGui::PushID(label);
    if (ImGui::Button(preview, ImVec2(ImGui::CalcItemWidth(), 0.0f)))
        ImGui::OpenPopup("##channels");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    if (ImGui::BeginPopup("##channels")) {
        if (ImGui::SmallButton("All"))  { mask = 0xFFFFFFFFu; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) { mask = 0u; changed = true; }
        ImGui::Separator();
        for (int bit = 0; bit < 32; ++bit) {
            if (bit % 8 != 0) ImGui::SameLine();
            ImGui::PushID(bit);
            bool on = (mask & (1u << bit)) != 0u;
            char name[4];
            std::snprintf(name, sizeof(name), "%d", bit);
            if (ImGui::Checkbox(name, &on)) {
                mask = on ? (mask | (1u << bit)) : (mask & ~(1u << bit));
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// アセットのサムネイル用テクスチャ ID を解決する。画像なら本体、.mat なら albedo を返す。
// 解決できない (画像でない / 見つからない) 場合は nullptr。
// パス解決と .mat の albedo 探索は毎フレームやるには重いので結果を覚える。
// resetVersion が変わったらキャッシュ全体を捨てる (古いテクスチャ ID が無効になるため)。
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
// ゲージで量感とドラッグを、右の入力ボックスで正確なタイプ入力を同時に満たす。
// ImGui::SliderFloat の差し替え先として使えるよう、ラベルは右側に描く ("##" 始まりで非表示)。
// @param tooltip 非 nullptr なら、ゲージ / 入力ボックス / ラベルのどこをホバーしても表示する
//        (呼び出し側の IsItemHovered() では最後のアイテムしか拾えないため)。
// @return true if value changed
// 整数版のレンジ入力。float 版と同じ「ゲージ + 数値ボックス」の見た目で描く。
bool RangeField(const char* label, int& value, int min, int max,
                const char* fmt = "%d", const char* tooltip = nullptr);

bool RangeField(const char* label, float& value, float min, float max,
                const char* fmt = "%.3f", const char* tooltip = nullptr);

// ─── 数値調整をやりやすくするための共通部品 ───────────────────────────────
// 掴みやすさ (どの成分か即座に分かる) と刻みの妥当さをウィジェット側で担保する。

// 値の大きさに応じたドラッグ刻みを返す。現在値の 1% を目安に、下限 0.01 / 上限 1.0 で挟む。
// 固定刻みだと 0〜1 のブレンド率では粗すぎ、数百 m の距離では細かすぎる。
[[nodiscard]] inline float AdaptiveDragSpeed(float value)
{
    const float magnitude = std::abs(value) * 0.01f;
    return (std::min)(1.0f, (std::max)(0.01f, magnitude));
}

// 軸ごとに色分けした多成分ドラッグ入力。
//   X [1.234]  Y [0.000]  Z [-2.500]
// 頭文字は枠の外 (左隣) に軸色で描く。枠内に入れると数値と一緒に中央寄せされ、
// 桁数によって数値の左端が成分ごとにずれる。狭いときは頭文字を落として枠内へマーカーを出す。
// 成分ごとに独立した DragFloat なので Ctrl+Click / Alt / Shift はそのまま使える。
// BeginGroup で囲んであるので IsItemActivated() / IsItemDeactivatedAfterEdit() は
// 全成分ぶんまとめて機能する (Undo 追跡が壊れない)。
// @param id  "##" 始まりならラベル非表示。それ以外は DragFloat3 と同じく右側へ描く。
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
// 鍵を閉じている間は、掴んだ成分の変化率を他成分へそのまま掛けて比率を保つ。
// @param uniform リンク状態 (呼び出し側が保持する。ボタン押下でトグルされる)
// @return true if the vector changed
bool DragScaleAxes(const char* id, math::Vector3& scale, bool& uniform,
                   float speed = 0.01f);

// ラベルが列幅に収まらない場合だけ末尾を "..." に省略し、ホバーで全文を出す。
// WHY: Inspector のラベル列は幅が固定なので、長いフィールド名は値ウィジェットへ
//      食い込んで行のレイアウトを崩していた。省略 + ツールチップなら情報は失われない。
void LabelEllipsis(const char* text, float maxWidth);

// プロパティ 1 行ぶんのホバー地色を、行の中身より先に敷くためのスコープ。
// 縞模様 (ゼブラ) は採らない ─ Reflector 生成の行と手書きの行が混在しており、
// 片方だけに恒久的な縞が付くと不具合のように見えるため。
// ImGui は「これから描く行の高さ」を事前に知らないので、前フレームの実測高さを
// ImGuiStorage に覚えて背景を先に描く (初回だけ既定高さで代用する)。
struct PropertyRowScope {
    ImGuiID key = 0;      // 行高を覚える ImGuiStorage のキー
    float   top = 0.0f;   // 行の開始 Y (スクリーン座標)
};
[[nodiscard]] PropertyRowScope BeginPropertyRow();
void EndPropertyRow(const PropertyRowScope& row);

// プロパティ行で値ウィジェットを開始する X (= ラベル列の幅、ウィンドウローカル)。
// 自動生成の行と手書きの行で列位置がずれないよう、1 か所で決める。
// 割合だけだと狭いドックで数値が欠け、広げると余白だけ伸びるので上下でクランプする。
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
// ヘッダーと本文を 1 枚のカードとして囲い、左端にカテゴリ色の帯を通す。
// 10 枚以上積まれる画面で、スクロール中でも色と塊で目的地を拾えるようにする。

struct ComponentHeaderResult {
    bool   open           = false;  // 本文を描くか (折り畳み状態)
    bool   enabledChanged = false;  // 有効チェックが操作された
    bool   menuClicked    = false;  // ⋯ を押した / ヘッダーを右クリックした
    ImVec2 rectMin{};               // ヘッダー矩形 (呼び出し側の追加描画用)
    ImVec2 rectMax{};
};

// ドロップを受けたときの通知。
// @param draggedKey  掴まれた側の識別子 (ComponentReorderTarget::dragKey、既定は label)
// @param insertAfter true = このカードの直後へ、false = 直前へ挿入する
using ComponentReorderCallback =
    std::function<void(std::string_view draggedKey, bool insertAfter)>;

// カードをマウスで並び替えられるようにする指定。
// scope ごとにペイロード名を分け、同じリストの中でしかドロップが成立しないようにする
// (Inspector には Component の表示順・Script の実行順・PostProcess の適用順が同時に並ぶ)。
// dragKey は、同じ表示名が複数並びうるリストで要素を一意に指すための任意キー。
struct ComponentReorderTarget {
    const char*              scope   = nullptr; // 並び替えグループ ID (英数字・18 文字以内)
    const char*              dragKey = nullptr; // ペイロードに載せる識別子。null なら label
    ComponentReorderCallback onDrop{};

    // scope と onDrop の両方が揃って初めて並び替え可能とみなす。
    explicit operator bool() const { return scope != nullptr && static_cast<bool>(onDrop); }
};

// コンポーネントカードのヘッダー。
//   [帯] [▼] [✓] Name ................................... [⋯]
// @param label   表示名 (ImGui ID もこの文字列から作るので、同じ親の中で一意にすること)
// @param accent  左帯の色 (カテゴリ色)
// @param enabled 有効チェックを出す場合の参照先。nullptr ならチェックを描かない
//                (BoneComponent のように常に有効な補助コンポーネント用)
// @param reorder マウスでの並び替え指定。既定 (空) ならドラッグ元にもドロップ先にもならない
ComponentHeaderResult ComponentHeader(const char* label, ImU32 accent,
                                      bool* enabled, bool defaultOpen = true,
                                      const ComponentReorderTarget& reorder = {});

// カード本文のスコープ。淡い地色とヘッダーから続く左帯を敷き、中身を一段字下げする。
// PropertyRowScope と同じく前フレームの実測高さを ImGuiStorage に覚えて地色を先に描く。
// 横位置はヘッダーの実測矩形から取る (独自計算だとスクロールバーの有無でずれる)。
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

// このセッションで ComponentHeader が使った折り畳み状態の ImGuiID 一覧。
// ImGui の StateStorage には折り畳み以外の値も同居している (カード本文の高さ = float 等)。
// 保存側が区別できないと全エントリを int として書き戻し、float が潰れて復元される。
const std::unordered_set<ImGuiID>& ComponentHeaderStateIds();

// 汎用カード (GameObject ヘッダーなど、コンポーネント以外のまとまりを囲う)。
// 中身を描く前に呼び、必ず EndCard() で閉じる。
[[nodiscard]] ComponentBodyScope BeginCard();
void EndCard(const ComponentBodyScope& card);

// ─── 参照スロット (GameObject / Prefab / DataAsset) ───────────────────────
// 「今なにが入っているか」「ここへ落とせるか」「型が合っているか」を枠と色で同時に見せる。

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

// ─── リスト行 (配列フィールド) ─────────────────────────────────────────────
// 参照スロットの ◎ / × と同じ「自前グリフ + テーマ色 + ツールチップ + 無効化」へ揃える。
// 文字ボタン (SmallButton("^") 等) は太さと中心が行ごとにばらつき、アイコンとして読めない。

struct ListRowButtons {
    bool moveUp   = false;
    bool moveDown = false;
    bool remove   = false;
};

// リスト要素 1 行の右端に並べる操作ボタン (▲ ▼ ✕)。直前のアイテムへ SameLine で続ける。
// 端の要素では対応するボタンを無効表示にする (押せるのに動かないボタンを作らない)。
// @param removable false なら ✕ を出さない (FBZZ_FIXED_LIST)
// @param sameLine  false なら先頭の SameLine を省き、呼び出し側が置いたカーソル位置から描く。
//                  構造体配列の見出し行の右端へ寄せたいとき、SameLine だと打ち消されるため。
ListRowButtons ListRowToolbar(int index, int count, bool removable, bool sameLine = true);

// ListRowToolbar が占める横幅。値ウィジェットの幅を決めるのに使う。
[[nodiscard]] float ListRowToolbarWidth(bool removable);

// リスト全体を指す ID。並び替えのドロップ先を同じリスト内へ限定するために使う。
// リスト用の PushID 直後・要素ごとの PushID より前に 1 回だけ取得すること。
[[nodiscard]] ImGuiID ListScopeId();

// 行頭のドラッグつまみ。ここを掴んで行を並び替える。
// 行の本体は値ウィジェットで、そこを掴むのは「値を動かす」操作。掴みどころを分けないと
// 2 つの操作が同じドラッグに重なる。
// @param listId       ListScopeId() の値
// @param index        この行の index
// @param outInsertAfter ドロップ位置がこの行の前か後ろか
// @return ドロップされた要素の元 index。ドロップが無ければ -1
int ListRowDragHandle(ImGuiID listId, int index, bool& outInsertAfter);

// リスト見出し行の右側に置く「N item(s)」表示と + ボタン。
// @return + が押されたら true
bool ListAddButton(std::size_t count, bool addable);

// 単独の ✕ (削除) ボタン。並び替えを持たない要素見出し (構造体配列) 用。
bool ListRemoveButton();

// 行の右端に reserve だけ余白を空けたまま値ウィジェットを描くためのスコープ。
// BeginReferenceSlot / AssetPathField / DragAxes は幅を GetContentRegionAvail() から決める
// 複合ウィジェットで SetNextItemWidth を見ない。作業領域の右端そのものを詰める。
struct RightReserveScope {
    float previousWorkRight = 0.0f;
    bool  active            = false;
};
[[nodiscard]] RightReserveScope BeginRightReserve(float reserve);
void EndRightReserve(const RightReserveScope& scope);

// アセット参照欄 → AssetBrowser への「この参照先を一覧で見せろ」要求 (Unity の Ping 相当)。
// widgets 層は EditorContext を知らないので one-shot の静的チャネルに積み、
// EditorApp が毎フレーム 1 回だけ取り出してパネル間リクエストへ移す。
struct AssetRevealRequest {
    std::string path;                      // 欄に入っている値 (Assets 起点の相対パス)
    bool        selectInInspector = false; // true = Inspector の表示対象もこのアセットへ移す
};
void RequestAssetReveal(std::string assetPath, bool selectInInspector);
// 保留中の要求を取り出して消す。要求が無ければ false (out は変更しない)。
[[nodiscard]] bool ConsumeAssetRevealRequest(AssetRevealRequest& out);

// アセットパス入力フィールド。"..." ボタンで projectRoot/ 以下を検索できるモーダルを開く。
// filterExts: カンマ区切り拡張子 ".mat,.hlsl" (空 = すべてのファイル)
// 操作: シングルクリック = AssetBrowser で Ping / ダブルクリック = 選択して Inspector を切替 /
//       右クリック = パス直接編集・コピー・クリア / D&D と "..." は従来どおり。
// @return true if path was changed (InputText 編集 / drag-drop / picker 選択のいずれか)
bool AssetPathField(const char* label, std::string& path,
                    const char* filterExts,
                    const std::string& projectRoot);

// AssetPathField + ロードコールバック付き版。パス変更時に必ず再ロードが要るパターン
// (MeshRenderer / SkinnedMesh 等) のボイラープレートを排除する。
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

// ドラッグ中にスクロール領域の上下端へカーソルを置くと、自動で縦スクロールする設定。
// WHY: Hierarchy / Inspector / AssetBrowser などの各パネルが個別に実装すると、
//      端からの距離・速度・DeltaTime の扱いがばらつき、ドラッグ操作の感触が揃わない。
struct DragAutoScrollOptions {
    float edgeSize = 48.0f;  // 上下端からこの距離以内をスクロール帯にする (px)
    float maxSpeed = 720.0f; // スクロール帯の最端での最大速度 (px / 秒)
};

// 現在の ImGui ウィンドウを、ドラッグ中の自動スクロール対象にする。
// @return 今フレームに自動スクロールを試みた場合 true
bool UpdateDragAutoScroll(const DragAutoScrollOptions& options = {});

// 任意の画面座標矩形をドラッグ中の自動スクロール対象にする。
// Child ウィンドウや分割ペインなど、現在のウィンドウ全体とは異なる領域で使う。
// @return 今フレームに自動スクロールを試みた場合 true
bool UpdateDragAutoScroll(const ImVec2& regionMin, const ImVec2& regionMax,
                          const DragAutoScrollOptions& options = {});

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
