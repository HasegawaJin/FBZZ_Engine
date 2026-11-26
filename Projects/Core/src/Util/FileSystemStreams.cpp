/// @file    FileSystemStreams.cpp
/// @brief   Win32 パスを使ったテキストとバイナリの入出力。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Core/Util/FileSystem.hpp>
#include <Core/Util/StringUtils.hpp>
#include <sstream>

namespace fbzz::util {

namespace {

bool ReadStreamText(std::ifstream& stream, std::string& outText)
{
    if (!stream.is_open()) {
        return false;
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    outText = contents.str();
    return true;
}

bool WriteStreamText(std::ofstream& stream, const std::string& text)
{
    if (!stream.is_open()) {
        return false;
    }
    stream << text;
    // バッファの書き出しに伴う失敗も close() 後に確認する。
    stream.close();
    return stream.good();
}

}

bool FileSystem::ReadText(const std::string& path, std::string& outText)
{
    const std::wstring widePath = StringUtils::ToWide(path);
    if (widePath.empty()) {
        return false;
    }

    // 文字列版はテキストモードで CRLF を LF に変換する。
    std::ifstream stream(widePath);
    return ReadStreamText(stream, outText);
}

bool FileSystem::WriteText(const std::string& path, const std::string& text)
{
    const std::wstring widePath = StringUtils::ToWide(path);
    if (widePath.empty()) {
        return false;
    }

    // 文字列版はテキストモードで LF を CRLF に変換する。
    std::ofstream stream(widePath);
    return WriteStreamText(stream, text);
}

bool FileSystem::ReadText(const std::filesystem::path& path, std::string& outText)
{
    std::ifstream stream(path, std::ios::binary);
    return ReadStreamText(stream, outText);
}

bool FileSystem::WriteText(const std::filesystem::path& path, const std::string& text)
{
    if (!EnsureParentDirectory(path)) {
        return false;
    }
    std::ofstream stream(path, std::ios::binary);
    return WriteStreamText(stream, text);
}

std::ofstream FileSystem::OpenBinaryWriter(const std::filesystem::path& path)
{
    if (!EnsureParentDirectory(path)) {
        return {};
    }
    return std::ofstream(path, std::ios::binary);
}

bool FileSystem::ReadBinary(const std::filesystem::path& path, std::vector<uint8_t>& outBytes)
{
    outBytes.clear();

    // バイナリアセットの読み込み経路を FileSystem に集約し、Hub/Editor/Engine で
    // Windows の wchar_t パス対応と失敗時 bool 戻り値の方針を揃える。
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return false;
    }

    const std::streamsize byteCount = stream.tellg();
    if (byteCount <= 0) {
        return false;
    }

    outBytes.resize(static_cast<size_t>(byteCount));
    stream.seekg(0, std::ios::beg);
    stream.read(static_cast<char*>(static_cast<void*>(outBytes.data())), byteCount);
    if (!stream.good()) {
        outBytes.clear();
        return false;
    }

    return true;
}

bool FileSystem::WriteBinary(const std::filesystem::path& path, const void* data, size_t byteCount)
{
    if (!data && byteCount > 0) {
        return false;
    }
    std::ofstream stream = OpenBinaryWriter(path);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(byteCount));
    return stream.good();
}

}
