/// @file    SocketAttach.hpp
/// @brief   モデルの「指定ノード」をキャラクターのソケットボーンへ一致させてアタッチする。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <string_view>

namespace fbzz::scene {

class GameObject;

// modelRoot 配下の attachNodeName ノードのワールド変換がソケットと一致するように
// modelRoot を socket の子へ付け替える。
//
// WHY このAPIが要る:
//   DCC 由来のモデルはルート付近に座標系変換ノードを持つことがある
//   (Blender FBX の Z-up→Y-up の ±90°X など)。モデルをそのまま別モデルの
//   ボーン配下へ入れると、その変換が二重に掛かって姿勢が崩れる。
//   さらに「モデル原点」と「実際に握ってほしい位置」は一致しないことが多い。
//
//   本APIは modelRoot ではなく attachNodeName で指定したノードを基準にする。
//   modelRoot → attachNode の相対変換 A を実測し、modelRoot のローカルへ A⁻¹ を
//   入れることで、途中に何段ノードがあっても attachNode.world == socket.world になる。
//   よって座標系の差もモデル原点のズレも自動的に吸収され、
//   呼び出し側は補正値を一切持たなくてよい。
//
// 例: 拳銃の WRoot (グリップ) を右手のソケットへ合わせる
//     AttachToSocket(*pistolRoot, *sockHandR, "WRoot");
//
// attachNodeName が見つからない場合は modelRoot 自身を基準にする (従来動作)。
// 戻り値: アタッチできたら true。
bool AttachToSocket(GameObject& modelRoot,
                    GameObject& socket,
                    std::string_view attachNodeName);

// modelRoot 配下から名前でノードを探す (深さ優先、最初に一致したもの)。
// modelRoot 自身も検索対象に含む。
GameObject* FindDescendantByName(GameObject& root, std::string_view name);

} // namespace fbzz::scene
