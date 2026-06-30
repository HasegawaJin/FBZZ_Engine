// FBZZ Engine
// ScriptCodeGen.cpp | fbzz::editor
// エディター内からのソースコード生成ユーティリティ
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

namespace {

struct ScriptRegistration {
    std::string namespaceName;
    std::string className;
    std::string headerName;
};

std::string Trim(std::string_view s)
{
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const size_t end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(begin, end - begin + 1));
}

std::string ParseNamespaceLine(const std::string& line)
{
    const std::string trimmed = Trim(line);
    if (!trimmed.starts_with("namespace ")) return {};

    const size_t begin = std::string_view("namespace ").size();
    size_t end = trimmed.find_first_of(" {", begin);
    if (end == std::string::npos) end = trimmed.size();
    return trimmed.substr(begin, end - begin);
}

// ファイルを全行読み込む
std::vector<std::string> ReadLines(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return {};
    std::vector<std::string> lines;
    std::string line;
    std::istringstream input(text);
    while (std::getline(input, line))
        lines.push_back(line);
    return lines;
}

// 行リストをファイルに書き出す
bool WriteLines(const std::string& path, const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (const auto& l : lines) {
        output << l << '\n';
    }
    return util::FileSystem::WriteText(path, output.str());
}

// マーカー間の自動生成ブロックを丸ごと置き換える。
// WHY: ファイル削除時も古い include / 登録エントリを確実に消すため、追記ではなく同期で扱う。
bool ReplaceGeneratedBlock(const std::string& path,
                           const std::string& beginMarker,
                           const std::string& endMarker,
                           const std::vector<std::string>& generatedLines)
{
    auto lines = ReadLines(path);
    if (lines.empty()) return false;

    size_t beginIndex = lines.size();
    size_t endIndex = lines.size();
    const std::string beginText = "// " + beginMarker;
    const std::string endText = "// " + endMarker;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(lines[i]);
        if (beginIndex == lines.size() && trimmed.starts_with(beginText)) {
            beginIndex = i;
            continue;
        }
        if (beginIndex != lines.size() && trimmed.starts_with(endText)) {
            endIndex = i;
            break;
        }
    }

    if (beginIndex == lines.size() || endIndex == lines.size() || beginIndex >= endIndex) {
        FBZZ_LOG_WARN("ScriptCodeGen: marker block '%s' not found in %s",
                      beginMarker.c_str(), path.c_str());
        return false;
    }

    std::vector<std::string> next;
    next.reserve(lines.size() + generatedLines.size());
    next.insert(next.end(), lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(beginIndex + 1));
    next.insert(next.end(), generatedLines.begin(), generatedLines.end());
    next.insert(next.end(), lines.begin() + static_cast<std::ptrdiff_t>(endIndex), lines.end());

    if (next == lines) return true;
    return WriteLines(path, next);
}

// FBZZ_SCRIPT( / FBZZ_DATA_ASSET( のような「マクロ名(」を走査して登録情報を集める汎用版。
// WHY: スクリプトとデータアセットは同じヘッダ群に同じ構文で宣言されるため、走査トークンだけ
//      差し替えれば同じ収集ロジックを使い回せる (DRY)。
std::vector<ScriptRegistration> CollectRegistrations(const std::string& scriptsDir,
                                                     const std::string& macroToken)
{
    std::vector<ScriptRegistration> registrations;
    if (scriptsDir.empty()) return registrations;

    const std::filesystem::path scriptsRoot = util::FileSystem::PathFromUtf8(scriptsDir);
    const std::filesystem::path assetsRoot =
        scriptsRoot.filename() == L"Scripts" ? scriptsRoot.parent_path() : scriptsRoot;
    if (!util::FileSystem::Exists(assetsRoot)) return registrations;

    for (const auto& path : util::FileSystem::ListFilesRecursive(assetsRoot)) {
        if (path.extension() != L".hpp") continue;

        const std::string includePath = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(util::FileSystem::RelativePath(path, assetsRoot)));
        if (includePath.ends_with(".generated.hpp")) continue;

        std::string text;
        if (!util::FileSystem::ReadText(path, text)) continue;

        std::string currentNamespace = "sandbox";
        std::string line;
        std::istringstream input(text);
        while (std::getline(input, line)) {
            if (const std::string ns = ParseNamespaceLine(line); !ns.empty())
                currentNamespace = ns;

            size_t pos = 0;
            while ((pos = line.find(macroToken, pos)) != std::string::npos) {
                const size_t begin = pos + macroToken.size();
                const size_t end = line.find(')', begin);
                if (end == std::string::npos) break;

                const std::string className = Trim(std::string_view(line).substr(begin, end - begin));
                if (!className.empty()) {
                    registrations.push_back({
                        currentNamespace.empty() ? "sandbox" : currentNamespace,
                        className,
                        includePath
                    });
                }
                pos = end + 1;
            }
        }
    }

    std::sort(registrations.begin(), registrations.end(),
        [](const ScriptRegistration& a, const ScriptRegistration& b) {
            if (a.headerName != b.headerName) return a.headerName < b.headerName;
            if (a.namespaceName != b.namespaceName) return a.namespaceName < b.namespaceName;
            return a.className < b.className;
        });
    registrations.erase(std::unique(registrations.begin(), registrations.end(),
        [](const ScriptRegistration& a, const ScriptRegistration& b) {
            return a.namespaceName == b.namespaceName &&
                   a.className == b.className &&
                   a.headerName == b.headerName;
        }), registrations.end());

    return registrations;
}

bool EnsureDirectoriesRecursive(const std::string& path)
{
    if (path.empty()) return false;

    return util::FileSystem::EnsureDirectory(util::FileSystem::PathFromUtf8(path));
}

// C++ スクリプトのテンプレートを生成する (新方式: 自己登録リフレクション + 1 ファイル inline)
// WHY: 旧方式の .generated.hpp / 専用 .cpp / _IMPL ガードを廃止。
//      FBZZ_FIELD でフィールドを足すだけで Inspector/シリアライズが自動追従し、
//      FBZZ_REFLECT が Reflect() を生成する。実装は inline で同ヘッダに書く。
std::string BuildScriptTemplate(const std::string& name)
{
    const std::string className = name + "Component";

    std::ostringstream ss;
    ss << "// FBZZ Engine\n";
    ss << "// " << className << ".hpp | sandbox\n";
    ss << "#pragma once\n";
    ss << "\n";
    ss << "#include <Engine/Scene/Script.hpp>\n";
    ss << "\n";
    ss << "using namespace fbzz::scene;\n";
    ss << "using namespace fbzz::math;\n";
    ss << "using namespace fbzz::input;\n";
    ss << "using fbzz::Time;\n";
    ss << "\n";
    ss << "namespace sandbox {\n";
    ss << "\n";
    ss << "class " << className << " : public Script {\n";
    ss << "    FBZZ_SCRIPT(" << className << ")\n";
    ss << "public:\n";
    ss << "    // フィールドはここに書くだけで Inspector / シリアライズに自動反映される。\n";
    ss << "    // 表示名 \"\" は変数名から自動生成される (例: speed -> \"Speed\")。\n";
    ss << "    // FBZZ_FIELD(float, speed, 5.0f, \"\")\n";
    ss << "    // FBZZ_REF(GameObject, target, \"Target\")  // ドラッグ&ドロップ参照\n";
    ss << "\n";
    ss << "    void OnUpdate() override;\n";
    ss << "};\n";
    ss << "\n";
    ss << "// Reflect() をフィールド宣言から自動生成する (旧 .generated.hpp の置き換え)。\n";
    ss << "FBZZ_REFLECT(" << className << ")\n";
    ss << "\n";
    ss << "// ── 実装 (inline) ──\n";
    ss << "inline void " << className << "::OnUpdate()\n";
    ss << "{\n";
    ss << "}\n";
    ss << "\n";
    ss << "} // namespace sandbox\n";
    return ss.str();
}

// Surface VS+PS シェーダーテンプレートを生成する
std::string BuildSurfaceHlslTemplate(const std::string& name)
{
    std::ostringstream ss;
    ss << "// FBZZ Engine\n";
    ss << "// Material/Custom/" << name << ".hlsl | Material\n";
    ss << "// " << name << " カスタムマテリアルシェーダー\n";
    ss << "#ifndef " << name << "_HLSL\n";
    ss << "#define " << name << "_HLSL\n";
    ss << "\n";
    ss << "#include \"Common/Constants.hlsli\"\n";
    ss << "#include \"Common/Structs.hlsli\"\n";
    ss << "#include \"Platform/DX11.hlsli\"\n";
    ss << "#include \"Rendering/Lighting.hlsli\"\n";
    ss << "\n";
    ss << "cbuffer MaterialConstants : register(CB_MATERIAL)\n";
    ss << "{\n";
    ss << "    float4 albedo;      // RGBA ベースカラー\n";
    ss << "    uint   textureMask; // テクスチャフラグ\n";
    ss << "};\n";
    ss << "\n";
    ss << "Texture2D    texAlbedo   : register(TEX_ALBEDO);\n";
    ss << "SamplerState sampDefault : register(SAMPLER_DEFAULT);\n";
    ss << "\n";
    ss << "PSInput VSMain(VSInput v)\n";
    ss << "{\n";
    ss << "    PSInput o;\n";
    ss << "    float4 worldPos4 = mul(float4(v.position, 1.0f), world);\n";
    ss << "    o.worldPos   = worldPos4.xyz;\n";
    ss << "    o.svPosition = mul(worldPos4, viewProjection);\n";
    ss << "    o.normal     = normalize(mul(v.normal,  (float3x3)worldInvTranspose));\n";
    ss << "    o.tangent    = normalize(mul(v.tangent, (float3x3)world));\n";
    ss << "    o.uv         = v.uv;\n";
    ss << "    return o;\n";
    ss << "}\n";
    ss << "\n";
    ss << "float4 PSMain(PSInput p) : SV_Target0\n";
    ss << "{\n";
    ss << "    float3 color = (textureMask & 1u)\n";
    ss << "        ? texAlbedo.Sample(sampDefault, p.uv).rgb\n";
    ss << "        : albedo.rgb;\n";
    ss << "    // TODO: ライティング計算をここに追加する\n";
    ss << "    return float4(color, 1.0f);\n";
    ss << "}\n";
    ss << "\n";
    ss << "#endif // " << name << "_HLSL\n";
    return ss.str();
}

// PostProcess フルスクリーン VS+PS テンプレートを生成する
std::string BuildPostProcessHlslTemplate(const std::string& name)
{
    std::ostringstream ss;
    ss << "// FBZZ Engine\n";
    ss << "// PostProcess/Custom/" << name << ".hlsl | PostProcess\n";
    ss << "// " << name << " カスタムポストプロセスシェーダー\n";
    ss << "#ifndef " << name << "_HLSL\n";
    ss << "#define " << name << "_HLSL\n";
    ss << "\n";
    ss << "#include \"Common/Constants.hlsli\"\n";
    ss << "#include \"Common/Structs.hlsli\"\n";
    ss << "#include \"Platform/DX11.hlsli\"\n";
    ss << "\n";
    ss << "Texture2D    texScene    : register(t0);\n";
    ss << "SamplerState sampDefault : register(SAMPLER_DEFAULT);\n";
    ss << "\n";
    ss << "// フルスクリーン三角形用 VS (ジオメトリ不要)\n";
    ss << "PSFullscreenInput VSMain(uint id : SV_VertexID)\n";
    ss << "{\n";
    ss << "    PSFullscreenInput o;\n";
    ss << "    o.uv         = float2((id << 1) & 2, id & 2);\n";
    ss << "    o.svPosition = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n";
    ss << "    return o;\n";
    ss << "}\n";
    ss << "\n";
    ss << "float4 PSMain(PSFullscreenInput p) : SV_Target0\n";
    ss << "{\n";
    ss << "    float4 color = texScene.Sample(sampDefault, p.uv);\n";
    ss << "    // TODO: エフェクトをここに追加する\n";
    ss << "    return color;\n";
    ss << "}\n";
    ss << "\n";
    ss << "#endif // " << name << "_HLSL\n";
    return ss.str();
}

// Compute Shader テンプレートを生成する
std::string BuildComputeHlslTemplate(const std::string& name)
{
    std::ostringstream ss;
    ss << "// FBZZ Engine\n";
    ss << "// PostProcess/Custom/" << name << ".cs.hlsl | Compute\n";
    ss << "// " << name << " カスタムコンピュートシェーダー\n";
    ss << "#ifndef " << name << "_CS_HLSL\n";
    ss << "#define " << name << "_CS_HLSL\n";
    ss << "\n";
    ss << "#include \"Common/Constants.hlsli\"\n";
    ss << "#include \"Platform/DX11.hlsli\"\n";
    ss << "\n";
    ss << "RWTexture2D<float4> outputTex : register(u0);\n";
    ss << "Texture2D           inputTex  : register(t0);\n";
    ss << "SamplerState        sampPoint : register(SAMPLER_DEFAULT);\n";
    ss << "\n";
    ss << "[numthreads(8, 8, 1)]\n";
    ss << "void CSMain(uint3 id : SV_DispatchThreadID)\n";
    ss << "{\n";
    ss << "    uint2 dim;\n";
    ss << "    outputTex.GetDimensions(dim.x, dim.y);\n";
    ss << "    if (id.x >= dim.x || id.y >= dim.y) return;\n";
    ss << "\n";
    ss << "    float2 uv    = (float2(id.xy) + 0.5f) / float2(dim);\n";
    ss << "    float4 color = inputTex.SampleLevel(sampPoint, uv, 0);\n";
    ss << "    // TODO: 処理をここに追加する\n";
    ss << "    outputTex[id.xy] = color;\n";
    ss << "}\n";
    ss << "\n";
    ss << "#endif // " << name << "_CS_HLSL\n";
    return ss.str();
}

} // namespace

// =============================================================================
// 公開 API
// =============================================================================

std::string ScriptCodeGen::CreateScript(const std::string& name,
                                        const std::string& scriptsDir,
                                        const std::string& dllCppPath,
                                        const std::string& staticCppPath)
{
    if (name.empty() || scriptsDir.empty()) return {};

    const std::string className  = name + "Component";
    const std::string headerName = className + ".hpp";
    const std::string headerPath = scriptsDir + "/" + headerName;

    // 重複チェック
    if (util::FileSystem::Exists(headerPath)) {
        FBZZ_LOG_WARN("ScriptCodeGen: %s already exists", headerPath.c_str());
        return {};
    }

    // .hpp テンプレートを書き出す
    if (!EnsureDirectoriesRecursive(scriptsDir)) {
        FBZZ_LOG_ERROR("ScriptCodeGen: failed to create Scripts directory: %s", scriptsDir.c_str());
        return {};
    }
    if (!util::FileSystem::WriteText(headerPath, BuildScriptTemplate(name))) {
        FBZZ_LOG_ERROR("ScriptCodeGen: failed to write file: %s", headerPath.c_str());
        return {};
    }

    // 新方式: .generated.hpp も専用 .cpp も生成しない。
    // WHY: Reflect() は FBZZ_REFLECT がヘッダ内で生成し、実装は inline 化したため
    //      外部生成ファイルや独立 TU が不要になった (1 スクリプト = 1 ファイル)。
    //      DLL/EXE エントリがヘッダを include するだけで実装も取り込まれる。

    // WHY: 生成後は追記ではなく Scripts/ の実ファイル一覧から再同期する。
    //      これにより、削除済みスクリプトの古い登録も同じ経路で消せる。
    SyncScriptRegistry(scriptsDir, dllCppPath, staticCppPath);

    FBZZ_LOG_INFO("ScriptCodeGen: script generated -> %s", headerPath.c_str());
    return headerPath;
}

bool ScriptCodeGen::SyncScriptRegistry(const std::string& scriptsDir,
                                       const std::string& dllCppPath,
                                       const std::string& staticCppPath)
{
    if (scriptsDir.empty()) return false;

    // スクリプトとデータアセットを別々に収集する (同じヘッダ群を別トークンで走査)。
    const auto scripts    = CollectRegistrations(scriptsDir, "FBZZ_SCRIPT(");
    const auto dataAssets = CollectRegistrations(scriptsDir, "FBZZ_DATA_ASSET(");

    bool ok = true;

    // include ブロックは「スクリプト or データアセットを宣言する全ヘッダ」の和集合。
    // WHY: DataAsset 専用ヘッダ (FBZZ_SCRIPT を持たない) も DLL/EXE の TU に取り込む必要があるため、
    //      両者のヘッダをマージし、重複を排除してから #include 行を作る。
    std::vector<std::string> headers;
    headers.reserve(scripts.size() + dataAssets.size());
    for (const auto& reg : scripts)    headers.push_back(reg.headerName);
    for (const auto& reg : dataAssets) headers.push_back(reg.headerName);
    std::sort(headers.begin(), headers.end());
    headers.erase(std::unique(headers.begin(), headers.end()), headers.end());

    std::vector<std::string> includeLines;
    includeLines.reserve(headers.size());
    for (const auto& h : headers)
        includeLines.push_back("#include \"" + h + "\"");

    if (!dllCppPath.empty() && util::FileSystem::Exists(dllCppPath)) {
        ok &= ReplaceGeneratedBlock(dllCppPath,
                                    "@@FBZZ_SCRIPT_INCLUDES_BEGIN",
                                    "@@FBZZ_SCRIPT_INCLUDES_END",
                                    includeLines);
    }

    if (!staticCppPath.empty() && util::FileSystem::Exists(staticCppPath)) {
        ok &= ReplaceGeneratedBlock(staticCppPath,
                                    "@@FBZZ_SCRIPT_INCLUDES_BEGIN",
                                    "@@FBZZ_SCRIPT_INCLUDES_END",
                                    includeLines);
    }

    const std::string scriptListPath = scriptsDir + "/ScriptList.inl";
    if (util::FileSystem::Exists(scriptListPath)) {
        std::vector<std::string> entryLines;
        entryLines.reserve(scripts.size());
        for (const auto& reg : scripts) {
            entryLines.push_back("FBZZ_SCRIPT_ENTRY(" + reg.namespaceName + ", " + reg.className + ")");
        }

        ok &= ReplaceGeneratedBlock(scriptListPath,
                                    "@@FBZZ_SCRIPT_ENTRIES_BEGIN",
                                    "@@FBZZ_SCRIPT_ENTRIES_END",
                                    entryLines);
    }

    // DataAsset 登録リスト。DataAssetList.inl が無いプロジェクトでは何もしない (後方互換)。
    const std::string dataAssetListPath = scriptsDir + "/DataAssetList.inl";
    if (util::FileSystem::Exists(dataAssetListPath)) {
        std::vector<std::string> entryLines;
        entryLines.reserve(dataAssets.size());
        for (const auto& reg : dataAssets) {
            entryLines.push_back("FBZZ_DATA_ASSET_ENTRY(" + reg.namespaceName + ", " + reg.className + ")");
        }

        ok &= ReplaceGeneratedBlock(dataAssetListPath,
                                    "@@FBZZ_DATA_ASSET_ENTRIES_BEGIN",
                                    "@@FBZZ_DATA_ASSET_ENTRIES_END",
                                    entryLines);
    }

    if (ok) {
        FBZZ_LOG_INFO("ScriptCodeGen: synced registry (%d scripts, %d data assets)",
                      static_cast<int>(scripts.size()),
                      static_cast<int>(dataAssets.size()));
    }
    return ok;
}

std::string ScriptCodeGen::CreateHlsl(const std::string& name,
                                      const std::string& hlslDir,
                                      HlslKind kind)
{
    if (name.empty() || hlslDir.empty()) return {};

    std::string subDir;
    std::string fileName;
    std::string content;

    switch (kind) {
    case HlslKind::SurfaceVSPS:
        subDir   = hlslDir + "/Material/Custom";
        fileName = name + ".hlsl";
        content  = BuildSurfaceHlslTemplate(name);
        break;
    case HlslKind::PostProcessVSPS:
        subDir   = hlslDir + "/PostProcess/Custom";
        fileName = name + ".hlsl";
        content  = BuildPostProcessHlslTemplate(name);
        break;
    case HlslKind::ComputeCS:
        subDir   = hlslDir + "/PostProcess/Custom";
        fileName = name + ".cs.hlsl";
        content  = BuildComputeHlslTemplate(name);
        break;
    }

    if (!EnsureDirectoriesRecursive(subDir)) {
        FBZZ_LOG_ERROR("ScriptCodeGen: failed to create HLSL directory: %s", subDir.c_str());
        return {};
    }
    const std::string outPath = subDir + "/" + fileName;

    if (util::FileSystem::Exists(outPath)) {
        FBZZ_LOG_WARN("ScriptCodeGen: %s already exists", outPath.c_str());
        return {};
    }

    if (!util::FileSystem::WriteText(outPath, content)) {
        FBZZ_LOG_ERROR("ScriptCodeGen: failed to write file: %s", outPath.c_str());
        return {};
    }

    FBZZ_LOG_INFO("ScriptCodeGen: HLSL generated -> %s", outPath.c_str());
    return outPath;
}

// =============================================================================
// 内部実装
// =============================================================================

} // namespace fbzz::editor
