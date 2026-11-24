/// @file    ManagedServiceRoutes.hpp
/// @brief   標準コンポーネント用の非公開サービス経路。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include "../Interop/ManagedServiceArguments.hpp"
#include "ManagedServicesBridge.hpp"

namespace fbzz::scene::managed_detail {

class ManagedPhysicsBridge;

/// @pre ディスパッチが対象の所属、削除予約、パケットの構造を検証済みで、view は対象に束縛済み。
ManagedBridgeStatus RunManagedAnimatorService(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    const ManagedServiceArguments& arguments, ManagedServiceResult& output);
ManagedBridgeStatus RunManagedUiService(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    const ManagedServiceArguments& arguments, ManagedServiceResult& output);
ManagedBridgeStatus RunManagedCameraService(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    const ManagedServiceArguments& arguments, ManagedServiceResult& output);
ManagedBridgeStatus RunManagedNavigationService(ManagedPhysicsBridge& physics, Script& view, GameObject& target,
    GameObject* related, const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments,
    ManagedServiceResult& output, bool fixedUpdate);
ManagedBridgeStatus RunManagedCharacterService(GameObject& target, const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments,
    ManagedServiceResult& output, bool fixedUpdate, bool collisionCallback);

}
