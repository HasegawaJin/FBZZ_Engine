// FBZZ Engine
// WeaponSockets.hpp | sandbox
// 銃の装着点・アニメーションイベント・クリップ名の一元定義。
// アタッチしないユーティリティ (FBZZ_SCRIPT を持たない)。
//
// WHY 1 ファイルに集めるか:
//   ソケット名は Blender (エクスポート) と エンジン (scene.Find) の両方に現れる文字列で、
//   しかも片方だけ直しても *コンパイルは通る*。壊れ方が「実行して銃が原点に出るまで
//   気付かない」種類なので、リテラルを散らしてはいけない。PolarityTypes.hpp が配色を
//   1 箇所に閉じ込めているのと同じ理由でここへ集約する。
//
// WHY 旧名も持つか:
//   .scene に保存済みの GameObject 名は旧 FBX 由来のまま (GunSocket_Hand_L 等)。
//   新 FBX を再インポートするまでは旧名しか存在しないため、両方を順に探す。
//   移行が済んだら kLegacy* を消せばよい。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <string_view>

namespace sandbox {

// ── Player 側ソケット (SK_Player の SOCKET_* ボーン) ─────────────────────────
inline constexpr const char* kSocketHandL    = "SOCKET_Hand_L";
inline constexpr const char* kSocketHandR    = "SOCKET_Hand_R";
inline constexpr const char* kSocketHolsterL = "SOCKET_Holster_L";
inline constexpr const char* kSocketHolsterR = "SOCKET_Holster_R";
inline constexpr const char* kSocketBack     = "SOCKET_Back";

// ── 武器側ソケット (WPN_Pistol_*_Rig のボーン) ──────────────────────────────
// WHY 左右で名前を分けないか:
//   武器は独立アセットで、L/R は「どちらの手に付いているか」でしかない。
//   武器の中では常に SOCKET_Muzzle が 1 本。名前に L/R を残すと、3 丁目の武器を
//   足した瞬間に破綻する。左右の区別は「どの武器 GameObject の下を探すか」で表す。
inline constexpr const char* kSocketGrip   = "SOCKET_Grip";
inline constexpr const char* kSocketMuzzle = "SOCKET_Muzzle";
inline constexpr const char* kSocketEject  = "SOCKET_Eject";

// ── 旧 FBX 由来の名前 (再インポート前のシーン救済用) ─────────────────────────
inline constexpr const char* kLegacySocketHandL    = "GunSocket_Hand_L";
inline constexpr const char* kLegacySocketHandR    = "GunSocket_Hand_R";
inline constexpr const char* kLegacySocketHolsterL = "Sock_Holster_L";
inline constexpr const char* kLegacySocketHolsterR = "Sock_Holster_R";
inline constexpr const char* kLegacySocketMuzzleL  = "Sock_Muzzle_L";
inline constexpr const char* kLegacySocketMuzzleR  = "Sock_Muzzle_R";
inline constexpr const char* kLegacySocketEjectL   = "Sock_Eject_L";
inline constexpr const char* kLegacySocketEjectR   = "Sock_Eject_R";

// ── 武器 GameObject の既定名 ────────────────────────────────────────────────
inline constexpr const char* kWeaponObjectL = "WPN_Pistol_L";
inline constexpr const char* kWeaponObjectR = "WPN_Pistol_R";

// ── 演出タイミング (秒 / 30fps 換算) ────────────────────────────────────────
//
// WHY アニメーションイベントを使わないか:
//   以前はここに WeaponAttach / MuzzleFlash といったイベント名を並べ、クリップの
//   Event Track に打たれている前提で実装していた。実際にはインポート後の .anim に
//   イベントは 1 つも入っておらず (Draw_Pistols.anim / Fire_R.anim を確認済み)、
//   マズルフラッシュも薬莢も一度も出ていなかった。
//   イベント駆動の壊れ方は「何も起きない」なので、動いていないことに気付けない。
//   さらにクリップを差し替えるたびに打ち直しが要り、打ち忘れてもコンパイルは通る。
//
//   そこで「何秒後に何が起きるか」だけをここに定数として置き、エンジン側の時間で
//   駆動する。数値の出どころは Blender の曲線から実測したフレーム (30fps) で、
//   Export/fbzz_assets.json に同じ値が出力されている。クリップを差し替えたときに
//   ズレるのはイベント方式と同じだが、こちらは「ここを直す」場所が 1 箇所しかない。
inline constexpr float kFireMuzzleFlashDelay = 1.0f / 30.0f;  // Fire  1F
inline constexpr float kFireRecoilPeakDelay  = 2.0f / 30.0f;  // Fire  2F
inline constexpr float kFireShellEjectDelay  = 3.0f / 30.0f;  // Fire  3F
inline constexpr float kReloadMagOutDelay    = 8.0f / 30.0f;  // Reload  8F
inline constexpr float kReloadMagInDelay     = 48.0f / 30.0f; // Reload 48F
inline constexpr float kReloadSlideDelay     = 58.0f / 30.0f; // Reload 58F

// 手の識別。
//
// WHY 0/1 ではなく 1/2 か:
//   Blender 側の wpn_state (1 = 右銃を手に、2 = 両手に) がそのまま残っている。
//   0 は「未指定」を意味する値なので、0 を左手に割り当てると、値を入れ忘れた
//   データが左手として通ってしまう。どちらでもない値は必ず未指定として扱いたい。
enum class HandSide : int { Left = 2, Right = 1 };

// 11 章の操作表: 右入力 = 右銃 (＋) / 左入力 = 左銃 (−)。
// この対応は操作の直感性そのものなので、極性から手を引くのは 1 箇所に閉じる。
[[nodiscard]] inline HandSide HandOf(Polarity polarity)
{
    return polarity == Polarity::Plus ? HandSide::Right : HandSide::Left;
}

[[nodiscard]] inline const char* HandSocketName(HandSide hand)
{
    return hand == HandSide::Right ? kSocketHandR : kSocketHandL;
}

[[nodiscard]] inline const char* LegacyHandSocketName(HandSide hand)
{
    return hand == HandSide::Right ? kLegacySocketHandR : kLegacySocketHandL;
}

[[nodiscard]] inline const char* HolsterSocketName(HandSide hand)
{
    return hand == HandSide::Right ? kSocketHolsterR : kSocketHolsterL;
}

[[nodiscard]] inline const char* LegacyHolsterSocketName(HandSide hand)
{
    return hand == HandSide::Right ? kLegacySocketHolsterR : kLegacySocketHolsterL;
}

[[nodiscard]] inline const char* WeaponObjectName(HandSide hand)
{
    return hand == HandSide::Right ? kWeaponObjectR : kWeaponObjectL;
}

// Inspector でアサインした GameObject 参照が、本当にその手の銃を指しているか。
//
// WHY 名前で裏を取るか:
//   Ref<GameObject> / EntityRef はシーン内の「並び順の番号」で保存される。GameObject を
//   1 つ増減させると以降の番号が全部ずれるが、ずれた参照は無効にはならず、
//   *別のオブジェクトを指したまま有効* になる。無効化されるなら気付けるのに対し、
//   これは「銃として敵を掴む」ような形で静かに壊れる。名前が食い違ったら参照を捨て、
//   名前引きへ落とす方が、間違ったまま動き続けるより早く原因に辿り着ける。
[[nodiscard]] inline bool IsWeaponObject(const fbzz::scene::GameObject* object, HandSide hand)
{
    return object != nullptr && object->name == WeaponObjectName(hand);
}

// ── 武器クリップ名 ──────────────────────────────────────────────────────────
// 1 クリップ 1 FBX の運用に合わせ、パスではなくクリップ名だけを持つ。
// 実際の .fbx パスは各スクリプトの FBZZ_FIELD_FILE で指定する。
// 変形 (コンパクト形態 ⇄ 銃形態)。Fold_Barrel / Fold_Top / Fold_Root を動かす。
// WHY 変形が要るか: 銃は幅 25.5cm あり、胴体と腕の隙間 (腰の高さで 13.6〜17.2cm) に
//     物理的に入らない。どこへホルスターを置いても腕と 1mm 以下まで接近する。
//     コンパクト形態は 15.6cm 幅まで縮むので、初めて収納が成立する。
inline constexpr const char* kClipPistolFold       = "Pistol_Fold";        // 銃 → コンパクト (20F)
inline constexpr const char* kClipPistolDeploy     = "Pistol_Deploy";      // コンパクト → 銃 (20F)
inline constexpr const char* kClipPistolFoldedIdle = "Pistol_Folded_Idle"; // 収納中の保持ポーズ (loop)

inline constexpr const char* kClipPistolIdle   = "Pistol_Idle";
inline constexpr const char* kClipPistolDraw   = "Pistol_Draw";
inline constexpr const char* kClipPistolDry    = "Pistol_Dry";
inline constexpr const char* kClipPistolReload = "Pistol_Reload";
inline constexpr const char* kClipPistolFireL  = "Pistol_Fire_L";
inline constexpr const char* kClipPistolFireR  = "Pistol_Fire_R";

[[nodiscard]] inline const char* PistolFireClipName(HandSide hand)
{
    return hand == HandSide::Right ? kClipPistolFireR : kClipPistolFireL;
}

// ── 階層探索 ────────────────────────────────────────────────────────────────
//
// WHY scene.Find() で済ませないか:
//   SOCKET_Muzzle は左右の銃にそれぞれ 1 本ずつ存在する。名前がシーン内で一意でない
//   以上、グローバル検索では「どちらか片方」が返る。しかもどちらが返るかは
//   GameObject の生成順に依存するため、症状が日によって変わる。必ず武器の下を探す。
[[nodiscard]] inline fbzz::scene::GameObject*
FindInSubtree(fbzz::scene::GameObject& root, std::string_view name)
{
    if (std::string_view(root.name) == name)
        return &root;
    const int childCount = root.GetChildCount();
    for (int i = 0; i < childCount; ++i) {
        if (fbzz::scene::GameObject* child = root.GetChild(i)) {
            if (fbzz::scene::GameObject* found = FindInSubtree(*child, name))
                return found;
        }
    }
    return nullptr;
}

// 新名 → 旧名の順に部分木を探す。移行期間中はどちらのシーンでも通る。
[[nodiscard]] inline fbzz::scene::GameObject*
FindSocketInSubtree(fbzz::scene::GameObject& root,
                    std::string_view preferredName,
                    std::string_view legacyName)
{
    if (fbzz::scene::GameObject* found = FindInSubtree(root, preferredName))
        return found;
    return legacyName.empty() ? nullptr : FindInSubtree(root, legacyName);
}

} // namespace sandbox
