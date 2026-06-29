// FBZZ Engine
// ScriptCodeGen.cpp | fbzz::editor
// エディター内からのソースコード生成ユーティリティ
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace fbzz::editor {

namespace {

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

// 既存ファイル内のマーカー行の次に新しい行を挿入する
// marker: 検索するマーカー文字列 (BEGIN マーカー)
bool InsertAfterMarker(const std::string& path,
                       const std::string& marker,
                       const std::string& newLine)
{
    auto lines = ReadLines(path);
    if (lines.empty()) return false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const size_t first = lines[i].find_first_not_of(" \t");
        const std::string_view line = (first == std::string::npos)
            ? std::string_view{}
            : std::string_view(lines[i]).substr(first);
        const std::string expectedMarkerLine = "// " + marker;
        if (line.starts_with(std::string_view(expectedMarkerLine))) {
            size_t insertIndex = i + 1;
            if (marker == "@@FBZZ_SCRIPT_ENTRIES_BEGIN" &&
                insertIndex < lines.size() &&
                lines[insertIndex].find("static const std::vector<ScriptEntry> entries = {") != std::string::npos) {
                ++insertIndex;
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertIndex), newLine);
            return WriteLines(path, lines);
        }
    }
    FBZZ_LOG_WARN("ScriptCodeGen: marker '%s' not found in %s",
                  marker.c_str(), path.c_str());
    return false;
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

    const std::string relInclude = "\"Scripts/" + headerName + "\"";

    // スクリプトエントリの登録先を決定する。
    // 新形式: scriptsDir/ScriptList.inl が存在すれば一元管理ファイルへ挿入 (EXE/DLL 共通)。
    // 旧形式: ScriptList.inl がない古いプロジェクトは従来通り DLL/EXE の各 .cpp へ挿入。
    const std::string scriptListPath = scriptsDir + "/ScriptList.inl";
    const bool hasScriptList = util::FileSystem::Exists(
        util::FileSystem::PathFromUtf8(scriptListPath));

    // WHY: ScriptList.inl の登録と DLL/EXE 側 include は独立して壊れ得る。
    //      ScriptList に名前があるだけで include 追記をスキップすると、
    //      ファクトリ展開時に型が見えず Script DLL のビルドが失敗する。
    bool scriptListHasEntry = hasScriptList && AlreadyRegistered(scriptListPath, className);

    // DLL 側 include 挿入 (形式: #define Xxx_IMPL + #include "Scripts/Xxx.hpp")
    if (!dllCppPath.empty()) {
        const bool dllAlreadyRegistered = AlreadyRegistered(dllCppPath, className);
        if (!dllAlreadyRegistered)
            InsertScriptIncludeDll(dllCppPath, className, relInclude);

        if (hasScriptList) {
            if (!scriptListHasEntry) {
                InsertScriptListEntry(scriptListPath, className);
                scriptListHasEntry = true;
            }
        } else if (!dllAlreadyRegistered) {
            InsertScriptEntry(dllCppPath, className);  // 旧形式フォールバック
        }
    }

    // EXE 側 include 挿入 (形式: #include "Scripts/Xxx.hpp")
    // WHY: SandboxStandalone は DLL をロードせず EXE 内の静的登録でスクリプトを解決する。
    //      _IMPL なしでフルインクルードするため DLL 側と形式が異なり、include のみ個別管理する。
    //      重複チェックは staticCppPath 自体で行う (ScriptList.inl は DLL ブロックで更新済みのため)。
    if (!staticCppPath.empty()) {
        const bool staticAlreadyRegistered = AlreadyRegistered(staticCppPath, className);
        if (!staticAlreadyRegistered)
            InsertScriptInclude(staticCppPath, relInclude);

        if (hasScriptList) {
            if (!scriptListHasEntry) {
                InsertScriptListEntry(scriptListPath, className);
                scriptListHasEntry = true;
            }
        } else if (!staticAlreadyRegistered) {
            InsertScriptStaticEntry(staticCppPath, className);  // 旧形式フォールバック
        }
    }

    FBZZ_LOG_INFO("ScriptCodeGen: script generated -> %s", headerPath.c_str());
    return headerPath;
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

bool ScriptCodeGen::InsertScriptInclude(const std::string& cppPath,
                                        const std::string& headerRelPath)
{
    return InsertAfterMarker(cppPath,
                             "@@FBZZ_SCRIPT_INCLUDES_BEGIN",
                             "#include " + headerRelPath);
}

// DLL エントリへスクリプトヘッダの include を挿入する。
// WHY: 新方式は実装を inline 化したため _IMPL 事前定義は不要。ヘッダを include
//      するだけで実装もこの TU に取り込まれる (複数 TU から include されても ODR 安全)。
bool ScriptCodeGen::InsertScriptIncludeDll(const std::string& dllCppPath,
                                           const std::string& className,
                                           const std::string& headerRelPath)
{
    (void)className;  // _IMPL 事前定義を廃止したため未使用
    return InsertAfterMarker(dllCppPath, "@@FBZZ_SCRIPT_INCLUDES_BEGIN",
                             "#include " + headerRelPath);
}

bool ScriptCodeGen::InsertScriptListEntry(const std::string& scriptListPath,
                                          const std::string& className)
{
    // FBZZ_SCRIPT_ENTRY(ns, T) マクロで展開される形式で挿入する。
    // EXE/DLL どちらのコンシューマーも同じ .inl を異なるマクロ定義で読む。
    const std::string entry = "FBZZ_SCRIPT_ENTRY(sandbox, " + className + ")";
    return InsertAfterMarker(scriptListPath, "@@FBZZ_SCRIPT_ENTRIES_BEGIN", entry);
}

bool ScriptCodeGen::InsertScriptEntry(const std::string& dllCppPath,
                                      const std::string& className)
{
    const std::string entry =
        "        { ::sandbox::" + className + "::TYPE_NAME,"
        " []() { return std::make_unique<::sandbox::" + className + ">(); } },";
    return InsertAfterMarker(dllCppPath, "@@FBZZ_SCRIPT_ENTRIES_BEGIN", entry);
}

bool ScriptCodeGen::InsertScriptStaticEntry(const std::string& staticCppPath,
                                             const std::string& className)
{
    return InsertAfterMarker(staticCppPath,
                             "@@FBZZ_SCRIPT_ENTRIES_BEGIN",
                             "FBZZ_REGISTER_SCRIPT(::sandbox::" + className + ")");
}

bool ScriptCodeGen::AlreadyRegistered(const std::string& dllCppPath,
                                      const std::string& className)
{
    std::string text;
    if (!util::FileSystem::ReadText(dllCppPath, text)) return false;
    std::string line;
    std::istringstream input(text);
    while (std::getline(input, line)) {
        if (line.find(className) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace fbzz::editor
