// FBZZ Engine
// ScriptAnimatorProxy.hpp | fbzz::scene
// Script から AnimatorComponent を操作するショートハンド
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Script;

struct ScriptAnimatorProxy {
    Script* script = nullptr;

    void SetFloat(std::string_view name, float v) const;
    void SetInt(std::string_view name, int v) const;
    void SetBool(std::string_view name, bool v) const;
    void SetTrigger(std::string_view name) const;
    bool IsInState(std::string_view name) const;
    void SetFloat(GameObject* go, std::string_view name, float v) const;
    void SetInt(GameObject* go, std::string_view name, int v) const;
    void SetBool(GameObject* go, std::string_view name, bool v) const;
    void SetTrigger(GameObject* go, std::string_view name) const;
    bool IsInState(GameObject* go, std::string_view name) const;

    float       GetFloat(std::string_view name) const;
    int         GetInt  (std::string_view name) const;
    bool        GetBool (std::string_view name) const;
    float       GetNormalizedTime() const;
    float       GetFloat(GameObject* go, std::string_view name) const;
    int         GetInt  (GameObject* go, std::string_view name) const;
    bool        GetBool (GameObject* go, std::string_view name) const;
    float       GetNormalizedTime(GameObject* go) const;
    std::vector<std::pair<std::string, float>> GetCurrentBlendWeights() const;
    std::string GetCurrentState() const;
    // クロスフェード遷移先ステート名 (遷移中でなければ空) とその正規化時間 0..1。
    // コンボの Slash→Slash 遷移中に次段の振りタイミングを正しく判定するために使う。
    std::string GetBlendToState() const;
    std::string GetBlendToState(GameObject* go) const;
    float       GetBlendToNormalizedTime() const;
    float       GetBlendToNormalizedTime(GameObject* go) const;
    void        SetSpeed(float speed) const;
    void        Play(std::string_view stateName) const;
    void        SetSpeed(GameObject* go, float speed) const;
    void        Play(GameObject* go, std::string_view stateName) const;
};

} // namespace fbzz::scene
