# RobotMinimal 生成プロンプト

生成: 組み込み image_gen。PNGは生成結果を無加工で保存。切り出しはFBZZEngineのSpriteメタデータで指定。

## robot_player_frame

Create ONE transparent PNG 2D HUD frame for a robot action game. Simple clean vector-style game UI, flat solid fills and crisp geometric edges. Single long slim horizontal empty dark charcoal gauge track, thin medium-gray border. Robot identity ONLY from two small angular armor clamps at the left and right ends, each made of TWO simple interlocking flat quadrilaterals, and one tiny rectangular slit per end. No screws, no scratches, no surface textures, no tiny decorative lines, no gradients, no lighting, no glow, no shadow, no text, no fill color. Subdued gray tones only. Shape roughly 16:1 aspect ratio. Symmetrical. Straight-on 2D, actual transparent alpha outside frame. This is the fixed frame/background layer; colored fill will be a separate game object.

## robot_boss_frame

Create ONE game UI sprite PNG: a simple boss health EMPTY FRAME. Flat vector-style like an illustrator made solid polygons. Extremely thin long charcoal track with thin medium-gray outline. Robot motif ONLY two compact angular gray armor brackets at each end with ONE inset rectangular dark slot. Symmetrical, fewer than 12 polygons total, no decorative tiny details. Middle uninterrupted simple straight rectangular track. End brackets slightly larger than the player frame, but still restrained. No screws, scratches, metallic texture, gradient, glow, shadow, text, icons. Actual transparent alpha background outside silhouette. Front-on 2D HUD. Very wide thin bar, 22:1. Fixed background layer for separately controlled fill.

## robot_fill_white

Generate a technical game UI alpha mask PNG. ONE pure white solid horizontal rectangle, straight square corners, very long and thin, 24:1 shape, centered. Interior exactly white opaque. Exterior truly transparent alpha zero. No other colors, no gray, no shading, no glow, no shadow, no texture, no frame, no text. Crisp vector-like rectangle. This white fill sprite will be tinted and clipped by a game engine to show health and stamina, so no ornament or endcaps.

## robot_stamina_green

ONE minimal game HUD stamina fill PNG, truly transparent alpha background. A single long very thin rectangular strip filled with uniform fresh grass GREEN #72BD4B. Green, NOT teal or cyan. Square corners. Flat vector-style, no frame, no bevel, no texture, no glow, no shadow, no gradient, no border, no text, no decoration. 24:1 proportions. Intended as a separate fill layer clipped horizontally by a game engine.
