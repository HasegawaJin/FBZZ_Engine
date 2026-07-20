// FBZZ Engine
// VFXEditorLauncher.cpp | fbzz::editor
// 独立VFXEditorプロセス起動のWin32実装
#include <Editor/Util/VFXEditorLauncher.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Ai/NamedPipeClient.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace fbzz::editor {

namespace {

constexpr const wchar_t* VFX_PIPE_NAME = L"\\\\.\\pipe\\FBZZVFXEditorCommandBus";

// AssetBrowserで進行中の1ドラッグを次フレームのrelease判定まで保持する。
struct DragState {
    std::string projectRoot;
    std::string assetPath;
};

DragState& GetDragState()
{
    // Editorメインスレッドだけが使用するため、所有者を関数ローカルへ閉じ込めてグローバル可変状態を避ける。
    static DragState state;
    return state;
}

// CreateProcessWへ空白を含むパスを安全に渡すため、1引数を引用符で囲む。
std::wstring Quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

// Main EditorとVFXEditor間だけで使う制御要求を共通Bus envelopeへ格納する。
std::string BuildControlRequest(const char* type, const std::string& path = {})
{
    ai::JsonValue payload = ai::JsonValue::MakeObject();
    payload.Set("t", ai::JsonValue(type));
    if (!path.empty()) payload.Set("path", ai::JsonValue(path));
    ai::JsonValue root = ai::JsonValue::MakeObject();
    root.Set("protocol", ai::JsonValue(ai::kEditorProtocol));
    root.Set("id", ai::JsonValue("vfx-editor-control"));
    root.Set("kind", ai::JsonValue("command"));
    root.Set("payload", std::move(payload));
    root.Set("dryRun", ai::JsonValue(false));
    root.Set("source", ai::JsonValue("editor"));
    return ai::SerializeJson(root);
}

// VFX専用Pipeへ1要求を同期送信する内部ショートカット。
bool SendRequest(const std::string& request, std::string& response, std::uint32_t timeoutMs)
{
    return ai::NamedPipeClient::Request(VFX_PIPE_NAME, request, response, timeoutMs);
}

// 制御要求がtransport成功だけでなくBus応答として成功したか確認する。
bool IsOkResponse(const std::string& response)
{
    const auto root = ai::ParseJson(response);
    const ai::JsonValue* ok = root.has_value() ? root->Find("ok") : nullptr;
    return ok != nullptr && ok->IsBool() && ok->AsBool();
}

// release位置の最上位OSウィンドウがVFXEditorか判定する。
HWND FindVFXEditorWindowUnderCursor()
{
    POINT point{};
    if (!GetCursorPos(&point)) return nullptr;
    HWND window = GetAncestor(WindowFromPoint(point), GA_ROOT);
    if (window == nullptr) return nullptr;
    wchar_t title[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    return std::wstring_view(title).starts_with(L"FBZZ VFX Editor") ? window : nullptr;
}

// 既存VFXEditorを復元して前面へ移し、drop結果を即座に確認できるようにする。
void BringVFXEditorToFront()
{
    EnumWindows([](HWND window, LPARAM) -> BOOL {
        wchar_t title[256]{};
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        if (!std::wstring_view(title).starts_with(L"FBZZ VFX Editor")) return TRUE;
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return FALSE;
    }, 0);
}

} // namespace

bool VFXEditorLauncher::Launch(const std::string& projectRoot, const std::string& assetPath)
{
    std::string response;
    if (SendRequest(BuildControlRequest("vfx.editor.ping"), response, 50)) {
        if (!assetPath.empty())
            (void)SendRequest(BuildControlRequest("vfx.editor.dropAsset", assetPath), response, 500);
        BringVFXEditorToFront();
        return true;
    }
    const std::filesystem::path executable =
        util::FileSystem::GetExecutableDirectory() / L"FBZZVFXEditor.exe";
    if (!util::FileSystem::Exists(executable)) {
        FBZZ_LOG_ERROR("VFXEditorLauncher: FBZZVFXEditor.exeが見つかりません: %s",
                       util::StringUtils::PathToUtf8(executable).c_str());
        return false;
    }

    std::wstring command = Quote(executable.wstring()) + L" --project "
        + Quote(util::StringUtils::ToWide(projectRoot));
    if (!assetPath.empty())
        command += L" --asset " + Quote(util::StringUtils::ToWide(assetPath));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = util::StringUtils::ToWide(projectRoot);
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP, nullptr, workingDirectory.c_str(), &startup, &process);
    if (created == FALSE) {
        FBZZ_LOG_ERROR("VFXEditorLauncher: CreateProcessWに失敗しました (error=%lu)", GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool VFXEditorLauncher::ShouldRouteRequest(const std::string& request)
{
    std::string error;
    const auto root = ai::ParseJson(request, &error);
    if (!root.has_value()) return false;
    const auto envelope = ai::ParseBusRequest(*root, &error);
    if (!envelope.has_value()) return false;
    const std::string type = envelope->PayloadType();
    if (type == "vfx.preview") return true;
    if (type != "viewport.capture") return false;
    const ai::JsonValue* view = envelope->payload.Find("view");
    return view != nullptr && view->IsString() && view->AsString() == "vfx";
}

bool VFXEditorLauncher::IsVFXAuthoringCommand(const std::string& request)
{
    std::string error;
    const auto root = ai::ParseJson(request, &error);
    if (!root.has_value()) return false;
    const auto envelope = ai::ParseBusRequest(*root, &error);
    return envelope.has_value() && envelope->IsCommand()
        && envelope->PayloadType().starts_with("vfx.");
}

void VFXEditorLauncher::PrepareForVFXAuthoringCommand()
{
    std::string response;
    (void)SendRequest(BuildControlRequest("vfx.editor.flush"), response, 750);
}

void VFXEditorLauncher::NotifyVFXAuthoringCommand(const std::string& request)
{
    std::string error;
    const auto root = ai::ParseJson(request, &error);
    if (!root.has_value()) return;
    const auto envelope = ai::ParseBusRequest(*root, &error);
    if (!envelope.has_value()) return;
    const ai::JsonValue* pathValue = envelope->payload.Find("path");
    const std::string path = pathValue != nullptr && pathValue->IsString()
        ? pathValue->AsString() : std::string{};
    std::string response;
    (void)SendRequest(BuildControlRequest("vfx.editor.assetChanged", path), response, 750);
}

bool VFXEditorLauncher::EnsureAndForward(const std::string& projectRoot,
                                         const std::string& request,
                                         std::string& response)
{
    if (SendRequest(request, response, 50)) return true;
    if (!Launch(projectRoot)) return false;
    // 新規プロセスのEngine/Renderer/Command Bus初期化だけを待つ。通常は数百msで接続可能になる。
    for (int attempt = 0; attempt < 80; ++attempt) {
        Sleep(75);
        if (SendRequest(request, response, 75)) return true;
    }
    return false;
}

void VFXEditorLauncher::TrackAssetDrag(const std::string& projectRoot,
                                       const std::string& assetPath)
{
    DragState& state = GetDragState();
    state.projectRoot = projectRoot;
    state.assetPath = assetPath;
}

void VFXEditorLauncher::UpdateTrackedAssetDrag()
{
    DragState& state = GetDragState();
    if (state.assetPath.empty() || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) return;
    if (FindVFXEditorWindowUnderCursor() != nullptr) {
        std::string response;
        const bool delivered = EnsureAndForward(state.projectRoot,
            BuildControlRequest("vfx.editor.dropAsset", state.assetPath), response)
            && IsOkResponse(response);
        if (delivered) {
            Toast::Success("VFX Editorへアセットを追加しました");
            BringVFXEditorToFront();
        } else {
            Toast::Error("VFX Editorへアセットを渡せませんでした");
        }
    }
    state.projectRoot.clear();
    state.assetPath.clear();
}

} // namespace fbzz::editor
