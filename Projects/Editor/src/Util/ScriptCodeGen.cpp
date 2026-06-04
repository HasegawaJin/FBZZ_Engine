// FBZZ Engine
// ScriptCodeGen.cpp | fbzz::editor
// エディター内からのソースコード生成ユーティリティ
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fbzz::editor {

namespace {

// ファイルを全行読み込む
std::vector<std::string> ReadLines(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line))
        lines.push_back(line);
    return lines;
}

// 行リストをファイルに書き出す
bool WriteLines(const std::string& path, const std::vector<std::string>& lines)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    for (const auto& l : lines) {
        ofs << l << '\n';
    }
    return true;
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
        if (lines[i].find(marker) != std::string::npos) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i + 1), newLine);
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

    std::error_code ec;
    const std::filesystem::path dir(path);
    if (std::filesystem::exists(dir, ec))
        return std::filesystem::is_directory(dir, ec);

    // WHY: util::FileSystem::EnsureDirectory は Win32 CreateDirectoryW 直呼びで1階層だけを作る。
    //      Shader 生成先は Assets/Shaders/Material/Custom のような多段構造なので、
    //      コード生成ユーティリティ側では recursive 作成を使う。
    return std::filesystem::create_directories(dir, ec) && !ec;
}

// C++ スクリプトのテンプレートを生成する
std::string BuildScriptTemplate(const std::string& name)
{
    // クラス名 = name + "Component" にする (例: "EnemyAI" → "EnemyAIComponent")
    const std::string className = name + "Component";

    std::ostringstream ss;
    ss << "// FBZZ Engine\n";
    ss << "// " << className << ".hpp | sandbox\n";
    ss << "// " << className << " スクリプト\n";
    ss << "#pragma once\n";
    ss << "\n";
    ss << "// WHY: Sandbox スクリプトは engine 層からインクルードされない末端ヘッダのため、\n";
    ss << "//      using namespace を許可する。詳細は AGENTS.md を参照。\n";
    ss << "#include <Engine/Scene/Script.hpp>\n";
    ss << "\n";
    ss << "using namespace fbzz::scene;\n";
    ss << "using namespace fbzz::math;\n";
    ss << "\n";
    ss << "namespace sandbox {\n";
    ss << "\n";
    ss << "class " << className << " : public Script {\n";
    ss << "public:\n";
    ss << "    static constexpr const char* TYPE_NAME = \"" << className << "\";\n";
    ss << "    const char* GetTypeName() const override { return TYPE_NAME; }\n";
    ss << "\n";
    ss << "    // Inspector / Serializer に公開するフィールドをここに宣言する\n";
    ss << "    // 例: float speed = 5.0f;\n";
    ss << "\n";
    ss << "    void Reflect(IReflector& r) override\n";
    ss << "    {\n";
    ss << "        // 例: r.Field(\"Speed\", speed);\n";
    ss << "        (void)r;\n";
    ss << "    }\n";
    ss << "\n";
    ss << "    void OnStart() override {}\n";
    ss << "\n";
    ss << "    void OnUpdate(float dt) override\n";
    ss << "    {\n";
    ss << "        (void)dt;\n";
    ss << "    }\n";
    ss << "};\n";
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

    const std::string relInclude = "\"Scripts/" + headerName + "\"";

    // SandboxScriptsDll.cpp への自動登録 (DLL ホットリロード用)
    if (!dllCppPath.empty() && !AlreadyRegistered(dllCppPath, className)) {
        InsertScriptInclude(dllCppPath, relInclude);
        InsertScriptEntry(dllCppPath, className);
    }

    // SandboxScripts.cpp への自動登録 (RuntimeBuild Standalone exe 用)
    // WHY: SandboxStandalone は DLL をロードせず EXE 内の静的登録でスクリプトを解決する。
    //      ここで追記しないと RuntimeBuild 後の exe に新スクリプトが含まれない。
    if (!staticCppPath.empty() && !AlreadyRegistered(staticCppPath, className)) {
        InsertScriptInclude(staticCppPath, relInclude);
        InsertScriptStaticEntry(staticCppPath, className);
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
    std::ifstream ifs(dllCppPath);
    if (!ifs) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find(className) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace fbzz::editor
