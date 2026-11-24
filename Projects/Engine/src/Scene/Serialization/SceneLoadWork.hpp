/// @file    SceneLoadWork.hpp
/// @brief   シーン読み込みの CPU デコード中に所有スレッドのイベント処理を進める。
/// @author  Hasegawa Jin
/// @date    2026-11-10
#pragma once
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <chrono>
#include <utility>

namespace fbzz::scene::detail {

/// @pre work は分離したデータだけを扱い、Scene、AssetStore、GPU リソースに触れない。
template<class Work>
auto RunSceneLoadWork(Work&& work, void (*pump)())
{
    if (!pump || TaskSystem::WorkerCount() == 0) return work();

    auto result = TaskSystem::Submit(std::forward<Work>(work));
    while (result.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready)
        pump();
    pump();
    return result.get();
}

} /// @note namespace fbzz::scene::detail
