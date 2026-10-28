<!-- @file    ai-validation-results.md -->
<!-- @brief   Final schema14 AI infrastructure validation, measured limits and consumer migration boundary. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-12 -->
# AI基盤の検証結果

2026-10-12、AI/BT専用基盤とC# facade・一括native bridgeをEngineへ反映し、Audio/Terrainと共通schema **`0x46425A5A4D414E14`** のRelease buildとCPU検証を完了した。installed SDK公開と実魚Scene移行は別担当の確認後に行う。

## 実装範囲

- BTのDFS/共有定義と個体実行状態、Running/parallel状態、abort/reset、reactive guard/cooldown/time limit、Blackboardの正確な整数比較・revisionを修正。native BTのschedulerとPatrol actionのroute所有を連携した。
- `AiCrowd` はbounded3D近傍、cohesion/alignment/separation、索敵・恐怖記憶、wander/seek/arrive/pursue/flee/hold/external、加速/旋回制限、床/天井/壁/球障害物、更新距離LOD・決定予算・決定論的seedをまとめる。1region/stepにつき1native dispatch。
- `AiPoseBatch` はcached world poseを一括反映し、無効入力の原子的拒否・expired個体skip・scale保持を検証。物理bodyと親子混合batchは契約対象外。
- `AiBehaviorTree` はC# custom action/conditionの共有定義と個体state。`AiEcology` は有限資源/捕食/出生予約/保存、`AiDensityField` は粗い栄養密度の保存則/流れ/実量消費を扱う。`AiCrowdQueries` はsnapshotから音/VFX cueの少数個体を選ぶ。

## 共有Release結果

|検証|結果|
|---|---|
|EngineAuto / EditorAuto / Launcher build|exit0、warning0/error0|
|AI/Terrainの実C#→CoreCLR→native接続|3件PASS|
|Engine関連17 suites|169件PASS|
|Editor関連11 suites|54件PASS|
|managed全体|153件PASS、warning0/error0|

native合計226件に、BT14・Blackboard22・Crowd17・Crowd transfer8・Pose8・BTSystem3の計72件を含む。新CoreCLR3件は1個体の実Crowd/pose/scale、PhysicsなしのTerrain/Scene floor/ceiling、Edit scope拒否を確認する。GeneratedManagedBridge7、Audio16、Terrain/Water、材質関連12も成功した。

schemaの64bit固定値と全163 operation名/数値・enum countの厳密な検査を維持して、新世代14と `AI_CROWD_STEP=340` をテスト期待表へ追加した。manifest検査fixtureの出力はrunnerのbinから一意tempへ移し、既存の受理・source hash改竄拒否・旧catalog保持・manifest欠落拒否を全て実行した。Windowsの候補DLL mappingが終了するrunnerプロセス終了後に、専用scratchを削除した。テストのskip・検査削除はしていない。

## 1000体の独立CPU計測

MSVC /O2 /MD、1000体、warmup120回、計測600回。通常Crowd p95 **0.1590ms**、64sphere/3threat p95 **0.7352ms**、両方steady heap allocation **0**。C# Crowd1000×300stepは300dispatch/0B、pose1000×100commitは100dispatch/0B。snapshot cueはnative0/0B。生態1000体はp95 0.3812ms/warm0B。

共有hostでの独立計測であり、実Sceneのterrain query/interop全体、animation、音/VFX、GPUを含まない。実ゲームのframe改善率は未測定。

## consumer移行と残る確認

baselineは `Scratch/Aquarium/OceanReefV2Consumer` の1024魚、Marine845行（非空817）、Benthic284行（非空273）。実Scene・既存ゲームscriptは統合担当が移行する。独自spawn・Inspector・地形cache・演出を残し、linked-grid/steer/limit/pose更新等の共通計算をAPIへ移す。`Projects/Managed/Authoring` の4サンプルとSchoolingJewels4種設定を利用できる。

SDK `0.10.0` は公開前のschema13を維持している。親担当がBAT完了後にnative/Host/ScriptApi/Contractを同世代として公開し、統合担当がcatalogのmodel/clip GUIDを接続する。同条件で行数・削除した重複method・native dispatch/CPU/GC・群れ/逃走/constraint・GPU/見た目を比較する。現段階では移行完了や行数削減率を主張しない。

詳しい契約は [AI監査](ai-infrastructure-audit.md)、[Crowd bridge](ai-crowd-bridge.md)、[pose](ai-pose-batch-contract.md)、[C# BT](ai-script-behaviors.md)、[生態](ai-ecology.md)、[密度場](ai-density.md) を参照する。
