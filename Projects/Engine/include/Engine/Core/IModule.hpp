/// @file    IModule.hpp
/// @brief   Application の共通メインループから呼び出される実行単位インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

namespace fbzz::core {

/// @brief Application::Run() が駆動するゲーム / エディタ固有処理。
/// @note Window / Input / Time / Memory / Profiler の共通フレーム処理を Application に集約し、Module は «何を更新・描画するか» だけを持つ。
class IModule {
public:
    virtual ~IModule() = default;

    /// @brief ループ開始前に 1 回だけ呼ばれる。
    /// @return false なら OnShutdown() を呼んだうえでループに入らず終了する。
    [[nodiscard]] virtual bool OnInit() = 0;

    /// @brief OS の入力を読み終えた直後、アクション層 (InputActionMap) を評価する前に呼ばれる。
    /// @note 入力を注入する側 (Playtest の再生) はここで書く。OnUpdate で書くとアクション層に届くのが 1 フレーム遅れる。
    virtual void OnInputPolled() {}

    /// @brief 毎フレームの主更新。
    /// @note ゲームの Scene / Script / Physics 更新順は ProjectRuntime::Update() へ委譲する。Module が直接駆動すると Editor と Standalone の差が再発する。
    virtual void OnUpdate(float dt) = 0;

    /// @brief OnUpdate() の後の遅延更新 (アニメーション・IK・LateUpdate 相当)。
    virtual void OnLateUpdate(float dt) = 0;

    /// @brief 描画。renderer.BeginFrame() から EndFrame() までを持つ。
    virtual void OnRender() = 0;

    /// @brief ループ終了後、Application::Shutdown() より前に 1 回だけ呼ばれる。
    virtual void OnShutdown() = 0;
};

} // namespace fbzz::core
