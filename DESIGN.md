# Design

## First-run launcher

The native macOS setup app (`launcher/Launcher.swift`) uses a service-bay theme:
graphite panels, warm ivory type, amber actions and a robot engineering schematic.
The artwork is original and available before the player's disc is unpacked. Its
left rail carries the brand and schematic; the main panel has ISO/drop and CD
actions, three preparation stages, launch, and contextual errors. Settings shows
the local install folder; Help explains disc requirements and first-time setup.
CDs are recognized by the game's installer GUID. A verified ready installation
opens directly into the engine without displaying the setup window.

Launcher colors: panel `#141718`, ivory `#E8E3D4`, muted `#878F8C`, amber
`#FFB529`. Typography uses native system sans serif, rounded heavy headings and
monospaced technical labels. This setup interface is separate from the original
game menus documented below.

This project reimplements a 2003 game, so its look is the original's: the menus, HUD, fonts and
colours all come from the game's own data files. This file records the conventions our engine uses
to present that data, and the few places where the look is our own.

The data is the final official patch's (2.123, PATCHES.md); the notes below describe its look.

## Front end (menus)

- **Scene.** The 3D GUI scene in OMFBGGUI.DEObjects: persistent `V.*` actors (the frame, the
  transition doors, the tunnel backdrop, the environment) and one `F.<Screen>` folder per page, its
  `VE.*` actors the buttons. Their meshes, materials, lights, particles and scripts are the game's.
- **Backdrop squares.** 2.123's tunnel backdrop drifts large translucent pale-blue squares
  (additive, (0.62, 0.86, 1) at 0.09): the backdrop's `SpotsRenderer` lost its `Blinklightimage`
  texture in 2.123, so its 5-unit billboards draw untextured.
- **Frame.** The GUI is laid out in a 4:3 field (x 0..1, y 0..0.75), drawn through a 60° horizontal
  field of view looking down −Z and stretched to fill the window (FINDINGS 126, 161).
- **Buttons.** A hit rectangle over each button's `DisplayArea` mesh. Hover, leave and click go to
  the button's script (`GUIEnter`, `GUILeave`, `GUISelect`), which lights the button's glass panel
  (`LightingGlass`, `LightedGlass_Back` / `_Next`) while the mouse is over it and plays `MouseEnter` /
  `ButtonClick`; the electric arcs and sparks of the disc release are gone.
- **Labels.** Flat text in the engine's `Font_Ariel` (from Core.ModuleDLL), 16 of 600 virtual pixels
  high, white, centred in the button.
- **Page changes.** The doors (`V.MainDoors`) are an iris: 16 leaves turned 22.5° apart, swung in
  along `IrisLeafPath` (410 ms), the page swapped behind them, then swung open. The launch shows the
  main menu with the iris opening once and nothing closing first. The main frame shows on every page
  except the main menu and in-game menus. At launch the screen stays black for the app manager's
  first five frames; the menu then shows for the doors' 55 ms opening delay before the leaves
  (parked off screen until then) appear almost closed and swing open, as in the original. The iris covers everything on the page, its text, images
  and bars included; only notifications draw over it.
- **Controls** (the engine's 3D GUI controls, `game/controls.*`). Bevelled frames (5 quads, per-corner
  highlight / shadow colours with a slight face gradient, swapped when pressed); faces grey 0.5 at
  half alpha, window backs (0, 0, 0.1) at 0.7, white button text, a 30-pixel blue-to-navy title bar;
  lists with (0, 0, 0.5) selected rows and 24-pixel tiled scroll bars without arrows; combo boxes,
  sliders, check and radio buttons, edit fields with a yellow caret; Yes/No and text-input dialogs.
  A read-only control (an Apply with nothing to apply, Play with no recording) is dimmed by a black
  cover at half alpha.
  Sizes in screen pixels (the original's units), so controls stay the same pixel size at any
  resolution.
- **Lists (description selectors, Replays).** Rows 20 screen pixels at 768 high, names 19 high, white;
  the selection a solid navy bar (0, 0, 0.5), Windows' classic highlight as the original shows it;
  locked entries grey. No title above the list.
- **Pilot panel.** The name centred at the top; the photo (to 0.58 across) in a pale frame; beside it
  four stat rows (icon tinted (0.5, 1, 1), name, a teal well filled cyan) and "Colors" over the
  pilot's three colour swatches; the biography centred below, then "RECON DATA" and the unlocked
  "Observed Preferences" ("<stat>: <level>"). Text that overflows scrolls with the mouse wheel.
- **Model picker (Choose Creature's View).** The page's own 3D scene, `LoadScene`'s hangar
  (`ChooseFighterModelPickerEnvironment`), fills the 4:3 field through its `EnvironmentCamera`, the
  robot (a `FighterTemplate` creature carrying the model, standing undeformed) on `RobotPedistal`
  in the last chosen pilot's colours. Over it, in the LoadFrame rectangle:
  the model's name (top 30%), the `VCRFastBackward` / `VCRFastForward` images as arrows, Back and
  Next along the bottom 30%. No main frame or backdrop.
- **Replays screen.** `F.MatchBrowser`: the "Recordings Browser" list (Name, Type, Date, Time, Length
  columns, the selected row highlighted) at ListWindow; the selected recording's game type (18
  virtual pixels), name, date, time and length in the Window, with Delete and Rename in a column
  from 0.6 across; Play (disabled until a selection) and Back. Our own: the delete question and its
  Yes / No, and the rename field, drawn in the Window.
- **VCR panel (replays, V).** `F.VCR` along the bottom; in its middle 60%: the Play/Pause, frame
  advance, rewind and fast-forward images (a cyan underline marks the running fast mode), the speed
  labels, a seek slider (pale blue track, cyan knob) and Observer / Snapshot labels.
- **Single-player loading screen** (the rules display). No main frame; the match name (19 px),
  commentary (14 px) and next match (16 px), all pale blue (0.8, 0.9, 1), and progress text in the
  page's markers; the match snapshot (a 256 × 256 capture from the observer's camera) fills the
  MatchScreenShot box once the player has been knocked out or landed a kill.
- **Notifications** (`DE_CDialog3d::UserInformation`, built from the same controls as every
  dialog). A centred dialog with the standard VPU 4 up bevel and caption title bar (title, close
  box), sized to its text: a VPU 3 down panel in the window back colour at alpha 0.7; the
  `Not_UserInformation` icon (64 × 64 VPU, tinted white at 0.8) centred in a VPU 75 column on the
  left; the text lines (VPU 14, rows 1.5 high) centred beside it, wrapped at VPU 400 / 600 / 700
  less the column; OK as a plain 15-VPU label in a VPU 120 × 38 box (black at 0.2) centred in a
  bottom strip VPU(VPU 60) high, at the right, filled by a push button (2-pixel up bevel, grey face
  at half alpha), which under the mouse glows blue over the button from the box's top-left and
  bottom-right corners.
  OK, the close box, Enter or Esc closes it; it drags by its title bar.

## In game

- **HUD and messages.** Drawn from the scripts' screen-space primitives (text in `Font_Ariel` or the
  in-game `Font_Metalic`, quads, status bars), as the game's own HUD data describes them. One font
  unit is the text size; HUD letters stretch with the screen's shape (x by its width, y by its height).
  Every text primitive is on screen (crowd responses and pilot quotes too). Stat bars fill along
  their length with a glowing cap at the value's end; the upright bars use the texture turned.
- **Help.** A tree list: Plus / Minus icons 10 px wide at depth × 20 + 5, text at (depth + 1) × 20,
  branches collapsed at first, no tree lines. The topic's page fills the Window: its text blocks
  word-wrapped to the page less the scroll bar's width and twice the font, left-aligned in 2.123's
  data (centred where a page asks, as Credits does), stacked downwards; a page taller than the
  Window gets the launcher's 24-pixel vertical scroll bar and scrolls by bar, track or wheel.
- **Shadows (off by default).** Each robot's silhouette darkens the floor under it to about half (Medium, High), from the static lights' direction (High adds one from the moving lights); Low casts one from straight above, darker.
- **Camera.** The game's fight camera (the player's `CameraActor`, 70.5° horizontal at 4:3) while the
  player fights. Before and between rounds, after the player's fighter is beaten, and through a whole
  replay the view is the observer camera, which cuts between the arena's vantage points, orbits and
  follows the fighters, its field of view easing with the framing (Manual Observer in the VCR panel
  flies it from the keys and mouse at 60°).
