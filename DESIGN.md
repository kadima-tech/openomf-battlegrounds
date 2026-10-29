# Design

This project reimplements a 2003 game, so its look is the original's: the menus, HUD, fonts and
colours all come from the game's own data files. This file records the conventions our engine uses
to present that data, and the few places where the look is our own.

## Front end (menus)

- **Scene.** The 3D GUI scene in OMFBGGUI.DEObjects: persistent `V.*` actors (the frame, the
  transition doors, the tunnel backdrop, the environment) and one `F.<Screen>` folder per page, its
  `VE.*` actors the buttons. Their meshes, materials, lights, particles and scripts are the game's.
- **Frame.** The GUI is laid out in a 4:3 field (x 0..1, y 0..0.75) and is drawn 4:3, pillar- or
  letterboxed in the window, through a 60° horizontal field of view looking down −Z (FINDINGS 126).
- **Buttons.** A hit rectangle over each button's `DisplayArea` mesh. Hover, leave and click go to
  the button's script (`GUIEnter`, `GUILeave`, `GUISelect`), which spins its rotors, lights its
  lightning and sparks and plays `MouseEnter` / `ButtonClick`.
- **Labels.** Flat text in the engine's `Font_Ariel` (from Core.ModuleDLL), 16 of 600 virtual pixels
  high, white, centred in the button.
- **Page changes.** The doors (`V.MainDoors`) slide shut (350 ms), the page is swapped behind them,
  and they slide open (500 ms). The main frame shows on every page except the main menu and in-game
  menus. The doors cover everything on the page, its text, images and bars included; only
  notifications draw over them.
- **Controls** (the engine's 3D GUI controls, `game/controls.*`). Bevelled frames (5 quads, per-corner
  highlight / shadow colours with a slight face gradient, swapped when pressed); faces grey 0.5 at
  half alpha, window backs (0, 0, 0.1) at 0.7, white button text, a 30-pixel blue-to-navy title bar;
  lists with (0, 0, 0.5) selected rows and 24-pixel tiled scroll bars without arrows; combo boxes,
  sliders, check and radio buttons, edit fields with a yellow caret; Yes/No and text-input dialogs.
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
  robot on `RobotPedistal` in the last chosen pilot's colours. Over it, in the LoadFrame rectangle:
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
- **Single-player loading screen** (the rules display). No main frame; the match name (19 px,
  pale yellow), commentary (14 px, pink), next match (16 px, grey) and progress text in the
  page's markers; the match snapshot (a 256 × 256 capture from the observer's camera) fills the
  MatchScreenShot box once the player has been knocked out or landed a kill.
- **Notifications** (`DE_CDialog3d::UserInformation`, measured from a screenshot of the
  original). A centred box 0.737 screen heights wide with a light grey bevelled frame, a navy
  title bar shading from (0, 0, 0.08) on the left to (0, 0, 0.68) on the right with the title in
  white and a grey-blue close box with a black X, a grey separator, then a black body: the
  `Not_UserInformation` icon top left, the text centred and wrapped to the body's width, and OK
  bottom right in a blue (0.17, 0.16, 0.57) frame on a slate face. Only OK or the close box (or
  Enter / Esc) closes it.

## In game

- **HUD and messages.** Drawn from the scripts' screen-space primitives (text in `Font_Ariel` or the
  in-game `Font_Metalic`, quads, status bars), as the game's own HUD data describes them.
- **Shadows (off by default).** Each robot's silhouette darkens the floor under it to about half (Medium, High), from the static lights' direction (High adds one from the moving lights); Low casts one from straight above, darker.
- **Camera.** The game's fight camera (the player's `CameraActor`, 70.5° horizontal at 4:3) while the
  player fights. Before and between rounds, after the player's fighter is beaten, and through a whole
  replay the view is the observer camera, which cuts between the arena's vantage points, orbits and
  follows the fighters, its field of view easing with the framing (Manual Observer in the VCR panel
  flies it from the keys and mouse at 60°).
