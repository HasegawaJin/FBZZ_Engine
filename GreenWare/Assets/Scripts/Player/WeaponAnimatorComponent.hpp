// FBZZ Engine
// WeaponAnimatorComponent.hpp | sandbox
// 銃 1 丁ぶんの見た目を担当する。WPN_Pistol_L / WPN_Pistol_R それぞれに 1 つ付ける。
//
// WHY 銃側にスクリプトを置くか:
//   スライドの後退・薬莢の射出・マズルフラッシュは「銃で起きること」であって、
//   撃った側 (PolarityGunComponent) の関心ではない。撃つ側に書くと、銃を 2 丁に
//   した時点で左右ぶんの分岐が撃つ側に生え、3 丁目で破綻する。
//   PolarityGunComponent は PlayFire() を 1 回呼ぶだけでよい。
//
// WHY 発生タイミングを自前の経過秒で持つか:
//   マズルフラッシュはスライドが下がりきる前、薬莢はその 2 フレーム後に出る。
//   間隔は Blender の曲線から実測した 1F / 3F (30fps)。撃った瞬間に全部出すと
//   薬莢がスライドより先に飛ぶので、この差は必ず要る。
//   以前はこれをクリップの Event Track に打つ前提にしていたが、インポート後の
//   Fire_R.anim にイベントは 1 つも入っておらず、マズルフラッシュも薬莢も一度も
//   出ていなかった。イベントが無いときの挙動が「無音・無表示」なので、壊れていても
//   気付けない。WeaponSockets.hpp の定数を唯一の出どころにして、エンジンの時間で回す。
//
// WHY 極性 (Polarity) を持たないか:
//   この銃が ＋ か − かはゲームルールの話で、見た目の再生には要らない。
//   持たせると PolarityGunComponent と二重管理になり、どちらが正か分からなくなる。
//   必要な区別は「左手か右手か」だけ (薬莢の飛ぶ向きが逆になるため)。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class WeaponAnimatorComponent : public Script {
    FBZZ_SCRIPT(WeaponAnimatorComponent)

    // 発砲音はここから鳴らす。無くても撃てるが手応えが消える (企画書 12.1)。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    FBZZ_GROUP("Identity")
    // 薬莢の飛ぶ向きが左右で逆なので、クリップの選択にだけ使う。
    FBZZ_FIELD_ENUM(HandSide, hand, HandSide::Right, "Hand", "Left", "Right")

    FBZZ_GROUP("Clips (1 クリップ 1 FBX)")
    FBZZ_FIELD(std::string, slotLayerName, "Base Layer", "Slot Layer")
    FBZZ_TOOLTIP("Slot 再生なので Animator Controller にステートを作らなくてよい")
    FBZZ_FIELD_FILE(fireClipFile,   "", "Fire Clip",   ".fbx,.anim")
    FBZZ_FIELD(std::string, fireClipName,   "", "Fire Clip Name")
    FBZZ_FIELD_FILE(dryClipFile,    "", "Dry Clip",    ".fbx,.anim")
    FBZZ_FIELD(std::string, dryClipName,    "", "Dry Clip Name")
    FBZZ_FIELD_FILE(reloadClipFile, "", "Reload Clip", ".fbx,.anim")
    FBZZ_FIELD(std::string, reloadClipName, "", "Reload Clip Name")

    FBZZ_GROUP("Transform (収納変形)")
    FBZZ_FIELD_FILE(deployClipFile, "", "Deploy Clip", ".fbx,.anim")
    FBZZ_FIELD(std::string, deployClipName, "Pistol_Deploy", "Deploy Clip Name")
    FBZZ_FIELD_FILE(foldClipFile,   "", "Fold Clip",   ".fbx,.anim")
    FBZZ_FIELD(std::string, foldClipName,   "Pistol_Fold",   "Fold Clip Name")
    FBZZ_FIELD_FILE(foldedIdleClipFile, "", "Folded Idle Clip", ".fbx,.anim")
    FBZZ_FIELD(std::string, foldedIdleClipName, "Pistol_Folded_Idle", "Folded Idle Clip Name")
    FBZZ_TOOLTIP("Fold の再生が終わったら、この保持ポーズへ自動で引き継ぐ")
    FBZZ_TOOLTIP("Clip Name が空なら FBX 側の最初のテイクを使う")
    FBZZ_FIELD_RANGE(float, fireFadeIn,  0.0f, "Fire Fade In",  0.0f, 0.5f)
    FBZZ_TOOLTIP("発砲はフェードさせない。立ち上がりが鈍ると撃った感触が消える")
    FBZZ_FIELD_RANGE(float, fireFadeOut, 0.08f, "Fire Fade Out", 0.0f, 0.5f)

    FBZZ_GROUP("Effects")
    // WHY Instantiate ではなく Spawn か: 連射で毎回プレファブを読み直すと
    //     GameObject 配列が再確保され、保持している GameObject* が無効化される。
    FBZZ_FIELD_REF(PrefabRef, muzzleFlashPrefab, "Muzzle Flash Prefab")
    FBZZ_FIELD_REF(PrefabRef, shellPrefab,       "Shell Casing Prefab")
    FBZZ_FIELD_FILE(sfxFire,  "", "SFX Fire",  ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxShell, "", "SFX Shell", ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxDry,   "", "SFX Dry",   ".wav,.ogg")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawSockets, false, "Draw Sockets")

    // ── 公開 API (撃つ側はこれだけ呼べばよい) ──────────────────────────────
    bool PlayFire();
    bool PlayDry();
    bool PlayReload();

    // 収納変形。Draw/Holster の入力と同時に呼ぶ (装着イベントより前から回し始める)。
    // WHY 装着イベントを待たないか: Deploy は 20F = 0.67 秒あり、Draw クリップの
    //     装着フレーム (9F / 11F) より長い。イベントで開始すると、手に収まってから
    //     おもむろに展開が始まる。先に回しておくと「取り出しながら展開する」になる。
    bool PlayDeploy();
    bool PlayFold();
    [[nodiscard]] bool IsFolded() const { return m_folded; }

    // 弾道・エフェクトの発生点。ソケットが解決できなければ銃自身の原点を返す。
    [[nodiscard]] Vector3    MuzzlePosition() const;
    [[nodiscard]] Quaternion MuzzleRotation() const;
    [[nodiscard]] Vector3    EjectPosition()  const;
    [[nodiscard]] bool       HasMuzzleSocket() const { return m_muzzle != nullptr; }

    void OnStart() override;
    void OnUpdate() override;
    void OnDrawGizmos() override;

private:
    // ソケットは銃の部分木の中だけを探す。
    // WHY: SOCKET_Muzzle は左右の銃に 1 本ずつ存在するため、scene.Find() では
    //      どちらが返るか GameObject の生成順に依存する (症状が日替わりになる)。
    void ResolveSockets();
    bool PlayClip(const std::string& file, const std::string& clipName,
                  float fadeIn, float fadeOut);
    // 発砲の見た目。撃った瞬間から数えた経過秒で順に出す。
    void SpawnMuzzleFlash();
    void SpawnShellCasing();

    GameObject* m_muzzle = nullptr;
    GameObject* m_eject  = nullptr;
    bool        m_warnedMissingSocket = false;
    // 収納形態か。Fold 再生の完了を待って保持ポーズへ移すためのステート。
    bool        m_folded = false;
    bool        m_waitingFoldEnd = false;

    // 発砲演出の進行。撃ってからの経過秒と、もう出したかのフラグ。
    // WHY 1 本のタイマーで持つか: マズル → 薬莢の 2 フレーム差はこの銃の性格そのもので、
    //     別々のタイマーにすると片方だけ調整されて間隔が崩れる。
    float m_fireTime         = -1.0f;   // 負なら発砲中でない
    bool  m_muzzleFlashDone  = false;
    bool  m_shellEjectDone   = false;
};

FBZZ_REFLECT(WeaponAnimatorComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void WeaponAnimatorComponent::OnStart()
{
    ResolveSockets();
}

inline void WeaponAnimatorComponent::ResolveSockets()
{
    GameObject* self = scene.Self();
    if (!self) return;

    const bool right = hand == HandSide::Right;
    m_muzzle = FindSocketInSubtree(*self, kSocketMuzzle,
                                   right ? kLegacySocketMuzzleR : kLegacySocketMuzzleL);
    m_eject  = FindSocketInSubtree(*self, kSocketEject,
                                   right ? kLegacySocketEjectR : kLegacySocketEjectL);

    if (!m_muzzle && !m_warnedMissingSocket) {
        // 黙って銃の原点へフォールバックすると、弾道だけがグリップから出る。
        // 見た目が「なんとなくズレている」だけなので、言わないと気付けない。
        debug.LogError("WeaponAnimatorComponent: SOCKET_Muzzle not found under this weapon. "
                       "Re-import the weapon FBX, or check the socket bone name.");
        m_warnedMissingSocket = true;
    }
}

inline bool WeaponAnimatorComponent::PlayClip(const std::string& file,
                                              const std::string& clipName,
                                              float fadeIn, float fadeOut)
{
    if (file.empty())
        return false;
    animator.PlaySlot(slotLayerName, file, clipName, fadeIn, fadeOut, 1.0f, false);
    return true;
}

inline bool WeaponAnimatorComponent::PlayFire()
{
    // 発砲演出の時計をここで 0 に戻す。連射で撃ち直されたら前回の残りは捨てる
    // (前回の薬莢が出ていなければ、それはもう遅すぎるので出さない)。
    m_fireTime        = 0.0f;
    m_muzzleFlashDone = false;
    m_shellEjectDone  = false;
    return PlayClip(fireClipFile, fireClipName, fireFadeIn, fireFadeOut);
}

inline bool WeaponAnimatorComponent::PlayDry()
{
    // 空撃ちは 1F 目のクリックだけ。遅延させる意味がないのでその場で鳴らす。
    if (!sfxDry.empty()) audio.PlayOneShot(sfxDry);
    return PlayClip(dryClipFile, dryClipName, fireFadeIn, fireFadeOut);
}

inline bool WeaponAnimatorComponent::PlayReload()
{
    return PlayClip(reloadClipFile, reloadClipName, 0.10f, 0.10f);
}

inline bool WeaponAnimatorComponent::PlayDeploy()
{
    m_folded = false;
    m_waitingFoldEnd = false;
    // 展開は Slot を止めてから流す。畳んだ保持ポーズが残っていると混ざる。
    animator.StopSlot(slotLayerName, 0.0f);
    return PlayClip(deployClipFile, deployClipName, 0.0f, 0.10f);
}

inline bool WeaponAnimatorComponent::PlayFold()
{
    if (!PlayClip(foldClipFile, foldClipName, 0.0f, 0.0f))
        return false;
    // Fold は「畳み終わった形」で止まっていてほしいが、Slot は終端でフェードアウトして
    // ベースポーズ (= 銃形態) へ戻ってしまう。終了を待って保持ポーズへ引き継ぐ。
    m_waitingFoldEnd = true;
    return true;
}

inline void WeaponAnimatorComponent::OnUpdate()
{
    // 発砲の見た目を経過秒で進める。マズルフラッシュはスライドが下がりきる前 (1F)、
    // 薬莢はその 2 フレーム後。撃った瞬間に全部出すと薬莢がスライドより先に飛ぶ。
    if (m_fireTime >= 0.0f) {
        m_fireTime += std::max(Time::deltaTime, 0.0f);
        if (!m_muzzleFlashDone && m_fireTime >= kFireMuzzleFlashDelay) {
            m_muzzleFlashDone = true;
            SpawnMuzzleFlash();
        }
        if (!m_shellEjectDone && m_fireTime >= kFireShellEjectDelay) {
            m_shellEjectDone = true;
            SpawnShellCasing();
        }
        if (m_muzzleFlashDone && m_shellEjectDone)
            m_fireTime = -1.0f;
    }

    if (!m_waitingFoldEnd) return;
    if (animator.IsSlotPlaying(slotLayerName)) return;
    m_waitingFoldEnd = false;
    m_folded = true;
    if (!foldedIdleClipFile.empty())
        animator.PlaySlot(slotLayerName, foldedIdleClipFile, foldedIdleClipName,
                          0.0f, 0.05f, 1.0f, true);
}

inline void WeaponAnimatorComponent::SpawnMuzzleFlash()
{
    if (!sfxFire.empty()) audio.PlayOneShot(sfxFire);
    if (!muzzleFlashPrefab.path.empty())
        scene.Spawn(muzzleFlashPrefab, MuzzlePosition(), MuzzleRotation());
}

inline void WeaponAnimatorComponent::SpawnShellCasing()
{
    if (!sfxShell.empty()) audio.PlayOneShot(sfxShell);
    // 飛ぶ向きは SOCKET_Eject の姿勢が持っているので、左右で分岐させる必要はない。
    if (m_eject && !shellPrefab.path.empty())
        scene.Spawn(shellPrefab, EjectPosition(), m_eject->transform.worldRotation);
}

inline Vector3 WeaponAnimatorComponent::MuzzlePosition() const
{
    if (m_muzzle && m_muzzle->IsValid())
        return m_muzzle->transform.worldPosition;
    return transform.worldPosition;
}

inline Quaternion WeaponAnimatorComponent::MuzzleRotation() const
{
    if (m_muzzle && m_muzzle->IsValid())
        return m_muzzle->transform.worldRotation;
    return transform.worldRotation;
}

inline Vector3 WeaponAnimatorComponent::EjectPosition() const
{
    if (m_eject && m_eject->IsValid())
        return m_eject->transform.worldPosition;
    return MuzzlePosition();
}

inline void WeaponAnimatorComponent::OnDrawGizmos()
{
    if (!drawSockets) return;
    if (m_muzzle) debug.DrawSphere(MuzzlePosition(), 0.02f, { 1.0f, 0.8f, 0.2f, 1.0f });
    if (m_eject)  debug.DrawSphere(EjectPosition(),  0.02f, { 0.2f, 0.8f, 1.0f, 1.0f });
    if (m_muzzle)
        debug.DrawRay(MuzzlePosition(),
                      MuzzleRotation() * Vector3::FORWARD * 0.5f,
                      { 1.0f, 0.8f, 0.2f, 1.0f });
}

} // namespace sandbox
