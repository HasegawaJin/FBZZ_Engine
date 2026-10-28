/// @file    ScriptCodeGen.cpp
/// @brief   エディター内からのソースコード生成ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-03
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <ctime>
#include <sstream>
#include <string>
#include <string_view>

namespace fbzz::editor {

namespace {

std::string CurrentLocalDate();

/// @brief C# の ASCII 識別子と予約語を検証する。
/// @param name 検証する型名。
/// @return 識別子として使える場合は true。
bool IsCSharpIdentifier(std::string_view name)
{
    if (name.empty() || name.size() > 128) return false;
    const auto isStart = [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') || value == '_';
    };
    const auto isPart = [&isStart](unsigned char value) {
        return isStart(value) || (value >= '0' && value <= '9');
    };
    if (!isStart(static_cast<unsigned char>(name.front()))) return false;
    for (const char value : name) {
        if (!isPart(static_cast<unsigned char>(value))) return false;
    }

    static constexpr std::string_view keywords[] = {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char",
        "checked", "class", "const", "continue", "decimal", "default", "delegate", "do",
        "double", "else", "enum", "event", "explicit", "extern", "false", "finally",
        "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int",
        "interface", "internal", "is", "lock", "long", "namespace", "new", "null",
        "object", "operator", "out", "override", "params", "private", "protected",
        "public", "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof",
        "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true",
        "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using",
        "virtual", "void", "volatile", "while",
    };
    return std::find(std::begin(keywords), std::end(keywords), name) == std::end(keywords);
}

/// @brief 現在のローカル日付を ISO 8601 の日付形式で返す。
/// @return `YYYY-MM-DD` 形式の日付。
std::string CurrentLocalDate()
{
    const std::time_t now = std::time(nullptr);
    std::tm date{};
    if (localtime_s(&date, &now) != 0) return {};
    char text[11]{};
    if (std::strftime(text, sizeof(text), "%Y-%m-%d", &date) == 0) return {};
    return text;
}

/// @brief GameObject に追加する C# Script の初期ソースを作る。
/// @param className 生成する型名。
/// @return C# ソース。
std::string BuildCSharpScriptTemplate(const std::string& className)
{
    std::ostringstream ss;
    ss << "/// @file    " << className << ".cs\n";
    ss << "/// @brief   GameObject に追加する C# Script。\n";
    ss << "/// @author  Hasegawa Jin\n";
    ss << "/// @date    " << CurrentLocalDate() << "\n";
    ss << "using Fbzz;\n\n";
    ss << "class " << className << " : global::Fbzz.Script\n";
    ss << "{\n";
    ss << "    protected override void Update()\n";
    ss << "    {\n";
    ss << "    }\n";
    ss << "}\n";
    return ss.str();
}

/// @brief アタッチしない C# helper の初期ソースを作る。
/// @param className 生成する型名。
/// @return C# ソース。
std::string BuildCSharpHelperTemplate(const std::string& className)
{
    std::ostringstream ss;
    ss << "/// @file    " << className << ".cs\n";
    ss << "/// @brief   C# Script から利用する helper 型。\n";
    ss << "/// @author  Hasegawa Jin\n";
    ss << "/// @date    " << CurrentLocalDate() << "\n";
    ss << "class " << className << "\n";
    ss << "{\n";
    ss << "}\n";
    return ss.str();
}

/// @brief C# DataAsset 型の初期ソースを作る。
/// @param className 生成する型名。
/// @return C# ソース。
std::string BuildCSharpDataAssetTemplate(const std::string& className)
{
    std::ostringstream ss;
    ss << "/// @file    " << className << ".cs\n";
    ss << "/// @brief   C# DataAsset の共有データ型。\n";
    ss << "/// @author  Hasegawa Jin\n";
    ss << "/// @date    " << CurrentLocalDate() << "\n";
    ss << "using Fbzz;\n\n";
    ss << "class " << className << " : global::Fbzz.DataAsset\n";
    ss << "{\n";
    ss << "    public float value;\n";
    ss << "}\n";
    return ss.str();
}

/// @brief Surface VS+PS シェーダーテンプレートを生成する。
std::string BuildSurfaceHlslTemplate(const std::string& name)
{
    std::ostringstream ss;
    ss << "/// @file    Material/Custom/" << name << ".hlsl\n";
    ss << "/// @brief   " << name << " カスタムマテリアルシェーダー。\n";
    ss << "/// @author  Hasegawa Jin\n";
    ss << "/// @date    " << CurrentLocalDate() << "\n";
    ss << "#ifndef " << name << "_HLSL\n";
    ss << "#define " << name << "_HLSL\n";
    ss << "\n";
    ss << "/// @note 下で MaterialConstants を自前で宣言するので、Constants.hlsli の既定定義を止める。\n";
    ss << "#define FBZZ_MATERIAL_CONSTANTS\n";
    ss << "#include \"Common/Constants.hlsli\"\n";
    ss << "#include \"Common/Structs.hlsli\"\n";
    ss << "#include \"Platform/Backend.hlsli\"\n";
    ss << "#include \"Rendering/Lighting.hlsli\"\n";
    ss << "#include \"Common/BindlessIndices.hlsli\"\n";
    ss << "#include \"Common/MaterialTextures.hlsli\"\n";
    ss << "\n";
    ss << "cbuffer MaterialConstants : register(CB_MATERIAL)\n";
    ss << "{\n";
    ss << "    /// @note RGBA ベースカラー。\n";
    ss << "    float4 albedo;\n";
    ss << "    /// @note テクスチャフラグ。\n";
    ss << "    uint   textureMask;\n";
    ss << "    /// @note 宣言したテクスチャ枠だけが Inspector に出るため cbuffer 内へ置く。\n";
    ss << "    FBZZ_MATERIAL_TEX_ALBEDO\n";
    ss << "};\n";
    ss << "\n";
    ss << "FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);\n";
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
    ss << "    /// @note TODO: ライティング計算をここに追加する\n";
    ss << "    return float4(color, 1.0f);\n";
    ss << "}\n";
    ss << "\n";
    ss << "#endif /// @note " << name << "_HLSL\n";
    return ss.str();
}

/// @brief ParticleEmitter 用の PS テンプレートを生成する。
/// @note VS は書かせない。ビルボード展開と 29 個の定数は定型で写経ミスが分かりにくいため、 ParticleMaterial.hlsli が VSMain まで供給し、雛形は PSMain だけにする。
std::string BuildParticleHlslTemplate(const std::string& name)
{
    std::ostringstream ss;
    ss << "/// @file    Material/Custom/" << name << ".hlsl\n";
    ss << "/// @brief   " << name << " カスタムパーティクルシェーダー。\n";
    ss << "/// @author  Hasegawa Jin\n";
    ss << "/// @date    " << CurrentLocalDate() << "\n";
    ss << "/// @note 使い方:\n";
    ss << "/// @note   1. .mat を作り render_path = \"particle\" と shader = このファイル を書く\n";
    ss << "/// @note      (render_path が particle でないとエンジンはこのシェーダーを採用しない)\n";
    ss << "/// @note   2. ParticleEmitter の Material にその .mat を割り当てる\n";
    ss << "/// @note   3. 下の MaterialConstants に足した変数は .mat の [params] と\n";
    ss << "/// @note      **名前** で結ばれる (宣言順は無関係)\n";
    ss << "\n";
    ss << "/// @note GPU シミュレーション (simulationMode = Gpu) のエミッターへ差す場合は、\n";
    ss << "/// @note include より前に #define FBZZ_PARTICLE_GPU を足すこと。\n";
    ss << "/// @note 自前で VSMain を書く場合は #define FBZZ_PARTICLE_CUSTOM_VS を足す。\n";
    ss << "\n";
    ss << "/// @note 使えるテクスチャは t0 (.mat の [textures] albedo) と t2/t3/t4。\n";
    ss << "/// @note t1 / t5〜t9 はパーティクルパスが占有している。\n";
    ss << "\n";
    ss << "#include \"Material/Effects/ParticleMaterial.hlsli\"\n";
    ss << "\n";
    ss << "cbuffer MaterialConstants : register(CB_MATERIAL)\n";
    ss << "{\n";
    ss << "    /// @note .mat: params.tintColor = [1.0, 1.0, 1.0, 1.0]\n";
    ss << "    float4 tintColor;\n";
    ss << "    /// @note .mat: params.softness = 2.0\n";
    ss << "    float  softness;\n";
    ss << "};\n";
    ss << "\n";
    ss << "/// @note VSMain は ParticleMaterial.hlsli が供給する。\n";
    ss << "float4 PSMain(ParticlePSIn p) : SV_Target0\n";
    ss << "{\n";
    ss << "    /// @note localUv はクワッド内の [0,1]。中心を原点にした半径で形を作る。\n";
    ss << "    float r = length(p.localUv - 0.5f) * 2.0f;\n";
    ss << "    if (r >= 1.0f) discard;\n";
    ss << "\n";
    ss << "    float shape = pow(saturate(1.0f - r), max(softness, 0.01f));\n";
    ss << "\n";
    ss << "    /// @note p.color は粒子の色 (グラデーション適用済み)。alpha は寿命フェード。\n";
    ss << "    /// @note 出力は非事前乗算 — RGB を自分で alpha 倍しない (ブレンドが掛ける)。\n";
    ss << "    /// @note TODO: エフェクトをここに追加する\n";
    ss << "    float3 rgb   = p.color.rgb * tintColor.rgb * max(gEmissiveScale, 0.0f);\n";
    ss << "    float  alpha = shape * p.color.a * tintColor.a;\n";
    ss << "    clip(alpha - 0.003f);\n";
    ss << "    return float4(rgb, alpha);\n";
    ss << "}\n";
    return ss.str();
}

/// @brief PostProcess フルスクリーン VS+PS テンプレートを生成する。
/// @note 契約は Assets/Shaders/PostProcess/Custom/CustomPostProcessTemplate.hlsl が正本。 フルスクリーン三角形は Common/Fullscreen.hlsli、サンプラーは clamp (s0 は DX12 で WRAP)、 入力 UV は FBZZ_CustomInputUV を通す (downscale 時は結果が RT の左上にしか無い)。
std::string BuildPostProcessHlslTemplate(const std::string& name)
{
    std::ostringstream ss;
    ss << "/// @file    PostProcess/Custom/" << name << ".hlsl\n";
    ss << "/// @brief   " << name << " カスタムポストプロセスシェーダー。\n";
    ss << "/// @author  Hasegawa Jin\n";
    ss << "/// @date    " << CurrentLocalDate() << "\n";
    ss << "/// @note 使える定数・入力・段の説明は PostProcess/Custom/CustomPostProcessTemplate.hlsl を参照。\n";
    ss << "#ifndef " << name << "_HLSL\n";
    ss << "#define " << name << "_HLSL\n";
    ss << "\n";
    ss << "#include \"Common/Constants.hlsli\"\n";
    ss << "#include \"Common/Fullscreen.hlsli\"\n";
    ss << "#include \"Platform/Backend.hlsli\"\n";
    ss << "#include \"Rendering/PostProcess.hlsli\"\n";
    ss << "#include \"Common/BindlessIndices.hlsli\"\n";
    ss << "\n";
    ss << "/// @note t5 = 画面の色 (直前のパスの出力)。\n";
    ss << "FBZZ_TEX2D(texInput, TEX_GBUFFER0_SLOT);\n";
    ss << "/// @note 全画面フェッチなので clamp (s0 は DX12 では WRAP で、画面端が反対側を読む)。\n";
    ss << "SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);\n";
    ss << "\n";
    ss << "FBZZFullscreenVertex VSMain(uint id : SV_VertexID)\n";
    ss << "{\n";
    ss << "    return FBZZMakeFullscreenVertex(id);\n";
    ss << "}\n";
    ss << "\n";
    ss << "float4 PSMain(FBZZFullscreenVertex p) : SV_Target0\n";
    ss << "{\n";
    ss << "    /// @note 入力 UV は FBZZ_CustomInputUV を通す (downscale 中の倍率と、有効範囲の端での留め)。\n";
    ss << "    const float2 inputUV  = FBZZ_CustomInputUV(p.uv);\n";
    ss << "    const float3 original = texInput.SampleLevel(sampLinear, inputUV, 0).rgb;\n";
    ss << "\n";
    ss << "    /// @note TODO: エフェクトをここに追加する (customParameters.xyzw / customParameters2.xyzw が param0..7)\n";
    ss << "    float3 result = original;\n";
    ss << "\n";
    ss << "    /// @note customIntensity と customBlend で元画像と混ぜる (Inspector / Script から制御される)。\n";
    ss << "    result = lerp(original, result, saturate(customIntensity * customBlend));\n";
    ss << "    return float4(result, 1.0f);\n";
    ss << "}\n";
    ss << "\n";
    ss << "#endif /// @note " << name << "_HLSL\n";
    return ss.str();
}

/// @brief Compute Shader テンプレートを生成する。
std::string BuildComputeHlslTemplate(const std::string& name)
{
    std::ostringstream ss;
    ss << "/// @file    PostProcess/Custom/" << name << ".cs.hlsl\n";
    ss << "/// @brief   " << name << " カスタムコンピュートシェーダー。\n";
    ss << "/// @author  Hasegawa Jin\n";
    ss << "/// @date    " << CurrentLocalDate() << "\n";
    ss << "#ifndef " << name << "_CS_HLSL\n";
    ss << "#define " << name << "_CS_HLSL\n";
    ss << "\n";
    ss << "#include \"Common/Constants.hlsli\"\n";
    ss << "#include \"Platform/Backend.hlsli\"\n";
    ss << "#include \"Common/BindlessIndices.hlsli\"\n";
    ss << "\n";
    ss << "/// @note ComputeCall::uavOutputs[0] / srvInputs[0] の枠を添字で引く。\n";
    ss << "FBZZ_RWTEX2D(outputTex, UAV_OUTPUT_SLOT);\n";
    ss << "FBZZ_TEX2D(inputTex, 0);\n";
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
    ss << "    /// @note TODO: 処理をここに追加する\n";
    ss << "    outputTex[id.xy] = color;\n";
    ss << "}\n";
    ss << "\n";
    ss << "#endif /// @note " << name << "_CS_HLSL\n";
    return ss.str();
}

} /// @note namespace

/// @name 公開 API

bool ScriptCodeGen::IsValidCSharpName(const std::string& name)
{
    return IsCSharpIdentifier(name);
}

std::string ScriptCodeGen::CreateCSharpClass(const std::string& name,
                                             const std::string& scriptsDir,
                                             CSharpKind kind)
{
    if (scriptsDir.empty() || !IsCSharpIdentifier(name)) return {};

    std::string className = name;
    if (kind == CSharpKind::Script && !className.ends_with("Component"))
        className += "Component";
    if (!IsCSharpIdentifier(className)) return {};

    const std::string filePath = scriptsDir + "/" + className + ".cs";
    if (util::FileSystem::Exists(filePath)) {
        FBZZ_LOG_WARN("ScriptCodeGen: %s already exists", filePath.c_str());
        return {};
    }
    if (!util::FileSystem::EnsureDirectory(scriptsDir)) {
        FBZZ_LOG_ERROR("ScriptCodeGen: failed to create Scripts directory: %s", scriptsDir.c_str());
        return {};
    }

    const std::string source = [&] {
        switch (kind) {
        case CSharpKind::Script:    return BuildCSharpScriptTemplate(className);
        case CSharpKind::Helper:    return BuildCSharpHelperTemplate(className);
        case CSharpKind::DataAsset: return BuildCSharpDataAssetTemplate(className);
        }
        return std::string{};
    }();
    if (source.empty() || !util::FileSystem::WriteText(filePath, source)) {
        FBZZ_LOG_ERROR("ScriptCodeGen: failed to write file: %s", filePath.c_str());
        return {};
    }
    return filePath;
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
    case HlslKind::ParticlePS:
        subDir   = hlslDir + "/Material/Custom";
        fileName = name + ".hlsl";
        content  = BuildParticleHlslTemplate(name);
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

    if (!util::FileSystem::EnsureDirectory(subDir)) {
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

/// @name 内部実装

} /// @note namespace fbzz::editor
