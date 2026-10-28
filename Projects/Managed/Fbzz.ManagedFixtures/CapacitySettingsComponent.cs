/// @file    CapacitySettingsComponent.cs
/// @brief   Real native save/reload/edit coverage beyond former field and array presentation limits.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using Fbzz;

namespace ManagedFixtures;

public sealed class CapacitySettingsComponent : Script
{
    public int[] values = MakeValues();
    public List<int> list = new(MakeValues());
    public string text = new('x', 1024 * 1024 + 23);
    static int[] MakeValues()
    {
        int[] values = new int[40001];
        for (int index = 0; index < values.Length; index++) values[index] = index;
        return values;
    }
}
