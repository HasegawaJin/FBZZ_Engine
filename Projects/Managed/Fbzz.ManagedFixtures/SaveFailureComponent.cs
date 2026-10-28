/// @file    SaveFailureComponent.cs
/// @brief   保存 callback が作る不正値・取得失敗・例外の保全を検証する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using Fbzz;

namespace ManagedFixtures;

[Serializable]
public sealed class SaveFailureSettings
{
    public float value = 1f;
    public string label = "valid";
    public SaveFailureSettings next;
}

public sealed class SaveFailureComponent : Script
{
    public SaveFailureSettings settings = new();
    public float speed = 2f;
    public float[] values = [];
    public string note = "valid";
    public int failureMode;

    protected override void OnBeforeSerialize()
    {
        if (failureMode == 1) settings.value = float.NaN;
        else if (failureMode == 2) settings.label = null;
        else if (failureMode == 3) settings.next = settings;
        else if (failureMode == 4) throw new InvalidOperationException("Saving fixture failed before serialization.");
        else if (failureMode == 5) speed = float.NaN;
        else if (failureMode == 6) values = null;
        else if (failureMode == 7) note = "\uD800";
    }
}
