/// @file    SocketAttach.hpp
/// @brief   モデルの「指定ノード」をキャラクターのソケットボーンへ一致させてアタッチする。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <string_view>

namespace fbzz::scene {

class GameObject;

/// @brief modelRoot 配下の attachNodeName ノードのワールド変換がソケットと一致するように modelRoot を socket の子へ付け替える。
/// @param attachNodeName 基準にするノード名。見つからない場合は modelRoot 自身を基準にする (従来動作)。
/// @return アタッチできたら true。
/// @note modelRoot→attachNode の相対変換の逆を modelRoot のローカルへ入れることで、途中に何段ノードがあっても attachNode.world == socket.world になり、DCC 由来の座標系変換 (例: Blender FBX の Z-up→Y-up) やモデル原点のズレを自動的に吸収する。
bool AttachToSocket(GameObject& modelRoot,
                    GameObject& socket,
                    std::string_view attachNodeName);

/// @brief root 配下から名前でノードを探す (深さ優先、最初に一致したもの)。root 自身も検索対象に含む。
GameObject* FindDescendantByName(GameObject& root, std::string_view name);

} // namespace fbzz::scene
