<!-- @file    csharp-runtime-textures.md -->
<!-- @brief   Script-owned writable texture masks and transient material bindings. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-26 -->
# C# runtime textures

`RuntimeTexture.CreateR8(width, height)` and `CreateRGBA8` create zero-cleared
linear UNORM textures. Dimensions are pixels, 1–16384 per axis. Backend failure
throws; ordinary asset loading and `TextureAssetRef` remain unchanged.

`UpdateRegion(x,y,width,height,ReadOnlySpan<byte>,rowPitch=0)` uses a top-left
origin. Zero pitch selects tightly packed rows. R8 consumes one byte and RGBA8
four bytes per texel. The required span length is `(height-1)*pitch+rowBytes`.
The complete rectangle is validated and copied into an unpublished reception
before one renderer-thread `ITexture::UpdateRegion` call. Invalid input changes
neither the CPU publication nor GPU texture. Backend rejection returns false.
Transport packets are bounded to 64 KiB; a texture or rectangle is not limited
to one packet. Uncommitted receptions are discarded at the callback boundary.

The exact creating Script, managed generation, instance and Scene own the
resource. Every operation obtains fresh synchronous callback authority. A
retained object does not preserve a callback or cross-owner access. `Dispose`
releases deterministically; disable/fault/destroy/retirement also release the
native records. No finalizer invokes the Engine. An already retired facade can
be disposed from a later generation without reviving its native resource.

`Material.TrySetRuntimeTexture(slot,shaderProperty,RuntimeTexture)` validates the live
Material birth, texture owner and reflected texture slot. The binding is
transient and never enters an asset GUID, authoring snapshot or Scene copy.
Shader property names and the canonical slots (`albedo`–`tex7`) are accepted;
reserved engine slots are rejected. Per-draw resolution checks the resource
manager identity, reset version and resource handle generation. Disposed or
reset resources therefore cannot become dangling SRVs. Explicit texture,
shared-material and override-clearing operations replace the runtime binding.

The native path reuses `ResourceManager::CreateDynamicTexture` and dynamic
R8/RGBA8 texture updates already used by the font atlas. This exposes resource
creation, byte upload and binding; brush painting and UV rasterization remain
ordinary game C# logic.
