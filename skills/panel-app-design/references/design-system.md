# Design System for 1024x600

## Brief

Write these down before drawing:

- purpose and audience;
- tone (calm tool, dense dashboard, game poster, editorial, industrial);
- real copy, including Chinese punctuation;
- required states: idle/running/paused/done, selected/pressed/disabled,
  empty/error;
- hardware constraints: no screenshot, one full-screen bitmap, 1.4 MiB FAT.

Then write a compact plan: 4-6 color tokens, font roles, an ASCII wireframe with
coordinates, one memorable element, and a sentence explaining why the result is
not a generic template.

## Grid

- canvas: 1024x600, full-screen, no scrolling unless deliberate;
- safe margin: 32-40 px;
- header: 0-100 px;
- content: x=40..984, y=100..576;
- bottom action row: 56-64 px high, buttons at least 44 px tall.

Reference layouts:

- timer: panel 680x472 at (172,104); ring 290 at center (512,300); preset row
  y=462 h=50; control row y=520 h=48; buttons 120/200/120 with 20 px gaps.
- card/game: full-bleed table or scene, fixed sprite sizes (for example
  96x140), status strip at top or bottom, controls outside the play area.

## Tokens (starting point, not a mandate)

```text
bg        #070b14
panel     #111a2b
panel-2   #0c1322
accent    #36d1c4
warning   #ffcf6b
danger    #ff5f6d
text      #f2f6ff
muted     #7f8ca6
button    #1b2740
border    #2c3d5e
```

For a poster-like App, choose a scene palette instead: deep ink, one warm
accent, one metallic gold, and muted paper. Do not use the default purple/cyan
gradient unless the brief asks for it.

## Type

- display/logo: 40-64 px, often an image rather than text;
- title: 34-48 px;
- digits: 72-96 px;
- body: 18-20 px;
- small: 16 px;
- Chinese line height is about 1.44x the font size.

Load one TTF subset and set it as default. Use explicit font handles for display
and small text. Check coverage before uploading.

## Interaction

- touch targets >=44 px;
- leave 8-20 px between rows that can be pressed;
- calculate `y + h` before writing the next row; avoid negative coordinates
  that push content off-screen;
- center with the object option `align="center"`; `text_align` is not
  implemented;
- every interactive state needs a visible pressed/selected/disabled difference;
- provide a clear empty/error state with the next action.

## Anti-template checklist

Reject these unless the brief explicitly calls for them:

- every section is the same rounded card with the same shadow;
- centered title + centered subtitle + centered big number on every screen;
- decorative 01/02/03 labels with no meaning;
- all-uppercase micro-labels everywhere;
- every element fades/slides in;
- emoji used as icons;
- glowing everything with no hierarchy;
- default purple/teal gradient on a dark background.

Keep one memorable element: a strong title, a single oversized number, a
distinctive frame, or a good background. Let the rest be quiet.

## Verification without screenshots

There is no screenshot or pixel-probe API. Use:

1. an ASCII wireframe with coordinates;
2. static checks for overlaps and font coverage;
3. a short serial log of which layers used SD vs fallback;
4. the user's visual confirmation on the real panel.
