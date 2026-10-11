#include "pch.h"

#include "access/screens.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/speech.h"
#include "access/text.h"
#include "access/vocab.h"

#include <algorithm>
#include <string>
#include <vector>

namespace access::screens {

    namespace {

        using graph::AnnouncementKinds::Label;
        using graph::AnnouncementKinds::Role;
        using graph::AnnouncementKinds::Selected;
        using graph::AnnouncementKinds::Value;
        using graph::AnnouncementKinds::Enabled;

        constexpr int16_t SCREEN_WIDTH = 640;
        constexpr int16_t SCREEN_HEIGHT = 480;
        constexpr int16_t MAIN_MENU_BUTTON_X = 0x19F;
        constexpr int16_t MAIN_MENU_ABSENT_FIELD = -1000;

        // The one control-type registry for this host (spec section 3.3).
        graph::ControlType Make_Type_(const char* key, vocab::e_vocab_string role) {
            graph::ControlType type;
            type.Key = key;
            type.Order = { Label, Role, Value, Selected, Enabled };
            type.Common = [role]() {
                return std::vector<graph::NodeAnnouncement>{
                    graph::NodeAnnouncement([role]() { return std::string(vocab::Get(role)); }, false, Role)
                };
            };
            return type;
        }

        const graph::ControlType button_type = Make_Type_("button", vocab::VOCAB_ROLE_BUTTON);
        const graph::ControlType toggle_type = Make_Type_("toggle", vocab::VOCAB_ROLE_TOGGLE);
        const graph::ControlType choice_type = Make_Type_("choice", vocab::VOCAB_ROLE_CHOICE);
        const graph::ControlType slider_type = Make_Type_("slider", vocab::VOCAB_ROLE_SLIDER);
        const graph::ControlType list_item_type = Make_Type_("list_item", vocab::VOCAB_ROLE_LIST_ITEM);
        const graph::ControlType text_field_type = Make_Type_("text_field", vocab::VOCAB_ROLE_TEXT_FIELD);
        const graph::ControlType map_area_type = Make_Type_("map_area", vocab::VOCAB_ROLE_MAP_AREA);

        bool Is_Valid_Field_(int16_t field_idx) {
            return field_idx > 0 && field_idx < fields::fields_count;
        }

        bool Is_Usable_String_(const char* text) {
            return text != nullptr && text != fields::invalid_field_string_ptr && text[0] != '\0';
        }

        // The text the game itself draws on the field, when it draws any.
        std::string Field_Own_Text_(const s_field* field) {
            const char* text = nullptr;
            switch (field->field_type) {
                case FIELD_TYPE_BUTTON: text = fields::Field_Data_Button(field)->help; break;
                case FIELD_TYPE_IRREGULAR_BUTTON: text = fields::Field_Data_Irregular_Button(field)->help; break;
                case FIELD_TYPE_RADIO: text = fields::Field_Data_Radio(field)->help; break;
                case FIELD_TYPE_LOCKED: text = fields::Field_Data_Locked_Button(field)->help; break;
                case FIELD_TYPE_MULTI_BUTTON: text = fields::Field_Data_Multi_Button(field)->help; break;
                case FIELD_TYPE_STRING_LIST: text = fields::Field_Data_String_List(field)->help; break;
                default: break;
            }
            return Is_Usable_String_(text) ? text::Normalize_Game_Text(text) : std::string();
        }

        int16_t Field_Help_Id_(const s_field* field) {
            return help::Find_Help_Id(field->x, field->y, field->x_end, field->y_end);
        }

        std::string Help_Title_(int16_t help_id) {
            help::s_help_text help_text;
            return help::Read_Help_Text(help_id, &help_text) ? help_text.title : std::string();
        }

        std::string Hotkey_Text_(const s_field* field) {
            unsigned char key = (unsigned char)field->hotkey;
            if (key <= ' ' || key >= 0x7F) {
                return std::string();
            }
            if (key >= 'a' && key <= 'z') {
                key = (unsigned char)(key - 'a' + 'A');
            }
            return vocab::Format(vocab::VOCAB_HOTKEY, std::string(1, (char)key));
        }

        // Label precedence: what the game draws on the field, then its help title, then its hotkey.
        std::string Field_Label_(int16_t field_idx) {
            const s_field* field = &fields::fields[field_idx];
            std::string label = Field_Own_Text_(field);
            if (label.empty()) {
                label = Help_Title_(Field_Help_Id_(field));
            }
            if (label.empty()) {
                label = Hotkey_Text_(field);
            }
            if (label.empty()) {
                label = vocab::Format(vocab::VOCAB_UNLABELED, std::to_string(field_idx));
            }
            return label;
        }

        const graph::ControlType* Field_Type_(const s_field* field) {
            switch (field->field_type) {
                case FIELD_TYPE_RADIO:
                case FIELD_TYPE_LOCKED: return &toggle_type;
                case FIELD_TYPE_MULTI_BUTTON: return &choice_type;
                case FIELD_TYPE_SCROLL:
                case FIELD_TYPE_SCROLL_RETURN: return &slider_type;
                case FIELD_TYPE_STRING_LIST: return &list_item_type;
                case FIELD_TYPE_CONTINUOUS_INPUT:
                case FIELD_TYPE_LEGACY_INPUT: return &text_field_type;
                case FIELD_TYPE_GRID:
                case FIELD_TYPE_CIRCLE: return &map_area_type;
                default: return &button_type;
            }
        }

        // The toggle or slider value line; empty for fields without one.
        std::string Field_Value_(int16_t field_idx) {
            if (!Is_Valid_Field_(field_idx)) {
                return std::string();
            }
            const s_field* field = &fields::fields[field_idx];
            switch (field->field_type) {
                case FIELD_TYPE_RADIO:
                    return vocab::Get(*fields::Field_Data_Radio(field)->selection != 0 ? vocab::VOCAB_STATE_ON : vocab::VOCAB_STATE_OFF);
                case FIELD_TYPE_LOCKED:
                    return vocab::Get(*fields::Field_Data_Locked_Button(field)->selection != 0 ? vocab::VOCAB_STATE_ON : vocab::VOCAB_STATE_OFF);
                case FIELD_TYPE_SCROLL:
                case FIELD_TYPE_SCROLL_RETURN:
                {
                    const s_field_scroll* scroll = fields::Field_Data_Scroll(field);
                    return scroll->value != nullptr
                        ? vocab::Format(vocab::VOCAB_VALUE, std::to_string(*scroll->value))
                        : std::string();
                }
                default:
                    return std::string();
            }
        }

        // The current member of a choice group or list; drives where focus lands (spec section 3.2).
        bool Field_Is_Selected_(const s_field* field) {
            if (field->field_type == FIELD_TYPE_MULTI_BUTTON) {
                const s_field_multi_button* choice = fields::Field_Data_Multi_Button(field);
                return choice->selection != nullptr && *choice->selection == choice->button_value;
            }
            if (field->field_type == FIELD_TYPE_STRING_LIST) {
                const s_field_string_list* row = fields::Field_Data_String_List(field);
                return row->is_selectable != 0 && row->value != nullptr && *row->value == row->help_start;
            }
            return false;
        }

        void Speak_Field_Help_(int16_t field_idx) {
            help::s_help_text help_text;
            bool found = Is_Valid_Field_(field_idx) &&
                help::Read_Help_Text(Field_Help_Id_(&fields::fields[field_idx]), &help_text) &&
                !help_text.body.empty();
            speech::Say(found ? help_text.title + ". " + help_text.body : std::string(vocab::Get(vocab::VOCAB_NO_TOOLTIP)), true);
        }

        // A node over one live game field: activation goes through the game's own hotkey path.
        graph::NodeVtable Field_Vtable_(int16_t field_idx, const std::string& label) {
            const s_field* field = &fields::fields[field_idx];
            graph::NodeVtable vtable;
            vtable.Type = Field_Type_(field);
            vtable.Announcements.push_back(graph::NodeAnnouncement::Static(label));
            vtable.Announcements.back().Kind = Label;
            vtable.Announcements.push_back(graph::NodeAnnouncement([field_idx]() { return Field_Value_(field_idx); }, true, Value));
            if (Field_Is_Selected_(field)) {
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(vocab::VOCAB_STATE_SELECTED)));
                vtable.Announcements.back().Kind = Selected;
            }
            if (field->field_type != FIELD_TYPE_SCROLL) {
                vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
            }
            vtable.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };
            return vtable;
        }

        void Add_Field_Item_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            int16_t field_idx, const std::string& label) {
            context->node_fields[key] = field_idx;
            builder->AddItem(graph::ControlId::Structural(key), Field_Vtable_(field_idx, label));
        }

        std::string Screen_Name_(int16_t screen) {
            switch (screen) {
                case SCREEN_MAIN: return vocab::Get(vocab::VOCAB_SCREEN_MAIN);
                case SCREEN_COLONY: return vocab::Get(vocab::VOCAB_SCREEN_COLONY);
                case SCREEN_DESIGN: return vocab::Get(vocab::VOCAB_SCREEN_DESIGN);
                case SCREEN_FLEET: return vocab::Get(vocab::VOCAB_SCREEN_FLEET);
                case SCREEN_RACE: return vocab::Get(vocab::VOCAB_SCREEN_RACE);
                case SCREEN_INFO: return vocab::Get(vocab::VOCAB_SCREEN_INFO);
                case SCREEN_MAIN_MENU: return vocab::Get(vocab::VOCAB_SCREEN_MAIN_MENU);
                case SCREEN_NEW_GAME: return vocab::Get(vocab::VOCAB_SCREEN_NEW_GAME);
                case SCREEN_HALL_OF_FAME: return vocab::Get(vocab::VOCAB_SCREEN_HALL_OF_FAME);
                case SCREEN_MULTI_PLAYER: return vocab::Get(vocab::VOCAB_SCREEN_MULTI_PLAYER);
                case SCREEN_PLANET_DATA: return vocab::Get(vocab::VOCAB_SCREEN_PLANET_DATA);
                case SCREEN_COLONY_SUMMARY: return vocab::Get(vocab::VOCAB_SCREEN_COLONY_SUMMARY);
                case SCREEN_OFFICERS: return vocab::Get(vocab::VOCAB_SCREEN_OFFICERS);
                case SCREEN_PLANET_SUMMARY: return vocab::Get(vocab::VOCAB_SCREEN_PLANET_SUMMARY);
                case SCREEN_TECH_CHANGE: return vocab::Get(vocab::VOCAB_SCREEN_TECH_CHANGE);
                case SCREEN_REPORTS: return vocab::Get(vocab::VOCAB_SCREEN_REPORTS);
                case SCREEN_TURN_SUMMARY: return vocab::Get(vocab::VOCAB_SCREEN_TURN_SUMMARY);
                case SCREEN_CREDITS: return vocab::Get(vocab::VOCAB_SCREEN_CREDITS);
                default: return vocab::Format(vocab::VOCAB_SCREEN_NUMBER, std::to_string(screen));
            }
        }

        // ---- Main menu -------------------------------------------------------------------------

        struct s_main_menu_entry {
            int32_t button;
            const char* key;
            vocab::e_vocab_string fallback_label;
        };

        const s_main_menu_entry main_menu_buttons[] = {
            { MAIN_MENU_BTN_CONTINUE, "main_menu/continue", vocab::VOCAB_MAIN_MENU_CONTINUE },
            { MAIN_MENU_BTN_LOAD, "main_menu/load", vocab::VOCAB_MAIN_MENU_LOAD },
            { MAIN_MENU_BTN_NEW, "main_menu/new", vocab::VOCAB_MAIN_MENU_NEW },
            { MAIN_MENU_BTN_MULTIPLAY, "main_menu/multiplayer", vocab::VOCAB_MAIN_MENU_MULTIPLAYER },
            { MAIN_MENU_BTN_HELP, "main_menu/hall_of_fame", vocab::VOCAB_MAIN_MENU_HALL_OF_FAME },
            { MAIN_MENU_BTN_QUIT, "main_menu/quit", vocab::VOCAB_MAIN_MENU_QUIT },
        };

        // The main menu's own loop owns the field list only while its New Game field is where
        // Add_Main_Menu_Fields put it; the load popup and file animation rebuild the list.
        bool Main_Menu_Is_Active_() {
            if (MOX::current_screen != SCREEN_MAIN_MENU || MAINMENU::mainmenu == nullptr) {
                return false;
            }
            int16_t new_field = MAINMENU::mainmenu->fields[MAIN_MENU_BTN_NEW].field;
            return Is_Valid_Field_(new_field) && fields::fields[new_field].x == MAIN_MENU_BUTTON_X &&
                fields::fields[new_field].hotkey == 'n';
        }

        std::string Main_Menu_Instance_Key_() {
            return "main_menu";
        }

        void Main_Menu_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int32_t i = 0; i < (int32_t)(sizeof(main_menu_buttons) / sizeof(main_menu_buttons[0])); ++i) {
                const s_main_menu_entry* button = &main_menu_buttons[i];
                int16_t field_idx = MAINMENU::mainmenu->fields[button->button].field;
                int16_t help_id = EVANHELP::main_menu_screen_help_list[button->button].help_id;

                std::string label = Help_Title_(help_id);
                if (label.empty()) {
                    label = vocab::Get(button->fallback_label);
                }

                if (Is_Valid_Field_(field_idx)) {
                    Add_Field_Item_(builder, context, button->key, field_idx, label);
                    continue;
                }

                // Continue and Load stay visible but greyed out when there is nothing to load.
                if (field_idx == MAIN_MENU_ABSENT_FIELD) {
                    graph::NodeVtable vtable;
                    vtable.Type = &button_type;
                    vtable.Announcements.push_back(graph::NodeAnnouncement::Static(label));
                    vtable.Announcements.back().Kind = Label;
                    vtable.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(vocab::VOCAB_STATE_UNAVAILABLE)));
                    vtable.Announcements.back().Kind = Enabled;
                    vtable.OnTooltip = [help_id]() {
                        help::s_help_text help_text;
                        bool found = help::Read_Help_Text(help_id, &help_text) && !help_text.body.empty();
                        speech::Say(found ? help_text.body : std::string(vocab::Get(vocab::VOCAB_NO_TOOLTIP)), true);
                    };
                    builder->AddItem(graph::ControlId::Structural(button->key), vtable);
                }
            }

            if (Is_Valid_Field_(MAINMENU::credits_field)) {
                Add_Field_Item_(builder, context, "main_menu/credits", MAINMENU::credits_field,
                    vocab::Get(vocab::VOCAB_MAIN_MENU_CREDITS));
            }

            int16_t continue_field = MAINMENU::mainmenu->fields[MAIN_MENU_BTN_CONTINUE].field;
            builder->SetStart(graph::ControlId::Structural(Is_Valid_Field_(continue_field) ? "main_menu/continue" : "main_menu/new"));
        }

        std::string Main_Menu_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_MAIN_MENU);
        }

        // ---- New Game --------------------------------------------------------------------------

        // Each setting is a hidden field over its picture plus a click-thru field over its label
        // strip; both cycle the same value forward, and the game draws the value's string itself.
        struct s_new_game_setting {
            int32_t field_slot;
            const char* key;
            char** strings;
            int16_t* value;
            int16_t count;
        };

        const s_new_game_setting new_game_settings[] = {
            { 0, "new_game/difficulty", NEWGAME::diff_button_strings, &NEWGAME::diff_button_variable,
                (int16_t)(sizeof(NEWGAME::diff_button_strings) / sizeof(NEWGAME::diff_button_strings[0])) },
            { 1, "new_game/galaxy_size", NEWGAME::gal_size_button_strings, &NEWGAME::gal_size_button_variable,
                (int16_t)(sizeof(NEWGAME::gal_size_button_strings) / sizeof(NEWGAME::gal_size_button_strings[0])) },
            { 2, "new_game/galaxy_age", NEWGAME::gal_age_button_strings, &NEWGAME::gal_age_button_variable,
                (int16_t)(sizeof(NEWGAME::gal_age_button_strings) / sizeof(NEWGAME::gal_age_button_strings[0])) },
            { 3, "new_game/players", NEWGAME::opp_button_strings, &NEWGAME::opp_button_variable,
                (int16_t)(sizeof(NEWGAME::opp_button_strings) / sizeof(NEWGAME::opp_button_strings[0])) },
            { 5, "new_game/tech_level", NEWGAME::civ_button_strings, &NEWGAME::civ_button_variable,
                (int16_t)(sizeof(NEWGAME::civ_button_strings) / sizeof(NEWGAME::civ_button_strings[0])) },
        };

        struct s_new_game_toggle {
            int16_t* selection;
            const char* key;
        };

        // In on-screen order, top to bottom.
        const s_new_game_toggle new_game_toggles[] = {
            { (int16_t*)&NEWGAME::combat_button_variable, "new_game/tactical_combat" },
            { &NEWGAME::random_events_button_variable, "new_game/random_events" },
            { &NEWGAME::xenon_attacks_button_variable, "new_game/antarans" },
        };

        constexpr int32_t NEW_GAME_CANCEL_SLOT = 6;
        constexpr int32_t NEW_GAME_ACCEPT_SLOT = 8;
        constexpr int16_t NEW_GAME_DIFFICULTY_X = 0x69;

        // The New Game loop owns the field list only while its difficulty strip is where
        // Update_Fields put it; race selection runs nested inside this loop with fields of its own.
        bool New_Game_Is_Active_() {
            if (MOX::current_screen != SCREEN_NEW_GAME) {
                return false;
            }
            int16_t difficulty_field = NEWGAME::fields[0];
            return Is_Valid_Field_(difficulty_field) &&
                fields::fields[difficulty_field].field_type == FIELD_TYPE_HIDDEN &&
                fields::fields[difficulty_field].x == NEWGAME::g_x + NEW_GAME_DIFFICULTY_X;
        }

        std::string New_Game_Instance_Key_() {
            return "new_game";
        }

        std::string New_Game_Setting_Value_(const s_new_game_setting* setting) {
            int16_t value = *setting->value;
            if (value < 0 || value >= setting->count || !Is_Usable_String_(setting->strings[value])) {
                return std::string();
            }
            return text::Normalize_Game_Text(setting->strings[value]);
        }

        // The tech level box's help title names one level ("Tech Level : Prewarp") whatever is
        // selected, so only the part before the colon labels the setting.
        std::string New_Game_Setting_Label_(int16_t field_idx) {
            std::string label = Field_Label_(field_idx);
            size_t colon = label.find(':');
            if (colon != std::string::npos && colon > 0) {
                label = text::Normalize_Spoken_Text(label.substr(0, colon));
            }
            return label;
        }

        int16_t Find_Radio_Field_(const int16_t* selection) {
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (field->field_type == FIELD_TYPE_RADIO && fields::Field_Data_Radio(field)->selection == selection) {
                    return i;
                }
            }
            return 0;
        }

        // Help titles come first here: Update_Fields gives every button and toggle the same
        // E_Strings(0x0C) text, which cannot tell them apart.
        std::string New_Game_Label_(int16_t field_idx) {
            std::string label = Help_Title_(Field_Help_Id_(&fields::fields[field_idx]));
            return label.empty() ? Field_Label_(field_idx) : label;
        }

        void New_Game_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int32_t i = 0; i < (int32_t)(sizeof(new_game_settings) / sizeof(new_game_settings[0])); ++i) {
                const s_new_game_setting* setting = &new_game_settings[i];
                int16_t field_idx = NEWGAME::fields[setting->field_slot];
                if (!Is_Valid_Field_(field_idx)) {
                    continue;
                }

                graph::NodeVtable vtable;
                vtable.Type = &choice_type;
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(New_Game_Setting_Label_(field_idx)));
                vtable.Announcements.back().Kind = Label;
                vtable.Announcements.push_back(graph::NodeAnnouncement([setting]() { return New_Game_Setting_Value_(setting); }, true, Value));
                vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
                vtable.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };

                context->node_fields[setting->key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(setting->key), vtable);
            }

            for (int32_t i = 0; i < (int32_t)(sizeof(new_game_toggles) / sizeof(new_game_toggles[0])); ++i) {
                int16_t field_idx = Find_Radio_Field_(new_game_toggles[i].selection);
                if (Is_Valid_Field_(field_idx)) {
                    Add_Field_Item_(builder, context, new_game_toggles[i].key, field_idx, New_Game_Label_(field_idx));
                }
            }

            int16_t cancel_field = NEWGAME::fields[NEW_GAME_CANCEL_SLOT];
            if (Is_Valid_Field_(cancel_field)) {
                Add_Field_Item_(builder, context, "new_game/cancel", cancel_field, New_Game_Label_(cancel_field));
            }
            int16_t accept_field = NEWGAME::fields[NEW_GAME_ACCEPT_SLOT];
            if (Is_Valid_Field_(accept_field)) {
                Add_Field_Item_(builder, context, "new_game/accept", accept_field, New_Game_Label_(accept_field));
            }

            builder->SetStart(graph::ControlId::Structural("new_game/difficulty"));
        }

        std::string New_Game_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_NEW_GAME);
        }

        // ---- Any screen: the live field list -----------------------------------------------------

        bool Is_Offscreen_(const s_field* field) {
            return field->x >= SCREEN_WIDTH || field->y >= SCREEN_HEIGHT || field->x_end < field->x ||
                field->y_end < field->y;
        }

        bool Is_Catch_All_(const s_field* field) {
            return field->field_type == FIELD_TYPE_HIDDEN && field->x <= 0 && field->y <= 0 &&
                field->x_end >= SCREEN_WIDTH - 1 && field->y_end >= SCREEN_HEIGHT - 1;
        }

        // Keyboard-only hotkeys have no visual counterpart, and a full-screen "click anywhere"
        // region is listed only when it is the screen's sole control.
        bool Field_Is_Listed_(int16_t field_idx, bool has_specific_field) {
            const s_field* field = &fields::fields[field_idx];
            if (field->field_type == FIELD_TYPE_MULTI_HOTKEY || Is_Offscreen_(field)) {
                return false;
            }
            return !(Is_Catch_All_(field) && has_specific_field);
        }

        bool Fields_Is_Active_() {
            return fields::fields != nullptr && fields::fields_count >= 2;
        }

        std::string Fields_Instance_Key_() {
            const s_field* first = &fields::fields[1];
            char key[96];
            snprintf(key, sizeof(key), "fields/%d/%p/%d,%d,%d", (int)MOX::current_screen,
                (const void*)fields::help_struct_pointer, (int)first->x, (int)first->y, (int)first->field_type);
            return key;
        }

        void Fields_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            bool has_specific_field = false;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (field->field_type != FIELD_TYPE_MULTI_HOTKEY && !Is_Offscreen_(field) && !Is_Catch_All_(field)) {
                    has_specific_field = true;
                    break;
                }
            }

            for (int16_t i = 1; i < fields::fields_count; ++i) {
                if (!Field_Is_Listed_(i, has_specific_field)) {
                    continue;
                }
                const s_field* field = &fields::fields[i];
                char key[48];
                snprintf(key, sizeof(key), "field/%d/%d,%d", (int)i, (int)field->x, (int)field->y);
                Add_Field_Item_(builder, context, key, i, Field_Label_(i));
            }
        }

        std::string Fields_Name_() {
            return Screen_Name_(MOX::current_screen);
        }

        // ---- Galaxy map --------------------------------------------------------------------------

        constexpr int16_t GALAXY_GRID_X = 22;
        constexpr int16_t GALAXY_GRID_Y = 22;

        // The main loop hit-tests stars and fleets at the pointer when its map grid is pressed, so
        // a star node puts the pointer on the star and presses the grid.
        bool Galaxy_Is_Active_() {
            if (MOX::current_screen != SCREEN_MAIN || MOX::zoom_in == 1) {
                return false;
            }
            int16_t grid = MOX::main_screen_grid_field;
            return Is_Valid_Field_(grid) && fields::fields[grid].field_type == FIELD_TYPE_GRID &&
                fields::fields[grid].x == GALAXY_GRID_X && fields::fields[grid].y == GALAXY_GRID_Y &&
                Is_Valid_Field_(MAINSCR::next_button);
        }

        std::string Galaxy_Instance_Key_() {
            return "galaxy";
        }

        std::string Galaxy_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_MAIN);
        }

        int16_t Capital_Star_() {
            int16_t planet_idx = MOX::player[MOX::PLAYER_NUM].home_planet_id;
            if (planet_idx < 0 || planet_idx >= MOX::planet_count) {
                return -1;
            }
            int16_t star_idx = MOX::planet[planet_idx].star_index;
            return star_idx >= 0 && star_idx < MOX::NUM_STARS ? star_idx : -1;
        }

        // Mirrors the visibility rules of MAINSCR::Get_Star_Name, which cannot be called here
        // because it changes the font state as it decides.
        bool Star_Name_Is_Shown_(int16_t star_idx) {
            const s_star_data* star = &MOX::star[star_idx];
            if (star->spectral_class == STAR_CLASS_BLACK_HOLE) {
                return false;
            }
            if (HAROLD::Player_Is_Omniscient(MOX::PLAYER_NUM)) {
                return true;
            }
            bool has_contact = star->owner >= 0 && star->owner < MAX_PLAYERS && HAROLD::Contact_With_One_Colony(star_idx);
            return has_contact || HAROLD::Player_Has_Visited(MOX::PLAYER_NUM, star_idx) != 0;
        }

        // The name is drawn in the owner's colour only with contact (or galactic lore).
        int16_t Star_Shown_Owner_(int16_t star_idx) {
            const s_star_data* star = &MOX::star[star_idx];
            if (star->owner < 0 || star->owner >= MAX_PLAYERS || !Star_Name_Is_Shown_(star_idx)) {
                return -1;
            }
            if (star->owner == MOX::PLAYER_NUM || HAROLD::Player_Is_Omniscient(MOX::PLAYER_NUM) ||
                HAROLD::Contact_With_One_Colony(star_idx)) {
                return star->owner;
            }
            return -1;
        }

        std::string Star_Label_(int16_t star_idx) {
            const s_star_data* star = &MOX::star[star_idx];
            if (star->spectral_class == STAR_CLASS_BLACK_HOLE) {
                return vocab::Get(vocab::VOCAB_STAR_BLACK_HOLE);
            }
            if (Star_Name_Is_Shown_(star_idx)) {
                char name[sizeof(star->name) + 1] = {};
                memcpy(name, star->name, sizeof(star->name));
                return text::Normalize_Game_Text(name);
            }
            static const vocab::e_vocab_string colours[] = {
                vocab::VOCAB_STAR_BLUE, vocab::VOCAB_STAR_WHITE, vocab::VOCAB_STAR_YELLOW,
                vocab::VOCAB_STAR_ORANGE, vocab::VOCAB_STAR_RED, vocab::VOCAB_STAR_BROWN,
            };
            int8_t spectral = star->spectral_class;
            std::string colour = spectral >= 0 && spectral < (int8_t)(sizeof(colours) / sizeof(colours[0]))
                ? vocab::Get(colours[spectral])
                : std::string();
            return text::Normalize_Spoken_Text(vocab::Format(vocab::VOCAB_STAR_UNEXPLORED, colour));
        }

        // Eight-way compass direction; map y grows southward. tan(22.5 degrees) is about 0.414.
        vocab::e_vocab_string Direction_(int32_t dx, int32_t dy) {
            int32_t ax = dx < 0 ? -dx : dx;
            int32_t ay = dy < 0 ? -dy : dy;
            if (ay * 1000 <= ax * 414) {
                return dx > 0 ? vocab::VOCAB_DIRECTION_EAST : vocab::VOCAB_DIRECTION_WEST;
            }
            if (ax * 1000 <= ay * 414) {
                return dy > 0 ? vocab::VOCAB_DIRECTION_SOUTH : vocab::VOCAB_DIRECTION_NORTH;
            }
            if (dy < 0) {
                return dx > 0 ? vocab::VOCAB_DIRECTION_NORTH_EAST : vocab::VOCAB_DIRECTION_NORTH_WEST;
            }
            return dx > 0 ? vocab::VOCAB_DIRECTION_SOUTH_EAST : vocab::VOCAB_DIRECTION_SOUTH_WEST;
        }

        std::string Star_Distance_(int16_t star_idx, int16_t capital) {
            if (capital < 0) {
                return std::string();
            }
            if (star_idx == capital) {
                return vocab::Get(vocab::VOCAB_STAR_CAPITAL);
            }
            int32_t parsecs = GEO::Parsecs_Between_Stars(capital, star_idx);
            std::string direction = vocab::Get(Direction_(MOX::star[star_idx].x - MOX::star[capital].x,
                MOX::star[star_idx].y - MOX::star[capital].y));
            return parsecs == 1
                ? vocab::Format(vocab::VOCAB_DISTANCE_ONE, direction)
                : vocab::Format(vocab::VOCAB_DISTANCE, std::to_string(parsecs), direction);
        }

        // Ship icons are built from stacks that Remove_Non_Detected_Ships already filtered, so
        // they are exactly the fleets the player can see.
        std::string Star_Fleets_(int16_t star_idx) {
            int32_t count = 0;
            for (int16_t i = 0; i < MOX::ship_icon_count; ++i) {
                if (MOX::ship_icon[i].star_idx == star_idx) {
                    ++count;
                }
            }
            if (count == 0) {
                return std::string();
            }
            return count == 1 ? std::string(vocab::Get(vocab::VOCAB_FLEET_COUNT_ONE))
                              : vocab::Format(vocab::VOCAB_FLEET_COUNT, std::to_string(count));
        }

        // The main loop hit-tests the pointer when its grid is pressed: fleet icons win over stars
        // unless the fleet box is open, in which case stars win (mainscr_main.cpp).
        bool Point_Picks_Star_(int16_t x, int16_t y, int16_t star_idx) {
            const s_field* grid = &fields::fields[MOX::main_screen_grid_field];
            if (x < grid->x || x > grid->x_end || y < grid->y || y > grid->y_end) {
                return false;
            }
            int16_t star_hit = MAINSCR::Find_Star_At_XY(x, y);
            if (MOVEBOX::Moveable_Box_Selected(2)) {
                return star_hit == star_idx;
            }
            return star_hit == star_idx && MAINSCR::Find_Ship_Icon_At_XY(x, y) == -1;
        }

        constexpr int16_t STAR_CLICK_SEARCH_RADIUS = 12;

        // The star's centre when it resolves to the star, else the nearest point that does; a
        // fleet icon parked beside the star or a neighbouring star can cover the centre.
        bool Star_Click_Point_(int16_t star_idx, int16_t* out_x, int16_t* out_y) {
            if (!MAINSCR::Star_On_Screen(star_idx)) {
                return false;
            }
            int16_t cx = 0;
            int16_t cy = 0;
            MAINSCR::Get_Star_Draw_Coords(star_idx, &cx, &cy);
            for (int16_t ring = 0; ring <= STAR_CLICK_SEARCH_RADIUS; ++ring) {
                for (int16_t dy = (int16_t)-ring; dy <= ring; ++dy) {
                    for (int16_t dx = (int16_t)-ring; dx <= ring; ++dx) {
                        if (dx != -ring && dx != ring && dy != -ring && dy != ring) {
                            continue;
                        }
                        if (Point_Picks_Star_((int16_t)(cx + dx), (int16_t)(cy + dy), star_idx)) {
                            *out_x = (int16_t)(cx + dx);
                            *out_y = (int16_t)(cy + dy);
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        // Fleet icon hit rectangles are cached by the last draw, so after the view moves the
        // press waits until a draw has refreshed them.
        bool Ship_Icons_Are_Drawn_() {
            for (int16_t i = 0; i < MOX::ship_icon_count; ++i) {
                int16_t x = 0;
                int16_t y = 0;
                SHIPS::Get_Ship_Icon_Coords(i, &x, &y);
                if (MOX::ship_icon[i].x != x || MOX::ship_icon[i].y != y) {
                    return false;
                }
            }
            return true;
        }

        bool Press_Star_(int16_t star_idx) {
            int16_t x = 0;
            int16_t y = 0;
            if (!Star_Click_Point_(star_idx, &x, &y)) {
                return false;
            }
            platform::Force_Cursor_Pos(x, y);
            navigator::Request_Field_Activation(MOX::main_screen_grid_field);
            return true;
        }

        constexpr int32_t PENDING_STAR_MAX_TICKS = 30;

        // A star the view was moved to, waiting for the redraw before it is pressed.
        int16_t pending_star = -1;
        int32_t pending_star_ticks = 0;

        void Say_Star_Unreachable_(int16_t star_idx) {
            speech::Say(vocab::Format(vocab::VOCAB_STAR_UNREACHABLE, Star_Label_(star_idx)), true);
        }

        // The player never has to manage the view: an off-screen star is brought into view the
        // way the game's own mini-map does it, then pressed once the map has been redrawn.
        void Activate_Star_(int16_t star_idx) {
            pending_star = -1;
            if (Ship_Icons_Are_Drawn_() && Press_Star_(star_idx)) {
                return;
            }
            if (!MAINSCR::Star_On_Screen(star_idx)) {
                MAINSCR::Center_Map(MOX::star[star_idx].x, MOX::star[star_idx].y);
            }
            pending_star = star_idx;
            pending_star_ticks = 0;
        }

        void Poll_Pending_Star_() {
            if (pending_star < 0) {
                return;
            }
            ++pending_star_ticks;
            bool timed_out = pending_star_ticks >= PENDING_STAR_MAX_TICKS;
            if (pending_star_ticks < 2 || (!Ship_Icons_Are_Drawn_() && !timed_out)) {
                return;
            }
            int16_t star_idx = pending_star;
            pending_star = -1;
            if (!Press_Star_(star_idx)) {
                Say_Star_Unreachable_(star_idx);
            }
        }

        void Add_Star_Item_(graph::GraphBuilder* builder, s_build_context* context, int16_t star_idx, int16_t capital) {
            char key[24];
            snprintf(key, sizeof(key), "star/%d", (int)star_idx);

            graph::NodeVtable vtable;
            vtable.Announcements.push_back(graph::NodeAnnouncement::Static(Star_Label_(star_idx)));
            vtable.Announcements.back().Kind = Label;

            int16_t owner = Star_Shown_Owner_(star_idx);
            if (owner >= 0) {
                char race[sizeof(MOX::player[owner].race_name) + 1] = {};
                memcpy(race, MOX::player[owner].race_name, sizeof(MOX::player[owner].race_name));
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(text::Normalize_Game_Text(race)));
            }
            std::string distance = Star_Distance_(star_idx, capital);
            if (!distance.empty()) {
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(distance));
            }
            vtable.Announcements.push_back(graph::NodeAnnouncement([star_idx]() { return Star_Fleets_(star_idx); }, true, Value));

            int16_t x = 0;
            int16_t y = 0;
            if (Star_Click_Point_(star_idx, &x, &y)) {
                context->node_points[key] = s_pointer_target{ x, y };
            }
            vtable.OnActivate = [star_idx]() { Activate_Star_(star_idx); };

            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        bool Is_Galaxy_Panel_Field_(int16_t field_idx) {
            const int16_t panel[] = {
                MAINSCR::game_button, MAINSCR::next_button, MAINSCR::zoom_in_button, MAINSCR::zoom_out_button,
                MAINSCR::colonies_button, MAINSCR::planets_button, MAINSCR::fleets_button,
                MAINSCR::leaders_button, MAINSCR::races_button, MAINSCR::info_button,
                MAINSCR::treasury_window_field, MAINSCR::command_points_window_field, MAINSCR::food_window_field,
                MAINSCR::freighters_window_field, MAINSCR::research_window_field,
            };
            for (int16_t panel_field : panel) {
                if (panel_field == field_idx) {
                    return true;
                }
            }
            return false;
        }

        void Galaxy_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t capital = Capital_Star_();

            std::vector<int16_t> stars;
            for (int16_t i = 0; i < MOX::NUM_STARS; ++i) {
                stars.push_back(i);
            }
            if (capital >= 0) {
                std::stable_sort(stars.begin(), stars.end(), [capital](int16_t a, int16_t b) {
                    int32_t da = a == capital ? -1 : GEO::Parsecs_Between_Stars(capital, a);
                    int32_t db = b == capital ? -1 : GEO::Parsecs_Between_Stars(capital, b);
                    return da < db;
                });
            }

            builder->BeginStop("galaxy/stars");
            builder->PushContext(vocab::Get(vocab::VOCAB_GALAXY_STARS));
            for (int16_t star_idx : stars) {
                Add_Star_Item_(builder, context, star_idx, capital);
            }
            builder->PopContext();

            builder->BeginStop("galaxy/controls");
            builder->PushContext(vocab::Get(vocab::VOCAB_GALAXY_CONTROLS));
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                if (Is_Galaxy_Panel_Field_(i)) {
                    char key[32];
                    snprintf(key, sizeof(key), "galaxy/control/%d,%d", (int)fields::fields[i].x, (int)fields::fields[i].y);
                    Add_Field_Item_(builder, context, key, i, Field_Label_(i));
                }
            }
            builder->PopContext();

            // Popups such as the system display add their fields to this loop's list.
            std::vector<int16_t> other;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (i == MOX::main_screen_grid_field || Is_Galaxy_Panel_Field_(i) || field->field_type == FIELD_TYPE_CIRCLE ||
                    Is_Catch_All_(field) || !Field_Is_Listed_(i, true)) {
                    continue;
                }
                other.push_back(i);
            }
            if (!other.empty()) {
                builder->BeginStop("galaxy/other");
                builder->PushContext(vocab::Get(vocab::VOCAB_GALAXY_OTHER));
                for (int16_t i : other) {
                    char key[48];
                    snprintf(key, sizeof(key), "galaxy/other/%d/%d,%d", (int)i, (int)fields::fields[i].x, (int)fields::fields[i].y);
                    Add_Field_Item_(builder, context, key, i, Field_Label_(i));
                }
                builder->PopContext();
            }

            if (!stars.empty()) {
                char start[24];
                snprintf(start, sizeof(start), "star/%d", (int)stars[0]);
                builder->SetStart(graph::ControlId::Structural(start));
            }
        }

        // The system display and fleet box open inside the main loop without moving focus, so a
        // change of either is spoken here; a sighted player sees the box appear.
        int16_t polled_system = -1;
        int16_t polled_fleet_stack = -1;

        void Galaxy_Poll_(bool just_attached) {
            if (just_attached) {
                polled_system = -1;
                polled_fleet_stack = -1;
                pending_star = -1;
            }
            Poll_Pending_Star_();

            int16_t system = MOVEBOX::Moveable_Box_Selected(0) ? MOX::moveable_box[0].id : -1;
            if (system < 0 || system >= MOX::NUM_STARS) {
                system = -1;
            }
            if (system != polled_system) {
                polled_system = system;
                if (system >= 0) {
                    speech::Say(vocab::Format(vocab::VOCAB_SYSTEM_DISPLAY, Star_Label_(system)), false);
                }
            }

            int16_t fleet_stack = MOVEBOX::Moveable_Box_Selected(2) ? MOX::fleet_box_ship_stack : -1;
            if (fleet_stack < 0 || fleet_stack >= MOX::ship_stack_count) {
                fleet_stack = -1;
            }
            if (fleet_stack != polled_fleet_stack) {
                polled_fleet_stack = fleet_stack;
                if (fleet_stack >= 0) {
                    int16_t star_idx = SHIPSTAK::Ship_Stack_Star_Id(MOX::ship_stack_start[fleet_stack]);
                    speech::Say(star_idx >= 0 && star_idx < MOX::NUM_STARS
                        ? vocab::Format(vocab::VOCAB_FLEET_DISPLAY_AT, Star_Label_(star_idx))
                        : std::string(vocab::Get(vocab::VOCAB_FLEET_DISPLAY)), false);
                }
            }
        }

        const s_screen_recipe recipes[] = {
            { "main_menu", Main_Menu_Is_Active_, Main_Menu_Instance_Key_, Main_Menu_Build_, Main_Menu_Name_ },
            { "new_game", New_Game_Is_Active_, New_Game_Instance_Key_, New_Game_Build_, New_Game_Name_ },
            { "galaxy", Galaxy_Is_Active_, Galaxy_Instance_Key_, Galaxy_Build_, Galaxy_Name_, Galaxy_Poll_ },
            { "fields", Fields_Is_Active_, Fields_Instance_Key_, Fields_Build_, Fields_Name_ },
        };

    } // namespace

    const s_screen_recipe* Recipes(int32_t* count) {
        *count = (int32_t)(sizeof(recipes) / sizeof(recipes[0]));
        return recipes;
    }

} // namespace access::screens
