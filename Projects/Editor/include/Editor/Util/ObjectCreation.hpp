/// @file    ObjectCreation.hpp
/// @brief   GameObject 生成 (プリセット・Prefab・スクリプト) の親付け・命名・配置・選択・Undo の実体。
/// @author  Hasegawa Jin
/// @date    2026-09-16
/// @note Hierarchy・メインメニュー・Operator (node.create_* / prefab.instantiate)・AI バス (preset.create / node.create) はすべてここを通る。
/// @see Docs/design/editor-operator-model.md
#pragma once

#include <Editor/Util/UndoStack.hpp>
#include <Math/Vector3.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene { class GameObject; class Scene; }

namespace fbzz::editor {

struct EditorContext;

enum class CreateObjectSource {
    Preset, ///< @brief key はプリセット id (ObjectPresets.hpp)
    Prefab, ///< @brief key は .prefab / .vfx のパス (ディスク上、または projectRoot 相対)
    Script, ///< @brief key はスクリプト型名
};

struct CreateObjectRequest {
    CreateObjectSource source = CreateObjectSource::Preset;
    std::string   key;
    /// @brief 親の instanceId。空ならルート。
    std::string   parentGuid;
    /// @brief 空なら既定名を兄弟の中で一意化する ("Cube (1)")。指定した名前はそのまま使う。
    std::string   name;
    bool          hasPosition = false;
    /// @brief ローカル座標。hasPosition のときだけ使う。
    math::Vector3 position{};
    /// @brief ルートに置くとき Scene View の注視点を原点にする。子に置くときは無視される。
    bool          placeInView = false;
};

/// @brief 要求が今のシーンで実行できるか。
/// @param outCode 失敗時の理由コード (NO_SCENE / NODE_NOT_FOUND / UNKNOWN_PRESET / PREFAB_NOT_FOUND / UNKNOWN_SCRIPT / BAD_ARG / SCENE_CAPACITY)。
/// @return 実行できなければ false。
[[nodiscard]] bool ValidateCreateObjectRequest(const EditorContext& ctx, const CreateObjectRequest& request,
                                               std::string& outCode, std::string& outMessage);

/// @brief 履歴に出す表示名 ("Create Cube" / "Instantiate Door")。
[[nodiscard]] std::string CreateObjectLabel(const CreateObjectRequest& request);

/// @brief 生成を行う Undo コマンドを作る。
/// @param applyNow true ならこの場で生成 (Operator は Push だけの契約)、false なら Execute で生成 (AI バスの dryRun / transaction)。
/// @param onCreated 生成のたびに要求どおり作ったルートの instanceId を受け取る。null 可。
/// @return 検証に失敗したか、applyNow で生成に失敗したら nullptr。
/// @note 子に置くとプリセットの固定オフセット (Camera の (0,2,-5) 等) は捨てて親の原点へ置く。ルート+placeInView では注視点からの相対として残す。
/// @note UI プリセットの祖先に Canvas が無ければ、activeUICanvas か新しい Canvas の下へ入れる。
/// @note Redo は前回と同じ instanceId を名乗り直す (後続コマンドの guid 参照を切らないため)。
[[nodiscard]] std::unique_ptr<ICommand> MakeCreateObjectCommand(
    EditorContext& ctx, const CreateObjectRequest& request, std::string label, bool applyNow,
    std::function<void(const std::vector<std::string>&)> onCreated = {});

/// @brief parent の子 (null ならルート) の中で重ならない名前を返す。
/// @param self 比較から外す GameObject (自分自身)。null 可。
/// @note base 末尾の " (N)" は外してから数える。
[[nodiscard]] std::string MakeUniqueSiblingName(const scene::Scene& targetScene, const scene::GameObject* parent,
                                                const std::string& base, const scene::GameObject* self);

/// @brief go かその祖先が Prefab インスタンスか。
[[nodiscard]] bool IsInsidePrefabInstance(const scene::GameObject* go);

/// @brief Prefab インスタンスの下へ足すときの注意文。
/// @note 差分 (PrefabOverrides) はプロパティ単位しか追わず、足した子は override にならない。
[[nodiscard]] const char* PrefabInstanceChildWarning();

} /// @note namespace fbzz::editor
