/// @file    InputActionMapSerializer.cpp
/// @brief   .inputactions (TOML) の読み書き。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note TOML は .scene / ProjectSettings と共通で依存を増やさない。バインドは BuildPipeline の
/// @note CopyProjectFiles で配布物に含めるゲーム設定のため、Assets/EditorConfig/ ではなく
/// @note ProjectSettings/ に置く (EditorConfig は配布物から漏れる)。
#include "Engine/Input/InputActionMap.hpp"
#include <Engine/Input/InputBindingsDocument.hpp>
#include <sstream>
#include "Engine/Core/Logger.hpp"
#include "Engine/Util/FileSystem.hpp"

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

/// @brief 未知の source 名は Key として扱わず、明示的に失敗させる。
/// @note 綴り間違いを Key(0) に落とすと原因不明の無反応バインドになるため、読み込み時に警告を出し捨てる。
bool ParseSource(std::string_view text, BindingSource& out)
{
    if (text == "Key")            { out = BindingSource::KEY;            return true; }
    if (text == "MouseButton")    { out = BindingSource::MOUSE_BUTTON;   return true; }
    if (text == "GamepadButton")  { out = BindingSource::GAMEPAD_BUTTON; return true; }
    if (text == "GamepadAxis")    { out = BindingSource::GAMEPAD_AXIS;   return true; }
    if (text == "MouseAxis")      { out = BindingSource::MOUSE_AXIS;     return true; }
    return false;
}

toml::table BindingToTable(const InputBinding& binding, bool explicitDefaults)
{
    toml::table table;
    table.insert("source", SourceToString(binding.source));

    /// @note ゲームパッドは列挙名で保存する (数値だと enum への要素追加で既存ファイルの意味がずれる)。
    /// @note       キーコードは Win32 仮想キーコードで値が固定なので数値のままでよい。
    if (binding.source == BindingSource::GAMEPAD_BUTTON) {
        table.insert("button", ToString(static_cast<GamepadButton>(binding.code)));
    } else if (binding.source == BindingSource::GAMEPAD_AXIS) {
        table.insert("axis", ToString(static_cast<GamepadAxis>(binding.code)));
    } else {
        table.insert("code", static_cast<int64_t>(binding.code));
    }

    /// @note 既定値と同じフィールドは書き出さない。差分の読みやすさを優先する。
    if (explicitDefaults || binding.scale != 1.0f)           table.insert("scale", static_cast<double>(binding.scale));
    if (explicitDefaults || binding.padIndex != -1)          table.insert("pad", static_cast<int64_t>(binding.padIndex));
    if (explicitDefaults || binding.buttonThreshold != 0.5f)
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

toml::array BindingsToArray(const std::vector<InputBinding>& bindings, bool explicitDefaults)
{
    toml::array array;
    for (const InputBinding& binding : bindings) {
        array.push_back(BindingToTable(binding, explicitDefaults));
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

} /// @note namespace

bool InputActionMap::LoadFromFile(const std::string& path)
{
    std::error_code error;
    const std::filesystem::path filePath = util::FileSystem::PathFromUtf8(path);
    if (!std::filesystem::exists(filePath, error)) {
        FBZZ_LOG_INFO("InputActionMap: %s が見つかりません — 既定バインドを使用します",
                      path.c_str());
        return false;
    }

    std::ifstream stream(filePath, std::ios::binary);
    if (!stream.is_open()) {
        FBZZ_LOG_ERROR("InputActionMap: %s を読み込めません", path.c_str());
        return false;
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    InputBindingsDocument document;
    if (!document.LoadFromToml(contents.str())) {
        FBZZ_LOG_ERROR("InputActionMap: failed to parse %s", path.c_str());
        return false;
    }
    Clear();
    for (const auto& axis : document.axes) AddAxis(axis);
    for (const auto& action : document.actions) AddAction(action);
    return true;
}

bool InputActionMap::SaveToFile(const std::string& path)
{
    InputBindingsDocument document;
    document.axes = GetAxes();
    document.actions = GetActions();
    /// @note 親ディレクトリが無い場合は作る (新規プロジェクトの初回保存)。
    std::error_code error;
    const std::filesystem::path filePath = util::FileSystem::PathFromUtf8(path);
    if (filePath.has_parent_path()) {
        std::filesystem::create_directories(filePath.parent_path(), error);
    }

    std::ofstream stream(filePath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        FBZZ_LOG_ERROR("InputActionMap: %s を書き込めません", path.c_str());
        return false;
    }

    stream << "# FBZZ Engine — Input Action Map\n"
              "# ゲーム入力のバインド定義。エディタの Project Settings > Input から編集できる。\n\n";
    stream << document.ToToml();
    stream << '\n';

    return stream.good();
}

bool InputBindingsDocument::LoadFromToml(std::string_view text)
{
    const auto parsed = toml::parse(text);
    if (!parsed) return false;
    const toml::table& root = parsed.table();

    /// @note 先にローカルへ組み立て、成功確定後に差し替える。パース途中の失敗で現在のバインドが
    /// @note       半端に壊れるのを防ぐ。
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

    axes = std::move(loadedAxes);
    actions = std::move(loadedActions);
    return true;
}

std::string InputBindingsDocument::ToToml(bool explicitDefaults) const
{
    toml::table root;

    toml::array axisArray;
    for (const InputAxis& axis : axes) {
        toml::table table;
        table.insert("name", axis.name);
        table.insert("deadZone",    static_cast<double>(axis.deadZone));
        table.insert("gravity",     static_cast<double>(axis.gravity));
        table.insert("sensitivity", static_cast<double>(axis.sensitivity));
        table.insert("snap", axis.snap);
        if (explicitDefaults || axis.raw) table.insert("raw", axis.raw);

        /// @note 空の配列は書き出さない。既定バインドのファイルが読みやすくなる。
        if (explicitDefaults || !axis.positive.empty()) table.insert("positive", BindingsToArray(axis.positive, explicitDefaults));
        if (explicitDefaults || !axis.negative.empty()) table.insert("negative", BindingsToArray(axis.negative, explicitDefaults));
        if (explicitDefaults || !axis.analog.empty())   table.insert("analog",   BindingsToArray(axis.analog, explicitDefaults));

        axisArray.push_back(std::move(table));
    }
    root.insert("axis", std::move(axisArray));

    toml::array actionArray;
    for (const InputAction& action : actions) {
        toml::table table;
        table.insert("name", action.name);
        table.insert("bindings", BindingsToArray(action.bindings, explicitDefaults));
        actionArray.push_back(std::move(table));
    }
    root.insert("action", std::move(actionArray));

    std::ostringstream stream;
    stream << root;
    return stream.str();
}

} /// @note namespace fbzz::input
