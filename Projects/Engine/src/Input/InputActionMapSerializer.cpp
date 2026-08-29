/// @file    InputActionMapSerializer.cpp
/// @brief   .inputactions (TOML) の読み書き。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY TOML か:
/// シーン (.scene) / プロジェクト設定と同じ toml++ を使い、依存を増やさない。
/// バイナリではなくテキストにすることで、Git 上でキーコンフィグの差分が読める。
///
/// WHY ProjectSettings/ に置くか:
/// 入力バインドはエディタ設定ではなくゲーム設定であり、
/// BuildPipeline の CopyProjectFiles ステップで配布物に含める必要がある。
/// Assets/EditorConfig/ に置くと配布物から漏れる。
#include "Engine/Input/InputActionMap.hpp"
#include "Engine/Core/Logger.hpp"

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fbzz::input {

namespace {

const char* SourceToString(BindingSource source)
{
    switch (source) {
    case BindingSource::KEY:            return "Key";
    case BindingSource::MOUSE_BUTTON:   return "MouseButton";
    case BindingSource::GAMEPAD_BUTTON: return "GamepadButton";
    case BindingSource::GAMEPAD_AXIS:   return "GamepadAxis";
    case BindingSource::MOUSE_AXIS:     return "MouseAxis";
    default:                            return "Key";
    }
}

// 未知の source 名は Key として扱わず、明示的に失敗させる。
// WHY: 綴り間違いを黙って Key(0) に落とすと「なぜか反応しないバインド」になり、
//      原因究明が極めて困難になる。読み込み時に警告を出して該当バインドを捨てる。
bool ParseSource(std::string_view text, BindingSource& out)
{
    if (text == "Key")            { out = BindingSource::KEY;            return true; }
    if (text == "MouseButton")    { out = BindingSource::MOUSE_BUTTON;   return true; }
    if (text == "GamepadButton")  { out = BindingSource::GAMEPAD_BUTTON; return true; }
    if (text == "GamepadAxis")    { out = BindingSource::GAMEPAD_AXIS;   return true; }
    if (text == "MouseAxis")      { out = BindingSource::MOUSE_AXIS;     return true; }
    return false;
}

toml::table BindingToTable(const InputBinding& binding)
{
    toml::table table;
    table.insert("source", SourceToString(binding.source));

    // ゲームパッドは列挙名で保存する。
    // WHY: 数値だと enum への要素追加で既存ファイルの意味が丸ごとずれる。
    //      キーコードは Win32 の仮想キーコードで値が固定されているため数値のままでよい。
    if (binding.source == BindingSource::GAMEPAD_BUTTON) {
        table.insert("button", ToString(static_cast<GamepadButton>(binding.code)));
    } else if (binding.source == BindingSource::GAMEPAD_AXIS) {
        table.insert("axis", ToString(static_cast<GamepadAxis>(binding.code)));
    } else {
        table.insert("code", static_cast<int64_t>(binding.code));
    }

    // 既定値と同じフィールドは書き出さない。差分の読みやすさを優先する。
    if (binding.scale != 1.0f)           table.insert("scale", static_cast<double>(binding.scale));
    if (binding.padIndex != -1)          table.insert("pad", static_cast<int64_t>(binding.padIndex));
    if (binding.buttonThreshold != 0.5f)
        table.insert("threshold", static_cast<double>(binding.buttonThreshold));

    return table;
}

bool TableToBinding(const toml::table& table, InputBinding& out)
{
    const auto sourceText = table["source"].value<std::string>();
    if (!sourceText) return false;

    InputBinding binding{};
    if (!ParseSource(*sourceText, binding.source)) {
        FBZZ_LOG_WARN("InputActionMap: 未知の binding source \"%s\" — このバインドを無視します",
                      sourceText->c_str());
        return false;
    }

    if (binding.source == BindingSource::GAMEPAD_BUTTON) {
        const auto name = table["button"].value<std::string>();
        GamepadButton button{};
        if (!name || !ParseGamepadButton(name->c_str(), button)) {
            FBZZ_LOG_WARN("InputActionMap: 未知の GamepadButton — このバインドを無視します");
            return false;
        }
        binding.code = static_cast<uint32_t>(button);
    } else if (binding.source == BindingSource::GAMEPAD_AXIS) {
        const auto name = table["axis"].value<std::string>();
        GamepadAxis axis{};
        if (!name || !ParseGamepadAxis(name->c_str(), axis)) {
            FBZZ_LOG_WARN("InputActionMap: 未知の GamepadAxis — このバインドを無視します");
            return false;
        }
        binding.code = static_cast<uint32_t>(axis);
    } else {
        binding.code = static_cast<uint32_t>(table["code"].value_or(int64_t{ 0 }));
    }

    binding.scale           = static_cast<float>(table["scale"].value_or(1.0));
    binding.padIndex        = static_cast<int>(table["pad"].value_or(int64_t{ -1 }));
    binding.buttonThreshold = static_cast<float>(table["threshold"].value_or(0.5));

    out = binding;
    return true;
}

toml::array BindingsToArray(const std::vector<InputBinding>& bindings)
{
    toml::array array;
    for (const InputBinding& binding : bindings) {
        array.push_back(BindingToTable(binding));
    }
    return array;
}

void ArrayToBindings(const toml::node* node, std::vector<InputBinding>& out)
{
    out.clear();
    const auto* array = node ? node->as_array() : nullptr;
    if (!array) return;

    for (const auto& element : *array) {
        const auto* table = element.as_table();
        if (!table) continue;

        InputBinding binding{};
        if (TableToBinding(*table, binding)) out.push_back(binding);
    }
}

} // namespace

bool InputActionMap::LoadFromFile(const std::string& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        FBZZ_LOG_INFO("InputActionMap: %s が見つかりません — 既定バインドを使用します",
                      path.c_str());
        return false;
    }

    toml::parse_result parsed = toml::parse_file(path);
    if (!parsed) {
        FBZZ_LOG_ERROR("InputActionMap: %s のパースに失敗しました: %s",
                       path.c_str(), std::string(parsed.error().description()).c_str());
        return false;
    }

    const toml::table& root = parsed.table();

    // 先にローカルへ組み立て、成功が確定してから差し替える。
    // WHY: パース途中で失敗した場合に現在のバインドが半端に壊れるのを防ぐ。
    std::vector<InputAxis>   loadedAxes;
    std::vector<InputAction> loadedActions;

    if (const auto* axisArray = root["axis"].as_array()) {
        for (const auto& element : *axisArray) {
            const auto* table = element.as_table();
            if (!table) continue;

            InputAxis axis{};
            axis.name = (*table)["name"].value_or(std::string{});
            if (axis.name.empty()) continue;

            axis.deadZone    = static_cast<float>((*table)["deadZone"].value_or(0.25));
            axis.gravity     = static_cast<float>((*table)["gravity"].value_or(8.0));
            axis.sensitivity = static_cast<float>((*table)["sensitivity"].value_or(8.0));
            axis.snap        = (*table)["snap"].value_or(true);
            axis.raw         = (*table)["raw"].value_or(false);

            ArrayToBindings((*table)["positive"].node(), axis.positive);
            ArrayToBindings((*table)["negative"].node(), axis.negative);
            ArrayToBindings((*table)["analog"].node(),   axis.analog);

            loadedAxes.push_back(std::move(axis));
        }
    }

    if (const auto* actionArray = root["action"].as_array()) {
        for (const auto& element : *actionArray) {
            const auto* table = element.as_table();
            if (!table) continue;

            InputAction action{};
            action.name = (*table)["name"].value_or(std::string{});
            if (action.name.empty()) continue;

            ArrayToBindings((*table)["bindings"].node(), action.bindings);
            loadedActions.push_back(std::move(action));
        }
    }

    Clear();
    for (const InputAxis& axis : loadedAxes)       AddAxis(axis);
    for (const InputAction& action : loadedActions) AddAction(action);

    FBZZ_LOG_INFO("InputActionMap: %s を読み込みました (axis=%zu action=%zu)",
                  path.c_str(), loadedAxes.size(), loadedActions.size());
    return true;
}

bool InputActionMap::SaveToFile(const std::string& path)
{
    toml::table root;

    toml::array axisArray;
    for (const InputAxis& axis : GetAxes()) {
        toml::table table;
        table.insert("name", axis.name);
        table.insert("deadZone",    static_cast<double>(axis.deadZone));
        table.insert("gravity",     static_cast<double>(axis.gravity));
        table.insert("sensitivity", static_cast<double>(axis.sensitivity));
        table.insert("snap", axis.snap);
        if (axis.raw) table.insert("raw", true);

        // 空の配列は書き出さない。既定バインドのファイルが読みやすくなる。
        if (!axis.positive.empty()) table.insert("positive", BindingsToArray(axis.positive));
        if (!axis.negative.empty()) table.insert("negative", BindingsToArray(axis.negative));
        if (!axis.analog.empty())   table.insert("analog",   BindingsToArray(axis.analog));

        axisArray.push_back(std::move(table));
    }
    root.insert("axis", std::move(axisArray));

    toml::array actionArray;
    for (const InputAction& action : GetActions()) {
        toml::table table;
        table.insert("name", action.name);
        table.insert("bindings", BindingsToArray(action.bindings));
        actionArray.push_back(std::move(table));
    }
    root.insert("action", std::move(actionArray));

    // 親ディレクトリが無い場合は作る (新規プロジェクトの初回保存)。
    std::error_code error;
    const std::filesystem::path filePath(path);
    if (filePath.has_parent_path()) {
        std::filesystem::create_directories(filePath.parent_path(), error);
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        FBZZ_LOG_ERROR("InputActionMap: %s を書き込めません", path.c_str());
        return false;
    }

    stream << "# FBZZ Engine — Input Action Map\n"
              "# ゲーム入力のバインド定義。エディタの Project Settings > Input から編集できる。\n\n";
    stream << root;
    stream << '\n';

    return stream.good();
}

} // namespace fbzz::input
