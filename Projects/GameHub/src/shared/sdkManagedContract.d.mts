/// @file    sdkManagedContract.d.mts
/// @brief   Node と GameHub が共有する公開 managed 契約検査の型。
/// @author  Hasegawa Jin
/// @date    2026-10-06
export function AssertManagedAuthoringContract(root: string, configuration: string,
  files: Record<string, unknown> | undefined): Promise<void>;
