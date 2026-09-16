# Player — 両手剣・54ボーン

制作元: `Assets/_src/MiniBotC/MiniBotC_PlayerMotions.blend`。2026-09-14に完成版を出力。

- `Player.fbx`: LOD0、体＋補修指。`Player_LOD1.fbx` / `Player_LOD2.fbx` は軽量版。
- `Animations/`: 承認済み22クリップ。1ファイル1テイク、30fps、120サンプル/秒。
- `Sword/`: 1本の剣、3段階LOD。`Companion/`: 4骨のおとも、3段階LOD。
- `Textures/`: 使用画像19枚を収集。FBXからの相対参照。
- `Clips.csv` / `ExportManifest.json`: 秒数・ループ・GUID参照の正本。
- `Assets/Animation/Player/Player.animcontroller`: 20ステートで全22クリップを使用。

FBZZ Editorで各FBXをインポートする。取り込み設定とループは `.fbx.meta` に設定済み。
Controllerは `Library/Baked/<FBXのGUID>/anims/<Take>.anim` の導出GUIDを参照するため、FBXのインポート後に解決される。

単位1m、上+Y。表示用VisualのY回転を180度にしてゲームの+Z前方へ合わせる。Root Motionは無効。
Swordを `Socket_Weapon_R` へ、剣側 `Attach_Grip` を基準に取り付ける。敗北時の接地も武器Socketにベイク済み。
おともの移動・浮遊はゲーム側で制御する。

旧データはgit履歴に残る (差し替え前の`GreenWare/Assets/Models/Player/`)。4シーンの旧Playerは削除済みで、新Playerはまだシーンへ配置していない。
再読み込みで31FBXの形状・骨格・全クリップ・武器姿勢を確認済み。FBZZ Editorでのインポート・実機再生は未確認。
詳細: `Docs/design/minibot-c-fbzz-export.md` / `Docs/design/player-export-controller.md`。
