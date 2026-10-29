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
- **Lists (description selectors, Replays).** Rows 20 screen pixels at 768 high, names 19 high, white;
  the selection a solid navy bar (0, 0, 0.5), Windows' classic highlight as the original shows it;
  locked entries grey. No title above the list.
- **Pilot panel.** The name centred at the top; the photo (to 0.58 across) in a pale frame; beside it
  four stat rows (icon tinted (0.5, 1, 1), name, a teal well filled cyan) and "Colors" over the
  pilot's three colour swatches; the biography centred below.
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
- **Notifications (our own look).** A centred panel, dark blue-grey (0.05, 0.08, 0.12) at 90%
  opacity inside a pale blue (0.55, 0.75, 0.9) edge; the title at 0.035 screen heights, the text
  wrapped at about 52 characters at 0.026, and "OK" in the edge colour. The original's dialog is
  not ported yet.

## In game

- **HUD and messages.** Drawn from the scripts' screen-space primitives (text in `Font_Ariel` or the
  in-game `Font_Metalic`, quads, status bars), as the game's own HUD data describes them.
- **Camera.** The game's fight camera (the player's `CameraActor`, 70.5° horizontal at 4:3).
