/// @file    Material.cs
/// @brief   明示スロットへの型付き runtime override と共有 GUID 参照。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Globalization;

namespace Fbzz;
public enum MaterialBlendMode { OPAQUE = 0, ALPHA = 1, ADDITIVE = 2, PREMULTIPLIED = 3 }

/// @note Component birth を保持する。slot は各呼出し時の番号で、slot 自体の永続 handle は提供しない。
public sealed class Material : NativeComponent
{
    internal Material(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.MATERIAL, lifetimeToken) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Units = "submesh_slots")]
    public int SlotCount => Call(0, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Write = "live")]
    public bool Enabled { get => Call(1, true).Flag; set => Call(2, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "invalid_slot_or_expired_component_throws; no_slot_zero_fallback")]
    public bool IsSlotVisible(int slot) => Call(3, true, Slot(slot)).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "invalid_slot_or_expired_component_throws_without_changes")]
    public void SetSlotVisible(int slot, bool visible) => Call(4, false, Slot(slot), [visible ? 1f : 0f]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read")]
    public bool HasProperty(int slot, string property) => PropertyCall(5, true, slot, property).Flag;
    /// @note TryGet は false で out を既定値へ設定する。TrySet の false は material を変更しない。
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_zero_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetFloat(int slot, string property, out float value)
    { NativeServiceResult result = PropertyCall(6, true, slot, property); value = result.Flag ? result.Scalar : 0f; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_native_property_rejection_without_changes; invalid_input_or_scope_throws")]
    public bool TrySetFloat(int slot, string property, float value) => PropertyCall(7, false, slot, property, [value]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_zero_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetInt(int slot, string property, out int value)
    { NativeServiceResult result = PropertyCall(8, true, slot, property); bool ok = result.Scalar == 1f; value = ok ? result.Int32 : 0; return ok; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_native_property_rejection_without_changes")]
    public bool TrySetInt(int slot, string property, int value) => PropertyCall(9, false, slot, property, text: [property, value.ToString(CultureInfo.InvariantCulture)]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_zero_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetUInt(int slot, string property, out uint value)
    { NativeServiceResult result = PropertyCall(10, true, slot, property); bool ok = result.Scalar == 1f; value = ok ? checked((uint)result.Integer) : 0; return ok; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_native_property_rejection_without_changes")]
    public bool TrySetUInt(int slot, string property, uint value) => PropertyCall(11, false, slot, property, text: [property, value.ToString(CultureInfo.InvariantCulture)]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_false_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetBool(int slot, string property, out bool value)
    { NativeServiceResult result = PropertyCall(12, true, slot, property); value = result.Flag && result.Scalar != 0f; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_native_property_rejection_without_changes")]
    public bool TrySetBool(int slot, string property, bool value) => PropertyCall(13, false, slot, property, [value ? 1f : 0f]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_default_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetVector2(int slot, string property, out Vector2 value)
    { NativeServiceResult result = PropertyCall(14, true, slot, property); value = result.Flag ? result.Vector2 : default; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_native_property_rejection_without_changes")]
    public bool TrySetVector2(int slot, string property, Vector2 value) => PropertyCall(15, false, slot, property, [value.X, value.Y]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_default_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetVector3(int slot, string property, out Vector3 value)
    { NativeServiceResult result = PropertyCall(16, true, slot, property); value = result.Flag ? result.Vector3 : default; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_native_property_rejection_without_changes")]
    public bool TrySetVector3(int slot, string property, Vector3 value) => PropertyCall(17, false, slot, property, [value.X, value.Y, value.Z]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_default_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetVector4(int slot, string property, out Vector4 value)
    { NativeServiceResult result = PropertyCall(18, true, slot, property); value = result.Flag ? result.Vector4 : default; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_native_property_rejection_without_changes")]
    public bool TrySetVector4(int slot, string property, Vector4 value) => PropertyCall(19, false, slot, property, [value.X, value.Y, value.Z, value.W]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Units = "linear_rgba", Failure = "false_and_default_out_for_missing_property_or_unavailable_reflection_or_type_mismatch")]
    public bool TryGetColor(int slot, string property, out Vector4 value)
    { NativeServiceResult result = PropertyCall(20, true, slot, property); value = result.Flag ? result.Vector4 : default; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "value=linear_rgba" }, Failure = "false_for_native_property_rejection_without_changes")]
    public bool TrySetColor(int slot, string property, Vector4 value) => PropertyCall(21, false, slot, property, [value.X, value.Y, value.Z, value.W]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_if_override_absent", Summary = "Removes this slot's property override, exposing the shared asset value.")]
    public bool ClearOverride(int slot, string property) => PropertyCall(22, false, slot, property).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Summary = "Clears numeric, texture and render overrides of the explicitly selected slot.")]
    public void ClearAllOverrides(int slot) => Call(23, false, Slot(slot));
    /// @note 共有 .mat を書き換えない。別 path への成功した割当変更だけが選択 slot の override を消去する。
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_unknown_guid_wrong_asset_type_or_load_failure_without_changes", Summary = "Assigns a real GUID-resolved .mat asset to an existing slot; an empty reference clears it. Never adds or resizes the component.")]
    public bool TrySetSharedMaterial(int slot, MaterialAssetRef material) => Call(24, false, Slot(slot), text: [material.IsEmpty ? "" : material.Reference]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_empty_out_when_unassigned_or_without_registered_guid")]
    public bool TryGetSharedMaterial(int slot, out MaterialAssetRef material)
    { NativeServiceResult result = Call(25, true, Slot(slot)); material = result.Flag ? new(result.String) : default; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_unknown_guid_wrong_asset_type_load_or_property_rejection_without_changes")]
    public bool TrySetTexture(int slot, string property, TextureAssetRef texture)
    { if (texture.IsEmpty) throw new ArgumentException("Texture reference cannot be empty; use ClearOverride.", nameof(texture)); return PropertyCall(26, false, slot, property, text: [property, texture.Reference]).Flag; }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "foreign_or_disposed_texture_or_expired_component_throws; missing_property_or_renderer_reset_returns_false_without_changes", Summary = "Binds an owned runtime texture to this material slot without changing the shared asset or creating an asset GUID.")]
    public bool TrySetRuntimeTexture(int slot, string property, RuntimeTexture texture)
    {
        ArgumentNullException.ThrowIfNull(texture); ValidateProperty(slot, property);
        ScriptExecutionScope scope = Scope(); long token = texture.Token(scope);
        return Call(42, false, slot, text: [property, token.ToString(CultureInfo.InvariantCulture)]).Flag;
    }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_empty_out_when_property_missing_or_without_registered_guid")]
    public bool TryGetTexture(int slot, string property, out TextureAssetRef texture)
    { NativeServiceResult result = PropertyCall(27, true, slot, property); texture = result.Flag ? new(result.String) : default; return result.Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterHelp = new[] { "value=Sixteen row-major float values; shader-defined space and units." }, Failure = "false_for_reflection_type_rejection_without_changes")]
    public bool TrySetMatrix(int slot, string property, Matrix4 value)
    { Span<float> values = stackalloc float[16]; value.CopyTo(values); return PropertyCall(28, false, slot, property, values).Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_default_out_for_unavailable_or_wrong_type")]
    public bool TryGetMatrix(int slot, string property, out Matrix4 value)
    {
        NativeServiceResult result = PropertyCall(29, true, slot, property); value = default;
        if (!result.Flag) return false;
        if (result.ValueCount != 16) throw new InvalidOperationException("Malformed native matrix result.");
        Span<float> values = stackalloc float[16]; for (int index = 0; index < 16; index++) values[index] = result.Value(index);
        value = new(values); return true;
    }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_and_empty_out_for_unavailable_property; invalid_scope_throws", Summary = "Returns every numeric value in the actual reflected property without a total transport item limit.")]
    public bool TryGetValues(int slot, string property, out double[] values)
    {
        ValidateProperty(slot, property); ScriptExecutionScope scope = Scope();
        double[]? result = scope.Bridge.ReadMaterialValues(scope, GameObject.ObjectId, slot, property); values = result ?? []; return result is not null;
    }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Failure = "false_for_unavailable_property_or_short_buffer; shortage_Required_and_Written_zero; entire_output_unchanged_on_failure", Summary = "Reads all values into a caller buffer; Required identifies the complete property size, Written is zero on shortage, and the unused tail is preserved.")]
    public bool TryGetValues(int slot, string property, Span<double> buffer, out CollectionReadResult capacity)
    { ValidateProperty(slot, property); ScriptExecutionScope scope = Scope(); return scope.Bridge.ReadMaterialValues(scope, GameObject.ObjectId, slot, property, buffer, out capacity); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_reflection_type_count_or_range_rejection_without_changes; invalid_nonfinite_or_scope_throws", Summary = "Stages every float64 numeric value, validates reflection dimensions and representable ranges, then publishes once; no packet item ceiling is imposed on the property.")]
    public bool TrySetValues(int slot, string property, ReadOnlySpan<double> values)
    { ValidateProperty(slot, property); ScriptExecutionScope scope = Scope(); return scope.Bridge.WriteMaterialValues(scope, GameObject.ObjectId, slot, property, values); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "invalid_enum_throws_without_changes")]
    public void SetBlendMode(int slot, MaterialBlendMode mode)
    { if ((uint)mode > 3) throw new ArgumentOutOfRangeException(nameof(mode)); Call(36, false, Slot(slot), [(float)mode]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Summary = "Reads the effective explicit-slot blend mode, falling back to the shared material.")]
    public MaterialBlendMode GetBlendMode(int slot) => (MaterialBlendMode)Call(37, true, Slot(slot)).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void SetDoubleSided(int slot, bool enabled) => Call(38, false, Slot(slot), [enabled ? 1f : 0f]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Summary = "Reads the effective double-sided flag including shared fallback.")]
    public bool IsDoubleSided(int slot) => Call(39, true, Slot(slot)).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "queue=int32_render_sort_key" })]
    public void SetRenderQueue(int slot, int queue) => Call(40, false, Slot(slot), text: [queue.ToString(CultureInfo.InvariantCulture)]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "read", Units = "int32_render_sort_key", Summary = "Reads the effective render queue including shared fallback.")]
    public int GetRenderQueue(int slot) => Call(41, true, Slot(slot)).Int32;
    static int Slot(int slot) { if (slot < 0) throw new ArgumentOutOfRangeException(nameof(slot)); return slot; }
    NativeServiceResult Call(uint method, bool read, long integer = 0, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
        => Invoke((uint)ScriptServiceKind.SERVICE_MATERIAL, method, read, integer, values, text);
    NativeServiceResult PropertyCall(uint method, bool read, int slot, string property, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
    { ValidateProperty(slot, property); return Call(method, read, slot, values, text.IsEmpty ? [property] : text); }
    static void ValidateProperty(int slot, string property)
    { _ = Slot(slot); ArgumentException.ThrowIfNullOrWhiteSpace(property); if (property.Contains('\0')) throw new ArgumentException("Material property cannot contain NUL.", nameof(property)); }
}
