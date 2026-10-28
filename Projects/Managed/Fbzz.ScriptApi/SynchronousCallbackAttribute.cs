/// @file    SynchronousCallbackAttribute.cs
/// @brief   Declare and enforce delegates that must finish within the current Engine callback.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Runtime.CompilerServices;

namespace Fbzz;

/// @note Async delegates cannot retain Engine access after an await. Use owned Jobs waits or routines instead.
[AttributeUsage(AttributeTargets.Parameter, AllowMultiple = false)]
public sealed class SynchronousCallbackAttribute : Attribute { }

internal static class ScriptCallbackSafety
{
    /// @note Inspect every multicast target before registration; wrapped async calls remain the author's responsibility.
    internal static void RequireSynchronous(Delegate? callback, string parameterName)
    {
        if (callback is null) return;
        foreach (Delegate invocation in callback.GetInvocationList())
            if (invocation.Method.IsDefined(typeof(AsyncStateMachineAttribute), inherit: false))
                throw new ArgumentException("Engine callbacks must be synchronous. Use Jobs.Wait, WaitFrames or Run for delayed Engine work; apply Task results in Update.", parameterName);
    }
}
