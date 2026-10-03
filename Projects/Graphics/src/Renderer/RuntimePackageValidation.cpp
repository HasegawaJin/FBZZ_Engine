/// @file    RuntimePackageValidation.cpp
/// @brief   PE データ export と app-local 描画ランタイムの事前検証。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Graphics/Renderer/RuntimePackageValidation.hpp>
#if FBZZ_ENABLE_DX12
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "GraphicsRuntimeContract.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
namespace fbzz::renderer {
namespace {
struct PeImage {
    std::vector<unsigned char> bytes;
    IMAGE_NT_HEADERS64 headers{};
    std::vector<IMAGE_SECTION_HEADER> sections;

    template<class T> bool Read(std::size_t offset, T& value) const
    {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return true;
    }

    bool Open(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return false;
        bytes.assign(std::istreambuf_iterator<char>(stream), {});
        IMAGE_DOS_HEADER dos{};
        if (!Read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return false;
        const auto offset = static_cast<std::size_t>(dos.e_lfanew);
        if (!Read(offset, headers) || headers.Signature != IMAGE_NT_SIGNATURE
            || headers.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
            || headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || headers.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return false;
        const auto sectionOffset = offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)
            + headers.FileHeader.SizeOfOptionalHeader;
        sections.resize(headers.FileHeader.NumberOfSections);
        for (std::size_t i = 0; i < sections.size(); ++i)
            if (!Read(sectionOffset + i * sizeof(IMAGE_SECTION_HEADER), sections[i])) return false;
        return true;
    }

    bool Offset(DWORD rva, std::size_t length, std::size_t& offset) const
    {
        if (rva < headers.OptionalHeader.SizeOfHeaders) {
            offset = rva;
            return offset <= bytes.size() && length <= bytes.size() - offset;
        }
        for (const auto& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const auto delta = static_cast<std::size_t>(rva - section.VirtualAddress);
            if (delta > section.SizeOfRawData || length > section.SizeOfRawData - delta) continue;
            offset = static_cast<std::size_t>(section.PointerToRawData) + delta;
            return offset <= bytes.size() && length <= bytes.size() - offset;
        }
        return false;
    }

    template<class T> bool ReadRva(DWORD rva, T& value) const
    {
        std::size_t offset = 0;
        return Offset(rva, sizeof(T), offset) && Read(offset, value);
    }

    bool String(DWORD rva, std::string& value) const
    {
        value.clear();
        for (std::size_t i = 0; i < 1024; ++i) {
            char character = 0;
            if (i > MAXDWORD - rva || !ReadRva(rva + static_cast<DWORD>(i), character)) return false;
            if (character == '\0') return true;
            value.push_back(character);
        }
        return false;
    }

    /// @see https://learn.microsoft.com/windows/win32/debug/pe-format Export address and name tables.
    bool Export(const char* name, DWORD& rva) const
    {
        if (headers.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
        const auto& directory = headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        IMAGE_EXPORT_DIRECTORY exports{};
        if (!directory.VirtualAddress || !ReadRva(directory.VirtualAddress, exports)) return false;
        if (exports.NumberOfNames > bytes.size() / sizeof(DWORD)) return false;
        for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
            DWORD nameRva = 0;
            WORD ordinal = 0;
            std::string exportedName;
            if (!ReadRva(exports.AddressOfNames + i * sizeof(DWORD), nameRva)
                || !String(nameRva, exportedName)) return false;
            if (exportedName != name) continue;
            if (!ReadRva(exports.AddressOfNameOrdinals + i * sizeof(WORD), ordinal)
                || ordinal >= exports.NumberOfFunctions
                || !ReadRva(exports.AddressOfFunctions + ordinal * sizeof(DWORD), rva)) return false;
            const auto end = static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size;
            return rva < directory.VirtualAddress || rva >= end;
        }
        return false;
    }
};

bool ValidateVersion(const std::filesystem::path& file, const unsigned int (&expected)[4], std::string& reason)
{
    PeImage image;
    if (!image.Open(file)) {
        reason = file.string() + ": missing or invalid x64 PE file";
        return false;
    }
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    std::vector<unsigned char> buffer(size);
    void* data = nullptr;
    UINT length = 0;
    if (!size || !GetFileVersionInfoW(file.c_str(), 0, size, buffer.data())
        || !VerQueryValueW(buffer.data(), L"\\", &data, &length)
        || length < sizeof(VS_FIXEDFILEINFO)) {
        reason = file.string() + ": file version unavailable";
        return false;
    }
    const auto* version = static_cast<const VS_FIXEDFILEINFO*>(data);
    const std::array<unsigned int, 4> actual{HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
        HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS)};
    if (!std::equal(actual.begin(), actual.end(), std::begin(expected))) {
        reason = file.string() + ": file version mismatch (found " + std::to_string(actual[0]) + "."
            + std::to_string(actual[1]) + "." + std::to_string(actual[2]) + "." + std::to_string(actual[3])
            + "; expected " + std::to_string(expected[0]) + "." + std::to_string(expected[1]) + "."
            + std::to_string(expected[2]) + "." + std::to_string(expected[3]) + ")";
        return false;
    }
    return true;
}
}

/// @see https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html#application-and-games Data export contract.
bool ValidateGraphicsRuntimePackage(const std::filesystem::path& exePath, std::string& reason)
{
    reason.clear();
    PeImage image;
    DWORD versionRva = 0;
    DWORD pathRva = 0;
    unsigned int version = 0;
    std::uint64_t pathAddress = 0;
    std::string path;
    if (!image.Open(exePath) || !image.Export("D3D12SDKVersion", versionRva)
        || !image.Export("D3D12SDKPath", pathRva) || !image.ReadRva(versionRva, version)
        || !image.ReadRva(pathRva, pathAddress) || pathAddress < image.headers.OptionalHeader.ImageBase
        || pathAddress - image.headers.OptionalHeader.ImageBase > MAXDWORD
        || !image.String(static_cast<DWORD>(pathAddress - image.headers.OptionalHeader.ImageBase), path)
        || version != runtimecontract::SDK_VERSION || path != runtimecontract::SDK_PATH) {
        reason = exePath.string() + ": Agility data exports missing or mismatched (found SDK="
            + std::to_string(version) + " path=" + path + "; expected SDK=" + std::to_string(runtimecontract::SDK_VERSION)
            + " path=" + runtimecontract::SDK_PATH + "); apply fbzz_enable_agility_sdk and relink the EXE";
        return false;
    }
    const auto root = exePath.parent_path();
    if (!ValidateVersion(root / "D3D12/D3D12Core.dll", runtimecontract::CORE_VERSION, reason)
        || !ValidateVersion(root / "dxcompiler.dll", runtimecontract::DXC_VERSION, reason)
        || !ValidateVersion(root / "dxil.dll", runtimecontract::DXC_VERSION, reason)) return false;
    std::error_code error;
    const auto layers = root / "D3D12/d3d12SDKLayers.dll";
    if (std::filesystem::exists(layers, error)
        && !ValidateVersion(layers, runtimecontract::CORE_VERSION, reason)) return false;
    return true;
}
}
#else
namespace fbzz::renderer {
bool ValidateGraphicsRuntimePackage(const std::filesystem::path&, std::string& reason)
{
    reason.clear();
    return true;
}
}
#endif
