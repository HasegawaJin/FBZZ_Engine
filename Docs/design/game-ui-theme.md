# Shared game UI themes

`UIThemeAsset` is a built-in `.fzdata` asset for Canvas-based game UI. It is separate from the Editor's ImGui theme. Several independent Canvas roots, including screens in different scenes, can reference the same file.

Create one with **Asset Browser > Create > Data Asset > UIThemeAsset**, edit its reflected fields in the existing asset Inspector, and assign it to the `UICanvas.theme` slot. The existing Inspector provides saving and Undo snapshots. `Assets/UI/Themes/SharedGameUI.fzdata` is a small optional starting asset; its font is the existing Roboto atlas and PNG. No external fonts, images or GPU resources are introduced.

## Supported fields

| Target | Shared values | Local override |
| --- | --- | --- |
| UIImage | Image tint | `overrideThemeColor` |
| UIButton on an UIImage object | Normal, hovered, pressed, disabled and navigation focus colors | `overrideThemeColors` |
| UIText | Text color | `overrideThemeColor` |
| UIText | Font reference; empty selects the existing project default | `overrideThemeFont` |
| UIText | Font size, letter spacing, line-spacing multiplier | `overrideThemeTypography` |
| UILayoutGroup | Main and grid cross spacing | `overrideThemeSpacing` |
| UILayoutGroup | Left, top, right, bottom padding | `overrideThemePadding` |

All override flags default to false. They matter only when the Canvas has a valid theme. Empty, missing, wrong-type or numerically invalid theme assignments use the original component values. Existing scenes have an empty theme assignment and retain their previous appearance. Inline values remain serialized so clearing a theme or changing themes does not destroy local settings.

Themed button state colors multiply image tint. The priority is disabled, pressed, navigation focused, pointer hovered, normal. Focus comes from enabled `UINavigation` on the button object; navigation itself still uses the existing Canvas navigation settings. A local button color override retains the existing relative tint algorithm. Set the image tint override as well when the entire button should keep its local appearance.

Slider/toggle/input-field internals, sprite swaps, textures, materials, animations, sounds, rect anchors, hit areas, auto-size limits and content-size fitter padding remain individually authored. The theme changes only the listed values; it is not a widget framework. Use independent Canvas roots for screens. Nested Canvas composition follows the existing multi-pass behavior and is not redefined here.

## Runtime and persistence

Native callers can switch a live screen on the normal Scene/AssetManager owner thread:

```cpp
auto* canvas = screen.GetComponent<fbzz::scene::UICanvas>();
if (canvas) canvas->theme.path = "guid:d47673217b2e4d3ca9565a78329d8c44|Assets/UI/Themes/SharedGameUI.fzdata";
```

Clear `theme.path` to restore inline settings. The next UISystem call resolves one value snapshot per Canvas and uses that same snapshot for text measurement, layout and rendering. Existing DataAssetRegistry edits/reloads are visible on the next call. No new C# API is added; the reflected Editor slot and native field are the supported entry points in this change.

Scene serialization uses the existing GUID codec for the theme reference. Font values accept a `.fnt` GUID/path, a dynamic `.ttf`/`.ttc`/`.otf` file, or the existing extensionless atlas base path. The existing font picker/drop returns an atlas base without `.fnt`; the theme resolves that base through its registered `.fnt` file before saving, so picker assignments also persist as GUIDs and survive font moves. Normalization runs before reflected writes and after picker/read changes. Existing GUIDs, explicit dynamic files and unregistered legacy atlas bases retain their representation. A missing explicit font has the existing UIText loading behavior; empty font selects the project default.

The asset owns only CPU values and strings. UISystem keeps no DataAsset pointer across frames. Working text/layout copies preserve component authoring; runtime text size fields continue to be updated as before. Existing font atlas cache, texture resolver, resource reset/version checks and owner-thread release paths keep ownership of all graphics resources. Theme switches reuse those caches. Updating the contents of an already cached font follows the existing explicit UISystem font-cache flush contract; switching themes does not force unrelated atlas rebuilds.

## Verification

`UIThemeAssetTests.cpp` covers create/save/reload/snapshot restore, two Canvas assignments, runtime switching, empty/missing/removed/wrong-type/invalid theme fallback, real button event state resolution, focus/disabled priority, local overrides, scene save/reload, GUID moves, font GUID/base-path resolution, picker-produced atlas bases saved before a font move, preservation of existing GUID/dynamic/legacy values, legacy fields and registry reset. Tests use the production resolver/helpers without a renderer or OS input. They do not validate GPU output; a later Editor playtest should inspect the same theme on two independent screens and all supported states.
