# Screen reader accessibility

The `src/access/` layer makes screens usable with a screen reader. It builds a parallel navigation tree over live game state, speaks focus changes through Tolk, and activates controls through the game's own field handling. It follows the Graph A11y Kernel specification used by several other game accessibility mods.

The layer is a prototype. The main menu, New Game, and galaxy screens and the game's message box have dedicated recipes; every other screen falls back to a generic view of the live field list.

## Running it

Copy the x64 or x86 `Tolk.dll` that matches the build, together with its screen reader client DLLs, next to `orion2re.exe`. Without Tolk the layer still runs and writes `speech.log` in the working directory, which records every line it would have spoken. `I` marks speech that interrupts and `Q` marks queued speech.

| Key | Action |
| --- | --- |
| Up / Down | Move between controls. |
| Left / Right | Move within a row, or adjust a slider. |
| Home / End | First / last control. |
| Tab / Shift+Tab | Next / previous zone. |
| Enter | Activate the focused control. |
| Backspace | Right-click the focused control, where the game gives it a right-click action. |
| Space | Read the focused control's help text. |
| Ctrl+L | Repeat the screen name and focused control. |
| Ctrl+R | Repeat the last spoken line. |
| Ctrl+Shift+A | Turn the layer off or on. |

Escape and every other key keep their game meaning. Numeric keypad keys are never claimed, because screen readers use them for their own commands.

## Source layout

- `src/access/graph/` is the kernel, lifted unchanged from the OpenXcom port apart from its namespace. It is a self-contained component in upstream style and throws `std::logic_error` on builder misuse; `navigator.cpp` catches those at the single point where recipes are built. Its conformance suite is `test_access_graph`.
- `navigator.cpp` owns screen matching, key dispatch, the announce-once differ, and the activation handoff.
- `screens.cpp` holds the screen recipes and the control-type registry.
- `help_text.cpp` reads titles and bodies from the current language's help LBX for the screen's active help list.
- `text.cpp` removes inline text commands and decodes CP850; `vocab.cpp` holds every string the layer authors.
- `speech.cpp` loads Tolk with `SDL_LoadObject`, chunks long text, and writes the transcript.

## Game integration

The layer touches the game in four places:

- `platform.cpp` offers each key-down to `access::navigator::Offer_Key_Down()` on the platform thread before translation. A claimed key is queued for the game thread and never reaches the legacy keyboard ring. The claim check reads only atomics, so it never waits for the game thread.
- `fields::Get_Input()` calls `access::navigator::Tick()` on every poll. A requested activation then runs through the same press path as a field hotkey, so radio and locked toggles, the pressed drawing, the auto function, and the field sound all behave as they would for a keyboard shortcut. A requested right-click follows `Interpret_Mouse_Input`'s order instead: a help box under the pointer opens first, then an enabled cancel returns -1, and otherwise the game reads the negated field index.
- `TEXTBOX::Do_Text_Box()` publishes the title and text it is showing while it waits for input.
- `MOX2::thunk_main2()` initializes and shuts down the layer on the game thread.

Keys are claimed only while a cursor is seated, no continuous text field is active, and the game thread has polled fields within the last 300 milliseconds. Text-entry loops, animations, and loads that do not poll fields therefore receive their keys normally.

## Recipes

A recipe is an `s_screen_recipe`: an `Is_Active` predicate re-derived from live state each tick, an instance key, a `Build` function, and a screen name. The first active recipe in `screens::Recipes()` wins, and a change of recipe or instance key announces the screen again.

The generic `fields` recipe lists every on-screen field in declaration order. It labels a field with the text the game draws on it, then the title of the help box containing its centre, then its hotkey. It omits keyboard-only hotkeys and lists a full-screen catch-all region only when the screen has no other control.

Per-screen recipes should replace the generic one wherever a screen draws its values as text or pictures, because those values are not stored in the field list. The New Game recipe is an example: each setting has two fields that share one help box, and the selected value is drawn separately, so the recipe reads the value from the string the screen draws.

When the focused control changes, the pointer moves to the centre of its field so that the game's hover highlight follows the cursor.

A recipe may also declare a `Poll` hook, run every tick while it is attached, to notice changes inside the screen's own loop, such as the galaxy screen's system display opening. Rather than speaking, it calls `navigator::Request_Focus_Stop()`: the differ moves focus into that Tab-stop on its next pass and announces the landing, so every focus change is still spoken from one place.

The galaxy recipe lists every star by distance from the player's capital, using the same visibility rules as the drawn star names, and fleets only from the detection-filtered ship icons. The main loop hit-tests the pointer when its map grid is pressed, so a star node places the pointer on a point that `MAINSCR::Find_Star_At_XY` and `Find_Ship_Icon_At_XY` resolve to that star, and presses the grid. A star outside the current view is first brought into view with `MAINSCR::Center_Map`, as the game's mini-map does, so the player never manages zoom or scrolling.

The system display that opens over the map has no text in its fields: the game draws planet and fleet details from data when the pointer hovers over them. Its stop lists planets innermost first, then fleets in orbit, gates, and the wormhole line, each read with the lines of the matching hover box (`SYS::Do_Print_Planet_Info`, `Do_Print_System_Ship_Data`, `Print_Gate_Info`). The title bar, background, and orbit grid are left out. Pressing a planet or fleet presses its field, so the game opens the colony screen or fleet box as a click would. An asteroid belt has no field, so its node places the pointer on the belt's orbit and presses the orbit grid. Backspace on a planet right-clicks it, which opens the game's colony potential box.

The fleet box's stop reads the status line `FLEETPOP::Print_Fleet_Box_Info` draws, then each ship and whether it is selected, then its buttons labelled from the help list. The game selects a ship only while the left button is held over its icon: the main screen's draw calls `MAINSCR::Set_Painted_Fleet_Fields` for the icon under the pointer, so Enter on a ship makes that same call. A press would only show the ship's name. While ships are selected, a star also reads the ETA line the fleet box shows for the star under the pointer, and pressing a star orders the move.

`TEXTBOX::Do_Text_Box` leaves the screen's fields in place under its own full-screen field, so it publishes the title and text it shows in `TEXTBOX::shown_title` and `shown_text` while it waits for input. The text box recipe matches first while those are set, and lists the message one line at a time. Enter on any line dismisses the box, as any click would.
