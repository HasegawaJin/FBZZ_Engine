// GreenWare
// GreenWareScriptsDll.cpp | greenware
// スクリプト DLL のエントリポイント
//
// WHY (コールバック渡し設計):
//   fbzz_engine は shared runtime として EXE / Script DLL から共有される。
//   ただし Script DLL は任意のユーザーコードを後からロードする拡張境界なので、
//   登録 API は DLL 側からグローバル状態へ暗黙アクセスするより、EXE が渡す関数ポインタ経由にする。
//   これにより ScriptFactory の所有者をホスト側へ固定し、将来の外部プラグイン SDK 化でも
//   境界が明確なまま保てる。
//
// スクリプト追加手順:
//   1. Assets/Scripts/ に Xxx.hpp を作成 (Script 継承、TYPE_NAME 定義)
//   2. @@FBZZ_SCRIPT_INCLUDES_BEGIN/END の include を同期
//   3. Assets/Scripts/ScriptList.inl の FBZZ_SCRIPT_ENTRY(ns, Xxx) を同期
//   → Editor の AssetBrowser から "Create → C++ Script..." でも自動生成できる

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptDllAbi.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/Camera/TpsCameraComponent.hpp"
#include "Scripts/Combat/EnemyChaserComponent.hpp"
#include "Scripts/Combat/EnemyHealthBarComponent.hpp"
#include "Scripts/Combat/EnemyHealthComponent.hpp"
#include "Scripts/Data/PlayerTuning.hpp"
#include "Scripts/Data/PolarityTuning.hpp"
#include "Scripts/Game/CameraFollowManagerComponent.hpp"
#include "Scripts/Game/CameraShakeManagerComponent.hpp"
#include "Scripts/Game/CombatManagerComponent.hpp"
#include "Scripts/Game/GameFlowComponent.hpp"
#include "Scripts/Game/HitstopManagerComponent.hpp"
#include "Scripts/Game/ImpactFeedbackManagerComponent.hpp"
#include "Scripts/Game/ResultPresenterComponent.hpp"
#include "Scripts/Game/RumbleManagerComponent.hpp"
#include "Scripts/Game/ScreenEffectManagerComponent.hpp"
#include "Scripts/Game/TimeManagerComponent.hpp"
#include "Scripts/Player/AimMarkerComponent.hpp"
#include "Scripts/Player/BeamScorchComponent.hpp"
#include "Scripts/Player/CrosshairComponent.hpp"
#include "Scripts/Player/EyeBlinkComponent.hpp"
#include "Scripts/Player/PlayerAimComponent.hpp"
#include "Scripts/Player/PlayerComponent.hpp"
#include "Scripts/Player/PlayerControllerComponent.hpp"
#include "Scripts/Player/PlayerHeadLookComponent.hpp"
#include "Scripts/Player/PlayerHealthBarComponent.hpp"
#include "Scripts/Player/PlayerHealthComponent.hpp"
#include "Scripts/Player/PolarityGunComponent.hpp"
#include "Scripts/Player/PolarityGunHudComponent.hpp"
#include "Scripts/Player/WeaponAnimatorComponent.hpp"
#include "Scripts/Player/WeaponRigComponent.hpp"
#include "Scripts/Polarity/PolarityBodyComponent.hpp"
#include "Scripts/Polarity/PolarityFieldComponent.hpp"
#include "Scripts/Polarity/PolarityTargetComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

// DataAsset 型を DataAssetFactory へ直接自己登録する (純共有 ScriptableObject)。
#define FBZZ_DATA_ASSET_ENTRY(ns, T) FBZZ_REGISTER_DATA_ASSET(::ns::T)
#include "Scripts/DataAssetList.inl"
#undef FBZZ_DATA_ASSET_ENTRY

#ifdef GAMESCRIPTS_EXPORTS
#  define GAMESCRIPTS_API __declspec(dllexport)
#else
#  define GAMESCRIPTS_API __declspec(dllimport)
#endif

namespace {

struct ScriptEntry {
    std::string name;
    std::function<std::unique_ptr<fbzz::scene::Script>()> factory;
};

// WHY: エントリは Assets/Scripts/ScriptList.inl で一元管理する。
//      ScriptCodeGen は ScriptList.inl と include ブロックを同期するため、このファイルのエントリは手動編集不要。
const std::vector<ScriptEntry>& AllEntries()
{
    static const std::vector<ScriptEntry> entries = {
#define FBZZ_SCRIPT_ENTRY(ns, T) \
        { ::ns::T::TYPE_NAME, []() { return std::make_unique<::ns::T>(); } },
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY
    };
    return entries;
}

} // namespace

extern "C" {

// ホストと DLL の型レイアウトが一致する場合だけ ScriptFactory 登録を許可する。
// WHY: 個別フィールドを返すことで ValidateAbi() がミスマッチ箇所をログに出力できる。
GAMESCRIPTS_API fbzz::scene::ScriptDllAbiInfo FBZZScripts_GetAbiInfo()
{
    return fbzz::scene::GetScriptDllAbiInfo();
}

GAMESCRIPTS_API int FBZZScripts_Count()
{
    return static_cast<int>(AllEntries().size());
}

GAMESCRIPTS_API const char* FBZZScripts_TypeName(int i)
{
    const auto& entries = AllEntries();
    if (i < 0 || i >= static_cast<int>(entries.size())) return "";
    return entries[static_cast<size_t>(i)].name.c_str();
}

// WHY: FBZZScripts_Register はエンジン共通のエントリポイント名。
//      ScriptDllLoader はこの名前だけを探すため、プロジェクト固有名を使わない。
GAMESCRIPTS_API void FBZZScripts_Register(
    void(*registerFn)(const char* typeName, std::function<std::unique_ptr<fbzz::scene::Script>()>))
{
    if (!registerFn) return;
    for (const auto& entry : AllEntries())
        registerFn(entry.name.c_str(), entry.factory);
}

} // extern "C"
