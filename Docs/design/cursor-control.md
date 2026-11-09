# Cursor and game input ownership

Cursor settings express game intent. The host decides when that intent may affect the OS. Pointer visibility, movement restriction and ownership of physical gameplay input are separate concepts.

| Concept | Native API | C# API |
|---|---|---|
| Requested visibility | `GetRequestedPolicy().visible` | `Cursor.RequestedVisible` |
| Requested free/confined/locked movement | `GetRequestedPolicy().lockMode` | `Cursor.RequestedLockMode` |
| Applied visibility and movement | `GetState().effective` | `Cursor.EffectiveVisible`, `Cursor.EffectiveLockMode` |
| Physical gameplay input owner | `GetState().inputOwner` | `Cursor.InputOwner` |
| Host constraint and OS focus | `GetState().suspendedReason`, `.windowActive` | `Cursor.SuspendedReason`, `Cursor.WindowActive` |

Games continue to use owned `Cursor.Push(lockMode, visible, priority)` requests. They do not acquire OS input ownership themselves. A host release preserves requests, priorities and shape intent. Setting a visible/free cursor does not give input back to Editor; the release action does. Editor's existing `Free` override affects the cursor only, and leaves physical input ownership independent.

`Locked` fixes the pointer at the target center and makes its effective visibility hidden even when requested visibility is true. `Confined` retains absolute pointer motion inside the game target and respects requested visibility. `None` leaves movement unrestricted and respects requested visibility. A missing/invalid target or failed OS restriction restores a visible, unrestricted pointer. Editor targets the visible Game View rectangle, including detached views; standalone targets the client rectangle.

The visibility and lock split follows [Unity Cursor](https://docs.unity3d.com/ScriptReference/Cursor-lockState.html). The visible/hidden confined combinations are also explicit in [Godot mouse modes](https://docs.godotengine.org/en/stable/classes/class_input.html#enum-input-mousemode). The host ownership handoff and dedicated release shortcut follow the [Unreal PIE interaction model](https://dev.epicgames.com/documentation/en-us/unreal-engine/playing-and-simulating-in-unreal-engine).

## Host lifecycle

- In Editor, Focused/Maximized Play may acquire input once after the requested Game View focus is established. The initial focus attempt is bounded; Unfocused Play waits for a click.
- Clicking Game View or selecting acquisition hands physical input to the game. Only controls already held at acquisition are consumed until their individual release. Editor UI keeps its raw input; an unrelated new key remains usable.
- Shift+F1 releases physical gameplay input and restores the pointer. Editor keeps its existing Escape release/stop behavior for compatibility. Standalone leaves Escape available to gameplay.
- Game View focus loss, OS focus loss, Pause, target loss, window movement and Play stop release input. OS focus return, resume and completing a move do not acquire it again. A deliberate game-target click is required.
- Standalone uses the same ownership and cursor semantics. Normal/error shutdown releases the process's cursor resources. The parent Editor does not acquire input on child exit.
- When input belongs to the host, physical keys/buttons/axes produce neutral gameplay input and vibration is stopped. An acquisition consumes only the physical controls that were held; pad axes use the existing default 0.25 deadzone and do not block other controls.

Explicit `input.inject` and `Input::Inject*` / `SetVirtual*` remain the production automation boundary. They can drive hidden/headless authoring Playtests without owning the OS pointer or reading physical host input. They do not capture the OS cursor or acquire physical input. Requested `InputActionMap::SetEnabled(false)` still disables evaluation. Host handoffs clear evaluated snapshots through `ResetRuntime` without changing requested enablement, bindings or settings.

Runtime UI opts into the same input domain with the appended `RenderSystemUIOptions.gameplayInput` flag. The default remains raw authoring preview input. Runtime button presses, wheel/text, navigation keys and pad controls use the same physical ownership and acquisition masks as scripts. Ownership/acquisition changes cancel pending button, toggle, slider, field, drag/drop and trigger interactions without manufacturing a release click, submit or drop. Interaction revisions are monotonic and cached per UI context, so Scene preview does not reset Game View interactions. Built-in navigation pointers are local to the UI pass; explicitly scripted `UIPointer` remains a virtual input source.

Physical host gestures use `Input::PhysicalKeyHeld`, `PhysicalKeyDown` and `PhysicalMouseButtonDown`, which exclude injected input. Injection therefore cannot become an OS recapture click. WM_CHAR is tagged with its native key so text from a held acquisition control can be suppressed individually; text already buffered in the acquisition frame is conservatively consumed. Raw Editor UI input and existing Input APIs retain their prior meaning. UI keeps the caller's viewport-relative coordinates; this change does not reinterpret `InjectMousePosition` client coordinates as Editor viewport coordinates.

Client-space `InjectMousePosition` and injected mouse buttons remain available to gameplay scripts/action maps without OS ownership. They do not enable runtime UI's caller-supplied physical viewport pointer while Host owns input. Hidden UI pointer automation uses explicit Canvas-space `UIPointer`; injected navigation keys also use the Canvas navigation path. This prevents a persistent injected-position flag or injected button from exposing a subsequently moved OS pointer to game UI.

## Compatibility and migration

Existing `CursorLockMode` enum values and `None`/`Confined`/`Locked` configuration strings are unchanged. Missing settings retain their existing defaults. Existing request ownership/generation cleanup and same-priority ordering are preserved.

Native `IsVisible`, `GetLockMode` and the historically named `GetEffectivePolicy` still return the winning **requested** values. `GetEffectivePolicy` is documented as deprecated; use `GetRequestedPolicy` for intent or `GetState` for applied status. C# `Visible` and `LockMode` retain their requested-value meaning and gain explicit `RequestedVisible` and `RequestedLockMode` aliases. Existing compiled managed method IDs 0–6 remain stable; new read-only queries use IDs 7–11.

Two deliberate behavior changes are visible: Locked now forces effective hidden visibility, and returning focus/resuming no longer silently recaptures input. Requests themselves remain unchanged in both cases.

## Verification boundary

`CursorControlPolicyTests.cpp` tests the pure ownership state machine, visibility/restriction combinations, release reasons, focus return, missing targets, per-control acquisition masks and the hidden automation source-selection rule without OS calls. Managed fake-bridge assertions verify stable legacy method IDs and the new status queries.

`UIInputPolicyTests.cpp` exercises the shared production button handler without a renderer: normal click, cancellation after a press, consumed acquisition, explicit scripted pointer click, client-position injection followed by physical motion while Host owns input, multi-control cancellation, physical/injected gesture separation and revision continuity. These tests do not claim to execute the complete rendered UI pipeline or OS interaction.

Native compilation, CPU tests, existing hidden Playtests and guarded interactive OS checks must be run by the build owner. Staging or passing a pure test does not prove Win32 focus, clipping, visibility or child-abnormal-exit behavior.

## Automated Play isolation

Command Bus `play.control` and `editor.op.invoke` (`play.start` / `play.toggle`), including Playtest scenarios, start an input-injected-only session. The operator bridge supplies the invocation source; request arguments cannot impersonate an interactive UI invocation.

- Automated sessions keep physical game input owned by the host. Cursor requests remain inspectable, but `AcquireGameInput` cannot apply them, even after focus changes or a Game View click. Explicit `input.inject` continues to work.
- Automatic Game View focus, maximization and focus-on-appearing are disabled for the session. Interactive toolbar, shortcut and menu Play retain their saved focus behavior.
- Deferred shadow baking and standalone save confirmation preserve the startup policy. Standalone passes `--play-no-physical-input` with `--editor-play`, applies the policy before window creation, and uses non-activating show/resize/fullscreen operations.
- `Cursor::ResetForEditor` never re-enables physical input. The in-editor session restores its previous host policy on completion or failed startup, preserving an injected-only batch host. A later interactive Play explicitly enables physical input without acquiring it until the existing handoff rules allow acquisition.
- Existing application focus checks and detached Game View targeting remain unchanged. Isolation does not depend on the cached foreground flag.

Regression coverage: `AutomationPlayDispatchTest`, `CursorControlPolicyTest`, `GameInputConsumptionTest`, and `StandalonePlaySessionTest`. These tests do not move the real cursor or launch game windows.
