/// @file    AudioSourceSettings.cs
/// @brief   Source-local mixing and voice allocation settings for the C# audio facade.
/// @author  Hasegawa Jin
/// @date    2026-10-11
namespace Fbzz;

public sealed partial class AudioSource
{
    /// @note Higher values protect non-looping voices during global budget contention. Applies at next voice start.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public int Priority
    {
        get => checked((int)Invoke(181, 46, true).Integer);
        set => Invoke(181, 47, false, value);
    }
    /// @note Applies at next voice start. Short repeated effects usually use preloaded PCM.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Streaming
    {
        get => Invoke(181, 48, true).Flag;
        set => Invoke(181, 49, false, value ? 1 : 0);
    }
    /// @note Normalized distance-dependent high-frequency damping, from 0 to 1.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public float AirAbsorption
    {
        get => Invoke(181, 50, true).Scalar;
        set => Invoke(181, 51, false, 0, [value]);
    }
    /// @note Relative-velocity pitch modulation, from 0 to 1. Zero disables it.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public float DopplerLevel
    {
        get => Invoke(181, 52, true).Scalar;
        set => Invoke(181, 53, false, 0, [value]);
    }
    /// @note Normalized cutoff from 0 to 1; 1 is unfiltered. Does not change shared mixer buses.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public float LowPassCutoff
    {
        get => Invoke(181, 54, true).Scalar;
        set => Invoke(181, 55, false, 0, [value]);
    }
    /// @note Interpolate from the current cutoff in simulation seconds. Zero seconds applies immediately.
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "seconds=seconds", "cutoff=normalized_0_1" }, Failure = "invalid_or_nonfinite_arguments_leave_settings_unchanged")]
    public void FadeLowPassTo(float cutoff, float seconds) => Invoke(181, 56, false, 0, [cutoff, seconds]);
}
