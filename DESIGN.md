# Design

## First-run launcher

The native setup app (`launcher/Launcher.swift` on macOS, `launcher/windows/Launcher.cpp`
on Windows 10/11 and `launcher/linux-launcher.c` with GTK 3 on Linux, the latter two drawn to the
same layout) uses a service-bay theme:
graphite panels, warm ivory type, amber actions and a robot engineering schematic.
The artwork is original and available before the player's disc is unpacked. Its
left rail carries the brand and schematic; the main panel has ISO/drop and CD
actions, three preparation stages, launch, and contextual errors. Settings shows
the local install folder; Help explains disc requirements and first-time setup.
CDs are recognized by the game's installer GUID. A verified ready installation
opens directly into the engine without displaying the setup window.

Launcher colors: panel `#141718`, ivory `#E8E3D4`, muted `#878F8C`, amber
`#FFB529`. Typography uses native system sans serif, rounded heavy headings and
monospaced technical labels; on Linux, the desktop's fonts (a condensed face for
headings when one is installed). This setup interface is separate from the original
game menus documented below.

This project reimplements a 2003 game, so its look is the original's: the menus, HUD, fonts and
colours all come from the game's own data files. This file records the conventions our engine uses
to present that data, and the few places where the look is our own.

The data is the final official patch's (2.123, PATCHES.md); the notes below describe its look.

## Front end (menus)

- **Scene.** The 3D GUI scene in OMFBGGUI.DEObjects: persistent `V.*` actors (the frame, the
  transition doors, the tunnel backdrop, the environment) and one `F.<Screen>` folder per page, its
  `VE.*` actors the buttons. Their meshes, materials, lights, particles and scripts are the game's.
- **Backdrop.** The tunnel (`V.Backdrop`: dim streaked tube, lightning, teal lines) on black. No
  drifting squares: 2.123's `SpotsRenderer` lost its `Blinklightimage` texture, and a particle
  renderer without an image draws nothing (FINDINGS 327).
- **Logo.** "ONE MUST FALL" light brushed metal (only the first pass lit, the base tint unused),
  "BATTLEGROUNDS" bright scrolling fire, unlit (FINDINGS 329); the fire scrolls on every visit.
- **Frame.** The GUI is laid out in a 4:3 field (x 0..1, y 0..0.75), drawn through a 60° horizontal
  field of view looking down −Z and stretched to fill the window (FINDINGS 126, 161). On a window
  wider than 4:3 the field sits unstretched in the middle, its controls with it, and the 3D scene
  (tunnel, iris, backdrops) widens to the window's edges.
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
- **Choose Cast** (between Choose Match and Choose Pilot). The casts listed by name in the left
  window's top half; below, "AI Difficulty:" and (with more than one match unlocked) "Starting
  Match:" combos, labels over the fields, locked matches blank. On the right a "Current Settings"
  strip (a grey header bar over a sunken panel) with each cast's chosen modifier icon in a small
  box, the selected cast's box framed white; under it the selected cast's modifiers as square icon
  buttons, six to a row: 1-pixel bevelled grey buttons (sunk while held or toggled down) with the
  icon filling the face, the chosen one's face cyan behind its icon (on a first visit a default that
  is not the first stays raised, as in the original), over a sunken panel with its description
  centred. Back and Next on the 3D buttons. Hosting, "Available Arenas:" (14 virtual pixels) and
  the multiplayer arenas under the casts, each a check box (the dark sunken box, a white X when
  checked) and its name, in the list's ordinary 20-pixel rows (the box, 23 pixels at 768, overlaps
  its neighbours slightly); all checked at first.
- **Pilot panel.** The name centred at the top; the photo (to 0.58 across) in a pale frame; beside it
  four stat rows (icon tinted (0.5, 1, 1), name, a teal well filled cyan) and "Colors" over the
  pilot's three colour swatches; the biography centred below, then "RECON DATA" and the unlocked
  "Observed Preferences" ("<stat>: <level>"). Text that overflows scrolls with the mouse wheel.
- **Model picker (Choose Creature's View).** The page's own 3D scene, `LoadScene`'s hangar
  (`ChooseFighterModelPickerEnvironment`), fills the 4:3 field through its `EnvironmentCamera`, the
  robot (a `FighterTemplate` creature carrying the model, standing undeformed) on `RobotPedistal`
  in the last chosen pilot's colours. Over it `VE.LoadFrame`'s grey console, and in its LoadFrame
  rectangle five push buttons (2-pixel bevel, half-alpha grey face): the model's name (top 30%),
  the `VCRFastBackward` / `VCRFastForward` images as arrows, Back and Next along the bottom 30%.
  No main frame or backdrop.
- **Replays screen.** `F.MatchBrowser`: the "Recordings Browser" list (Name, Type, Date, Time, Length
  columns, the selected row highlighted) at ListWindow; the selected recording's game type (18
  virtual pixels), name, date, time and length in the Window, with Delete and Rename in a column
  from 0.6 across; Play (disabled until a selection) and Back. Our own: the delete question and its
  Yes / No, and the rename field, drawn in the Window.
- **VCR panel (replays, V).** `F.VCR` along the bottom; in its middle 60%: the Play/Pause, frame
  advance, rewind and fast-forward buttons (2-pixel bevelled grey push buttons, the image filling
  the face and shifting with it when sunk; Play/Pause stays sunk, showing Play, while paused), the
  speed combos, a seek slider, the observer combo and Snapshot.
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
  unit is the text size; at 4:3 and narrower the HUD stretches with the screen's shape (x by its width,
  y by its height), as the original's. Wider screens keep each element its 4:3 size by the screen's
  height and attach it to the left edge, the centre or the right edge by the third of the 4:3 screen it
  sits in: pilot panels, photos, stat bars, names and the scoreboard at the edges, the centre panel,
  timer, announcements and quote texts in the middle, full-width bands across the whole screen, and
  what follows a fighter (its mini stats) over it.
  Every text primitive is on screen (crowd responses and pilot quotes too). A text's lines are
  justified one by one, 1.2 font units apart; quotes sit on a dark blue underlay band (0, 0, 0.15 at
  0.75 alpha) as tall as the text, with soft bands above and below. Stat bars fill along
  their length with a glowing cap at the value's end; the upright bars use the texture turned.
- **Help.** A tree list: Plus / Minus icons 10 px wide at depth × 20 + 5, text at (depth + 1) × 20,
  branches collapsed at first, no tree lines. The topic's page fills the Window: its text blocks
  word-wrapped to the page less the scroll bar's width and twice the font, left-aligned in 2.123's
  data (centred where a page asks, as Credits does), stacked downwards; a page taller than the
  Window gets the launcher's 24-pixel vertical scroll bar and scrolls by bar, track or wheel.
- **Pilot quotes.** The speaker's photo, at the top left, and the dark quote band are drawn over everything in the scene, the photo over the band, with the quote text on top.
- **Arena lights.** A light shines at its colour times its alpha; Blast Furnace is lit grey-white, and each flamethrower throws a warm orange glow only while it fires, fading after.
- **Shadows (off by default).** Each robot's silhouette darkens the floor under it to about half (Medium, High), from the static lights' direction (High adds one from the moving lights); Low casts one from straight above, darker.
- **Camera.** The game's fight camera (the player's `CameraActor`, 70.5° horizontal at 4:3) while the
  player fights. Before and between rounds, after the player's fighter is beaten, and through a whole
  replay the view is the observer camera, which cuts between the arena's vantage points, orbits and
  follows the fighters, its field of view easing with the framing (Manual Observer in the VCR panel
  flies it from the keys and mouse at 60°).

## Content editor

The DEObjects editor (`openbg-editor`, `docs/EDITOR_SPEC.md`) is a desktop tool styled like Unreal
Engine 5, not like the game's menus. `engine/editor/app/theme.{h,cpp}` holds the palette, the sizes
and `theme::apply_style()`, the Dear ImGui style function; the custom-drawn controls are in
`widgets.cpp`.
- **Colours:** chrome (menu, document tab strip, status bar) #141519, ground #191a21, panels #1d1e26
  with #33343f borders and #2c2d38 dividers, panel headers and toolbars #1f2028, inputs #16171d with
  #3b3d4b borders, buttons #2c2d38. Text #e8e8f0, labels #c3c5d3, secondary #a9acbd, muted #9295a6.
  A violet accent: selection #3a3566, active document tab #2e2a52 (its colour runs along the bottom of
  the tab strip), form tab underline #5a4fc0, checkboxes and sliders #6b5ce7, logo #8b7cf6. Play and
  the NEW badge are green #2f8f4e; warnings amber #ffc98a, OK green text #8fe3a8, errors #ff8f8f.
- **Type:** Inter (Regular, SemiBold) 14 px for UI and forms, 13 px for status and logs, 24 px
  SemiBold for document headings; JetBrains Mono for GUIDs, logs and the console; Lucide icons merged
  into Inter (no emoji). Fonts are rasterised at the framebuffer scale, so Retina is sharp; on Windows
  and Linux the display's content scale multiplies sizes and fonts.
- **Shape:** 5 px rounded inputs and buttons, 6-8 px rounded panels and tiles, 26 px tree rows,
  generous padding (10-20 px). Forms are label / field rows with a 180 px label column.
- **Layout:** menu bar with the violet logo; document tabs (Level Editor, then one per open asset,
  each closable); a toolbar (Save All, Content, Validate, Place, green Play, Simulate, Launch Game,
  Play Mode on the right); the level editor's dockable panels (Place left, Viewport centre, Outliner
  over Details right, Content Browser / Output Log / Validation / Script Console docked below); a
  status bar ("Ready" left, "<add-on> (Add-on) | DE v2.123" right, or "No add-on open"). The default
  dock gives the bottom (Content Browser) 45% of the height and Details two thirds of the right column.
- **No add-on open:** the full layout still shows; the viewport carries a small welcome card (title,
  one line, a violet "Reopen <last>" button when there is one, Open / New Add-on, a short recent
  list); the Outliner shows an empty state. Actions that need an add-on open a modal chooser
  (recent add-ons, Open..., New..., Cancel). A violet "accent" button tone (#3a3566 fill, #5a4fc0
  border, white text) marks the primary choice.
- **Content Browser:** a 250 px tree (the add-on with its types and counts, or an "Open or create an
  add-on..." row; a By Type / By File segmented toggle; "Base Game (Read-Only)" with a lock icon, then
  types or files with folders) and a virtualised grid of 150 x 160 tiles (or a list: name, class,
  file, BASE / NEW). Tiles show the thumbnail fitted on a checkerboard (pilot photos fill the tile)
  or the class icon tinted by type, the name, the short class below, a grey "BASE" badge on base
  content (green "NEW" on add-on content) and a violet border when selected. A breadcrumb (file >
  folders, Up) sits over the grid in By File.
- **Viewer tabs** (any object): a large preview panel on the left (image with channel toggles and
  fit / 1:1; a code view for scripts; a waveform with a green Play; the animated robot) and a 380-560
  px info panel on the right: name, class, a violet-bordered read-only banner with a lock for base
  content, then Details' sections (SeparatorText headings, label / value tables, violet links).
- **Code view:** JetBrains Mono 13 px on #16171d with faint line numbers; comments #6a9955, strings
  #ce9178, numbers #b5cea8, keywords #c586c0, types (C/E/B + capital) #4ec9b0, members and globals
  (`m_`, `g_`) #9cdcfe. The tournament's script view: keywords #569cd6, calls #dcdcaa, globals
  #4ec9b0, members #9cdcfe.
- **Read-only pilot:** the pilot editor with a "BASE GAME - READ-ONLY" badge by the heading, a banner
  with "Copy to Add-on..." (accent), and dimmed, disabled fields.
- **Asset editors:** the pilot editor puts a tall in-engine preview (photo, or the robot in the
  pilot's paint) with a "Preview (in-engine)" footer on the left, and on the right the heading, tabs
  (General / Attributes / AI Traits / Paint / Casts & Flags) over a violet underline, and a footer of
  validation chips.
- **Tournament editor:** three columns. Left, a "SESSION MASTER" tree (the tournament with an amber
  trophy, Matches (n) with M1... rows and a swords icon, Branches, Unlocks, Ending cinematic) with
  + Match / move / delete under it. Middle, the selected item's form ("Match: M2 Wild Science" in
  20 px SemiBold, 170 px labels; opponent pilot and robot dropdowns side by side, "Random ..." as the
  empty choice; quote rows of a speaker dropdown, a two-line text box and an x; On Win / On Lose
  dropdowns with an arrow icon) and a footer with a compile chip, Compile and a green "Play from
  match". Right, tabs "Script (BSubSession)" | "Script (BMaster)" | "Quotes": the code view with
  line numbers, `// <user ...>` blocks tinted #2e2a52 with their markers in the accent text colour,
  compile errors as #5a2a2a rows with red line numbers (tooltip with the message), the line a
  Validation link jumps to outlined in the accent; "Edit user blocks" turns it into a monospace text
  box whose Apply refuses edits outside the blocks.
- **HAR editor:** the pilot editor's shape: the robot (the game's fighter shader, drag to turn, an
  animation dropdown in the footer) on the left with its spheres drawn on their bones (attack
  spheres #ff6b6b, body spheres #6bb8ff, as 1 px outlines with a centre dot; the selected one
  orange, filled and 2.5 px); tabs General / Movement / Inputs / Hitboxes / Sounds / Paint / Script
  on the right. Inputs: a sequence list beside step cards of a 3 x 3 direction pad (lit cells
  #3a3566), six button toggles (grey any, green pressed, dark red released) and four timing fields.
- **Robot import** (the Import dialog's Bones / Animations / Hitboxes pages): the Bones page is a
  table of rig bones (a status icon, the bone, "optional" dimmed), a searchable joint dropdown per
  row (the joint's current owner in brackets), "found by" coloured by source (name / preset green
  #8fe3a8, guess amber #ffc98a, manual violet #8b7cf6, missing red #ff8f8f) and a 64 px confidence
  bar in the same colour; a Preset combo ("Auto (Mixamo)"), Map again and Clear above, with count
  chips. The preview draws the new robot (faces flat-shaded in the demo paint) beside the base robot
  in grey, the file's joints as violet dots with lines (unmapped grey, the selected bone's orange),
  the base's bones in blue (#6bb8ff) and the new rig's in green when posed; a footer has the pose
  combo (Bind pose or any sequence), play / pause and Base robot / Skeletons toggles.
- **Arena viewports:** the game's renderer fills each pane (lit, unlit, or collision: flat-shaded
  surfaces and affector meshes on #121318). Dark overlay buttons at the top left (layout, view,
  shading, Show menu) and top right (move / rotate gizmo, magnet snap, maximise); the arena's name
  dimmed beside them; a 2 px axis triad bottom left and the camera position bottom right. Overlay
  colours by kind: spawn points #4fc3f7 (numbered discs with a facing line), pickup spawners #8fe3a8
  (diamonds), nav links #f5d36b (lines, arrowheads, dots at the areas), surfaces #b18cff, force
  affectors #7fd1c7, hazards #ff8f6b, death volumes #ff5a5a, lights #ffe08a. The selection is white
  with an orange (#ff9f3a) outline; gizmo axes red / green / blue, yellow (#ffe066) under the mouse.
  With 2 or 4 panes the selected one has a 1.5 px violet border. Labels show for the selected or
  hovered marker only. "Copy to Add-on..." (accent) sits bottom right of a base arena.
- **Play in editor:** the toolbar's Play and Simulate become Pause / Step / speed (0.25x, 0.5x, 1x) /
  Eject / a green Stop while a match runs; the viewport gets a 3 px green (#2fbf5e) border, amber
  (#ffc98a) and 2 px while ejected, with a status pill ("PAUSED", "EJECTED (F8)") at the top. The Live
  Inspector is a tab beside Details: a creature picker, State / Stats (health green, energy blue,
  stun amber bars) / Physics / Input buffer sections; with no match it shows the Play Mode form,
  which also opens from the toolbar's Play Mode combo ("Jaguar vs Katana").
- **Import dialog** (File > Import 3D Model, or a file dropped on the window): a wide modal with no
  title bar, its heading (download icon, SemiBold) and the source path dimmed. Left, a 190 px step
  list on the panel-head colour: numbered circles (violet #6b5ce7 fill on the current step, a red x
  on a step with errors) with icon and title, the current row on the selection colour, and error /
  warning chips below. Middle, the page: SeparatorText sections, a 160 px label column, segmented
  accent buttons for choices (Prop | Arena, +Y | +Z), tables with role dropdowns whose icon is
  tinted by role (surfaces #b18cff, walls and jump pads #7fd1c7, death #ff5a5a, spawns #4fc3f7,
  pickups #8fe3a8, ignored muted). Right, a turntable preview on the viewport colour (#121318):
  visual meshes textured with a soft key light, collision and gameplay as 38 % tinted overlays and
  marker rings in the role colours, Visual / Collision toggles under it. Footer: Cancel, Back and an
  accent Next or Import. The Place panel lists imported props under "Imported Props" (violet box
  icons).
- **Not yet active** features (HAR and tournament editing) keep their place in
  the layout, greyed or with an empty state that says they arrive in a later iteration.
