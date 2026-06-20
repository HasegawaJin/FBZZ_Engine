// FBZZ Engine
// ScriptCodeGen.hpp | fbzz::editor
// エディター内からのソースコード生成ユーティリティ
//
// WHAT: C++ スクリプトヘッダと HLSL シェーダーのテンプレートを生成し、
//       SandboxScriptsDll.cpp と SandboxScripts.cpp の登録リストに自動追記する。
//
// WHY (マーカーコメント方式):
//   両 .cpp に "@@FBZZ_SCRIPT_INCLUDES_BEGIN/END" と
//   "@@FBZZ_SCRIPT_ENTRIES_BEGIN/END" を埋め込み、その区間にテキスト挿入する。
//   DLL 側: AllEntries() ベクタへのエントリ追加 (ホットリロード用)
//   静的側: FBZZ_REGISTER_SCRIPT マクロ追加 (RuntimeBuild の Standalone exe 用)
//   コード生成後にファイルが変更されると ScriptDllLoader の監視が反応し
//   自動リビルド → ホットリロードまで自動実行される。
#pragma once
#include <string>

namespace fbzz::editor {

class ScriptCodeGen {
public:
    enum class HlslKind {
        SurfaceVSPS,     // Material/Custom/ に VS+PS ペアを生成 (一般マテリアル用)
        PostProcessVSPS, // PostProcess/Custom/ に VS+PS ペアを生成 (フルスクリーン用)
        ComputeCS,       // PostProcess/Custom/ に CS を生成
    };

    // C++ スクリプトヘッダを scriptsDir に生成し、登録リストに追記する。
    // @param name          クラス名に使う識別子 ("EnemyAI" など; "Component" は自動付与しない)
    // @param dllCppPath    SandboxScriptsDll.cpp のパス (DLL 側登録。空なら省略)
    // @param staticCppPath SandboxScripts.cpp のパス (EXE 静的登録。空なら省略)
    // @return 生成した .hpp ファイルのフルパス (失敗時は空)
    static std::string CreateScript(const std::string& name,
                                    const std::string& scriptsDir,
                                    const std::string& dllCppPath,
                                    const std::string& staticCppPath = {});

    // FBZZ Header Tool -- FBZZ_FIELD マクロをパースして .generated.hpp を生成する。
    // @param headerPath  対象の .hpp ファイルパス
    // @ret 生成した .generated.hpp のパス (失敗時は空)
    static std::string GenerateReflect(const std::string& headerPath);

    // scriptsDir 配下の全 .hpp に対して GenerateReflect を一括実行する。
    static void GenerateReflectAll(const std::string& scriptsDir);

    // HLSL シェーダーファイルを生成する。
    // @param name     シェーダー名 ("MyEffect" など)
    // @param hlslDir  Assets/shaders/ のルートパス
    // @param kind     生成するシェーダーの種類
    // @return 生成した .hlsl ファイルのフルパス (失敗時は空)
    static std::string CreateHlsl(const std::string& name,
                                  const std::string& hlslDir,
                                  HlslKind kind);

private:
    // @@FBZZ_SCRIPT_INCLUDES_BEGIN の後に #include 行を挿入する (Standalone EXE 用)
    static bool InsertScriptInclude(const std::string& cppPath,
                                    const std::string& headerRelPath);
    // DLL 用: #define {ClassName}_IMPL + #include を挿入して宣言のみ取り込む
    static bool InsertScriptIncludeDll(const std::string& dllCppPath,
                                       const std::string& className,
                                       const std::string& headerRelPath);
    // SandboxScriptsDll.cpp の @@FBZZ_SCRIPT_ENTRIES_BEGIN の後に AllEntries エントリを挿入する
    static bool InsertScriptEntry(const std::string& dllCppPath,
                                  const std::string& className);
    // SandboxScripts.cpp の @@FBZZ_SCRIPT_ENTRIES_BEGIN の後に FBZZ_REGISTER_SCRIPT を挿入する
    static bool InsertScriptStaticEntry(const std::string& staticCppPath,
                                        const std::string& className);

    // 重複チェック: cppPath に既に同名エントリがあれば true
    static bool AlreadyRegistered(const std::string& cppPath,
                                  const std::string& className);
};

} // namespace fbzz::editor
