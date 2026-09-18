/// @file    NamedPipeClient.hpp
/// @brief   独立Editor Application間でNDJSON要求を送受信する同期Named Pipeクライアント。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#pragma once

#include <cstdint>
#include <string>

namespace fbzz::editor::ai {

class NamedPipeClient {
public:
    /// 1接続1要求で送信し、改行終端の応答を受け取る。timeoutMs以内に接続できなければfalse。
    [[nodiscard]] static bool Request(const std::wstring& pipeName,
                                      const std::string& request,
                                      std::string& response,
                                      std::uint32_t timeoutMs = 2000);
};

} // namespace fbzz::editor::ai
