/// @file    ImGuiReflector.hpp
/// @brief   ImGui implementation of scene script reflection fields.
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>  // カーブ / グラデーションのキャンバス編集
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

struct ImGuiReflector : scene::IReflector {
    // GO 名解決コールバック。InspectorCore から activeScene を渡して設定する。
    std::function<std::string(scene::EntityID)> m_goNameResolver;

    // オブジェクトピッカー◎用の候補 (EntityID, 表示名) 一覧プロバイダ。
    // WHY: D&D だけだと Hierarchy が深い/多いシーンで参照アサインが辛い。
    //      Unity のピッカーと同様、一覧から検索して選べる代替手段を提供する。
    //      InspectorCore から activeScene の全 GameObject を列挙して設定する。
    std::function<std::vector<std::pair<scene::EntityID, std::string>>()> m_goListProvider;

    // 型付き参照 (FBZZ_REF) の型チェック。指定 EntityID の GameObject が typeName の Script を
    // 持つかを返す。空 typeName (任意 GameObject) や未設定時は型不問。InspectorCore が配線する。
    std::function<bool(scene::EntityID, const char* typeName)> m_refTypeValidator;
    std::function<std::vector<std::string>()> m_tagListProvider;

    // アセットスロットの "..." パス検索ピッカー用。プロジェクトルート (絶対パス)。
    // InspectorCore から ctx.projectRoot を渡す。空なら検索ボタンを出さない。
    std::string m_projectRoot;

    // FBZZ_GROUP の折りたたみ状態。Group() が CollapsingHeader の開閉で更新し、
    // 各 Field はこれが false (閉) の間は描画をスキップする。グループ前の項目は true (既定)。
    bool m_groupOpen = true;
    bool m_rowDisabled = false;
    bool m_changed = false;

    // 描画中の行 (ホバー地色の高さ記録用)。行は入れ子にならないので 1 つで足りる。
    widgets::PropertyRowScope m_row{};

    // シリアライズ用 lowerCamelCase キーを Inspector 用の読みやすい表示名へ変換し、
    // 訳があれば訳へ差し替える。
    //
    // WHY ここで訳すか: Inspector に出るラベルは、組み込みコンポーネントも
    //     ユーザープロジェクトのスクリプトも全部この 1 関数を通る。呼び出し側を
    //     書き換えずに、エンジンの辞書へ 1 行足すだけで日本語になる。
    //
    // WHY ### を付けないか: 呼び出し元はどれも直前に PushID(PersistentKey(name)) を
    //     しており、ImGui の ID はシリアライズキーから作られる。ラベルを訳しても
    //     ID は動かないので、素の訳でよい (Text 系へ渡しても "###" が出ない)。
    static std::string HumanizeName(const char* name)
    {
        // 既に ASCII でない = プロジェクト側が表示名を自国語で書いている。
        // 整形も辞書引きもせず、そのまま出す。
        //
        // WHY 素通しが要るか: ユーザープロジェクトのフィールド名は «そのゲームの語彙» で、
        //     エンジンが訳語を持つ筋合いのものではない (エンジンの辞書に特定のゲームの
        //     用語が溜まっていく)。表示名は FBZZ_FIELD の第 4 引数で書けるので、
        //     作者が最初から日本語で書けばよい ── そこはプロジェクトの自由。
        //     整形も通してはいけない: 下の toupper はロケール次第で UTF-8 の
        //     先頭バイトを書き換えうるので、掛けると文字が壊れる。
        if (IsNonAscii(name)) return name;

        return loc::Text(RawHumanizeName(name).c_str());
    }

    static bool IsNonAscii(const char* text)
    {
        for (const char* p = text; *p != '\0'; ++p)
            if (static_cast<unsigned char>(*p) >= 0x80) return true;
        return false;
    }

    // 訳を通さない素の表示名。辞書の鍵はこちらの綴り。**ASCII 専用。**
    static std::string RawHumanizeName(const char* name)
    {
        std::string result;
        for (size_t i = 0; name[i] != '\0'; ++i) {
            const unsigned char current = static_cast<unsigned char>(name[i]);
            if (name[i] == '_') {
                result.push_back(' ');
                continue;
            }
            if (i > 0 && std::isupper(current) && name[i - 1] != ' ')
                result.push_back(' ');
            result.push_back(i == 0
                ? static_cast<char>(std::toupper(current))
                : name[i]);
        }
        return result;
    }

    // フィールド名の規約からアセット種別を決める。
    // WHY: 各コンポーネントにEditor専用メタデータを重複記述せず、xxxPathだけで
    //      ドラッグ＆ドロップと検索ピッカーを自動提供する。
    static const char* AssetFilterFor(const char* name)
    {
        if (std::strcmp(name, "texturePath") == 0) return widgets::kTextureAssetFilter;
        if (std::strcmp(name, "fontPath") == 0)    return ".png,.fnt,.ttf,.ttc,.otf";
        if (std::strcmp(name, "materialPath") == 0) return ".mat";
        if (std::strcmp(name, "meshPath") == 0 || std::strcmp(name, "modelPath") == 0)
            return ".fbx,.fzmodel";
        if (std::strcmp(name, "controllerPath") == 0) return ".animcontroller";
        if (std::strcmp(name, "clipPath") == 0) return widgets::kAudioClipAssetFilter;
        return nullptr;
    }

    // 入れ子オブジェクト / 配列 / 配列要素の見出し。
    // WHY: 既定の CollapsingHeader は全幅の濃い塗りバーで、コンポーネントカードの
    //      ヘッダーと同じ重みに見える。カードの中に何個も並ぶと「どこまでが 1 コンポーネントか」
    //      が読めなくなるため、入れ子側は塗りを持たない軽い見出しへ落として階層差を付ける。
    static bool SubHeader(const char* label)
    {
        ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Color(ThemeColor::SurfaceHover));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  EditorTheme::Color(ThemeColor::AccentSoft));
        ImGui::PushStyleColor(ImGuiCol_Text,          EditorTheme::Color(ThemeColor::TextMuted));
        // AllowOverlap: 見出しの上へ重ねた ▲▼✕ (構造体配列の要素操作) がクリックを拾えるようにする。
        // これが無いと見出しが先に HoveredId を取り、重ねたボタンは押しても反応しない。
        const bool open = ImGui::CollapsingHeader(
            label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopStyleColor(4);
        return open;
    }

    // ── レイアウト補助 (ラベル左 + 値が右いっぱいの 2 カラム) ──────────────────
    // WHY: 全フィールドで値の左端をそろえると Unity ライクで整然と見える。
    //      従来は ImGui 既定のラベル右寄せ + 参照スロットは手書きで名前を右に追記しており、
    //      幅が溢れて切れていた。ここで「ラベル左・値右いっぱい」に統一して見た目と崩れを解消する。

    // 値ウィジェットの開始 X (ラベル列幅)。ウィンドウ幅に追従しつつ下限を持たせる。
    // 手書きパネル (Transform 等) と列位置をそろえるため、算出は widgets 側に一本化する。
    float ValueColumnX() const { return widgets::PropertyLabelColumnWidth(); }

    // 1 フィールド行を開始する。ラベルを左に描き、続くウィジェットの左端をそろえる。
    // false ならグループ閉でスキップ。true のときは必ず EndRow() を呼ぶこと。
    bool BeginRow(const char* name)
    {
        if (!m_groupOpen || !FieldVisible()) return false;
        ImGui::PushID(PersistentKey(name));

        // 行のホバー地色を中身より先に敷く。実測高さは EndRow が記録する。
        m_row = widgets::BeginPropertyRow();

        const float col = ValueColumnX();
        // ラベル列に収まらない長い名前は末尾を省略し、全文はホバーのツールチップへ回す。
        // WHY: 以前は長い名前がそのまま値ウィジェットを押し出し、行ごとに値の左端が
        //      ずれて「どこを触ればいいか」が読み取りづらくなっていた。
        widgets::LabelEllipsis(
            HumanizeName(name).c_str(),
            col - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < col)
            ImGui::SetCursorPosX(col);
        // 続く単一ウィジェットを右端まで広げる (複数ボタンのスロットは Button が無視するので無害)。
        ImGui::SetNextItemWidth(-FLT_MIN);
        m_rowDisabled = !FieldEnabled();
        if (m_rowDisabled) ImGui::BeginDisabled();
        return true;
    }
    void EndRow()
    {
        if (m_rowDisabled) ImGui::EndDisabled();
        m_rowDisabled = false;
        widgets::EndPropertyRow(m_row);
        ImGui::PopID();
    }

    // ── 基本型 ───────────────────────────────────────────────────────────────

    void Field(const char* name, float& v) override
    {
        if (!BeginRow(name)) return;
        // 刻みが宣言されていなければ現在値の大きさに合わせる。
        // WHY: 固定 0.1 では 0〜1 のブレンド率が粗すぎ、数百 m の距離では細かすぎた。
        const float speed = FieldStep() > 0.0f ? FieldStep() : widgets::AdaptiveDragSpeed(v);
        const float minimum = HasFieldMin() ? FieldMin() : 0.0f;
        const char* format = CurrentFieldHint() == FieldHint::Angle ? "%.1f deg" : "%.3f";
        if (ImGui::DragFloat("##v", &v, speed, 0.0f, 0.0f, format)) {
            if (HasFieldMin()) v = (std::max)(v, minimum);
            m_changed = true;
        }
        EndRow();
    }

    void Field(const char* name, int& v) override
    {
        if (!BeginRow(name)) return;
        const int speed = FieldStep() > 0.0f ? (std::max)(1, static_cast<int>(FieldStep())) : 1;
        const int minimum = HasFieldMin() ? static_cast<int>(FieldMin()) : 0;
        if (CurrentFieldHint() == FieldHint::LayerMask) {
            // 閉じたままでも何番のレイヤーが有効かを読めるようにする (Flags と同じ規則)。
            std::string preview = "(None)";
            {
                int selectedCount = 0;
                std::string joined;
                for (int layer = 0; layer < 32; ++layer) {
                    if ((static_cast<uint32_t>(v) & (uint32_t{1} << layer)) == 0) continue;
                    ++selectedCount;
                    if (selectedCount <= 2) {
                        if (!joined.empty()) joined += ", ";
                        joined += std::to_string(layer);
                    }
                }
                if (selectedCount > 2)      preview = std::to_string(selectedCount) + " layers";
                else if (selectedCount > 0) preview = "Layer " + joined;
            }
            if (ImGui::BeginCombo("##v", preview.c_str())) {
                for (int layer = 0; layer < 32; ++layer) {
                    const uint32_t bit = uint32_t{1} << layer;
                    bool selected = (static_cast<uint32_t>(v) & bit) != 0;
                    char label[24];
                    std::snprintf(label, sizeof(label), "Layer %d", layer);
                    if (ImGui::Checkbox(label, &selected)) {
                        uint32_t mask = static_cast<uint32_t>(v);
                        if (selected) mask |= bit;
                        else mask &= ~bit;
                        v = static_cast<int>(mask);
                        m_changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else if (ImGui::DragInt("##v", &v, static_cast<float>(speed))) {
            if (HasFieldMin()) v = (std::max)(v, minimum);
            m_changed = true;
        }
        EndRow();
    }

    void Field(const char* name, bool& v) override
    {
        if (!BeginRow(name)) return;
        m_changed |= ImGui::Checkbox("##v", &v);
        EndRow();
    }

    // ベクトル系はすべて軸色付きの成分入力 (widgets::DragAxes) に通す。
    // WHY: DragFloat2/3/4 は同じ見た目の数値が並ぶだけで、どれが Y でどれが Z かを
    //      毎回数え直す必要があった。頭文字 + 色帯にすると視線だけで対象が分かり、
    //      隣の成分を掴む誤操作も減る。刻みは宣言が無ければ値の大きさに追従させる。
    void Field(const char* name, math::Vector2& v) override
    {
        if (!BeginRow(name)) return;
        float arr[2] = { v.x, v.y };
        if (widgets::DragAxes("##v", arr, 2, FieldStep())) {
            v = { arr[0], arr[1] };
            m_changed = true;
        }
        EndRow();
    }

    void Field(const char* name, math::Vector3& v) override
    {
        if (!BeginRow(name)) return;
        float arr[3] = { v.x, v.y, v.z };
        // 色として宣言された Vector3 はカラーピッカーで扱う (RGB を数値で合わせるのは非現実的)。
        const bool edited = CurrentFieldHint() == FieldHint::Color
            ? ImGui::ColorEdit3("##v", arr)
            : widgets::DragAxes("##v", arr, 3, FieldStep());
        if (edited) {
            v = { arr[0], arr[1], arr[2] };
            m_changed = true;
        }
        EndRow();
    }

    void Field(const char* name, math::Vector4& v) override
    {
        if (!BeginRow(name)) return;
        float arr[4] = { v.x, v.y, v.z, v.w };
        const bool edited = CurrentFieldHint() == FieldHint::Color
            ? ImGui::ColorEdit4("##v", arr)
            : widgets::DragAxes("##v", arr, 4, FieldStep());
        if (edited) {
            v = { arr[0], arr[1], arr[2], arr[3] };
            m_changed = true;
        }
        EndRow();
    }

    void Field(const char* name, std::string& v) override
    {
        if (!BeginRow(name)) return;
        const char* metadataFilter = CurrentFieldHint() == FieldHint::File
            ? FileExtensions().c_str() : nullptr;
        if (const char* filter = metadataFilter ? metadataFilter : AssetFilterFor(PersistentKey(name))) {
            const bool assetChanged = widgets::AssetPathField("##v", v, filter, m_projectRoot);
            m_changed |= assetChanged;
            if (assetChanged && std::strcmp(PersistentKey(name), "fontPath") == 0) {
                // WHY 静的アトラスだけ拡張子を落とすか: .fnt と PNG は 2 枚組で、
                //     FontAtlas はその共通のベースパスを受け取る。一方 .ttf/.ttc/.otf は
                //     動的モードでパスが実体そのものを指すため、落とすと参照が壊れる。
                std::filesystem::path path = util::FileSystem::PathFromUtf8(v);
                std::string ext = path.extension().string();
                for (char& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".png" || ext == ".fnt") {
                    path.replace_extension();
                    v = util::FileSystem::PathToUtf8(path);
                }
            }
        } else {
            // ミキサーバス名は ProjectSettings に定義された集合から選ばせる。
            // WHY 自由入力にしないか: 未知の名前は Master へ落ちるだけでエラーに
            //     ならないため、綴りミスが「なぜか音量設定が効かない」形でしか出ない。
            if (CurrentFieldHint() == FieldHint::AudioBus) {
                if (ImGui::BeginCombo("##v", v.empty() ? "(Master)" : v.c_str())) {
                    if (auto* manager = core::Application::Get().GetAudioManager()) {
                        for (const audio::BusDesc& bus : manager->BusLayout()) {
                            if (ImGui::Selectable(bus.name.c_str(), bus.name == v)) {
                                v = bus.name;
                                m_changed = true;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                EndRow();
                return;
            }
            if (CurrentFieldHint() == FieldHint::Tag && m_tagListProvider) {
                if (ImGui::BeginCombo("##v", v.empty() ? "(None)" : v.c_str())) {
                    for (const std::string& tag : m_tagListProvider()) {
                        const bool selected = tag == v;
                        if (ImGui::Selectable(tag.c_str(), selected)) {
                            v = tag;
                            m_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                EndRow();
                return;
            }
            std::string buf = v;
            const size_t extraCapacity = std::strcmp(name, "text") == 0 ? 1024 : 128;
            buf.resize(buf.size() + extraCapacity);
            if (CurrentFieldHint() == FieldHint::Multiline || std::strcmp(name, "text") == 0) {
                if (ImGui::InputTextMultiline("##v", buf.data(), buf.size(),
                                              { -FLT_MIN, ImGui::GetTextLineHeight() * 3.0f })) {
                    v = buf.data();
                    m_changed = true;
                }
            } else if (ImGui::InputText("##v", buf.data(), buf.size())) {
                v = buf.data();
                m_changed = true;
            }
        }
        EndRow();
    }

    void Field(const char* name, math::Quaternion& v) override
    {
        if (!BeginRow(name)) return;
        m_changed |= widgets::DragQuatEuler3("##v", v, 0.5f);
        EndRow();
    }

    // ── 参照型 ───────────────────────────────────────────────────────────────

    // 任意 GameObject 参照 (EntityID 単体)。型要件なしで DrawObjectRefSlot を呼ぶ。
    void Field(const char* name, scene::EntityID& v) override
    {
        if (!BeginRow(name)) return;
        const scene::EntityID before = v;
        DrawObjectRefSlot(v, "");
        m_changed |= v != before;
        EndRow();
    }

    // 型付きオブジェクト参照 (FBZZ_REF)。typeName の Script かコンポーネントを持つ GO のみ受理する。
    void RefField(const char* name, scene::EntityRef& v, const char* typeName) override
    {
        if (!BeginRow(name)) return;
        const scene::EntityID before = v.id;
        DrawObjectRefSlot(v.id, typeName);
        m_changed |= v.id != before;
        EndRow();
    }

    // オブジェクト参照スロット本体 (BeginRow 済みの値カラムに描く)。
    //   [参照ボタン(右いっぱい)] [◎ピッカー] [×クリア]
    //   typeName == ""  : 任意 GameObject 可
    //   typeName != ""  : その Script 型 / コンポーネント型を持つ GO のみ受理
    //                     (ドロップ検証 + ピッカー絞り込み + 不一致を赤表示)
    void DrawObjectRefSlot(scene::EntityID& v, const char* typeName)
    {
        const bool typed = typeName && typeName[0] != '\0';

        // GO 名を解決して表示ラベルを作る
        std::string goName = "(None)";
        if (v.IsValid() && m_goNameResolver)
            goName = m_goNameResolver(v);
        else if (v.IsValid())
            goName = "(ID:" + std::to_string(v.index) + ")";

        // 現在の参照が型要件を満たすか (validator 未設定時はチェック不能なので OK 扱い)。
        const bool typeOk = !typed || !v.IsValid() || !m_refTypeValidator ||
            m_refTypeValidator(v, typeName);

        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        const bool droppable = drag && drag->IsDataType("FBZZ_HIERARCHY_ENTITY");

        const auto state = !v.IsValid() ? widgets::ReferenceSlotState::Empty
                         : typeOk       ? widgets::ReferenceSlotState::Assigned
                                        : widgets::ReferenceSlotState::Invalid;

        // 本体スロット。枠の色で「落とせる / 型違い」を出すので、
        // 名前に "(type?)" を書き足していた旧表示は不要になった。
        widgets::BeginReferenceSlot("##ref", goName.c_str(), state, droppable, 2);

        if (ImGui::BeginDragDropTarget()) {
            if (auto* payload = ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY")) {
                scene::EntityID dropped;
                std::memcpy(&dropped, payload->Data, sizeof(dropped));
                if (!typed || !m_refTypeValidator || m_refTypeValidator(dropped, typeName))
                    v = dropped;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered()) {
            if (!typeOk)
                ImGui::SetTooltip("Type mismatch\nThis slot needs a GameObject with \"%s\"", typeName);
            else if (typed)
                ImGui::SetTooltip("Requires: %s\nDrag a GameObject from the Hierarchy", typeName);
            else
                ImGui::SetTooltip("Drag a GameObject from the Hierarchy");
        }

        const widgets::ReferenceSlotButtons buttons = widgets::EndReferenceSlot();
        if (buttons.clear) v = scene::EntityID::INVALID;
        // ◎ピッカー: ドロップせずとも一覧から検索して選べる D&D 代替。型付きなら候補も絞る。
        if (buttons.pick)  ImGui::OpenPopup("##gopick");

        if (ImGui::BeginPopup("##gopick")) {
            static char filter[64] = "";
            if (ImGui::IsWindowAppearing()) {
                filter[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(240.0f);
            ImGui::InputTextWithHint("##gofilter", "Search...", filter, sizeof(filter));
            ImGui::Separator();
            if (ImGui::Selectable("(None)", !v.IsValid()))
                v = scene::EntityID::INVALID;

            int shown = 0;
            if (m_goListProvider) {
                for (const auto& [id, nm] : m_goListProvider()) {
                    // 絞り込みは AssetSearch と同じスコア規則にそろえる。
                    // WHY: ここだけ大小文字を区別する find() だったため、"player" では
                    //      "Player" が出ず、他の検索欄と当たり方が食い違っていた。
                    if (filter[0] && AssetSearch::Match(nm, filter) == 0)
                        continue;
                    if (typed && m_refTypeValidator && !m_refTypeValidator(id, typeName))
                        continue;
                    ImGui::PushID(static_cast<int>(id.index));
                    if (ImGui::Selectable(nm.c_str(), id == v))
                        v = id;
                    ImGui::PopID();
                    ++shown;
                }
            }
            if (shown == 0)
                ImGui::TextDisabled("%s", typed ? "No GameObject has that script or component."
                                                : "No GameObject matches.");
            ImGui::EndPopup();
        }
    }

    void Field(const char* name, scene::PrefabRef& v) override
    {
        if (!BeginRow(name)) return;
        const std::string before = v.path;

        const auto  slash   = v.path.find_last_of("/\\");
        const char* display = v.path.empty() ? "(None)"
                            : (slash != std::string::npos ? v.path.c_str() + slash + 1
                                                          : v.path.c_str());

        const bool hasSearch = !m_projectRoot.empty();
        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        const bool droppable = drag && drag->IsDataType("ASSET_PATH");

        const ImVec2 slotPos = ImGui::GetCursorScreenPos();
        const bool bodyClicked = widgets::BeginReferenceSlot(
            "##prefab", display,
            v.path.empty() ? widgets::ReferenceSlotState::Empty
                           : widgets::ReferenceSlotState::Assigned,
            droppable, hasSearch ? 2 : 1);

        if (ImGui::BeginDragDropTarget()) {
            if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                v.path.assign(static_cast<const char*>(payload->Data),
                              static_cast<size_t>(payload->DataSize - 1));
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered()) {
            if (v.path.empty()) ImGui::SetTooltip("Drop a .prefab here, or click to browse");
            else                ImGui::SetTooltip("%s", v.path.c_str());
        }

        const widgets::ReferenceSlotButtons buttons = widgets::EndReferenceSlot(hasSearch);
        if (buttons.clear) v.path.clear();
        // 本体クリックでもピッカーを開く。狭い Inspector で ◎ を狙うのは
        // 当たり判定が小さく、外すたびに参照を触ってしまうため。
        if (hasSearch && (buttons.pick || bodyClicked))
            widgets::OpenAssetPicker(v.path, ".prefab", m_projectRoot,
                                     { slotPos.x, slotPos.y + ImGui::GetFrameHeight() + 2.0f });

        m_changed |= v.path != before;
        EndRow();
    }

    // DataAsset (純共有 ScriptableObject) 参照スロット。
    // Asset ブラウザから .fzdata をドロップして割り当てる。.fzdata かつ期待型一致のみ受理する。
    void Field(const char* name, scene::DataAssetRef& v) override
    {
        if (!BeginRow(name)) return;
        const std::string before = v.path;

        const auto  slash   = v.path.find_last_of("/\\");
        const char* display = v.path.empty() ? "(None)"
                            : (slash != std::string::npos ? v.path.c_str() + slash + 1
                                                          : v.path.c_str());

        const bool hasSearch = !m_projectRoot.empty();
        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        const bool droppable = drag && drag->IsDataType("ASSET_PATH");

        const ImVec2 slotPos = ImGui::GetCursorScreenPos();
        const bool bodyClicked = widgets::BeginReferenceSlot(
            "##dataasset", display,
            v.path.empty() ? widgets::ReferenceSlotState::Empty
                           : widgets::ReferenceSlotState::Assigned,
            droppable, hasSearch ? 2 : 1);

        if (ImGui::BeginDragDropTarget()) {
            if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                const std::string dropped(static_cast<const char*>(payload->Data),
                                          static_cast<size_t>(payload->DataSize - 1));
                const bool isFzdata = dropped.size() >= 7 &&
                    dropped.compare(dropped.size() - 7, 7, ".fzdata") == 0;
                if (isFzdata) {
                    bool typeOk = v.type.empty();
                    if (!typeOk) {
                        asset::DataAssetRegistry::Resolve(dropped);
                        typeOk = (asset::DataAssetRegistry::TypeOf(dropped) == v.type);
                    }
                    if (typeOk)
                        v.path = dropped;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered()) {
            if (!v.path.empty())      ImGui::SetTooltip("%s", v.path.c_str());
            else if (!v.type.empty()) ImGui::SetTooltip("Drop a .fzdata of type \"%s\" here",
                                                        v.type.c_str());
            else                      ImGui::SetTooltip("Drop a .fzdata here, or click to browse");
        }

        const widgets::ReferenceSlotButtons buttons = widgets::EndReferenceSlot(hasSearch);
        if (buttons.clear) v.path.clear();
        if (hasSearch && (buttons.pick || bodyClicked))
            widgets::OpenAssetPicker(v.path, ".fzdata", m_projectRoot,
                                     { slotPos.x, slotPos.y + ImGui::GetFrameHeight() + 2.0f });

        m_changed |= v.path != before;
        EndRow();
    }

    static const char* ExtensionsForAssetType(scene::ScriptAssetType type)
    {
        switch (type) {
        case scene::ScriptAssetType::Material:      return ".mat";
        case scene::ScriptAssetType::Texture:       return ".png,.jpg,.jpeg,.tga,.dds,.hdr,.exr,.bmp";
        case scene::ScriptAssetType::Sprite:        return ".png,.jpg,.jpeg,.tga,.dds,.bmp";
        case scene::ScriptAssetType::AudioClip:     return widgets::kAudioClipAssetFilter;
        case scene::ScriptAssetType::AnimationClip: return ".anim";
        case scene::ScriptAssetType::Scene:         return ".scene";
        case scene::ScriptAssetType::Shader:        return ".hlsl,.hlsli";
        case scene::ScriptAssetType::VFX:           return ".vfx";
        }
        return "";
    }

    void AssetField(const char* name,
                    scene::ScriptAssetReference& v,
                    scene::ScriptAssetType type) override
    {
        if (!BeginRow(name)) return;

        if (v.path.empty() && !v.guid.empty())
            v.path = v.ResolvePath();

        const char* extensions = ExtensionsForAssetType(type);

        const std::string before = v.path;
        if (widgets::AssetPathField("##assetRef", v.path, extensions, m_projectRoot) &&
            v.path != before) {
            v.SetPath(v.path);
            m_changed = true;
        }
        EndRow();
    }

    // ── 配列フィールド共通のリスト描画 ───────────────────────────────────────
    // 見た目は Inspector の他の行にそろえる:
    //   Name                              3 items [+]
    //     ⠿  [ 値ウィジェット ............. ]  [▲][▼][✕]
    // 行はプロパティ行 (ホバー地色) に乗せ、値の左端は他のフィールドと同じ列に置く。

    // 「target の前 / 後ろ」を、掴んだ要素を抜いた後の移動先 index へ変換する。
    // 抜いた分だけ後ろの要素が前へ詰まるので、自分より後ろへ挿すときは 1 引く。
    static int ListDropDestination(int dragged, int target, bool insertAfter)
    {
        int destination = insertAfter ? target + 1 : target;
        if (dragged < destination) --destination;
        return destination;
    }

    // 1 要素を目的の位置まで隣と入れ替えながら運ぶ。間の要素の相対順序は保たれる。
    //
    // WHY std::rotate を使わないか: std::vector<bool> の operator[] はプロキシ参照を返し、
    //     iter_swap / rotate が要求する ValueSwappable を処理系依存でしか満たさない。
    //     値のコピーで書けば bool 特殊化を含めどの要素型でも同じ 1 実装で通る。
    template<typename T>
    static bool ApplyListMove(std::vector<T>& values, int from, int to)
    {
        const int count = static_cast<int>(values.size());
        if (from < 0 || from >= count || to < 0 || to >= count || from == to) return false;
        const int step = (from < to) ? 1 : -1;
        for (int i = from; i != to; i += step) {
            T current = values[static_cast<size_t>(i)];
            values[static_cast<size_t>(i)] = values[static_cast<size_t>(i + step)];
            values[static_cast<size_t>(i + step)] = current;
        }
        return true;
    }

    // 見出し行 ("N items" + 追加ボタン)。追加された場合 true。
    // emplace は要素型を知る呼び出し側に任せる。
    bool DrawListHeaderRow(const char* name, std::size_t count, bool addable)
    {
        if (!BeginRow(name)) return false;
        const bool added = widgets::ListAddButton(count, addable);
        EndRow();
        return added;
    }

    // 要素 1 行ぶんの枠 (つまみ → 値 → ▲▼✕) を描き、操作要求を移動/削除へ畳む。
    // drawValue には値ウィジェットだけを描かせる (幅とカーソル位置はこちらで整える)。
    template<typename T, typename DrawValue>
    void DrawListRows(std::vector<T>& values, bool removable, DrawValue&& drawValue)
    {
        const ImGuiID listId = widgets::ListScopeId();
        const float toolbarWidth = widgets::ListRowToolbarWidth(removable);
        const int   count = static_cast<int>(values.size());

        int removeIndex = -1;
        int moveFrom = -1;
        int moveTo   = -1;

        for (int index = 0; index < count; ++index) {
            ImGui::PushID(index);
            const widgets::PropertyRowScope row = widgets::BeginPropertyRow();

            // つまみは値カラムより左 (ラベル列の右端) へ置く。ラベルの無い行なので、
            // ここが「この行の持ち手」であることが位置だけで分かる。
            bool insertAfter = false;
            const int dragged = widgets::ListRowDragHandle(listId, index, insertAfter);
            if (dragged >= 0) {
                moveFrom = dragged;
                moveTo   = ListDropDestination(dragged, index, insertAfter);
            }

            ImGui::SameLine();
            if (ImGui::GetCursorPosX() < ValueColumnX())
                ImGui::SetCursorPosX(ValueColumnX());

            // 右端に操作ボタンぶんの余白を確保してから値を描く。参照スロットのように
            // 自分で残り幅を測るウィジェットでも ▲▼✕ が押し出されない。
            const widgets::RightReserveScope reserve = widgets::BeginRightReserve(toolbarWidth);
            ImGui::SetNextItemWidth(-FLT_MIN);
            m_changed |= drawValue(values[static_cast<size_t>(index)]);
            widgets::EndRightReserve(reserve);

            const widgets::ListRowButtons buttons =
                widgets::ListRowToolbar(index, count, removable);
            if (buttons.moveUp)   { moveFrom = index; moveTo = index - 1; }
            if (buttons.moveDown) { moveFrom = index; moveTo = index + 1; }
            if (buttons.remove)   removeIndex = index;

            widgets::EndPropertyRow(row);
            ImGui::PopID();
        }

        // 走査中に配列を触るとイテレータが壊れるため、ここでまとめて適用する。
        if (removeIndex >= 0) {
            values.erase(values.begin() + removeIndex);
            m_changed = true;
        } else if (ApplyListMove(values, moveFrom, moveTo)) {
            m_changed = true;
        }
    }

    template<typename T, typename DrawValue>
    void DrawReorderableList(const char* name,
                             std::vector<T>& values,
                             DrawValue&& drawValue)
    {
        const bool canEdit  = FieldEnabled();
        const bool editable = !FixedList();

        if (DrawListHeaderRow(name, values.size(), editable)) {
            values.emplace_back();
            m_changed = true;
        }
        if (!canEdit || !m_groupOpen || !FieldVisible()) return;

        ImGui::PushID(PersistentKey(name));
        DrawListRows(values, editable, std::forward<DrawValue>(drawValue));
        ImGui::PopID();
    }

    void ListField(const char* name, std::vector<float>& values) override
    {
        DrawReorderableList(name, values, [](float& value) {
            return ImGui::DragFloat("##value", &value, widgets::AdaptiveDragSpeed(value));
        });
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        DrawReorderableList(name, values, [](int& value) {
            return ImGui::DragInt("##value", &value);
        });
    }
    // std::vector<bool> は operator[] がプロキシを返すため bool& を取れない。
    // 値ウィジェット側を auto&& で受け、プロキシ経由で書き戻す。
    // WHY 共通経路に寄せるか: 以前はここだけ独自ループで、並び替えボタンが無く
    //     (▲▼ が出ない)、行の地色も付かない別物の見た目になっていた。
    void ListField(const char* name, std::vector<bool>& values) override
    {
        DrawReorderableList(name, values, [](auto&& slot) {
            bool value = slot;
            if (!ImGui::Checkbox("##value", &value)) return false;
            slot = value;
            return true;
        });
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        DrawReorderableList(name, values, [](std::string& value) {
            char buffer[512] = {};
            std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
            if (!ImGui::InputText("##value", buffer, sizeof(buffer))) return false;
            value = buffer;
            return true;
        });
    }
    // リスト要素のベクトルも単体フィールドと同じ軸色付き入力に揃える。
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        DrawReorderableList(name, values, [](math::Vector2& value) {
            float data[2] = { value.x, value.y };
            if (!widgets::DragAxes("##value", data, 2, 0.0f)) return false;
            value = { data[0], data[1] };
            return true;
        });
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        DrawReorderableList(name, values, [](math::Vector3& value) {
            float data[3] = { value.x, value.y, value.z };
            if (!widgets::DragAxes("##value", data, 3, 0.0f)) return false;
            value = { data[0], data[1], data[2] };
            return true;
        });
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        DrawReorderableList(name, values, [](math::Vector4& value) {
            float data[4] = { value.x, value.y, value.z, value.w };
            if (!widgets::DragAxes("##value", data, 4, 0.0f)) return false;
            value = { data[0], data[1], data[2], data[3] };
            return true;
        });
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        DrawReorderableList(name, values, [this](scene::EntityRef& value) {
            const scene::EntityID before = value.id;
            DrawObjectRefSlot(value.id, "");
            return value.id != before;
        });
    }
    // 型付き参照リスト。各スロットへ typeName を渡し、型不一致のドロップを拒否する。
    void RefListField(const char* name,
                      std::vector<scene::EntityRef>& values,
                      const char* typeName) override
    {
        DrawReorderableList(name, values, [this, typeName](scene::EntityRef& value) {
            const scene::EntityID before = value.id;
            DrawObjectRefSlot(value.id, typeName ? typeName : "");
            return value.id != before;
        });
    }
    void AssetListField(const char* name,
                        std::vector<scene::ScriptAssetReference>& values,
                        scene::ScriptAssetType type) override
    {
        const char* extensions = ExtensionsForAssetType(type);
        DrawReorderableList(name, values,
            [this, extensions](scene::ScriptAssetReference& value) {
                if (value.path.empty() && !value.guid.empty())
                    value.path = value.ResolvePath();
                const std::string before = value.path;
                if (!widgets::AssetPathField("##value", value.path, extensions, m_projectRoot))
                    return false;
                if (value.path != before)
                    value.SetPath(value.path);
                return value.path != before;
            });
    }
    // ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。
    // WHY override を消したか: 折りたたみ表示のロジックが BeginObject と重複するため。

    // ── 入れ子オブジェクト ───────────────────────────────────────────────────
    // WHY 閉じている間も m_groupOpen で「描画しない」状態にするだけで、
    //     呼び出し側の Field() 呼び出し自体は止められない点に注意。
    //     BeginRow() が m_groupOpen を見て false を返すことで実際の描画が省かれる。
    void BeginObject(const char* name) override
    {
        ObjectScope scope{};
        scope.previousGroupOpen = m_groupOpen;
        scope.visible = m_groupOpen && FieldVisible();

        if (scope.visible) {
            ImGui::PushID(PersistentKey(name));
            scope.open = SubHeader(HumanizeName(name).c_str());
            if (scope.open) {
                ImGui::Indent();
                scope.disabled = !FieldEnabled();
                if (scope.disabled) ImGui::BeginDisabled();
            }
        }

        m_groupOpen = scope.visible && scope.open;
        m_objectScopes.push_back(scope);
    }

    void EndObject() override
    {
        if (m_objectScopes.empty()) return;
        const ObjectScope scope = m_objectScopes.back();
        m_objectScopes.pop_back();

        if (scope.visible) {
            if (scope.open) {
                if (scope.disabled) ImGui::EndDisabled();
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        m_groupOpen = scope.previousGroupOpen;
    }

    // ── 構造体配列 ───────────────────────────────────────────────────────────
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        ListScope scope{};
        scope.previousGroupOpen = m_groupOpen;
        scope.visible  = m_groupOpen && FieldVisible();
        scope.editable = FieldEnabled() && !FixedList();
        scope.count    = count;

        if (scope.visible) {
            ImGui::PushID(PersistentKey(name));
            scope.open = SubHeader(HumanizeName(name).c_str());
            if (scope.open) {
                ImGui::Indent();
                // 件数表示 + 追加ボタンは単純配列の見出し行と同じ部品を使う。
                if (widgets::ListAddButton(count, scope.editable)) {
                    // 要素の追加は戻り値で伝える。呼び出し側が resize する。
                    scope.count = count + 1;
                    m_changed = true;
                }
            }
        }

        m_groupOpen = scope.visible && scope.open;
        m_listScopes.push_back(scope);
        return scope.count;
    }

    void BeginObjectElement(std::size_t index) override
    {
        ObjectScope scope{};
        scope.previousGroupOpen = m_groupOpen;
        scope.visible = m_groupOpen;

        if (scope.visible) {
            ImGui::PushID(static_cast<int>(index));
            char label[32];
            std::snprintf(label, sizeof(label), "[%zu]", index);
            scope.open = SubHeader(label);

            // ▲▼✕ は折りたたみ状態に関わらず出す。
            // WHY: 閉じた要素を消したい / 並べ替えたい場合に、わざわざ開かせるのは不便。
            // WHY 単純配列と同じ部品を使うか: 以前ここだけ ✕ しか無く、順序が意味を持つ
            //     配列 (ウェーブ・コンボ段・ウェイポイント) を作り直すしかなかった。
            if (!m_listScopes.empty() && m_listScopes.back().editable) {
                ListScope& list = m_listScopes.back();
                // 見出し行の右端へ寄せる。SubHeader は全幅を占めるため、SameLine 任せだと
                // ボタンが行の外へ出る。直前アイテム (= 見出し) の右端から幅ぶん戻して置く。
                const float toolbarWidth = widgets::ListRowToolbarWidth(true);
                const float headerRight = ImGui::GetItemRectMax().x;
                ImGui::SameLine();
                ImGui::SetCursorScreenPos({ headerRight - toolbarWidth,
                                            ImGui::GetCursorScreenPos().y });

                const widgets::ListRowButtons buttons = widgets::ListRowToolbar(
                    static_cast<int>(index), static_cast<int>(list.count),
                    /*removable=*/true, /*sameLine=*/false);
                if (buttons.moveUp)   { list.moveFrom = index; list.moveTo = index - 1; }
                if (buttons.moveDown) { list.moveFrom = index; list.moveTo = index + 1; }
                if (buttons.remove)   list.removeIndex = index;
                if (buttons.moveUp || buttons.moveDown || buttons.remove)
                    m_changed = true;
            }
            if (scope.open) ImGui::Indent();
        }

        m_groupOpen = scope.visible && scope.open;
        m_objectScopes.push_back(scope);
    }

    void EndObjectElement() override
    {
        if (m_objectScopes.empty()) return;
        const ObjectScope scope = m_objectScopes.back();
        m_objectScopes.pop_back();

        if (scope.visible) {
            if (scope.open) ImGui::Unindent();
            ImGui::PopID();
        }
        m_groupOpen = scope.previousGroupOpen;
    }

    std::size_t EndObjectList() override
    {
        if (m_listScopes.empty()) return NO_REMOVE;
        const ListScope scope = m_listScopes.back();
        m_listScopes.pop_back();

        if (scope.visible) {
            if (scope.open) ImGui::Unindent();
            ImGui::PopID();
        }
        m_groupOpen = scope.previousGroupOpen;

        // 並び替え要求は直後の ObjectListMove で 1 回だけ引き取らせる。
        // 削除が同時に起きたフレームでは並び替えを捨てる: 呼び出し側は先に erase するため、
        // 残した index はもう別の要素を指している。
        m_pendingMove = {};
        if (scope.removeIndex == NO_REMOVE && scope.moveFrom != NO_REMOVE) {
            m_pendingMove.valid = true;
            m_pendingMove.from  = scope.moveFrom;
            m_pendingMove.to    = scope.moveTo;
        }
        return scope.removeIndex;
    }

    bool ObjectListMove(std::size_t& from, std::size_t& to) override
    {
        if (!m_pendingMove.valid) return false;
        from = m_pendingMove.from;
        to   = m_pendingMove.to;
        m_pendingMove = {};
        return true;
    }
    void ReferenceField(const char* name, scene::ScriptSerializedReference& value) override
    {
        const bool canEditChildren = FieldEnabled();
        if (!BeginRow(name)) return;
        const char* current = value.type.empty() ? "(None)" : value.type.c_str();
        if (ImGui::BeginCombo("##type", current)) {
            if (ImGui::Selectable("(None)", value.type.empty())) {
                value.Clear();
                m_changed = true;
            }
            for (const std::string& typeName :
                 scene::ScriptSerializableFactory::RegisteredTypeNames()) {
                const bool selected = typeName == value.type;
                if (ImGui::Selectable(typeName.c_str(), selected) &&
                    value.SetType(typeName)) {
                    m_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        EndRow();
        if (value.value) {
            ImGui::PushID(PersistentKey(name));
            ImGui::Indent();
            if (!canEditChildren) ImGui::BeginDisabled();
            value.value->Reflect(*this);
            if (!canEditChildren) ImGui::EndDisabled();
            ImGui::Unindent();
            ImGui::PopID();
        } else if (!value.type.empty()) {
            ImGui::TextDisabled("Missing serializable type: %s", value.type.c_str());
        }
    }

    void Space(float height) override
    {
        if (m_groupOpen)
            ImGui::Dummy({ 0.0f, (std::max)(0.0f, height) });
    }

    // 編集できない計算値 (FBZZ_COMPUTED / r.Readonly)。
    //
    // WHY override が要るか: IReflector 側の既定は no-op で、ここで受けなければ
    //     宣言されたフィールドが Inspector に「一切出ない」。UIButton の state や
    //     CameraRig の elapsed のように、出ている前提で書かれた項目が黙って消えていた。
    // 値カラムへ淡色テキストで置く。入力ウィジェットを無効化して見せる手もあるが、
    // 枠があると「押せば編集できそう」に見えるので、テキストのまま出す。
    void Readonly(const char* name, const std::string& v) override
    {
        if (!BeginRow(name)) return;
        ImGui::TextDisabled("%s", v.c_str());
        EndRow();
    }
    void Readonly(const char* name, float v) override
    {
        if (!BeginRow(name)) return;
        ImGui::TextDisabled("%.3f", v);
        EndRow();
    }
    void Readonly(const char* name, int v) override
    {
        if (!BeginRow(name)) return;
        ImGui::TextDisabled("%d", v);
        EndRow();
    }

    // カーブ / グラデーションは 1 行に収まらないキャンバス型のウィジェットで、
    // ラベルと補間モードの選択を自前で描く。プロパティ行には乗せず、そのまま流す。
    void Field(const char* name, scene::ParticleCurve& v) override
    {
        if (!m_groupOpen || !FieldVisible()) return;
        const bool disabled = !FieldEnabled();
        if (disabled) ImGui::BeginDisabled();
        m_changed |= widgets::CurveEditor(HumanizeName(name).c_str(), v,
                                          HasFieldMax() ? FieldMax() : 1.0f);
        if (disabled) ImGui::EndDisabled();
    }

    void Field(const char* name, scene::ParticleGradient& v) override
    {
        if (!m_groupOpen || !FieldVisible()) return;
        const bool disabled = !FieldEnabled();
        if (disabled) ImGui::BeginDisabled();
        m_changed |= widgets::GradientEditor(HumanizeName(name).c_str(), v);
        if (disabled) ImGui::EndDisabled();
    }

    // アクションボタン (FBZZ_BUTTON)。ラベル列は空けて、値カラムの幅いっぱいに置く。
    // WHY 押下で m_changed を立てるか: 呼ばれる関数はほぼ必ずスクリプトの状態を書き換える。
    //     立てないと Undo スナップショットが取られず、シーンも未保存扱いにならない。
    void Button(const char* label, ActionCallback action, void* userData) override
    {
        if (!m_groupOpen || !FieldVisible() || !action) return;

        ImGui::PushID(PersistentKey(label));
        const widgets::PropertyRowScope row = widgets::BeginPropertyRow();
        const bool disabled = !FieldEnabled();
        if (disabled) ImGui::BeginDisabled();

        ImGui::SetCursorPosX(ValueColumnX());
        if (ImGui::Button(label, { -FLT_MIN, 0.0f })) {
            action(userData);
            m_changed = true;
        }

        if (disabled) ImGui::EndDisabled();
        widgets::EndPropertyRow(row);
        ImGui::PopID();
    }

    // ── ヒント付きフィールド ─────────────────────────────────────────────────

    void FloatRange(const char* name, float& v, float min, float max) override
    {
        if (!BeginRow(name)) return;
        // ゲージ (スライダー) + 数値入力ボックス。"##" でラベルを隠し、左カラムの名前だけ使う。
        m_changed |= widgets::RangeField("##v", v, min, max);
        EndRow();
    }

    void IntRange(const char* name, int& v, int min, int max) override
    {
        if (!BeginRow(name)) return;
        // float 版と同じ「塗り付きゲージ + 数値ボックス」にそろえる。
        // WHY: 以前ここだけ素の SliderInt で、同じカードの中に見た目の違う
        //      レンジ入力が 2 種類並んでいた。
        m_changed |= widgets::RangeField("##v", v, min, max);
        EndRow();
    }

    // 直前のフィールドへ説明ツールチップを付ける (FBZZ_TOOLTIP)。
    // 直前に描画したアイテム (= 対象フィールド) にホバー中だけ表示する。
    void Tooltip(const char* text) override
    {
        if (!m_groupOpen) return;
        if (text && text[0] && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", text);
    }

    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        if (labels.empty() || !BeginRow(name)) return;
        const char* current = (v >= 0 && v < (int)labels.size()) ? labels[v] : "??";
        if (ImGui::BeginCombo("##v", current)) {
            for (int i = 0; i < (int)labels.size(); ++i) {
                const bool selected = (v == i);
                if (ImGui::Selectable(labels[i], selected)) {
                    v = i;
                    m_changed = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        EndRow();
    }

    // 複数選択コンボの表示文字列を作る。
    // WHY: 従来はプレビューが "Flags" / "Layers" の固定文字で、何が選ばれているかを
    //   知るには毎回コンボを開くしかなかった。閉じたままでも中身が読めるようにする。
    //   全部並べると欄からあふれるので、2 件を超えたら件数表示へ畳む。
    static std::string BitMaskPreview(int value, std::span<const char* const> labels)
    {
        int selected = 0;
        std::string joined;
        for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
            if ((value & (1 << index)) == 0) continue;
            ++selected;
            if (selected <= 2) {
                if (!joined.empty()) joined += ", ";
                joined += labels[static_cast<size_t>(index)];
            }
        }
        if (selected == 0) return "(None)";
        if (selected > 2)  return std::to_string(selected) + " selected";
        return joined;
    }

    void Flags(const char* name, int& v, std::span<const char* const> labels) override
    {
        if (labels.empty() || !BeginRow(name)) return;
        if (ImGui::BeginCombo("##v", BitMaskPreview(v, labels).c_str())) {
            for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
                const int bit = 1 << index;
                bool selected = (v & bit) != 0;
                if (ImGui::Checkbox(labels[static_cast<size_t>(index)], &selected)) {
                    if (selected) v |= bit;
                    else v &= ~bit;
                    m_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        EndRow();
    }

    // Inspector のグループ見出し。FBZZ_GROUP("Label") から r_.Group() 経由で呼ばれる。
    // WHY: IReflector::Group の既定実装は no-op のため、ここで override しないと
    //      FBZZ_GROUP の見出しが Inspector に一切描画されない (旧 Header() は未接続のままだった)。
    //
    // 見た目は「▾ ラベル ───────」の 1 行だけ。
    // WHY CollapsingHeader をやめたか: あれは枠付き (Framed) の TreeNode で、行高が
    //     FramePadding 2 つぶん増えるうえ全幅の塗りバーを持つ。コンポーネントカードの
    //     ヘッダーと同じ重さに見えるので、グループを 3〜4 個持つスクリプトでは
    //     見出しと区切り線だけで Inspector が埋まり、肝心の値が読み取りづらかった。
    //     枠なし TreeNode + ラベル右へ伸ばす罫線に落とすと、行高はテキスト 1 行ぶんで済み、
    //     罫線が区切り線を兼ねるので Separator も要らなくなる。
    // 折りたたみ状態は ImGui が ID (ラベル) ごとに記憶する。閉じている間は後続フィールドを
    // m_groupOpen で描画スキップする。
    void Group(const char* label) override
    {
        // 入れ子オブジェクトが閉じているときは、その中の見出しも出さない。
        if (InsideCollapsedObject()) {
            m_groupOpen = false;
            return;
        }

        // 上だけ少し空ける。下は詰めて、見出しと配下のフィールドが 1 かたまりに見えるようにする。
        ImGui::Dummy({ 0.0f, ImGui::GetStyle().ItemSpacing.y });

        ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Color(ThemeColor::SurfaceHover));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  EditorTheme::Color(ThemeColor::SurfaceHover));
        ImGui::PushStyleColor(ImGuiCol_Text,          EditorTheme::Color(ThemeColor::TextMuted));
        m_groupOpen = ImGui::TreeNodeEx(label,
                                        ImGuiTreeNodeFlags_DefaultOpen |
                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                        ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor(4);

        // ラベルの右端から行末まで細い罫線を引く。
        // SpanAvailWidth の行矩形は全幅なので、ラベル終端は「矢印ぶんの字下げ + 文字幅」で出す。
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const float  ruleY  = (rowMin.y + rowMax.y) * 0.5f;
        const float  ruleX  = rowMin.x + ImGui::GetTreeNodeToLabelSpacing()
                            + ImGui::CalcTextSize(label).x
                            + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f;
        if (ruleX < rowMax.x) {
            ImGui::GetWindowDrawList()->AddLine({ ruleX, ruleY }, { rowMax.x, ruleY },
                                                EditorTheme::ColorU32(ThemeColor::Border));
        }
    }

    void Field(const char* name, input::KeyCode& v) override
    {
        if (!BeginRow(name)) return;
        int raw = static_cast<int>(v);
        KeyCodeField(raw);
        v = static_cast<input::KeyCode>(raw);
        EndRow();
    }

    // BeginRow 済みの値カラムにキー設定ボタンを描く (ラベルは左カラムに描画済み)。
    void KeyCodeField(int& v)
    {
        static int* s_listeningPtr = nullptr;
        const bool isListening = (s_listeningPtr == &v);

        if (isListening) {
            ImGui::Button("Press any key...", { -1.0f, 0.0f });
            // Escape でキャンセル
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                s_listeningPtr = nullptr;
            } else {
                // ImGuiKey → Win32 VK の主要対応表でスキャン
                static constexpr struct { ImGuiKey imk; int vk; } kMap[] = {
                    {ImGuiKey_A,'A'},{ImGuiKey_B,'B'},{ImGuiKey_C,'C'},{ImGuiKey_D,'D'},
                    {ImGuiKey_E,'E'},{ImGuiKey_F,'F'},{ImGuiKey_G,'G'},{ImGuiKey_H,'H'},
                    {ImGuiKey_I,'I'},{ImGuiKey_J,'J'},{ImGuiKey_K,'K'},{ImGuiKey_L,'L'},
                    {ImGuiKey_M,'M'},{ImGuiKey_N,'N'},{ImGuiKey_O,'O'},{ImGuiKey_P,'P'},
                    {ImGuiKey_Q,'Q'},{ImGuiKey_R,'R'},{ImGuiKey_S,'S'},{ImGuiKey_T,'T'},
                    {ImGuiKey_U,'U'},{ImGuiKey_V,'V'},{ImGuiKey_W,'W'},{ImGuiKey_X,'X'},
                    {ImGuiKey_Y,'Y'},{ImGuiKey_Z,'Z'},
                    {ImGuiKey_0,'0'},{ImGuiKey_1,'1'},{ImGuiKey_2,'2'},{ImGuiKey_3,'3'},
                    {ImGuiKey_4,'4'},{ImGuiKey_5,'5'},{ImGuiKey_6,'6'},{ImGuiKey_7,'7'},
                    {ImGuiKey_8,'8'},{ImGuiKey_9,'9'},
                    {ImGuiKey_Space,   0x20}, // VK_SPACE
                    {ImGuiKey_Enter,   0x0D}, // VK_RETURN
                    {ImGuiKey_Backspace,0x08},// VK_BACK
                    {ImGuiKey_LeftShift,0x10},{ImGuiKey_RightShift,0x10},   // VK_SHIFT
                    {ImGuiKey_LeftCtrl, 0x11},{ImGuiKey_RightCtrl, 0x11},   // VK_CONTROL
                    {ImGuiKey_LeftAlt,  0x12},{ImGuiKey_RightAlt,  0x12},   // VK_MENU
                    {ImGuiKey_LeftArrow,0x25},{ImGuiKey_RightArrow,0x27},
                    {ImGuiKey_UpArrow,  0x26},{ImGuiKey_DownArrow, 0x28},
                    {ImGuiKey_F1,0x70},{ImGuiKey_F2,0x71},{ImGuiKey_F3,0x72},{ImGuiKey_F4,0x73},
                    {ImGuiKey_F5,0x74},{ImGuiKey_F6,0x75},{ImGuiKey_F7,0x76},{ImGuiKey_F8,0x77},
                    {ImGuiKey_F9,0x78},{ImGuiKey_F10,0x79},{ImGuiKey_F11,0x7A},{ImGuiKey_F12,0x7B},
                };
                for (auto& e : kMap) {
                    if (ImGui::IsKeyPressed(e.imk, false)) {
                        v = e.vk;
                        s_listeningPtr = nullptr;
                        break;
                    }
                }
            }
        } else {
            const char* keyName = KeyCodeName(v);
            char label[64];
            std::snprintf(label, sizeof(label), "[%s]", keyName);
            if (ImGui::Button(label, { -1.0f, 0.0f }))
                s_listeningPtr = &v;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("クリックでキー入力待ち (現在: %s / VK %d)", keyName, v);
        }
    }

    void FieldWithTooltip(const char* name, float& v, const char* tooltip)
    {
        ImGui::DragFloat(name, &v, 0.1f);
        if (ImGui::IsItemHovered() && tooltip && tooltip[0])
            ImGui::SetTooltip("%s", tooltip);
    }

    void Label(const char* name, const std::string& v)
    {
        ImGui::LabelText(name, "%s", v.c_str());
    }

    void Label(const char* name, float v)
    {
        ImGui::LabelText(name, "%.3f", v);
    }

    void Label(const char* name, int v)
    {
        ImGui::LabelText(name, "%d", v);
    }

    void Separator()
    {
        ImGui::Separator();
    }

private:
    // BeginObject / BeginObjectElement が積む描画スコープ。
    // ImGui の PushID / Indent / BeginDisabled を対で閉じるために、
    // 「開始時に何をしたか」を記録して EndObject で正確に巻き戻す。
    struct ObjectScope {
        bool previousGroupOpen = true;
        bool visible  = false;  // PushID したか
        bool open     = false;  // CollapsingHeader が開いているか (Indent したか)
        bool disabled = false;  // BeginDisabled したか
    };

    struct ListScope {
        bool        previousGroupOpen = true;
        bool        visible     = false;
        bool        open        = false;
        bool        editable    = false;  // + / x ボタンを出すか
        std::size_t count       = 0;
        std::size_t removeIndex = scene::IReflector::NO_REMOVE;
        std::size_t moveFrom    = scene::IReflector::NO_REMOVE;
        std::size_t moveTo      = scene::IReflector::NO_REMOVE;
    };

    // EndObjectList で確定し、直後の ObjectListMove が引き取るまでの一時置き場。
    // 入れ子の配列でも、EndObjectList → ObjectListMove は必ず隣り合って呼ばれるため 1 枠で足りる。
    struct PendingMove {
        bool        valid = false;
        std::size_t from  = 0;
        std::size_t to    = 0;
    };

    std::vector<ObjectScope> m_objectScopes;
    std::vector<ListScope>   m_listScopes;
    PendingMove              m_pendingMove;

    // 今いる位置が「閉じた入れ子オブジェクトの中」か。
    // WHY: m_groupOpen だけでは、同じオブジェクト内の前のグループが閉じているのか、
    //      オブジェクトごと閉じているのかを区別できない。前者では次のグループ見出しを
    //      描かなければならず、後者では描いてはいけない。
    [[nodiscard]] bool InsideCollapsedObject() const
    {
        if (m_objectScopes.empty()) return false;
        const ObjectScope& scope = m_objectScopes.back();
        return !scope.visible || !scope.open;
    }

    // Win32 VK コード → 表示名
    static const char* KeyCodeName(int vk)
    {
        switch (vk) {
        case 'A': return "A"; case 'B': return "B"; case 'C': return "C";
        case 'D': return "D"; case 'E': return "E"; case 'F': return "F";
        case 'G': return "G"; case 'H': return "H"; case 'I': return "I";
        case 'J': return "J"; case 'K': return "K"; case 'L': return "L";
        case 'M': return "M"; case 'N': return "N"; case 'O': return "O";
        case 'P': return "P"; case 'Q': return "Q"; case 'R': return "R";
        case 'S': return "S"; case 'T': return "T"; case 'U': return "U";
        case 'V': return "V"; case 'W': return "W"; case 'X': return "X";
        case 'Y': return "Y"; case 'Z': return "Z";
        case '0': return "0"; case '1': return "1"; case '2': return "2";
        case '3': return "3"; case '4': return "4"; case '5': return "5";
        case '6': return "6"; case '7': return "7"; case '8': return "8";
        case '9': return "9";
        case 0x20: return "Space";
        case 0x0D: return "Enter";
        case 0x08: return "Backspace";
        case 0x1B: return "Escape";
        case 0x10: return "Shift";
        case 0x11: return "Ctrl";
        case 0x12: return "Alt";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x70: return "F1";  case 0x71: return "F2";  case 0x72: return "F3";
        case 0x73: return "F4";  case 0x74: return "F5";  case 0x75: return "F6";
        case 0x76: return "F7";  case 0x77: return "F8";  case 0x78: return "F9";
        case 0x79: return "F10"; case 0x7A: return "F11"; case 0x7B: return "F12";
        default:   return "?";
        }
    }
};

} // namespace fbzz::editor
