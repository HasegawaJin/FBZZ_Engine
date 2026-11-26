/// @file    BusInternal.hpp
/// @brief   Command Bus のハンドラー表と、領域ファイル間で共有する型・補助関数。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see     Docs/design/ai-verification-loop.md «6. バスの分割»
#pragma once
#include <Editor/Ai/Serialization/Json.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
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
    bool        ok = false; ///< 処理が成功し、応答に result を使えるか。
    JsonValue   result;     ///< 成功時にクライアントへ返す値。
    std::string code;       ///< 失敗の種類を識別するエラーコード。
    std::string message;    ///< 失敗の理由をクライアントに伝える説明。

    static Outcome Ok(JsonValue value)
    {
        Outcome outcome;
        outcome.ok = true;
        outcome.result = std::move(value);
        return outcome;
    }
    static Outcome Err(std::string errorCode, std::string errorMessage)
    {
        Outcome outcome;
        outcome.code = std::move(errorCode);
        outcome.message = std::move(errorMessage);
        return outcome;
    }
};

/// @brief コンポーネント名で引いた型の状態。
struct TypeInfo {
    bool known = false;       ///< コンポーネント名が既知の型として解決されたか。
    bool present = false;     ///< 対象の GameObject がこの型のコンポーネントを持つか。
    bool addable = false;     ///< 登録上追加が許可され、既定構築とムーブが可能な型か。
    bool reflectable = false; ///< リフレクションを通してフィールドを扱えるか。
};

/// @brief 検証済みの編集を元に戻す際に、所有者・シーン・Undo 履歴の先頭が一致するかを照合する記録。
struct ReliabilityReceipt {
    std::string owner;             ///< 編集を要求した所有者の識別子。
    std::string id;                ///< 呼び出し側がロールバックで指定する記録の識別子。
    std::string label;             ///< Undo 履歴の先頭と照合する複合コマンド名。
    JsonValue afterGuard;          ///< 編集後の状態が保持されているかを照合する値。
    JsonValue beforeSnapshot;      ///< Undo 後に復元を確かめるための編集前のスナップショット。
    scene::Scene* scene = nullptr; ///< 編集対象だったシーンへの非所有参照。
    bool rolledBack = false;       ///< この記録の編集が既に取り消されたか。
};

/// @brief Dispatcher がフレームごとに更新し、ハンドラーが読む状態。
struct BusState {
    renderer::ResourceHandle<renderer::RenderTargetTag> sceneViewportRT{}; ///< シーンビューの描画先を指すハンドル。
    renderer::ResourceHandle<renderer::RenderTargetTag> gameViewportRT{};  ///< ゲームビューの描画先を指すハンドル。
    std::vector<ReliabilityReceipt> reliabilityReceipts;                   ///< このディスパッチャーで保持する直近の検証済み編集。
};

class BusHandlerTable;

/// @brief 1 要求ぶんの呼び出し文脈。
struct BusCall {
    editor::EditorContext& ctx;            ///< 操作対象のエディターとアクティブなシーン。
    const std::string&     type;           ///< ハンドラーを引く要求の型名。
    const JsonValue&       payload;        ///< 呼び出し側から受け取った引数。
    bool                   dryRun = false; ///< 変更を適用せずに事前検証する要求か。
    BusState&              state;          ///< 複数の要求にまたがってディスパッチャーが保持する状態。
    const BusHandlerTable& table;          ///< トランザクション内の要求の解決と操作一覧の取得に使う登録表。
};

/// @brief Query / 直接実行 Command の処理。dryRun の扱いはハンドラーが持つ。
using CommandFn = Outcome (*)(BusCall& call);
/// @brief Undo 可能な Command を組み立てる。適用 (UndoStack::Execute) と dryRun は Dispatcher が行う。
/// @param[out] createdSink 作ったノードの instanceId を書く先。応答の id になる。
/// @param[out] detailSink 適用前に分かる副作用 (改名・上限の引き上げ等) を書く先。null 可。
/// @return 失敗時は nullptr と err。
using BuilderFn = std::unique_ptr<ICommand> (*)(editor::EditorContext& ctx, const std::string& type,
                                                const JsonValue& payload, Outcome& err,
                                                std::shared_ptr<std::string> createdSink, JsonValue* detailSink);

enum class BusKind { QUERY, COMMAND, BUILDER };

struct BusHandlerEntry {
    std::string type;                     ///< 要求と照合する登録名。
    BusKind     kind = BusKind::QUERY;     ///< 照会・直接実行・コマンド構築の区分。
    CommandFn   handler = nullptr;        ///< 照会または直接実行に使う関数。
    BuilderFn   builder = nullptr;        ///< Undo 用コマンドの構築関数。トランザクションに対応する直接実行にも設定する。
    bool        returnsCreatedId = false; ///< 生成したノードの ID を応答に含めるか。
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
    std::vector<BusHandlerEntry> m_entries; ///< この表に登録済みの操作と呼び出し先。
};

/// @name 領域ごとの登録。Dispatcher のコンストラクタが順に呼ぶ。
/// @{
void RegisterEditorHandlers(BusHandlerTable& table);
void RegisterSceneHandlers(BusHandlerTable& table);
void RegisterScriptHandlers(BusHandlerTable& table);
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
[[nodiscard]] JsonValue SnapshotComponents(GameObject& go, bool observations = false);
/// @brief Script の識別子と状態のみを列挙する。保存用スナップショットからは呼ばない。
[[nodiscard]] JsonValue InspectScriptList(GameObject& go);
[[nodiscard]] std::string LowerAscii(std::string value);
/// @brief OperatorBridge の結果を Outcome へ詰め替える。Operator 層へ Outcome を見せないため境界で 1 度だけ変換する。
[[nodiscard]] Outcome FromBridge(OperatorBridgeResult bridge);
/// @brief dryRun の応答。変更せず «何をする予定か» を返す。
[[nodiscard]] Outcome DryRunPreview(const std::string& type);
/// @brief 2 つの JSON 値が同じ大分類 (数値/真偽/文字列/配列/オブジェクト) か。component.set の型検査用。
[[nodiscard]] bool CompatibleJsonType(const JsonValue& a, const JsonValue& b);
/// @brief projectRoot 配下のファイルへ解決する。外へ出るパスは false。
/// @note 未作成の末尾と親を許可し、既存リンクの解決後に境界を検証する。失敗時の出力は未変更。
/// @see https://learn.microsoft.com/en-us/cpp/standard-library/path-class?view=msvc-170#lexically_relative lexical 相対化。
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
