# Screen reader accessibility

The `src/access/` layer makes screens usable with a screen reader. It builds a parallel navigation tree over live game state, speaks focus changes through Tolk, and activates controls through the game's own field handling. It follows the Graph A11y Kernel specification used by several other game accessibility mods.

The layer is a prototype. The main menu, New Game, galaxy, and research screens and the game's message box have dedicated recipes; every other screen falls back to a generic view of the live field list.

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
| Ctrl+R | Repeat the last spoken line, or read the whole text while typing into a field. |
| Ctrl+Shift+A | Turn the layer off or on. |

Escape and every other key keep their game meaning. Numeric keypad keys are never claimed, because screen readers use them for their own commands.

## Source layout

- `src/access/graph/` is the kernel, lifted unchanged from the OpenXcom port apart from its namespace. It is a self-contained component in upstream style and throws `std::logic_error` on builder misuse; `navigator.cpp` catches those at the single point where recipes are built. Its conformance suite is `test_access_graph`.
- `navigator.cpp` owns screen matching, key dispatch, the announce-once differ, and the activation handoff.
- `screens.cpp` lists the screen recipes in match order. Each recipe lives in the file for its screen or family of screens (`screens_galaxy.cpp`, `screens_colony.cpp`, `screens_build.cpp`, `screens_design.cpp`, `screens_popups.cpp`, `screens_research.cpp`, `screens_menus.cpp`, and `screens_generic.cpp` for the field list any screen falls back to). `screens_common.h` shares the control-type registry and the field and readout helpers between them.
- `help_text.cpp` reads titles and bodies from the current language's help LBX for the screen's active help list.
- `text.cpp` removes inline text commands and decodes CP850; `vocab.cpp` holds every string the layer authors.
- `speech.cpp` loads Tolk with `SDL_LoadObject`, chunks long text, and writes the transcript.

## Game integration

The layer touches the game in these places:

- `platform.cpp` offers each key-down to `access::navigator::Offer_Key_Down()` on the platform thread before translation. A claimed key is queued for the game thread and never reaches the legacy keyboard ring. The claim check reads only atomics, so it never waits for the game thread.
- `fields::Get_Input()` calls `access::navigator::Tick()` on every poll. A requested activation then runs through the same press path as a field hotkey, so radio and locked toggles, the pressed drawing, the auto function, and the field sound all behave as they would for a keyboard shortcut. A scroll field has no hotkey press, so it is clicked at the pointer instead, which sets its value from the pointer as a mouse click would. A continuous text field is clicked too, through the same `Begin_Text_Input_` step `Interpret_Mouse_Input` uses, because a hotkey press never starts typing into it. A requested right-click follows `Interpret_Mouse_Input`'s order instead: a help box under the pointer opens first, then an enabled cancel returns -1, and otherwise the game reads the negated field index.
- `TEXTBOX::Do_Text_Box()` publishes the title and text it is showing while it waits for input.
- `TECH::_Tech_Select()` and its callers publish the research areas, Accept button, and open browse list.
- `GENDRAW::Confirmation_Box()` publishes its question while it waits for input.
- `COLONY::Colony_Screen()` sets `COLONY::awaiting_input` while its own loop waits for input.
- `COLBLDG::Build_Queue_Popup()` sets `COLBLDG::awaiting_input` while its own loop waits for input.
- `TURNSUM::Turn_Summary_Popup()` publishes its OK and page buttons while it waits for input.
- `MAINPUPS::Random_New_Officer_Popup()` sets `MAINPUPS::officer_popup_awaiting_input` while it waits for input.
- `MAINPUPS::Officer_Gains_Level_Popup()` sets `MAINPUPS::level_popup_awaiting_input` while it waits for input.
- `MAINPUPS::New_Colony_Selection_Popup()` sets `MAINPUPS::selection_popup_awaiting_input` while it waits for input.
- `COLLAND::Colony_Landing_Screen()` sets `COLLAND::awaiting_input` while its own loop waits for input.
- `DESIGN::Design_Screen()` sets `DESIGN::awaiting_input` while its own loop waits for input.
- `DESBOX::Generic_Replacement_Box()`, `Weapons_Replacement_Box()`, and `Special_Systems_Box()` set `DESBOX::shown_box` to say which picker waits for input.
- `SCIENCE::Science_Room()` publishes its footer and the current discovery's text while it waits for input.
- `namestar::Input_Box_Popup()` publishes its prompt while it waits for input.
- `MOX2::thunk_main2()` initializes and shuts down the layer on the game thread.

Keys are claimed only while a cursor is seated, no continuous text field is active, and the game thread has polled fields within the last 300 milliseconds. Text-entry loops, animations, and loads that do not poll fields therefore receive their keys normally.

While a continuous text field is active, the navigator reads it on every tick instead. The game only appends at the end and deletes from the end, so comparing `fields::continuous_string` (less the trailing underscore its cursor blinks with) with the previous tick tells what each keystroke did: it speaks the character typed, adding "full" at the length limit, the character Backspace deleted, or the whole text when the first keystroke replaces a popup's default. Editing starts with the current text, and ends with whether the text changed, since Enter keeps the text and Escape restores it and both leave it in `continuous_string`. The end is checked before the screen is matched, so it is heard ahead of the screen editing returns to, and the start after, so a popup that opens already editing is named first. `namestar::Input_Box_Popup`, which names stars, publishes its prompt in `namestar::shown_prompt`; its recipe speaks the prompt as the screen's name and lists no controls.

## Recipes

A recipe is an `s_screen_recipe`: an `Is_Active` predicate re-derived from live state each tick, an instance key, a `Build` function, and a screen name. The first active recipe in `screens::Recipes()` wins, and a change of recipe or instance key announces the screen again.

The generic `fields` recipe lists every on-screen field in declaration order. It labels a field with the text the game draws on it, then the title of the help box containing its centre, then its hotkey. It omits keyboard-only hotkeys. A full-screen catch-all region comes last, named "Click anywhere", because some screens, such as the detailed ship view, close only on a click outside their controls.

Per-screen recipes should replace the generic one wherever a screen draws its values as text or pictures, because those values are not stored in the field list. The New Game recipe is an example: each setting has two fields that share one help box, and the selected value is drawn separately, so the recipe reads the value from the string the screen draws.

When the focused control changes, the pointer moves to the centre of its field so that the game's hover highlight follows the cursor.

A recipe may also declare a `Poll` hook, run every tick while it is attached, to notice changes inside the screen's own loop, such as the galaxy screen's system display opening. Rather than speaking, it calls `navigator::Request_Focus_Stop()`: the differ moves focus into that Tab-stop on its next pass and announces the landing, so every focus change is still spoken from one place.

The galaxy recipe lists every star by distance from the player's capital, using the same visibility rules as the drawn star names, and fleets only from the detection-filtered ship icons. The main loop hit-tests the pointer when its map grid is pressed, so a star node places the pointer on a point that `MAINSCR::Find_Star_At_XY` and `Find_Ship_Icon_At_XY` resolve to that star, and presses the grid. A star outside the current view is first brought into view with `MAINSCR::Center_Map`, as the game's mini-map does, so the player never manages zoom or scrolling.

The system display that opens over the map has no text in its fields: the game draws planet and fleet details from data when the pointer hovers over them. Its stop lists planets innermost first, then fleets in orbit, gates, and the wormhole line, each read with the lines of the matching hover box (`SYS::Do_Print_Planet_Info`, `Do_Print_System_Ship_Data`, `Print_Gate_Info`). The title bar, background, and orbit grid are left out. Pressing a planet or fleet presses its field, so the game opens the colony screen or fleet box as a click would. An asteroid belt has no field, so its node places the pointer on the belt's orbit and presses the orbit grid. Backspace on a planet right-clicks it, which opens the game's colony potential box.

The fleet box's stop reads the status line `FLEETPOP::Print_Fleet_Box_Info` draws, then each ship and whether it is selected, then its buttons labelled from the help list. The game selects a ship only while the left button is held over its icon: the main screen's draw calls `MAINSCR::Set_Painted_Fleet_Fields` for the icon under the pointer, so Enter on a ship makes that same call. A press would only show the ship's name. While ships are selected, a star also reads the ETA line the fleet box shows for the star under the pointer, and pressing a star orders the move.

`TEXTBOX::Do_Text_Box` leaves the screen's fields in place under its own full-screen field, so it publishes the title and text it shows in `TEXTBOX::shown_title` and `shown_text` while it waits for input. The text box recipe matches first while those are set, and lists the message one line at a time. Enter on any line dismisses the box, as any click would.

The research screen runs inside whatever loop asked for it, so `TECH::_Tech_Select` publishes its areas in `TECH::shown_entries` while it waits for input, its Accept button in `shown_accept_field`, and the area whose browse list `_Tech_List` has open over it in `shown_list_entry`. The recipe groups each area's technologies under a heading naming the area, the field, and its cost. The area names are drawn as pictures, so they are read from the same `billtext.lbx` messages `_Tech_List` titles its list with, through a reader that skips `JIM::Get_Text_Message`'s frame updates. The game picks a technology from the field under the pointer and the press commits the pick, so Enter also makes that pick with `TECH::Set_Selected_Entry`, in case the press lands before the next draw. Backspace shows a technology's description in the message box.

`GENDRAW::Confirmation_Box` adds its Yes and No fields above the screen's own, so it publishes its question in `GENDRAW::shown_confirmation` while it waits. The confirmation recipe speaks the question as the screen's name and lists the two buttons.

The colony screen's popups (transport, troops, occupation policy, auto build) run nested loops from inside the colony loop without changing `MOX::current_screen`, so the colony recipe matches only while `COLONY::awaiting_input` is set around the loop's own `Get_Input`. Each job row is a scroll field whose value is the x of the press, and `COLONY::Check_Job_Fields` picks up the colonist in that slot together with the identical colonists after it. The recipe lists each row's colonists in the order `COLDRAW::Do_Colony_Info_Pop_Stuff_For_Pop` lays them out, and Enter puts the pointer inside the colonist's slot, using the same squish arithmetic, before pressing the row. Enter on a row's heading picks up its last colonist. While colonists are held the rows become hidden drop fields, so Enter anywhere in a row places them there, and the poll speaks what is held because picking up can remove the focused colonist from its row.

The build screen (`COLBLDG::Build_Queue_Popup`) draws everything from data: its building list, ship and specialist list, and queue are hidden fields, and its detail panel shows whatever is under the pointer. The recipe matches only while `COLBLDG::awaiting_input` is set, so the refit, auto build, and message popups that open from `Evaluate_Input` get their own recipes. Each item is read with the cost, maintenance, and build time lines `Draw_Cost_And_Time_Info` prints for it, and the tooltip key reads the description below them: the HELP.LBX record, the setting text from MAINTEXT.LBX, or the ship design paragraph the game builds. A Repeat slot is read as part of the item before it. Enter presses the item's field, and the poll compares the queue with a snapshot taken at the press to say what was added, removed, or moved, which also covers changes made after a confirmation popup closes. It also speaks a queue item being picked up or put back and the choice the Design and Repeat Build buttons wait for. Backspace on a queue item removes it by making the game's two presses, pick up and press again, one per poll, after letting go of anything already held.

The ship design screen (`DESIGN::Design_Screen`) draws every stat as text, so the recipe reads them from the same functions `Print_Current_Design` uses. It matches only while `DESIGN::awaiting_input` is set; the shield, computer, weapon, and special system pickers run nested loops that publish themselves in `DESBOX::shown_box` instead. The zones are the ship (name, hull size, picture), its systems (drive, armor, shield, computer), weapons, special systems, totals, and controls. Left and Right on a weapon press its plus and minus buttons, and say why when `Add_Design_Buttons` left the button out. Every empty weapon or special system row opens the picker on the first empty slot, so they are listed as one row with the prompt the game shows when the pointer is on them. Backspace on a fitted item shows its description, as a right-click does. Each press takes a snapshot of the design, and the poll speaks what changed once the game has acted, including after a picker closes, or that nothing changed when the game refused a press silently.

The planet selection popup (`MAINPUPS::New_Colony_Selection_Popup`), where a colony ship, colony base, outpost, troops, or artificial planet is placed, is a system display with the game's prompt as its title. Its stop is the galaxy system display's, read the same way, with the popup's own cancel button in place of the display's close button. Pressing a planet makes the choice, and a refused choice opens the game's message box. A system special notice drawn over the display is read on its own, with a Continue button that presses the popup's full-screen field to dismiss it. The officer level popup (`MAINPUPS::Officer_Gains_Level_Popup`), shown when a leader gains a level or a marooned hero is found, is read as its message, the leader's skills, and a Continue button.

The colony landing screen (`COLLAND::Colony_Landing_Screen`) plays the pods landing and waits for a click. The game prints the planet's details only once the animation is two frames from its end, so the recipe lists them, and its poll speaks them, only from then on. A Continue button presses the screen's full-screen field.

The turn summary sets `MOX::current_screen` to the main screen while it runs, so `TURNSUM::Turn_Summary_Popup` publishes its OK button in `TURNSUM::shown_ok_field`, and its page buttons beside it, while it waits for input. The recipe lists every message, not just the page on screen, from `MSG_::sprintf_msg`, which the popup's own draw calls every frame. A press on a message acts on the entry the draw last selected from the pointer, so Enter makes that selection first. Enter on a message on another page presses the page buttons until it is shown, then presses it.

The leader-for-hire popup labels its buttons only with one-letter hotkeys and draws everything else, so the recipe rebuilds the offer paragraph and the skill lines from the same strings `MAINPUPS::Draw_Random_New_Officer_Popup` and `OFFICER::Print_Officer_Data` use. Backspace on a skill right-clicks its help field, which shows the skill's description.

`SCIENCE::Science_Room` presents new technology one discovery at a time, drawing the text into a bitmap, so it publishes its footer line in `SCIENCE::shown_footer` while it waits for input, and the current discovery's title, weapon note, and description in `shown_title`, `shown_note`, and `shown_body` once it has drawn them. The first discovery is drawn after the first poll, so the recipe shows the footer until then, and its poll moves focus to each new title. Enter on any line advances, as a click does; Escape closes the room.
