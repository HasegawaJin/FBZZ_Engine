/// @file    LargeDataAssetLoadComponent.cs
/// @brief   実 Native/CoreCLR の大きな DataAsset 読込が末尾と constructor 分離を保持するか検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using Fbzz;
namespace ManagedFixtures;
public sealed class LargeDataAssetLoadComponent : Script
{
    public DataAssetRef<LargePortableProfileDataAsset> profile;
    public int textLength;
    public int lastValue;
    public bool tailValid;
    public int constructorCalls;
    protected override void Update()
    {
        var data = DataAssets.Load(profile);
        if (data is null) return;
        textLength = data.text.Length; lastValue = data.values[^1]; tailValid = data.text.EndsWith("終端");
        constructorCalls = PortableProfileDataAsset.constructorCalls;
    }
}
