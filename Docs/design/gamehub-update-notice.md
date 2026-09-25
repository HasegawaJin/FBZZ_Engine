# GameHub の新しい版の知らせ

GameHub が自分より新しい FBZZ Engine のリリースを見つけたら、画面上部の帯で知らせ、リリースページへ案内する。自動更新はしない (SDK とエディターの入れ替えは配布物の構成に関わるため)。

## 1. 見つけ方

- 問い合わせ先は GitHub Releases の最新 1 件 `GET https://api.github.com/repos/HasegawaJin/FBZZ_Engine/releases/latest`。下書きとプレリリースは返らない
- 比べるのはタグ (`vX.Y.Z`、`Docs/conventions/git.md` の書式) と GameHub の `ENGINE_VERSION` (CMake から注入)。数字の 3 桁で比べ、タグの方が大きいときだけ «新しい» とする
- 問い合わせは **main プロセスだけ**が `net.fetch` で行う。renderer の CSP (`connect-src`) は広げない。`net.fetch` は OS のプロキシ設定に従う
- 起動のたびに問い合わせない。結果を `hub_config.toml` の `[update]` に残し、**前回から 24 時間以内ならその結果を使う**。未認証の GitHub API は 1 時間 60 回までなので、それを消費しない
- 失敗 (オフライン・8 秒のタイムアウト・形式違い) のときは何も出さず、確認した時刻も更新しない (次の起動でもう一度試す)

```toml
[update]
check = true                 # 設定画面の «起動時に新しい版を確認する»
last_checked_at = "2026-09-19T12:00:00.000Z"
latest_version = "0.10.0"    # タグから v を外したもの
latest_url = "https://github.com/HasegawaJin/FBZZ_Engine/releases/tag/v0.10.0"
latest_notes = "…"           # リリースノートの冒頭 (最大 3 行・280 字)
dismissed_version = ""       # «この版は知らせない» で閉じた版
```

## 2. 知らせ方

- 帯には «v0.10.0 が公開されています (今は v0.9.1)»・リリースノートの冒頭・«リリースページを開く»・«この版は知らせない» を並べる
- «この版は知らせない» はその版だけを黙らせる。もっと新しい版が出たらまた出す
- リリースページは main が保存した URL だけを開く (renderer から任意の URL を `shell.openExternal` させない)。URL がこのリポジトリのリリースページの形でなければ開かない
- 設定で確認を切ると、問い合わせも帯も止まる

## 3. 使用する SDK が古い場合

- リリース確認とは独立して、選択中 SDK が GameHub より古い場合は常設の帯と設定画面で知らせる。オフライン・更新確認無効でも表示する
- プロジェクトの `engine.sdk_id` は設定より優先される。起動先を GameHub と選択中 SDK の新しい方と比較し、旧版なら一覧を「要確認」にする。`project.engine_version` が新しくても固定先を判定する
- 起動直前にも main で再判定し、SDK ID・ルート・構成を表示する。旧版での起動は明示的に選べる。取りやめ時は起動・最終起動日時の更新を行わない
- 設定変更だけではプロジェクトの固定先は変わらない旨を知らせる。SDK・固定先は自動変更しない。判読不能な ID は旧版と断定しない

## 4. やらないこと (今は)

- 自動ダウンロード・自動更新 (Squirrel / electron-updater): 配布物の署名と SDK の入れ替え手順が決まってから
- 更新直後の «変更点» 表示: 必要になったら
