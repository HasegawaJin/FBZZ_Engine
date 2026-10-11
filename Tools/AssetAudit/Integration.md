<!-- @file    Integration.md -->
<!-- @brief   Tool index registration and validation contract. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->
# 共通Tools索引への接続

実装の所有範囲は `Tools/AssetAudit/`。ユーザーの追加指示により、共通 `Tools/README.md` の索引に次の一行を追加しました。既存のヘッダーとAgentBuildなどの他担当の差分は保持しています。CMake、Engine、SDK、Importer、シーンは変更していません。

| 名前 | 役割 | 呼び出し元 |
|---|---|---|
| `AssetAudit/` | モデル・画像・アニメ・GUID参照を入力不変検証付きで読み取り、JSON/HTMLに報告 | ユーザーの明示CLI呼び出し。契約と検査範囲は `Tools/AssetAudit/README.md` |

AGENTS.md「ツールの置き場所」に従い、役割と呼び出し元を索引に記載しています。AgentLintと既存16fixtureテストの再検証結果は、`Reports/Validated-20261011/Verification.json` の `integrationVerification` に保存します。この接続に全アセットの再走査は不要です。

確定error0は全健全を意味しません。partial/unknown/pendingを含む実走査結果は未完了として扱い、対応率と検査制限を確認する契約をREADMEに明記しています。
