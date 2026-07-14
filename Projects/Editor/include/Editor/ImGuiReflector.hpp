// FBZZ Engine
// ImGuiReflector.hpp | fbzz::editor
// ImGui implementation of scene script reflection fields
#pragma once

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
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

    // アセットスロットの "..." パス検索ピッカー用。プロジェクトルート (絶対パス)。
    // InspectorCore から ctx.projectRoot を渡す。空なら検索ボタンを出さない。
    std::string m_projectRoot;

    // FBZZ_GROUP の折りたたみ状態。Group() が CollapsingHeader の開閉で更新し、
    // 各 Field はこれが false (閉) の間は描画をスキップする。グループ前の項目は true (既定)。
    bool m_groupOpen = true;

    // シリアライズ用 lowerCamelCase キーを Inspector 用の読みやすい表示名へ変換する。
    // WHY: Reflect() のキーを表示名に流用しても、シーン互換性を壊すキー変更なしで
    //      "fontSize" を "Font Size" のような Editor 表示へ自動変換できる。
    static std::string HumanizeName(const char* name)
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
        if (std::strcmp(name, "texturePath") == 0) return ".fztex,.png,.dds";
        if (std::strcmp(name, "fontPath") == 0)    return ".png,.fnt";
        if (std::strcmp(name, "materialPath") == 0) return ".mat";
        if (std::strcmp(name, "meshPath") == 0 || std::strcmp(name, "modelPath") == 0)
            return ".fbx,.fzmodel";
        if (std::strcmp(name, "controllerPath") == 0) return ".animcontroller";
        if (std::strcmp(name, "clipPath") == 0) return ".wav,.ogg,.mp3";
        return nullptr;
    }

    // ── レイアウト補助 (ラベル左 + 値が右いっぱいの 2 カラム) ──────────────────
    // WHY: 全フィールドで値の左端をそろえると Unity ライクで整然と見える。
    //      従来は ImGui 既定のラベル右寄せ + 参照スロットは手書きで名前を右に追記しており、
    //      幅が溢れて切れていた。ここで「ラベル左・値右いっぱい」に統一して見た目と崩れを解消する。

    // 値ウィジェットの開始 X (ラベル列幅)。ウィンドウ幅に追従しつつ下限を持たせる。
    float ValueColumnX() const
    {
        return (std::max)(ImGui::GetFontSize() * 7.0f, ImGui::GetWindowWidth() * 0.40f);
    }

    // 1 フィールド行を開始する。ラベルを左に描き、続くウィジェットの左端をそろえる。
    // false ならグループ閉でスキップ。true のときは必ず EndRow() を呼ぶこと。
    bool BeginRow(const char* name)
    {
        if (!m_groupOpen) return false;
        ImGui::PushID(name);
        ImGui::AlignTextToFramePadding();
        const std::string displayName = HumanizeName(name);
        ImGui::TextUnformatted(displayName.c_str());
        ImGui::SameLine();
        const float col = ValueColumnX();
        if (ImGui::GetCursorPosX() < col)
            ImGui::SetCursorPosX(col);
        // 続く単一ウィジェットを右端まで広げる (複数ボタンのスロットは Button が無視するので無害)。
        ImGui::SetNextItemWidth(-FLT_MIN);
        return true;
    }
    void EndRow() { ImGui::PopID(); }

    // ── 基本型 ───────────────────────────────────────────────────────────────

    void Field(const char* name, float& v) override
    {
        if (!BeginRow(name)) return;
        ImGui::DragFloat("##v", &v, 0.1f);
        EndRow();
    }

    void Field(const char* name, int& v) override
    {
        if (!BeginRow(name)) return;
        ImGui::DragInt("##v", &v);
        EndRow();
    }

    void Field(const char* name, bool& v) override
    {
        if (!BeginRow(name)) return;
        ImGui::Checkbox("##v", &v);
        EndRow();
    }

    void Field(const char* name, math::Vector2& v) override
    {
        if (!BeginRow(name)) return;
        float arr[2] = { v.x, v.y };
        if (ImGui::DragFloat2("##v", arr, 0.1f))
            v = { arr[0], arr[1] };
        EndRow();
    }

    void Field(const char* name, math::Vector3& v) override
    {
        if (!BeginRow(name)) return;
        float arr[3] = { v.x, v.y, v.z };
        if (ImGui::DragFloat3("##v", arr, 0.1f))
            v = { arr[0], arr[1], arr[2] };
        EndRow();
    }

    void Field(const char* name, math::Vector4& v) override
    {
        if (!BeginRow(name)) return;
        float arr[4] = { v.x, v.y, v.z, v.w };
        if (ImGui::ColorEdit4("##v", arr))
            v = { arr[0], arr[1], arr[2], arr[3] };
        EndRow();
    }

    void Field(const char* name, std::string& v) override
    {
        if (!BeginRow(name)) return;
        if (const char* filter = AssetFilterFor(name)) {
            if (widgets::AssetPathField("##v", v, filter, m_projectRoot)
                && std::strcmp(name, "fontPath") == 0) {
                std::filesystem::path path = util::FileSystem::PathFromUtf8(v);
                path.replace_extension();
                v = util::FileSystem::PathToUtf8(path);
            }
        } else {
            std::string buf = v;
            const size_t extraCapacity = std::strcmp(name, "text") == 0 ? 1024 : 128;
            buf.resize(buf.size() + extraCapacity);
            if (std::strcmp(name, "text") == 0) {
                if (ImGui::InputTextMultiline("##v", buf.data(), buf.size(),
                                              { -FLT_MIN, ImGui::GetTextLineHeight() * 3.0f }))
                    v = buf.data();
            } else if (ImGui::InputText("##v", buf.data(), buf.size())) {
                v = buf.data();
            }
        }
        EndRow();
    }

    void Field(const char* name, math::Quaternion& v) override
    {
        if (!BeginRow(name)) return;
        widgets::DragQuatEuler3("##v", v, 0.5f);
        EndRow();
    }

    // ── 参照型 ───────────────────────────────────────────────────────────────

    // 任意 GameObject 参照 (EntityID 単体)。型要件なしで DrawObjectRefSlot を呼ぶ。
    void Field(const char* name, scene::EntityID& v) override
    {
        if (!BeginRow(name)) return;
        DrawObjectRefSlot(v, "");
        EndRow();
    }

    // 型付きオブジェクト参照 (FBZZ_REF)。typeName の Script を持つ GO のみ受理する。
    void RefField(const char* name, scene::EntityRef& v, const char* typeName) override
    {
        if (!BeginRow(name)) return;
        DrawObjectRefSlot(v.id, typeName);
        EndRow();
    }

    // オブジェクト参照スロット本体 (BeginRow 済みの値カラムに描く)。
    //   [参照ボタン(右いっぱい)] [◎ピッカー] [×クリア]
    //   typeName == ""  : 任意 GameObject 可
    //   typeName != ""  : その Script 型を持つ GO のみ受理 (ドロップ検証 + ピッカー絞り込み + 不一致を赤表示)
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

        // ◎/× は frame 高さの正方形でそろえる。本体ボタンは残り幅いっぱい。
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float smallW  = ImGui::GetFrameHeight();
        float btnW = ImGui::GetContentRegionAvail().x - (smallW + spacing) * 2.0f;
        if (btnW < 24.0f) btnW = 24.0f;

        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        const bool droppable = drag && drag->IsDataType("FBZZ_HIERARCHY_ENTITY");
        int pushedCol = 0;
        if (droppable) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            pushedCol = 1;
        } else if (!typeOk) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
            pushedCol = 1;
        }
        const std::string label = typeOk ? goName : (goName + "  (type?)");
        ImGui::Button(label.c_str(), { btnW, 0.0f });
        if (pushedCol)
            ImGui::PopStyleColor();
        if (typed && ImGui::IsItemHovered())
            ImGui::SetTooltip("Requires: %s", typeName);

        if (ImGui::BeginDragDropTarget()) {
            if (auto* payload = ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY")) {
                scene::EntityID dropped;
                std::memcpy(&dropped, payload->Data, sizeof(dropped));
                if (!typed || !m_refTypeValidator || m_refTypeValidator(dropped, typeName))
                    v = dropped;
            }
            ImGui::EndDragDropTarget();
        }

        // ◎ピッカー: ドロップせずとも一覧から検索して選べる D&D 代替。型付きなら候補も絞る。
        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("O", { smallW, 0.0f }))
            ImGui::OpenPopup("##gopick");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("一覧から選択");
        if (ImGui::BeginPopup("##gopick")) {
            static char filter[64] = "";
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##gofilter", "Search...", filter, sizeof(filter));
            ImGui::Separator();
            if (ImGui::Selectable("(None)"))
                v = scene::EntityID::INVALID;
            if (m_goListProvider) {
                for (const auto& [id, nm] : m_goListProvider()) {
                    if (filter[0] && nm.find(filter) == std::string::npos)
                        continue;
                    if (typed && m_refTypeValidator && !m_refTypeValidator(id, typeName))
                        continue;
                    ImGui::PushID(static_cast<int>(id.index));
                    if (ImGui::Selectable(nm.c_str()))
                        v = id;
                    ImGui::PopID();
                }
            }
            ImGui::EndPopup();
        }

        // ×ボタンで参照クリア
        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("x", { smallW, 0.0f }))
            v = scene::EntityID::INVALID;
    }

    void Field(const char* name, scene::PrefabRef& v) override
    {
        if (!BeginRow(name)) return;

        const auto  slash   = v.path.find_last_of("/\\");
        const char* display = v.path.empty() ? "(None)"
                            : (slash != std::string::npos ? v.path.c_str() + slash + 1
                                                          : v.path.c_str());

        const float spacing  = ImGui::GetStyle().ItemSpacing.x;
        const float smallW   = ImGui::GetFrameHeight();
        const bool  hasSearch = !m_projectRoot.empty();
        const int   nSmall   = hasSearch ? 2 : 1; // [...] と [×]
        float btnW = ImGui::GetContentRegionAvail().x - (smallW + spacing) * nSmall;
        if (btnW < 24.0f) btnW = 24.0f;

        const ImVec2 slotPos = ImGui::GetCursorScreenPos();
        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        const bool droppable = drag && drag->IsDataType("ASSET_PATH");
        if (droppable)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        ImGui::Button(display, { btnW, 0.0f });
        if (droppable)
            ImGui::PopStyleColor();

        if (ImGui::BeginDragDropTarget()) {
            if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                v.path.assign(static_cast<const char*>(payload->Data),
                              static_cast<size_t>(payload->DataSize - 1));
            }
            ImGui::EndDragDropTarget();
        }

        // "..." パス検索ピッカー (.prefab を projectRoot 以下から検索)。
        if (hasSearch) {
            ImGui::SameLine(0.0f, spacing);
            if (ImGui::Button("...", { smallW, 0.0f }))
                widgets::OpenAssetPicker(v.path, ".prefab", m_projectRoot,
                                         { slotPos.x, ImGui::GetItemRectMax().y + 2.0f });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse Prefabs...");
        }

        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("x", { smallW, 0.0f }))
            v.path.clear();

        EndRow();
    }

    // DataAsset (純共有 ScriptableObject) 参照スロット。
    // Asset ブラウザから .fzdata をドロップして割り当てる。.fzdata かつ期待型一致のみ受理する。
    void Field(const char* name, scene::DataAssetRef& v) override
    {
        if (!BeginRow(name)) return;

        const auto  slash   = v.path.find_last_of("/\\");
        const char* display = v.path.empty() ? "(None)"
                            : (slash != std::string::npos ? v.path.c_str() + slash + 1
                                                          : v.path.c_str());

        const float spacing  = ImGui::GetStyle().ItemSpacing.x;
        const float smallW   = ImGui::GetFrameHeight();
        const bool  hasSearch = !m_projectRoot.empty();
        const int   nSmall   = hasSearch ? 2 : 1; // [...] と [×]
        float btnW = ImGui::GetContentRegionAvail().x - (smallW + spacing) * nSmall;
        if (btnW < 24.0f) btnW = 24.0f;

        const ImVec2 slotPos = ImGui::GetCursorScreenPos();
        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        const bool droppable = drag && drag->IsDataType("ASSET_PATH");
        if (droppable)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        ImGui::Button(display, { btnW, 0.0f });
        if (droppable)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !v.type.empty())
            ImGui::SetTooltip("Data Asset: %s (.fzdata)", v.type.c_str());

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

        // "..." パス検索ピッカー (.fzdata を projectRoot 以下から検索)。
        if (hasSearch) {
            ImGui::SameLine(0.0f, spacing);
            if (ImGui::Button("...", { smallW, 0.0f }))
                widgets::OpenAssetPicker(v.path, ".fzdata", m_projectRoot,
                                         { slotPos.x, ImGui::GetItemRectMax().y + 2.0f });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse Data Assets...");
        }

        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("x", { smallW, 0.0f }))
            v.path.clear();

        EndRow();
    }

    // ── ヒント付きフィールド ─────────────────────────────────────────────────

    void FloatRange(const char* name, float& v, float min, float max) override
    {
        if (!BeginRow(name)) return;
        // ゲージ (スライダー) + 数値入力ボックス。"##" でラベルを隠し、左カラムの名前だけ使う。
        widgets::RangeField("##v", v, min, max);
        EndRow();
    }

    void IntRange(const char* name, int& v, int min, int max) override
    {
        if (!BeginRow(name)) return;
        ImGui::SliderInt("##v", &v, min, max);
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
                if (ImGui::Selectable(labels[i], selected))
                    v = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        EndRow();
    }

    // Inspector のグループ見出し。FBZZ_GROUP("Label") から r_.Group() 経由で呼ばれる。
    // WHY: IReflector::Group の既定実装は no-op のため、ここで override しないと
    //      FBZZ_GROUP の見出しが Inspector に一切描画されない (旧 Header() は未接続のままだった)。
    void Group(const char* label) override
    {
        ImGui::Spacing();
        // シンプルな見出し: CollapsingHeader の塗りつぶしバーを消し「▸ ラベル」+ 薄い区切り線だけにする。
        // WHY: 既定の CollapsingHeader は全幅の塗りバーで重い。Header 系の色を透過にして
        //      矢印とラベルのみを残し、ホバー時だけ淡く反応させることで軽い見た目にする。
        // 折りたたみは維持 (閉じている間は後続フィールドの描画を省く / m_groupOpen)。状態は ImGui が記憶する。
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        m_groupOpen = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);
        // 見出し直下に薄い区切り線を引き、塗りバー無しでもグループの開始が分かるようにする。
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::Separator();
        ImGui::PopStyleColor();
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
