/// @file    PortableProfileDataAsset.cs
/// @brief   DataAsset の explicit create と passive read/edit、および旧 packet 境界を越す値を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Linq;
using Fbzz;
namespace ManagedFixtures;
public class PortableProfileDataAsset : DataAsset
{
    public static int constructorCalls;
    public int amount = 7;
    public int resetCount;
    public int validateCount;
    public int[] values = [];
    public string text = "";
    public MaterialAssetRef material;
    public PortableProfileDataAsset() { constructorCalls++; }
    protected override void Reset() { resetCount++; }
    protected override void OnValidate() { amount = Math.Clamp(amount, 0, 100); validateCount++; }
}
public sealed class LargePortableProfileDataAsset : PortableProfileDataAsset
{
    public LargePortableProfileDataAsset() { values = Enumerable.Range(0, 40001).ToArray(); text = new string('x', 18 * 1024 * 1024) + "終端"; }
}
