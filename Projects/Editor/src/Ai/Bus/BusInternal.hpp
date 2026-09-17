/// @file    BusInternal.hpp
/// @brief   Command Bus のハンドラー表と、領域ファイル間で共有する型・補助関数。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see     Docs/design/ai-verification-loop.md «6. バスの分割»
#pragma once
#include <Editor/Ai/Json.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/GameObject.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::math { struct Vector3; struct Quaternion; struct Matrix4; }
namespace fbzz::editor { class PlayModeController; }
namespace fbzz::editor::ai { struct OperatorBridgeResult; }

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

/// @brief ハンドラーの結果。ok=false なら code / message がエラー応答になる。
struct Outcome {
    bool        ok = false;
    JsonValue   result;
    std::string code;
    std::string message;
    static Outcome Ok(JsonValue value) { Outcome outcome; outcome.ok = true; outcome.result = std::move(value); return outcome; }
    static Outcome Err(std::string errorCode, std::string errorMessage)
    {
        Outcome outcome;
        outcome.code = std::move(errorCode);
        outcome.message = std::move(errorMessage);
        return outcome;
    }
};

/// @brief コンポーネント名で引いた型の状態。
struct TypeInfo { bool known = false; bool present = false; bool addable = false; bool reflectable = false; };

/// @brief Dispatcher がフレームごとに更新し、ハンドラーが読む状態。
struct BusState {
    renderer::ResourceHandle<renderer::RenderTargetTag> sceneViewportRT{};
    renderer::ResourceHandle<renderer::RenderTargetTag> gameViewportRT{};
};

class BusHandlerTable;

/// @brief 1 要求ぶんの呼び出し文脈。
struct BusCall {
    editor::EditorContext& ctx;
    const std::string&     type;
    const JsonValue&       payload;
    bool                   dryRun = false;
    BusState&              state;
    /// @note editor.transaction と editor.bus.list だけが表そのものを引く。
    const BusHandlerTable& table;
};

/// @brief Query / 直接実行 Command の処理。dryRun の扱いはハンドラーが持つ。
using CommandFn = Outcome (*)(BusCall& call);
/// @brief Undo 可能な Command を組み立てる。適用 (UndoStack::Execute) と dryRun は Dispatcher が行う。
/// @param createdSink 作ったノードの instanceId を書く先。応答の id になる。
/// @param detailSink  適用前に分かる副作用 (改名・上限の引き上げ等) を書く先。null 可。
/// @return 失敗時は nullptr と err。
using BuilderFn = std::unique_ptr<ICommand> (*)(editor::EditorContext& ctx, const std::string& type,
                                                const JsonValue& payload, Outcome& err,
                                                std::shared_ptr<std::string> createdSink, JsonValue* detailSink);

enum class BusKind { QUERY, COMMAND, BUILDER };

struct BusHandlerEntry {
    std::string type;
    BusKind     kind = BusKind::QUERY;
    CommandFn   handler = nullptr;
    /// @note COMMAND でも transaction へ混ぜられるものは builder を併せ持つ (fluid.set 等)。
    BuilderFn   builder = nullptr;
    bool        returnsCreatedId = false;
};

/// @brief 型名 → ハンドラーの表。
/// @note 自己登録の静的初期化子は使わない。FBZZEditor は STATIC で、参照されない .obj の初期化子は捨てられる。
class BusHandlerTable {
public:
    void AddQuery(std::string type, CommandFn handler);
    void AddCommand(std::string type, CommandFn handler, BuilderFn transactionBuilder = nullptr);
    void AddBuilder(std::string type, BuilderFn builder, bool returnsCreatedId = false);

    /// @return 未登録なら nullptr。
    [[nodiscard]] const BusHandlerEntry* Find(std::string_view type) const;
    [[nodiscard]] const std::vector<BusHandlerEntry>& Entries() const { return m_entries; }

private:
    void Add(BusHandlerEntry entry);
    std::vector<BusHandlerEntry> m_entries;
};

/// @name 領域ごとの登録。Dispatcher のコンストラクタが順に呼ぶ。
/// @{
void RegisterEditorHandlers(BusHandlerTable& table);
void RegisterSceneHandlers(BusHandlerTable& table);
void RegisterAssetHandlers(BusHandlerTable& table);
void RegisterBehaviorTreeHandlers(BusHandlerTable& table);
void RegisterPlayHandlers(BusHandlerTable& table);
void RegisterAnimationHandlers(BusHandlerTable& table);
void RegisterWorldHandlers(BusHandlerTable& table);
void RegisterFluidHandlers(BusHandlerTable& table);
void RegisterPlaytestHandlers(BusHandlerTable& table);
/// @}

/// @name 共有の補助 (BusCommon.cpp ほか)
/// @{
[[nodiscard]] std::string StringField(const JsonValue& obj, const char* key);
/// @brief 長さ 3 の数値配列を Vector3 として読む。
bool ReadVec3(const JsonValue& obj, const char* key, math::Vector3& out);
/// @brief AI へ公開してよい登録コンポーネント名か。Hidden 型は内部実装なので検索条件にも出さない。
[[nodiscard]] bool IsPublicComponentName(std::string_view componentName);
[[nodiscard]] TypeInfo InspectComponentType(GameObject& go, const std::string& comp);
void AddComponentByName(GameObject& go, const std::string& comp);
void RemoveComponentByName(GameObject& go, const std::string& comp);
/// @return 型不明・未装着・非反射なら nullopt。
[[nodiscard]] std::optional<JsonValue> ReadComponentFields(GameObject& go, const std::string& comp);
/// @return 失敗なら false と errMsg。
bool WriteComponentField(GameObject& go, const std::string& comp, const std::string& field,
                         const JsonValue& value, std::string& errMsg);
/// @brief 全反射コンポーネントを [{type, fields}] に写す (node.components と削除スナップショットで共用)。
[[nodiscard]] JsonValue SnapshotComponents(GameObject& go);
[[nodiscard]] std::string LowerAscii(std::string value);
/// @brief OperatorBridge の結果を Outcome へ詰め替える。Operator 層へ Outcome を見せないため境界で 1 度だけ変換する。
[[nodiscard]] Outcome FromBridge(OperatorBridgeResult bridge);
/// @brief dryRun の応答。変更せず «何をする予定か» を返す。
[[nodiscard]] Outcome DryRunPreview(const std::string& type);
/// @brief 2 つの JSON 値が同じ大分類 (数値/真偽/文字列/配列/オブジェクト) か。component.set の型検査用。
[[nodiscard]] bool CompatibleJsonType(const JsonValue& a, const JsonValue& b);
/// @brief projectRoot 配下のファイルへ解決する。外へ出るパスは false。
bool ResolveProjectFile(const editor::EditorContext& ctx, const std::string& requested,
                        std::filesystem::path& outPath, std::string& outRelative);
[[nodiscard]] const char* PlayStateName(const editor::PlayModeController& playMode);
[[nodiscard]] JsonValue VectorToJson(const math::Vector3& value);
[[nodiscard]] JsonValue QuaternionToJson(const math::Quaternion& value);
[[nodiscard]] JsonValue MatrixToJson(const math::Matrix4& matrix);
/// @return 未知のキー名は -1。
[[nodiscard]] int VirtualKeyFromName(std::string name);
/// @brief sprites を持たない Single Texture の «全面 1 枚» か。この 1 枚だけは ID を持たないので壊れていると判定しない。
[[nodiscard]] bool IsImplicitSingleSprite(const std::string& texturePath, const std::string& token);
/// @}

} // namespace fbzz::editor::ai::bus
