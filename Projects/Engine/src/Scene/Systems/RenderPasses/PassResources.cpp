/// @file    PassResources.cpp
/// @brief   未申告アクセスの検出と報告。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <Engine/Scene/Systems/RenderPasses/PassResources.hpp>

#include <Engine/Core/Logger.hpp>

#include <cassert>
#include <string>
#include <unordered_set>

#include <Windows.h>

namespace fbzz::scene {

namespace {

/// 未申告アクセスを «落ちる» 側に倒すか。環境変数 `FBZZ_RENDER_STRICT_DECLARATIONS`。
///
/// WHY 既定で落とさないか: 申告漏れは «絵は正しいが依存辺が張られていない» という形で
///     入るので、見つけた時点で全部塞げているとは限らない。塞ぎ切る前に落とすと
///     エディタが起動しなくなり、直す作業そのものができない。
/// WHY 警告で終わらせないか: 依存辺が無いパスは «順序が偶然に依存している» 状態で、
///     パスを 1 本足しただけで描き先が変わりうる。ログは流れて読まれなくなるので、
///     塞ぎ終えた後は落ちる側へ倒せる口を用意しておく。
bool StrictDeclarations()
{
    static const bool strict = [] {
        wchar_t     value[8]{};
        const DWORD length = GetEnvironmentVariableW(L"FBZZ_RENDER_STRICT_DECLARATIONS", value, 8);
        const bool  on     = (length > 0 && length < 8) && value[0] != L'0';
        if (on) {
            FBZZ_LOG_WARN("PassResources: 未申告アクセスで停止します "
                          "(切るには FBZZ_RENDER_STRICT_DECLARATIONS=0)");
        }
        return on;
    }();
    return strict;
}

} // namespace

bool PassResources::IsDeclared(std::string_view name) const
{
    for (const auto& access : m_accesses) {
        if (access.name == name)
            return true;
    }
    return false;
}

void PassResources::WarnIfUndeclared(std::string_view name) const
{
    if (IsDeclared(name)) return;
    // グラフの外から引いた場合 (反射プローブ捕捉など) は申告する相手が居ない。
    // 依存も順序も存在しないので、報告する意味が無い。
    if (m_accesses.empty()) return;

    const int passLen = static_cast<int>(m_passName.size());
    const int nameLen = static_cast<int>(name.size());

    if (StrictDeclarations()) {
        FBZZ_LOG_ERROR("PassResources: pass \"%.*s\" が申告していない \"%.*s\" を引きました。"
                       "Setup へ Read/Write を足すこと",
                       passLen, m_passName.data(), nameLen, name.data());
        assert(false && "undeclared render resource access (FBZZ_RENDER_STRICT_DECLARATIONS)");
        return;
    }

    // 同じ組み合わせで毎フレーム出すとログが読めなくなるので 1 回だけ。
    // WHY static を許すか: 診断の «もう言ったか» だけで、GPU の状態は持たない。
    static std::unordered_set<std::string> reported;
    std::string key(m_passName);
    key += " -> ";
    key += name;
    if (!reported.insert(key).second) return;

    FBZZ_LOG_WARN("PassResources: pass \"%.*s\" が申告していない \"%.*s\" を引きました。"
                  "Setup へ Read/Write を足すこと (依存辺が張られず、順序が偶然に依存します)",
                  passLen, m_passName.data(), nameLen, name.data());
}

} // namespace fbzz::scene
