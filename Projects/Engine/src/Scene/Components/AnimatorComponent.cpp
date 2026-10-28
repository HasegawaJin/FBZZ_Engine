/// @file    AnimatorComponent.cpp
/// @brief   AnimatorComponent のうち、ヘッダーへ置くと依存が重くなる診断だけを実装する。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <Engine/Scene/Components/AnimatorComponent.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>

#include <mutex>
#include <string>
#include <unordered_set>

namespace fbzz::scene {

namespace {

/// @note 既に報告した (API, レイヤー名) の組。
/// @note 名前引きに失敗する呼び出しは «毎フレーム» 来る。SetLayerWeight を Update から流している
/// @note       Script が名前を間違えると、抑止が無ければログが毎フレーム埋まり、他の手掛かりごと
/// @note       流してしまうため覚える。同じ Controller を使う敵が 20 体いても原因は 1 つで、
/// @note       20 行出しても情報は増えないため、プロセス全体で 1 つに持つ。
std::mutex& ReportedMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& ReportedKeys()
{
    static std::unordered_set<std::string> keys;
    return keys;
}

} /// @note namespace

const asset::AnimationClip& AnimatorComponent::ClipAt(std::size_t index) const
{
    static const asset::AnimationClip empty;
    if (!clips.empty()) return index < clips.size() ? clips[index] : empty;
    if (index >= sharedClips.size()) return empty;
    const auto& reference = sharedClips[index];
    if (const auto* clip = asset::AssetManager::Get(reference.clip)) return *clip;
    const auto* model = asset::AssetManager::Get(reference.model);
    return model && reference.modelClipIndex < model->clips.size()
        ? model->clips[reference.modelClipIndex] : empty;
}

void ReportUnknownAnimationLayer(const char* api,
                                 std::string_view layerName,
                                 const std::vector<AnimationLayer>& available)
{
    /// @note レイヤーが 1 本も無いのは «名前の間違い» ではなく «まだ何も入っていない» で、
    /// @note       Controller の読み込みが済む前に Script が先に走った瞬間に起きる (Script は
    /// @note       AnimatorSystem より前に動く)。ここで鳴らすと、正しい名前を書いていても
    /// @note       起動直後に必ず警告が出る。名前の取り違えは «候補があるのに一致しない» ときだけ
    /// @note       判定できるので、空のときは黙る。
    if (available.empty()) return;

    const char* apiName = api ? api : "?";
    /// @note string_view は終端されているとは限らないので、書式へ渡す前に必ず std::string にする。
    const std::string requested(layerName);

    {
        std::scoped_lock lock(ReportedMutex());
        if (!ReportedKeys().insert(std::string(apiName) + '|' + requested).second) return;
    }

    /// @note 打ち間違いは «そのレイヤーが無い» ではなく «正しい名前が何か» を出さないと直せない。
    std::string names;
    for (const auto& layer : available) {
        if (!names.empty()) names += ", ";
        names += layer.name;
    }
    if (names.empty()) names = "(none)";

    FBZZ_LOG_WARN(
        "AnimatorComponent::%s: no layer named '%s' - the call did nothing. "
        "Layers on this Animator: %s",
        apiName, requested.c_str(), names.c_str());
}

} /// @note namespace fbzz::scene
