/// @file    SocketAttach.cpp
/// @brief   モデルノードをソケットボーンへ合わせるアタッチ処理の実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/Scene/SocketAttach.hpp>

#include <Engine/Scene/GameObject.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

GameObject* FindDescendantByName(GameObject& root, std::string_view name)
{
    if (root.name == name) return &root;
    const int count = root.GetChildCount();
    for (int i = 0; i < count; ++i) {
        GameObject* child = root.GetChild(i);
        if (child == nullptr) continue;
        if (GameObject* found = FindDescendantByName(*child, name))
            return found;
    }
    return nullptr;
}

bool AttachToSocket(GameObject& modelRoot,
                    GameObject& socket,
                    std::string_view attachNodeName)
{
    if (&modelRoot == &socket) return false;
    if (socket.IsDescendantOf(modelRoot)) return false;

    GameObject* attach = attachNodeName.empty()
        ? &modelRoot
        : FindDescendantByName(modelRoot, attachNodeName);
    if (attach == nullptr) attach = &modelRoot;

    /// @note modelRoot → attachNode の相対変換 A をワールド値から実測する。途中のノード構成
    ///       (座標系変換ノードの有無や段数) に一切依存せずに済み、DCC が変わっても無修正でよい。
    const math::Quaternion invRootRot = modelRoot.transform.worldRotation.Inverse();
    const math::Quaternion relRot     = invRootRot * attach->transform.worldRotation;
    const math::Vector3    relPos     =
        invRootRot * (attach->transform.worldPosition - modelRoot.transform.worldPosition);

    if (!modelRoot.SetParent(&socket)) return false;

    /// @note attachNode.world == socket.world となる modelRoot のローカルは A⁻¹。
    ///       socket.world · A⁻¹ · A = socket.world
    const math::Quaternion localRot = relRot.Inverse();
    modelRoot.transform.rotation = localRot;
    modelRoot.transform.position = localRot * (-relPos);
    modelRoot.transform.scale    = math::Vector3::ONE;
    return true;
}

} // namespace fbzz::scene
