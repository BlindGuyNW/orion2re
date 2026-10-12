#include "pch.h"

#include "access/screens.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/speech.h"
#include "access/text.h"
#include "access/vocab.h"
#include "game/build_queue.h"

#include <algorithm>
#include <functional>
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
        // region is listed in place only when it is the screen's sole control.
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

            // Some screens close only on a click outside their controls, such as the detailed ship
            // view, so a full-screen region is still listed after them. What it does depends on
            // the screen, so it is named for the gesture.
            if (!has_specific_field) {
                return;
            }
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (!Is_Catch_All_(field)) {
                    continue;
                }
                std::string label = Field_Own_Text_(field);
                if (label.empty()) {
                    label = Hotkey_Text_(field);
                }
                if (label.empty()) {
                    label = vocab::Get(vocab::VOCAB_CLICK_ANYWHERE);
                }
                char key[48];
                snprintf(key, sizeof(key), "field/%d/anywhere", (int)i);
                Add_Field_Item_(builder, context, key, i, label);
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

        void Await_Star_(int16_t star_idx);
        std::string Star_Eta_Text_(int16_t star_idx);

        void Say_Star_Unreachable_(int16_t star_idx) {
            speech::Say(vocab::Format(vocab::VOCAB_STAR_UNREACHABLE, Star_Label_(star_idx)), true);
        }

        // The player never has to manage the view: an off-screen star is brought into view the
        // way the game's own mini-map does it, then pressed once the map has been redrawn.
        void Activate_Star_(int16_t star_idx) {
            pending_star = -1;
            if (Ship_Icons_Are_Drawn_() && Press_Star_(star_idx)) {
                Await_Star_(star_idx);
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
            if (Press_Star_(star_idx)) {
                Await_Star_(star_idx);
            } else {
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
            vtable.Announcements.push_back(graph::NodeAnnouncement([star_idx]() { return Star_Eta_Text_(star_idx); }, true, Value));

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

        const char* const GALAXY_SYSTEM_STOP = "galaxy/system";
        const char* const GALAXY_FLEET_STOP = "galaxy/fleet";

        int16_t Open_System_() {
            if (!MOVEBOX::Moveable_Box_Selected(0)) {
                return -1;
            }
            int16_t system = MOX::moveable_box[0].id;
            return system >= 0 && system < MOX::NUM_STARS ? system : -1;
        }

        int16_t Open_Fleet_Stack_() {
            if (!MOVEBOX::Moveable_Box_Selected(2)) {
                return -1;
            }
            int16_t stack = MOX::fleet_box_ship_stack;
            return stack >= 0 && stack < MOX::ship_stack_count ? stack : -1;
        }

        std::string Fleet_Box_Label_(int16_t stack) {
            int16_t star_idx = SHIPSTAK::Ship_Stack_Star_Id(MOX::ship_stack_start[stack]);
            return star_idx >= 0 && star_idx < MOX::NUM_STARS
                ? vocab::Format(vocab::VOCAB_FLEET_DISPLAY_AT, Star_Label_(star_idx))
                : std::string(vocab::Get(vocab::VOCAB_FLEET_DISPLAY));
        }

        // ---- Fleet box -----------------------------------------------------------------------------

        graph::NodeVtable Info_Vtable_(const std::vector<std::string>& lines) {
            graph::NodeVtable vtable;
            for (size_t i = 0; i < lines.size(); ++i) {
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(lines[i]));
                vtable.Announcements.back().Kind = i == 0 ? Label : Value;
            }
            return vtable;
        }

        // Mirrors FLEETPOP::Get_Fleetpop_Scanned_Ship_Info: the player's ships by
        // design name, other empires' warships only by hull.
        std::string Fleet_Icon_Name_(int16_t icon_idx) {
            int16_t ship_idx = HACCESS::Fleet_Movement_Ship_Id(icon_idx);
            const s_ship_data* ship = &MOX::ship[ship_idx];
            int16_t owner = HACCESS::Ship_Owner(ship_idx);
            if (owner != MOX::PLAYER_NUM && owner <= 8 && HACCESS::Is_Combat_Ship(ship_idx)) {
                return text::Normalize_Game_Text(TECHDATA::hull_data[(int8_t)ship->d.size].name);
            }
            char name[sizeof(ship->d.name) + 1] = {};
            memcpy(name, ship->d.name, sizeof(ship->d.name));
            return text::Normalize_Game_Text(name);
        }

        // The status line FLEETPOP::Print_Fleet_Box_Info draws under the ships. The draw adopts the
        // last chosen icon as the scanned ship one frame before it prints it; this reads the
        // settled result without changing anything.
        std::string Fleet_Box_Status_Text_() {
            if (!MOVEBOX::Moveable_Box_Selected(2) || MOX::fleet_box.count <= 0) {
                return std::string();
            }
            int16_t ship_idx = HACCESS::Fleet_Movement_Ship_Id(0);
            bool icon_selected = FLEETPOP::Some_Icon_Selected();
            int16_t scanned = MOX::fleet_ship_scanned;
            int16_t info_type = 9;
            int16_t value = -1;
            int16_t destination = -1;

            if (MOX::eta_star_id == -1) {
                if (scanned == -1 && !icon_selected && MAINSCR::fleet_icon_chosen != -1) {
                    scanned = MAINSCR::fleet_icon_chosen;
                }
                if (scanned != -1) {
                    info_type = 0;
                } else if (MOX::ship[ship_idx].status == 0) {
                    info_type = 1;
                    value = MOX::ship[ship_idx].location;
                } else {
                    FLEETPOP::Get_Absolute_Location_Scan_Info(ship_idx, &value, &destination, &info_type);
                }
            } else if (MOX::eta_star_id > -1 && icon_selected) {
                const s_ship_move_info* move = &MOX::g_ship_move_info;
                bool visited = HAROLD::Player_Has_Visited(MOX::PLAYER_NUM, MOX::eta_star_id) != 0;
                if (move->moving != 0) {
                    if (move->turns_left == 0) {
                        info_type = 1;
                        value = MOX::eta_star_id;
                    } else {
                        info_type = visited ? 2 : 3;
                        value = (uint8_t)move->turns_left;
                        destination = MOX::eta_star_id;
                    }
                } else if (move->blackhole_blocks != 0) {
                    info_type = 4;
                } else if (move->immobile != 0) {
                    info_type = 5;
                } else if (move->out_of_range != 0) {
                    info_type = visited ? 6 : 7;
                    value = (uint8_t)move->parsecs;
                    destination = MOX::eta_star_id;
                } else if (move->hyperspace_flux != 0) {
                    info_type = 8;
                }
            }

            char text[200] = {};
            const char* star_name = destination >= 0 && destination < MOX::NUM_STARS ? MOX::star[destination].name : "";
            switch (info_type) {
                case 0: return scanned >= 0 && scanned < MOX::fleet_box.count ? Fleet_Icon_Name_(scanned) : std::string();
                case 1: snprintf(text, sizeof(text), HAROLD::H_Message(0x67), MOX::star[value].name); break;
                case 2:
                    if (destination == MOX::NUM_STARS) {
                        strlcpy(text, HAROLD::H_Message(0x68), sizeof(text));
                    } else {
                        snprintf(text, sizeof(text), HAROLD::H_Message(value == 1 ? 0x69 : 0x6a), (int)value, star_name);
                    }
                    break;
                case 3: snprintf(text, sizeof(text), HAROLD::H_Message(value == 1 ? 0x6b : 0x6c), (int)value); break;
                case 4: strlcpy(text, HAROLD::H_Message(0x6d), sizeof(text)); break;
                case 5: strlcpy(text, HAROLD::H_Message(0x6e), sizeof(text)); break;
                case 6: snprintf(text, sizeof(text), HAROLD::H_Message(value == 1 ? 0x6f : 0x70), (int)value, star_name); break;
                case 7:
                    if (value == 0) {
                        strlcpy(text, HAROLD::H_Message(0x71), sizeof(text));
                    } else {
                        snprintf(text, sizeof(text), HAROLD::H_Message(0xeb), (int)value);
                    }
                    break;
                case 8: strlcpy(text, HAROLD::H_Message(0x73), sizeof(text)); break;
                default: break;
            }
            return text::Normalize_Game_Text(text);
        }

        // The ETA line the fleet box shows while the pointer rests on this star with ships selected.
        std::string Star_Eta_Text_(int16_t star_idx) {
            if (!MOVEBOX::Moveable_Box_Selected(2) || MOX::eta_star_id != star_idx || MOX::fleet_ship_scanned != -1) {
                return std::string();
            }
            return Fleet_Box_Status_Text_();
        }

        int16_t Visible_Fleet_Icon_Field_(int16_t icon_idx) {
            int16_t end = (int16_t)(MOX::fleet_icon_buttons_added + MOX::first_fleet_movement_icon);
            if (end > MOX::fleet_box.count) {
                end = MOX::fleet_box.count;
            }
            if (icon_idx < MOX::first_fleet_movement_icon || icon_idx >= end) {
                return 0;
            }
            int16_t field_idx = MOX::fleet_movement_icon[icon_idx].field;
            return Is_Valid_Field_(field_idx) ? field_idx : 0;
        }

        // The game selects ships only while the left button is physically held over an icon: the
        // main screen's draw paints the first icon's flipped state across each icon it passes
        // (mainscr_main.cpp). This makes the same call for one icon. A ship scrolled out of the box
        // has no field to paint, so it gets the rule Set_Painted_Fleet_Fields applies.
        void Toggle_Fleet_Ship_(int16_t icon_idx) {
            if (!MOVEBOX::Moveable_Box_Selected(2) || icon_idx < 0 || icon_idx >= MOX::fleet_box.count ||
                !FLEETPOP::Can_Select_Fleet_Box_Ship(0)) {
                return;
            }
            int16_t field_idx = Visible_Fleet_Icon_Field_(icon_idx);
            if (field_idx > 0) {
                MAINSCR::fltpop_field_selection_status = (uint8_t)(FLEETPOP::Icon_Selection_Status(field_idx) == 0);
                MAINSCR::Set_Painted_Fleet_Fields(field_idx);
                return;
            }
            s_ship_node* node = &MOX::ship_node[MOX::fleet_movement_icon[icon_idx].ship_node_idx];
            if (MAINSCR::Ship_Can_Be_Selected(node->ship_idx)) {
                node->selected = (int8_t)(node->selected == 0);
            }
        }

        std::string Fleet_Ship_State_(int16_t icon_idx) {
            if (!MOVEBOX::Moveable_Box_Selected(2) || icon_idx >= MOX::fleet_box.count) {
                return std::string();
            }
            const s_ship_node* node = &MOX::ship_node[MOX::fleet_movement_icon[icon_idx].ship_node_idx];
            return vocab::Get(node->selected != 0 ? vocab::VOCAB_STATE_SELECTED : vocab::VOCAB_STATE_NOT_SELECTED);
        }

        std::string Help_Label_Or_(int16_t field_idx, vocab::e_vocab_string fallback) {
            std::string label = Help_Title_(Field_Help_Id_(&fields::fields[field_idx]));
            return label.empty() ? std::string(vocab::Get(fallback)) : label;
        }

        bool Is_Fleet_Box_Field_(int16_t field_idx) {
            const s_movable_box* box = &MOX::moveable_box[2];
            const int16_t box_fields[] = {
                box->close_btn, box->drag_field, box->window_field_id, MOX::fleet_movement_all_button,
                MOX::fleet_box_scroll_up_button, MOX::fleet_box_scroll_dn_button, MOX::fleet_movement_bar_field,
                MOX::fleet_box_outpost_button, MOX::fleet_box_colonize_button, MOX::fleet_box_engage_button,
                MOX::fleet_box_transport_button, MOX::fleet_box_attack_button,
            };
            for (int16_t box_field : box_fields) {
                if (box_field == field_idx) {
                    return true;
                }
            }
            for (int16_t i = 0; i < MOX::fleet_box.count; ++i) {
                if (Visible_Fleet_Icon_Field_(i) == field_idx) {
                    return true;
                }
            }
            return false;
        }

        void Build_Fleet_Stop_(graph::GraphBuilder* builder, s_build_context* context, const char* stop, int16_t stack) {
            char prefix[48];
            snprintf(prefix, sizeof(prefix), "%s/%d", stop, (int)stack);
            std::string base = prefix;

            graph::NodeVtable status;
            status.Announcements.push_back(graph::NodeAnnouncement([]() { return Fleet_Box_Status_Text_(); }, true, Label));
            builder->AddItem(graph::ControlId::Structural(base + "/status"), status);

            bool own = FLEETPOP::Can_Select_Fleet_Box_Ship(0);
            for (int16_t i = 0; i < MOX::fleet_box.count; ++i) {
                int16_t ship_idx = HACCESS::Fleet_Movement_Ship_Id(i);
                std::string key = base + "/ship/" + std::to_string(ship_idx);
                graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ Fleet_Icon_Name_(i) });
                int16_t field_idx = Visible_Fleet_Icon_Field_(i);
                if (own) {
                    vtable.Type = &toggle_type;
                    vtable.Announcements.push_back(graph::NodeAnnouncement([i]() { return Fleet_Ship_State_(i); }, true, Value));
                    vtable.OnActivate = [i]() { Toggle_Fleet_Ship_(i); };
                    // A right click on the player's own ship opens its detailed view.
                    if (field_idx > 0) {
                        vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
                    }
                }
                if (field_idx > 0) {
                    context->node_fields[key] = field_idx;
                    vtable.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }

            if (Is_Valid_Field_(MOX::fleet_movement_all_button)) {
                Add_Field_Item_(builder, context, base + "/all", MOX::fleet_movement_all_button,
                    Help_Label_Or_(MOX::fleet_movement_all_button, vocab::VOCAB_FLEET_ALL));
            }
            const struct {
                int8_t active;
                int16_t field;
                const char* key;
            } actions[] = {
                { MOX::fleet_box.colonize_btn_active, MOX::fleet_box_colonize_button, "/colonize" },
                { MOX::fleet_box.outpost_btn_active, MOX::fleet_box_outpost_button, "/outpost" },
                { MOX::fleet_box.transport_btn_active, MOX::fleet_box_transport_button, "/transport" },
                { MOX::fleet_box.engage_btn_active, MOX::fleet_box_engage_button, "/engage" },
                { MOX::fleet_box.attack_btn_active, MOX::fleet_box_attack_button, "/attack" },
            };
            for (const auto& action : actions) {
                if (action.active != 0 && Is_Valid_Field_(action.field)) {
                    Add_Field_Item_(builder, context, base + action.key, action.field, Field_Label_(action.field));
                }
            }
            if (Is_Valid_Field_(MOX::moveable_box[2].close_btn)) {
                Add_Field_Item_(builder, context, base + "/close", MOX::moveable_box[2].close_btn, vocab::Get(vocab::VOCAB_CLOSE));
            }
        }

        // ---- System display ----------------------------------------------------------------------

        // The game keeps a few H_Message strings as printf formats; it formats them the same way.
        std::string H_Text_(int16_t message_id) {
            return text::Normalize_Game_Text(HAROLD::H_Message(message_id));
        }

        std::string Star_Name_(int16_t star_idx) {
            char name[sizeof(MOX::star[star_idx].name) + 1] = {};
            memcpy(name, MOX::star[star_idx].name, sizeof(MOX::star[star_idx].name));
            return text::Normalize_Game_Text(name);
        }

        std::string Race_Name_(int16_t player_idx) {
            char race[sizeof(MOX::player[player_idx].race_name) + 1] = {};
            memcpy(race, MOX::player[player_idx].race_name, sizeof(MOX::player[player_idx].race_name));
            return race;
        }

        // The lines SYS::Do_Print_Planet_Info draws when the pointer is over a planet, which can't
        // be called here because it draws as it formats. The first line is the planet's name.
        std::vector<std::string> Planet_Info_Lines_(int16_t planet_idx) {
            std::vector<std::string> lines;
            const s_planet_data* planet = &MOX::planet[planet_idx];
            char line[200];

            if (planet->planet_type == PLANET_TYPE_GAS_GIANT) {
                lines.push_back(H_Text_(0x15f));
                return lines;
            }
            if (planet->planet_type == PLANET_TYPE_ASTEROID) {
                lines.push_back(H_Text_(0x160));
                return lines;
            }

            const s_colony* colony = planet->colony_index > -1 ? &MOX::colony[planet->colony_index] : nullptr;
            uint8_t climate = (uint8_t)(colony != nullptr ? colony->climate : planet->climate);

            char name[64] = {};
            HACCESS::Do_Get_Planet_Name(planet_idx, name, 0);
            std::string title = name;
            if (colony != nullptr) {
                title += " (" + Race_Name_((int8_t)colony->owner);
                title += colony->outpost_flag != 0 ? std::string(HAROLD::H_Message(0x161)) : std::string(")");
            }
            lines.push_back(text::Normalize_Game_Text(title.c_str()));

            snprintf(line, sizeof(line), "%s, %s", MOX::planet_size_string[(int8_t)planet->size], MOX::planet_climate_string[climate]);
            lines.push_back(text::Normalize_Game_Text(line));

            int16_t max_population = COLCALC::Planet_Max_Population_For_Player(const_cast<s_planet_data*>(planet), MOX::PLAYER_NUM);
            if (colony != nullptr) {
                snprintf(line, sizeof(line), HAROLD::H_Message(0x181), (uint32_t)(uint8_t)colony->n_pops, (int32_t)max_population);
            } else {
                snprintf(line, sizeof(line), HAROLD::H_Message(0x162), (int32_t)max_population);
            }
            lines.push_back(text::Normalize_Game_Text(line));

            if ((int8_t)planet->gravity_class == PLANET_GRAVITY_NORMAL_G) {
                snprintf(line, sizeof(line), "%s", MOX::mineral_class_string[(int8_t)planet->mineral_class]);
            } else {
                snprintf(line, sizeof(line), "%s, %s", MOX::mineral_class_string[(int8_t)planet->mineral_class],
                    MOX::planet_gravity_string[(int8_t)planet->gravity_class]);
            }
            lines.push_back(text::Normalize_Game_Text(line));

            // Mirrors SYS::Print_Special_Building_Data and the visibility rule that gates it.
            bool can_show_buildings = MOX::player_has_ship_in_system != 0 ||
                HAROLD::Player_Has_Colony(MOX::PLAYER_NUM, planet->star_index) ||
                MOX::player[MOX::PLAYER_NUM].traits[TRAIT_OMNISCIENCE] != 0;
            if (colony != nullptr && can_show_buildings) {
                const int16_t buildings[] = {
                    BUILDING_STAR_FORTRESS, BUILDING_STAR_BASE, BUILDING_BATTLESTATION, BUILDING_MISSILE_BASE,
                    BUILDING_GROUND_BATTERIES, BUILDING_STELLAR_CONVERTER, BUILDING_FIGHTER_GARRISON,
                };
                for (int16_t building : buildings) {
                    if (colony->buildings[building] != 0) {
                        lines.push_back(text::Normalize_Game_Text(TECHDATA::buildings[building].name));
                    }
                }
            }

            int32_t special = (int8_t)planet->planet_special;
            if (special != 0 && special != 0xb) {
                lines.push_back(text::Normalize_Game_Text(MOX::planet_special_string[special]));
            }
            return lines;
        }

        // The lines SYS::Do_Print_System_Ship_Data draws for a fleet icon in the system display.
        std::vector<std::string> System_Fleet_Lines_(int16_t ship_node_num) {
            std::vector<std::string> lines;
            char line[200];

            int16_t node = ship_node_num >= SHIP_NODE_UI_TAG_BASE ? (int16_t)(ship_node_num - SHIP_NODE_UI_TAG_BASE) : ship_node_num;
            int16_t ship_idx = MOX::ship_node[node].ship_idx;
            int16_t owner = HACCESS::Ship_Owner(ship_idx);
            if (ship_node_num >= SHIP_NODE_UI_TAG_BASE || owner >= MAX_PLAYERS) {
                char name[sizeof(MOX::ship[ship_idx].d.name) + 1] = {};
                memcpy(name, MOX::ship[ship_idx].d.name, sizeof(MOX::ship[ship_idx].d.name));
                lines.push_back(text::Normalize_Game_Text(name));
                return lines;
            }

            int16_t total = 0;
            int16_t colony_ships = 0;
            int16_t outposts = 0;
            int16_t transports = 0;
            int16_t hulls[SHIP_SIZE_DOOM_STAR + 1];
            SYS::N_Ships_By_Type_In_Stack(ship_node_num, &total, &colony_ships, &outposts, &transports, hulls);

            snprintf(line, sizeof(line), HAROLD::H_Message(0x163), Race_Name_(owner).c_str());
            lines.push_back(text::Normalize_Game_Text(line));
            for (int16_t hull = 0; hull <= SHIP_SIZE_DOOM_STAR; ++hull) {
                if (hulls[hull] > 0) {
                    const char* hull_name = hulls[hull] == 1 ? TECHDATA::hull_data[hull].name : TECHDATA::hull_data[hull].size_name;
                    snprintf(line, sizeof(line), "%d %s", (int32_t)hulls[hull], hull_name);
                    lines.push_back(text::Normalize_Game_Text(line));
                }
            }
            const int16_t counts[] = { colony_ships, outposts, transports };
            const int16_t one_messages[] = { 0x164, 0x166, 0x168 };
            for (int32_t i = 0; i < 3; ++i) {
                if (counts[i] > 0) {
                    snprintf(line, sizeof(line), HAROLD::H_Message(counts[i] == 1 ? one_messages[i] : (int16_t)(one_messages[i] + 1)),
                        (int32_t)counts[i]);
                    lines.push_back(text::Normalize_Game_Text(line));
                }
            }
            return lines;
        }

        // SYS::Print_Gate_Info: gate_slot 0 is a dimensional portal, 3 an Artemis net, each naming
        // the races that own one here.
        std::string Gate_Text_(int16_t star_idx, int16_t gate_slot) {
            std::string text = H_Text_(gate_slot == 0 ? 0x17C : 0x17B);
            std::string owners;
            for (int16_t i = 0; i < MOX::NUM_PLAYERS; ++i) {
                bool owns = gate_slot == 0
                    ? HAROLD::Player_Has_Dimensional_Portal(&MOX::player[i], &MOX::star[star_idx])
                    : HAROLD::Player_Has_Artemis_Net(&MOX::player[i], &MOX::star[star_idx]);
                if (owns) {
                    owners += (owners.empty() ? "" : ", ") + Race_Name_(i);
                }
            }
            return text::Normalize_Spoken_Text(text + owners + ")");
        }

        // The wormhole line SYS::Draw_System_Display_Popup prints along the bottom of the display.
        std::string Wormhole_Text_(int16_t star_idx) {
            int16_t other = MOX::star[star_idx].wormhole_star_id;
            if (other < 0 || other >= MOX::NUM_STARS) {
                return std::string();
            }
            if (!HAROLD::Player_Has_Visited(MOX::PLAYER_NUM, other)) {
                return H_Text_(0x16d);
            }
            char line[200];
            snprintf(line, sizeof(line), HAROLD::H_Message(0x16c), Star_Name_(other).c_str());
            return text::Normalize_Game_Text(line);
        }

        // An asteroid belt has no field of its own: the main loop hit-tests the pointer against
        // the orbits when the display's grid is pressed (MAINSCR::Scan_System_Display_Fields).
        bool Asteroid_Point_(int32_t orbit, int16_t* out_x, int16_t* out_y) {
            int16_t grid_idx = MOX::system_display_grid_field;
            if (!Is_Valid_Field_(grid_idx)) {
                return false;
            }
            const s_field* grid = &fields::fields[grid_idx];
            int16_t cx = (int16_t)(MOX::system_display_base_x + 171);
            int16_t cy = (int16_t)(MOX::system_display_base_y + 134);
            for (int16_t step = 1; step < 200; ++step) {
                const int16_t candidates[2] = { (int16_t)(cx - step), (int16_t)(cx + step) };
                for (int16_t x : candidates) {
                    if (x < grid->x || x > grid->x_end || cy < grid->y || cy > grid->y_end) {
                        continue;
                    }
                    if (GEO::Get_Orbit_From_X_Y(cx - x, 0) == orbit) {
                        *out_x = x;
                        *out_y = cy;
                        return true;
                    }
                }
            }
            return false;
        }

        // Pressing a field the game reads as a click: a planet opens its colony screen when it is
        // the player's, a fleet opens its fleet box. A right click on a planet other than a gas
        // giant opens its colony potential (MAINSCR::Scan_System_Display_Fields).
        void Add_System_Field_Node_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            int16_t field_idx, const std::vector<std::string>& lines, const graph::ControlType* type, bool has_secondary) {
            graph::NodeVtable vtable = Info_Vtable_(lines);
            vtable.Type = type;
            vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
            if (has_secondary) {
                vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
            }
            vtable.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };
            context->node_fields[key] = field_idx;
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        void Add_System_Text_Node_(graph::GraphBuilder* builder, const std::string& key, const std::string& text) {
            builder->AddItem(graph::ControlId::Structural(key), Info_Vtable_(std::vector<std::string>{ text }));
        }

        // The display's own fields, which the system stop lists in its own order; everything else
        // in the box (title bar, background, orbit grid) is a drag or click target with no meaning.
        bool Is_System_Field_(int16_t field_idx) {
            const s_movable_box* box = &MOX::moveable_box[0];
            if (field_idx == box->close_btn || field_idx == box->drag_field || field_idx == box->window_field_id ||
                field_idx == MOX::system_display_grid_field) {
                return true;
            }
            for (int16_t i = 0; i < 5; ++i) {
                if (field_idx == MOX::planet_buttons[i]) {
                    return true;
                }
            }
            for (int16_t i = 0; i < 15; ++i) {
                if (MOX::system_display_ships[i].ship_idx > -1 && field_idx == MOX::system_display_ships[i].button) {
                    return true;
                }
            }
            for (int16_t i = 0; i < 4; ++i) {
                if (field_idx == MOX::gate_icon_field[i]) {
                    return true;
                }
            }
            return false;
        }

        void Build_System_Stop_(graph::GraphBuilder* builder, s_build_context* context, const char* stop, int16_t star_idx,
            int16_t close_field, const std::string& close_label) {
            char prefix[48];
            snprintf(prefix, sizeof(prefix), "%s/%d", stop, (int)star_idx);
            std::string base = prefix;

            if (MOX::look_at_colonies == 0 && !SYS::Ok_To_View_System(star_idx, MOX::PLAYER_NUM)) {
                if (!HAROLD::Player_Sighted_Monster(MOX::PLAYER_NUM, star_idx) && SYS::star_description_string[0] != '\0') {
                    Add_System_Text_Node_(builder, base + "/description", text::Normalize_Game_Text(SYS::star_description_string));
                }
            } else {
                // Planets inner orbit first; the display stores them sorted by drawing depth.
                std::vector<int16_t> slots;
                for (int16_t slot = 0; slot < 5; ++slot) {
                    if (MOX::sys_disp != nullptr && MOX::sys_disp[slot].orbit > -1) {
                        slots.push_back(slot);
                    }
                }
                std::stable_sort(slots.begin(), slots.end(), [](int16_t a, int16_t b) {
                    return MOX::planet[MOX::sys_disp[a].orbit].orbit < MOX::planet[MOX::sys_disp[b].orbit].orbit;
                });
                for (int16_t slot : slots) {
                    int16_t planet_idx = MOX::sys_disp[slot].orbit;
                    std::string key = base + "/planet/" + std::to_string(planet_idx);
                    std::vector<std::string> lines = Planet_Info_Lines_(planet_idx);
                    if (MOX::planet[planet_idx].planet_type == PLANET_TYPE_ASTEROID) {
                        int16_t x = 0;
                        int16_t y = 0;
                        if (Asteroid_Point_(MOX::planet[planet_idx].orbit, &x, &y)) {
                            context->node_points[key] = s_pointer_target{ x, y };
                            Add_System_Field_Node_(builder, context, key, MOX::system_display_grid_field, lines, nullptr, false);
                            continue;
                        }
                        builder->AddItem(graph::ControlId::Structural(key), Info_Vtable_(lines));
                    } else if (Is_Valid_Field_(MOX::planet_buttons[slot])) {
                        Add_System_Field_Node_(builder, context, key, MOX::planet_buttons[slot], lines, nullptr,
                            MOX::planet[planet_idx].planet_type != PLANET_TYPE_GAS_GIANT);
                    } else {
                        builder->AddItem(graph::ControlId::Structural(key), Info_Vtable_(lines));
                    }
                }

                // Build_System_Popup_Ships fills one slot per owner from the detection-filtered icons.
                for (int16_t slot = 0; slot < 15; ++slot) {
                    const s_system_display_ships* ships = &MOX::system_display_ships[slot];
                    if (ships->ship_idx <= -1 || ships->pic == nullptr || !Is_Valid_Field_(ships->button)) {
                        continue;
                    }
                    Add_System_Field_Node_(builder, context, base + "/fleet/" + std::to_string(slot), ships->button,
                        System_Fleet_Lines_(ships->ship_idx), &button_type, false);
                }

                const int16_t gate_slots[] = { 3, 0 };
                for (int16_t gate_slot : gate_slots) {
                    int16_t field_idx = MOX::gate_icon_field[gate_slot];
                    if (!Is_Valid_Field_(field_idx)) {
                        continue;
                    }
                    std::string key = base + "/gate/" + std::to_string(gate_slot);
                    graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ Gate_Text_(star_idx, gate_slot) });
                    context->node_fields[key] = field_idx;
                    builder->AddItem(graph::ControlId::Structural(key), vtable);
                }

                std::string wormhole = Wormhole_Text_(star_idx);
                if (!wormhole.empty()) {
                    Add_System_Text_Node_(builder, base + "/wormhole", wormhole);
                }
            }

            if (Is_Valid_Field_(close_field)) {
                Add_Field_Item_(builder, context, base + "/close", close_field, close_label);
            }
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

            // The system display and the fleet box replace each other, so at most one is open. Keys
            // carry the system or stack, so a different one starts at its first control.
            int16_t system = Open_System_();
            if (system >= 0) {
                builder->BeginStop(GALAXY_SYSTEM_STOP);
                builder->PushContext(vocab::Format(vocab::VOCAB_SYSTEM_DISPLAY, Star_Label_(system)));
                Build_System_Stop_(builder, context, GALAXY_SYSTEM_STOP, system, MOX::moveable_box[0].close_btn, vocab::Get(vocab::VOCAB_CLOSE));
                builder->PopContext();
            }

            int16_t fleet_stack = system >= 0 ? -1 : Open_Fleet_Stack_();
            if (fleet_stack >= 0) {
                builder->BeginStop(GALAXY_FLEET_STOP);
                builder->PushContext(Fleet_Box_Label_(fleet_stack));
                Build_Fleet_Stop_(builder, context, GALAXY_FLEET_STOP, fleet_stack);
                builder->PopContext();
            }

            // Any other popup adds its fields to this loop's list.
            std::vector<int16_t> other;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (i == MOX::main_screen_grid_field || Is_Galaxy_Panel_Field_(i) || field->field_type == FIELD_TYPE_CIRCLE ||
                    Is_Catch_All_(field) || !Field_Is_Listed_(i, true) || (system >= 0 && Is_System_Field_(i)) ||
                    (fleet_stack >= 0 && Is_Fleet_Box_Field_(i))) {
                    continue;
                }
                other.push_back(i);
            }
            if (!other.empty()) {
                builder->BeginStop("galaxy/other");
                builder->PushContext(vocab::Get(vocab::VOCAB_GALAXY_OTHER));
                for (int16_t i : other) {
                    char key[64];
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

        // The system display and fleet box open inside the main loop without moving focus, so the
        // poll asks for focus in them and the differ announces the landing (spec 7.3).
        int16_t polled_system = -1;
        int16_t polled_fleet_stack = -1;
        // A star pressed through its node: its system display takes focus even when it was
        // already open on that star, so Enter is never silent.
        int16_t awaited_star = -1;
        int32_t awaited_star_ticks = 0;

        void Await_Star_(int16_t star_idx) {
            awaited_star = star_idx;
            awaited_star_ticks = 0;
        }

        void Galaxy_Poll_(bool just_attached) {
            if (just_attached) {
                // Arriving on the screen lands on the capital; an already open box stays put.
                polled_system = Open_System_();
                polled_fleet_stack = Open_Fleet_Stack_();
                pending_star = -1;
                awaited_star = -1;
                return;
            }

            if (awaited_star >= 0) {
                ++awaited_star_ticks;
            }
            Poll_Pending_Star_();

            int16_t system = Open_System_();
            int16_t fleet_stack = Open_Fleet_Stack_();
            bool awaited_landed = awaited_star >= 0 && awaited_star_ticks >= 1 && system == awaited_star;

            if (system >= 0 && (system != polled_system || awaited_landed)) {
                navigator::Request_Focus_Stop(GALAXY_SYSTEM_STOP);
                awaited_star = -1;
            } else if (fleet_stack >= 0 && fleet_stack != polled_fleet_stack) {
                navigator::Request_Focus_Stop(GALAXY_FLEET_STOP);
                awaited_star = -1;
            }
            if (awaited_star >= 0 && awaited_star_ticks >= PENDING_STAR_MAX_TICKS) {
                awaited_star = -1;
            }

            polled_system = system;
            polled_fleet_stack = fleet_stack;
        }

        // ---- Text box ----------------------------------------------------------------------------

        // TEXTBOX::Do_Text_Box draws its message over the screen and waits for any input, leaving
        // the screen's own fields in place beneath a full-screen hidden field of its own.
        bool Text_Box_Is_Active_() {
            return TEXTBOX::shown_text != nullptr;
        }

        std::string Text_Box_Instance_Key_() {
            std::string key = "text_box/";
            if (TEXTBOX::shown_title != nullptr) {
                key += TEXTBOX::shown_title;
            }
            key += '/';
            key += TEXTBOX::shown_text;
            return key;
        }

        std::string Text_Box_Name_() {
            std::string title = text::Normalize_Game_Text(TEXTBOX::shown_title);
            return title.empty() ? std::string(vocab::Get(vocab::VOCAB_MESSAGE)) : title;
        }

        // The box's own dismiss field is the last one added; any input closes the box.
        int16_t Text_Box_Dismiss_Field_() {
            for (int16_t i = (int16_t)(fields::fields_count - 1); i > 0; --i) {
                if (Is_Catch_All_(&fields::fields[i])) {
                    return i;
                }
            }
            return 0;
        }

        // One node per line or paragraph break of the message, so a long message can be read a
        // paragraph at a time.
        void Text_Box_Build_(graph::GraphBuilder* builder, s_build_context*) {
            int16_t dismiss = Text_Box_Dismiss_Field_();
            std::string raw = TEXTBOX::shown_text;
            size_t start = 0;
            int32_t line_number = 0;
            while (start <= raw.size()) {
                size_t end = raw.find_first_of("\r\n", start);
                if (end == std::string::npos) {
                    end = raw.size();
                }
                std::string line = text::Normalize_Game_Text(raw.substr(start, end - start).c_str());
                start = end + 1;
                if (line.empty()) {
                    continue;
                }
                std::string key = "text_box/line/" + std::to_string(line_number++);
                graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ line });
                if (Is_Valid_Field_(dismiss)) {
                    vtable.OnActivate = [dismiss]() { navigator::Request_Field_Activation(dismiss); };
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        // ---- Research selection ------------------------------------------------------------------

        // TECH::_Tech_Select publishes its eight areas while it waits for input; _Tech_List, the
        // browse list for one area, opens over it without removing its fields.
        bool Research_Is_Active_() {
            return TECH::shown_entries != nullptr && TECH::shown_list_entry == nullptr &&
                Is_Valid_Field_(TECH::g_first_tech_field) && Is_Valid_Field_(TECH::g_last_tech_field);
        }

        std::string Research_Instance_Key_() {
            return TECH::shown_accept_field > 0 ? "research/change" : "research/select";
        }

        std::string Research_Name_() {
            return vocab::Get(TECH::shown_accept_field > 0 ? vocab::VOCAB_SCREEN_TECH_CHANGE : vocab::VOCAB_SCREEN_RESEARCH);
        }

        // TECH::Display_Entry_Text highlights the current field's chosen technology, or all of
        // its technologies for a creative race and the fields that research as a whole.
        bool Research_App_Is_Current_(const s_list_item_research* entry, int16_t app) {
            const s_player* player = MOX::cur_plyr_ptr;
            if (player == nullptr || (uint8_t)player->current_research_field != entry->tech_field_id) {
                return false;
            }
            int16_t field = entry->tech_field_id;
            if (player->traits[TRAIT_CREATIVE] == 1 || field == 0x37 || field == 0x39 || field == 0x1D || field == 0x16 ||
                field == 0x1C || field == 0x17) {
                return true;
            }
            uint8_t current = (uint8_t)player->current_research_application;
            return strcasecmp(entry->app_names[app], TECHDATA::technology_applications[current].name) == 0;
        }

        // The game picks the technology from the field under the pointer (Draw_Tech_Select), and
        // the press then commits whatever is picked. Picking it here too keeps a press that lands
        // before the next draw from committing the previous pick.
        void Choose_Research_(int16_t field_idx) {
            if (TECH::shown_entries != nullptr) {
                TECH::Set_Selected_Entry(TECH::shown_entries, field_idx);
            }
            navigator::Request_Field_Activation(field_idx);
        }

        void Research_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            const s_list_item_research* entries = TECH::shown_entries;
            std::string start;
            for (int16_t i = 0; i < 8; ++i) {
                const s_list_item_research* entry = &entries[i];
                if (entry->tech_field_id == 0) {
                    continue;
                }
                // The area's name is drawn as part of the picture; _Tech_List titles its list with
                // the same name from billtext.lbx.
                std::string area;
                help::Read_Text_Message("billtext.lbx", (int16_t)(0x40 + entry->id), &area);
                std::string heading = text::Normalize_Game_Text(entry->field_name);
                std::string cost = text::Normalize_Game_Text(entry->buf);
                if (!area.empty()) {
                    heading = area + ", " + heading;
                }
                builder->PushContext(cost.empty() ? heading : heading + ", " + cost);

                for (int16_t app = 0; app < entry->visible_app_count && app < 4; ++app) {
                    int16_t field_idx = entry->app_click_field_ids[app];
                    std::string key = "research/" + std::to_string(i) + "/" + std::to_string(app);
                    graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ text::Normalize_Game_Text(entry->app_names[app]) });
                    if (Research_App_Is_Current_(entry, app)) {
                        vtable.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(vocab::VOCAB_STATE_CURRENT)));
                        vtable.Announcements.back().Kind = Selected;
                    }
                    if (Is_Valid_Field_(field_idx)) {
                        vtable.OnActivate = [field_idx]() { Choose_Research_(field_idx); };
                        // A right click shows the technology's description.
                        vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
                        vtable.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };
                        context->node_fields[key] = field_idx;
                    }
                    builder->AddItem(graph::ControlId::Structural(key), vtable);
                    if (start.empty()) {
                        start = key;
                    }
                }

                if (TECH::allow_future_technology_browsing && Is_Valid_Field_(entry->button_field_id)) {
                    std::string label = vocab::Format(vocab::VOCAB_RESEARCH_BROWSE, area.empty() ? heading : area);
                    Add_Field_Item_(builder, context, "research/" + std::to_string(i) + "/browse", entry->button_field_id, label);
                }
                builder->PopContext();
            }

            if (Is_Valid_Field_(TECH::shown_accept_field)) {
                Add_Field_Item_(builder, context, "research/accept", TECH::shown_accept_field,
                    Help_Label_Or_(TECH::shown_accept_field, vocab::VOCAB_ACCEPT));
            }
            if (!start.empty()) {
                builder->SetStart(graph::ControlId::Structural(start));
            }
        }

        // ---- Confirmation box --------------------------------------------------------------------

        // GENDRAW::Confirmation_Box draws its question over the screen and adds its two buttons
        // above the screen's own fields; the question is spoken as the screen's name.
        bool Confirmation_Is_Active_() {
            return GENDRAW::shown_confirmation != nullptr;
        }

        std::string Confirmation_Instance_Key_() {
            return std::string("confirm/") + GENDRAW::shown_confirmation;
        }

        std::string Confirmation_Name_() {
            std::string question = text::Normalize_Game_Text(GENDRAW::shown_confirmation);
            return question.empty() ? std::string(vocab::Get(vocab::VOCAB_CONFIRM)) : question;
        }

        void Confirmation_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t yes = GENDRAW::first_field;
            int16_t no = (int16_t)(yes + 1);
            if (Is_Valid_Field_(yes)) {
                Add_Field_Item_(builder, context, "confirm/yes", yes, vocab::Get(vocab::VOCAB_YES));
            }
            if (Is_Valid_Field_(no)) {
                Add_Field_Item_(builder, context, "confirm/no", no, vocab::Get(vocab::VOCAB_NO));
            }
            builder->SetStart(graph::ControlId::Structural("confirm/yes"));
        }

        // ---- Turn summary ------------------------------------------------------------------------

        // TURNSUM::Turn_Summary_Popup lists the turn's messages a page at a time and publishes its
        // buttons while it waits for input. A press on a message opens the colony it names, when
        // it names one, and acts on the entry the draw last selected from the pointer.
        bool Turn_Summary_Is_Active_() {
            return TURNSUM::shown_ok_field > 0 && TURNSUM::g_turn_sum_header.items != nullptr;
        }

        std::string Turn_Summary_Instance_Key_() {
            return "turn_summary";
        }

        std::string Turn_Summary_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_TURN_SUMMARY);
        }

        // The current page runs from the page pointer to the next entry that starts a page
        // (LIST::Add_Fields_To_List_Page).
        bool Turn_Message_On_Page_(int16_t index) {
            const s_list_state* list = &TURNSUM::g_turn_sum_header;
            const s_list_item* item = list->selected_item;
            const s_list_item* end = &list->items[list->num_items];
            while (item != nullptr && item < end) {
                if (item == &list->items[index]) {
                    return true;
                }
                if (item + 1 >= end || item[1].stop_flag != 0) {
                    break;
                }
                ++item;
            }
            return false;
        }

        int16_t Turn_Message_Field_(int16_t index) {
            if (!Turn_Message_On_Page_(index)) {
                return 0;
            }
            const s_list_item* item = &TURNSUM::g_turn_sum_header.items[index];
            int16_t field_idx = item->app_count > 0 ? (int16_t)(int8_t)item->app_ids[0] : 0;
            return Is_Valid_Field_(field_idx) ? field_idx : 0;
        }

        // Makes the selection Draw_Turn_Summary_Popup would make with the pointer on the message,
        // so a press that lands before the next draw opens the right colony.
        void Press_Turn_Message_(int16_t field_idx) {
            s_list_state* list = &TURNSUM::g_turn_sum_header;
            LIST::Set_Selected_List_Entry(list, field_idx);
            TURNSUM::g_selected_msg_p = LIST::Get_Selected_List_Entry(list);
            navigator::Request_Field_Activation(field_idx);
        }

        // A message on another page, waiting for the popup's page buttons to bring it up.
        int16_t pending_turn_message = -1;
        int32_t pending_turn_message_ticks = 0;
        uint8_t pending_turn_message_page = 0;

        void Page_Toward_Turn_Message_(int16_t index) {
            const s_list_state* list = &TURNSUM::g_turn_sum_header;
            bool forward = list->selected_item == nullptr || &list->items[index] > list->selected_item;
            int16_t button = forward ? TURNSUM::shown_page_down_field : TURNSUM::shown_page_up_field;
            pending_turn_message_page = list->cur_page;
            if (Is_Valid_Field_(button)) {
                navigator::Request_Field_Activation(button);
            }
        }

        // The player never has to page: a message off the page is paged to, then pressed.
        void Activate_Turn_Message_(int16_t index) {
            int16_t field_idx = Turn_Message_Field_(index);
            if (field_idx > 0) {
                pending_turn_message = -1;
                Press_Turn_Message_(field_idx);
                return;
            }
            pending_turn_message = index;
            pending_turn_message_ticks = 0;
            Page_Toward_Turn_Message_(index);
        }

        void Turn_Summary_Poll_(bool just_attached) {
            if (just_attached) {
                pending_turn_message = -1;
                return;
            }
            if (pending_turn_message < 0) {
                return;
            }
            if (++pending_turn_message_ticks >= PENDING_STAR_MAX_TICKS) {
                pending_turn_message = -1;
                return;
            }
            int16_t field_idx = Turn_Message_Field_(pending_turn_message);
            if (field_idx > 0) {
                pending_turn_message = -1;
                Press_Turn_Message_(field_idx);
            } else if (TURNSUM::g_turn_sum_header.cur_page != pending_turn_message_page) {
                Page_Toward_Turn_Message_(pending_turn_message);
            }
        }

        // Every message, not just the page on screen: a sighted player pages through them all.
        void Turn_Summary_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            const s_list_state* list = &TURNSUM::g_turn_sum_header;
            for (int16_t i = 0; i < list->num_items; ++i) {
                uint8_t msg_idx = list->items[i].group_id;
                if (msg_idx > 0xFA) {
                    continue;
                }
                char text[256];
                MSG_::sprintf_msg(text, sizeof(text), &MSG_::msgs[msg_idx]);
                std::string key = "turn_summary/message/" + std::to_string(i);
                graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ text::Normalize_Game_Text(text) });
                vtable.Type = &button_type;
                vtable.OnActivate = [i]() { Activate_Turn_Message_(i); };
                int16_t field_idx = Turn_Message_Field_(i);
                if (field_idx > 0) {
                    context->node_fields[key] = field_idx;
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
            if (Is_Valid_Field_(TURNSUM::shown_ok_field)) {
                Add_Field_Item_(builder, context, "turn_summary/ok", TURNSUM::shown_ok_field, vocab::Get(vocab::VOCAB_OK));
            }
        }

        // ---- Leader for hire ----------------------------------------------------------------------

        // MAINPUPS::Random_New_Officer_Popup offers a leader for hire, at the start of a turn or
        // from the leaders screen. Its fields are two buttons, a help field per skill, and after a
        // choice in the tutorial a full-screen field that closes it.
        bool Officer_Offer_Is_Active_() {
            return MAINPUPS::officer_popup_awaiting_input != 0 && MAINPUPS::g_officer >= 0;
        }

        std::string Officer_Offer_Instance_Key_() {
            return "officer_offer/" + std::to_string(MAINPUPS::g_officer) + "/" + std::to_string(MAINPUPS::field_state);
        }

        // The title Draw_Random_New_Officer_Popup prints across the top.
        std::string Officer_Offer_Name_() {
            return text::Normalize_Game_Text(OFFICER::Leader_Name(MAINPUPS::g_officer, 1));
        }

        // The paragraph Draw_Random_New_Officer_Popup prints under the skills: the offer, or what
        // the leader does after the choice.
        std::string Officer_Offer_Message_() {
            int16_t officer = MAINPUPS::g_officer;
            int16_t player = MAINPUPS::g_player_n;
            int16_t state = MAINPUPS::field_state;
            int32_t cost = OFFICER::Calculate_Officer_Cost(player, officer);
            char name[100] = {};
            strlcpy(name, OFFICER::Leader_Name(officer, 0), sizeof(name));
            char message[500] = {};
            if (MAINPUPS::on_officer_screen_flag != 0 || state == 0 || state == 2) {
                char technology[500] = {};
                MAINPUPS::Leader_Technology_String(player, officer, technology, sizeof(technology));
                const char* format = MAINPUPS::on_officer_screen_flag != 0 ? HAROLD::H_Message(HESTR_125_S_WILL_COST_D_BCS_INIT)
                    : state == 2                                            ? ESTRINGS::E_Strings(5)
                                                                            : ESTRINGS::E_Strings(2);
                snprintf(message, sizeof(message), format, name, cost, (int)OFFICER::Officer_Maintenance(player, officer), technology);
                if (technology[0] != '\0' && strstr(message, technology) == nullptr) {
                    strlcat(message, "\n", sizeof(message));
                    strlcat(message, technology, sizeof(message));
                }
            } else if (state == 1) {
                snprintf(message, sizeof(message), ESTRINGS::E_Strings(6), name, cost);
            } else if (state == 3) {
                snprintf(message, sizeof(message), ESTRINGS::E_Strings(4), name, cost);
            }
            return text::Normalize_Game_Text(message);
        }

        // The help field OFFICER::Add_Skill_Description_Help_Fields made for a skill; a right click
        // on it shows the skill's description.
        int16_t Officer_Skill_Help_Field_(int16_t skill_idx) {
            const s_officer_skill_help* help = &OFFICER::officer_skill_help[0];
            for (int16_t i = 0; i < 5; ++i) {
                if (help->skill_id[i] == skill_idx && Is_Valid_Field_(help->skill_help_x[i])) {
                    return help->skill_help_x[i];
                }
            }
            return 0;
        }

        void Add_Officer_Skill_(graph::GraphBuilder* builder, s_build_context* context, const std::string& prefix, int16_t officer,
            int16_t skill_idx, bool has_help) {
            char value[80];
            snprintf(value, sizeof(value), MOX::skill_data[skill_idx].format_str,
                (int32_t)(int16_t)OFFICER::Officer_Skill_Bonus(MOX::PLAYER_NUM, officer, skill_idx));
            std::string key = prefix + "/skill/" + std::to_string(skill_idx);
            graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{
                text::Normalize_Game_Text(MOX::skill_data[skill_idx].name), text::Normalize_Game_Text(value) });
            int16_t field_idx = has_help ? Officer_Skill_Help_Field_(skill_idx) : 0;
            if (field_idx > 0) {
                vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
                context->node_fields[key] = field_idx;
            }
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        // OFFICER::Print_Officer_Data's order: the leader's special skills, then general ones.
        void Add_Officer_Skills_(graph::GraphBuilder* builder, s_build_context* context, const std::string& prefix, int16_t officer,
            bool has_help) {
            uint16_t type = MOX::leaders[officer].type;
            int32_t special_type = type == LEADER_TYPE_SHIP ? 1 : type == LEADER_TYPE_COLONY ? 2 : -1;
            for (int16_t skill_idx = 0; skill_idx < 0x36; ++skill_idx) {
                if ((int32_t)MOX::skill_data[skill_idx].skill_type == special_type &&
                    OFFICER::Officer_Has_Special_Skill(MOX::PLAYER_NUM, officer, skill_idx)) {
                    Add_Officer_Skill_(builder, context, prefix, officer, skill_idx, has_help);
                }
            }
            for (int16_t skill_idx = 0; skill_idx < 0x36; ++skill_idx) {
                if (MOX::skill_data[skill_idx].skill_type == 0 && OFFICER::Officer_Has_General_Skill(MOX::PLAYER_NUM, officer, skill_idx)) {
                    Add_Officer_Skill_(builder, context, prefix, officer, skill_idx, has_help);
                }
            }
        }

        void Officer_Offer_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t officer = MAINPUPS::g_officer;
            std::string message = Officer_Offer_Message_();
            if (!message.empty()) {
                builder->AddItem(graph::ControlId::Structural("officer_offer/message"), Info_Vtable_(std::vector<std::string>{ message }));
            }

            Add_Officer_Skills_(builder, context, "officer_offer", officer, true);

            // The buttons carry only one-letter hotkeys. Before the choice they are Hire and
            // Reject; after it, any press closes the popup.
            int16_t hire = MAINPUPS::mp_fields[7];
            int16_t reject = MAINPUPS::mp_fields[8];
            std::string start;
            if (MAINPUPS::field_state == 0) {
                if (Is_Valid_Field_(hire)) {
                    Add_Field_Item_(builder, context, "officer_offer/hire", hire, vocab::Get(vocab::VOCAB_OFFICER_HIRE));
                    start = "officer_offer/hire";
                }
                if (Is_Valid_Field_(reject)) {
                    Add_Field_Item_(builder, context, "officer_offer/reject", reject, vocab::Get(vocab::VOCAB_OFFICER_REJECT));
                }
            } else if (Is_Valid_Field_(reject)) {
                Add_Field_Item_(builder, context, "officer_offer/ok", reject, vocab::Get(vocab::VOCAB_OK));
                start = "officer_offer/ok";
            }
            if (!message.empty()) {
                start = "officer_offer/message";
            }
            if (!start.empty()) {
                builder->SetStart(graph::ControlId::Structural(start));
            }
        }

        // ---- Science room --------------------------------------------------------------------------

        // SCIENCE::Science_Room presents new technology (researched, stolen, captured, or found)
        // one discovery at a time. A click shows the next one and closes after the last; Escape,
        // its hotkey, closes it at once.
        bool Science_Room_Is_Active_() {
            return SCIENCE::shown_footer != nullptr;
        }

        std::string Science_Room_Instance_Key_() {
            return std::string("science/") + SCIENCE::shown_footer;
        }

        std::string Science_Room_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_DISCOVERY);
        }

        // One string per line or paragraph break, so long text can be read a piece at a time.
        std::vector<std::string> Game_Text_Lines_(const char* raw_text) {
            std::vector<std::string> lines;
            std::string raw = raw_text != nullptr ? raw_text : "";
            size_t start = 0;
            while (start <= raw.size()) {
                size_t end = raw.find_first_of("\r\n", start);
                if (end == std::string::npos) {
                    end = raw.size();
                }
                std::string line = text::Normalize_Game_Text(raw.substr(start, end - start).c_str());
                start = end + 1;
                if (!line.empty()) {
                    lines.push_back(line);
                }
            }
            return lines;
        }

        const char* const SCIENCE_ENTRY_STOP = "science/entry";

        void Science_Room_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t click = 0;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                if (Is_Catch_All_(&fields::fields[i])) {
                    click = i;
                    break;
                }
            }
            auto add_line = [builder, click](const std::string& key, const std::string& text) {
                graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ text });
                if (click > 0) {
                    vtable.OnActivate = [click]() { navigator::Request_Field_Activation(click); };
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            };

            // Keys carry the discovery's title, so the next one starts at its title.
            std::string title = SCIENCE::shown_title != nullptr ? text::Normalize_Game_Text(SCIENCE::shown_title) : std::string();
            std::string base = "science/" + title + "/";
            std::string footer = text::Normalize_Game_Text(SCIENCE::shown_footer);

            builder->BeginStop(SCIENCE_ENTRY_STOP);
            if (SCIENCE::shown_title == nullptr) {
                add_line(base + "footer", footer);
            } else {
                add_line(base + "title", title);
                if (SCIENCE::shown_note != nullptr) {
                    add_line(base + "note", text::Normalize_Game_Text(SCIENCE::shown_note));
                }
                std::vector<std::string> body = Game_Text_Lines_(SCIENCE::shown_body);
                for (size_t i = 0; i < body.size(); ++i) {
                    add_line(base + "body/" + std::to_string(i), body[i]);
                }
            }
            if (click > 0) {
                Add_Field_Item_(builder, context, base + "continue", click, vocab::Get(vocab::VOCAB_CONTINUE));
            }

            if (SCIENCE::shown_title != nullptr && !footer.empty()) {
                builder->BeginStop("science/footer");
                add_line("science/footer", footer);
            }
        }

        // The next discovery replaces the text without any input focus can see, so the poll
        // moves focus to its title.
        std::string polled_science_title;

        void Science_Room_Poll_(bool just_attached) {
            std::string title = SCIENCE::shown_title != nullptr ? std::string(SCIENCE::shown_title) : std::string();
            if (!just_attached && !title.empty() && title != polled_science_title) {
                navigator::Request_Focus_Stop(SCIENCE_ENTRY_STOP);
            }
            polled_science_title = title;
        }

        // ---- Colony screen -----------------------------------------------------------------------

        // The job rows' geometry, from COLONY::Add_Job_Field_For.
        constexpr int16_t COLONY_JOB_LEFT_X = 310;
        constexpr int16_t COLONY_JOB_RIGHT_X = 510;
        constexpr int16_t COLONY_JOB_TOP_Y = 62;
        constexpr int16_t COLONY_JOB_ROW_HEIGHT = 30;
        constexpr int16_t COLONY_POP_SPACING = 30;

        bool Colony_Is_Active_() {
            return MOX::current_screen == SCREEN_COLONY && COLONY::awaiting_input != 0 && COLONY::valid_colony != 0 &&
                COLONY::colony_handle >= 0 && COLONY::colony_handle < MOX::NUM_COLONIES;
        }

        std::string Colony_Instance_Key_() {
            return "colony/" + std::to_string(COLONY::colony_handle);
        }

        const s_colony* Shown_Colony_() {
            return &MOX::colony[COLONY::colony_handle];
        }

        // The title Draw_Info_Name_And_Pop prints across the top of the screen.
        std::string Colony_Name_() {
            char planet[64] = {};
            HACCESS::Do_Get_Planet_Name(COLONY::planet_handle, planet, 1);
            char title[160];
            if (Shown_Colony_()->occupation_policy == 0) {
                snprintf(title, sizeof(title), ESTRINGS::E_Strings(ESTR_ANNIHILATING_S), planet);
            } else {
                snprintf(title, sizeof(title), ESTRINGS::E_Strings(ESTR_SCOLONY_OF_S), COLONY::Colony_Type(COLONY::colony_handle), planet);
            }
            return text::Normalize_Game_Text(title);
        }

        // Counted as Sum_Colonists counts them, without its global tally: colonists picked up to be
        // moved are not counted until they are placed.
        std::string Colony_Population_Text_() {
            const s_colony* colony = Shown_Colony_();
            int32_t employed = 0;
            for (int16_t i = 0; i < (int16_t)colony->n_pops; ++i) {
                if ((colony->pop[i] & 0x200) != 0) {
                    ++employed;
                }
            }
            int32_t roundoff = 0;
            int32_t growth = 0;
            for (int32_t i = 0; i < 10; ++i) {
                roundoff += colony->pop_roundoff[i];
                growth += colony->pop_growth[i];
            }
            char line[104];
            snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_POP_D03D_K_DK), employed + roundoff / 1000, roundoff % 1000, growth);
            return text::Normalize_Game_Text(line);
        }

        // The flashing notice Draw_Info_Name_And_Pop prints in the corner.
        std::string Colony_Status_Text_() {
            int16_t star_idx = MOX::planet[Shown_Colony_()->planet].star_index;
            if ((MOX::star[star_idx].blockaded >> (MOX::PLAYER_NUM & 0x1F)) & 1) {
                return text::Normalize_Game_Text(ESTRINGS::E_Strings(ESTR_BLOCKADED1));
            }
            if (EVENTS::Event_Check_Plague(COLONY::colony_handle)) {
                return text::Normalize_Game_Text(ESTRINGS::E_Strings(ESTR_PLAGUE));
            }
            if (EVENTS::Event_Check_Population_Boom(COLONY::colony_handle)) {
                return text::Normalize_Game_Text(ESTRINGS::E_Strings(ESTR_POP_BOOM));
            }
            return std::string();
        }

        // COLDRAW::Draw_Info_Morale_Both draws one face per two points of morale, happy or
        // unhappy, and none under the governments that ignore morale.
        std::string Colony_Morale_Text_() {
            const s_colony* colony = Shown_Colony_();
            int32_t faces = (int8_t)colony->morale / 2;
            if (MOX::player[(int8_t)colony->owner].traits[TRAIT_CURRENT_GOVERNMENT] >= GOVERNMENT_UNIFICATION) {
                faces = 0;
            }
            faces = std::clamp(faces, -20, 20);
            return vocab::Format(vocab::VOCAB_COLONY_MORALE, faces > 0 ? "+" + std::to_string(faces) : std::to_string(faces));
        }

        // The icons COLDRAW::Draw_Colony_Prod_Both draws in one production row, as numbers.
        std::string Colony_Resource_Text_(int32_t type) {
            const s_colony* colony = Shown_Colony_();
            int32_t imports = colony->imports[type];
            int32_t traded = type != ECON_INDUSTRY ? abs(imports) : 0;
            int32_t secondary = 0;
            if (type == ECON_FOOD) {
                secondary = abs((int32_t)colony->imports[ECON_INDUSTRY]);
            } else if (type == ECON_INDUSTRY) {
                secondary = colony->pollution;
            }
            int32_t production = colony->production[type];
            int32_t maintenance = colony->maintenance[type];
            int32_t shortage = std::max(0, maintenance - imports - production);

            // The income row's two groups split production into upkeep and surplus
            // (COLCALC sets imports[ECON_BC] to upkeep minus production), so it is read whole.
            if (type == ECON_BC) {
                std::string line = vocab::Format(vocab::VOCAB_COLONY_INCOME, std::to_string(production));
                if (maintenance > 0) {
                    line += ", " + vocab::Format(vocab::VOCAB_COLONY_UPKEEP, std::to_string(maintenance));
                }
                return line;
            }

            // An exporting row draws what stays, a gap, then what leaves; together they are the
            // whole production, as the breakdown totals it.
            int32_t net = production;
            if (type == ECON_INDUSTRY && colony->maintenance[ECON_INDUSTRY] != 0) {
                net = std::max(0, production - maintenance);
            }

            vocab::e_vocab_string name = vocab::VOCAB_COLONY_INCOME;
            switch (type) {
                case ECON_FOOD: name = vocab::VOCAB_COLONY_FOOD; break;
                case ECON_INDUSTRY: name = vocab::VOCAB_COLONY_INDUSTRY; break;
                case ECON_RESEARCH: name = vocab::VOCAB_COLONY_RESEARCH; break;
                default: break;
            }
            std::string line = vocab::Format(name, std::to_string(std::max(0, net)));
            if (secondary > 0) {
                line += ", " + vocab::Format(type == ECON_INDUSTRY ? vocab::VOCAB_COLONY_POLLUTION : vocab::VOCAB_COLONY_INDUSTRY_USED,
                    std::to_string(secondary));
            }
            bool exported = imports < 0 || type == ECON_INDUSTRY;
            if (traded > 0) {
                line += ", " + vocab::Format(exported ? vocab::VOCAB_COLONY_EXPORTED : vocab::VOCAB_COLONY_IMPORTED, std::to_string(traded));
            }
            if (!exported && shortage > 0) {
                line += ", " + vocab::Format(vocab::VOCAB_COLONY_SHORT, std::to_string(shortage));
            }
            return line;
        }

        // The order COLDRAW::Do_Colony_Info_Pop_Stuff_For_Pop lays a job row out in, which is also
        // the slot order its hit-test counts. Colonists picked up to be moved are not in any row.
        std::vector<int16_t> Colony_Job_Pops_(int16_t job) {
            static const int16_t race_order[10] = { 9, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
            const s_colony* colony = Shown_Colony_();
            std::vector<int16_t> pops;
            for (int16_t state = 0; state < 7; ++state) {
                for (uint32_t conquered = 0; conquered < 2; ++conquered) {
                    for (int16_t race : race_order) {
                        for (int16_t i = 0; i < (int16_t)colony->n_pops; ++i) {
                            uint32_t pop = colony->pop[i];
                            if ((pop & 0xF) == (uint32_t)race && COLONY::Pop_To_Pop_State(COLONY::colony_handle, i) == state &&
                                ((pop & 0x180) >> 7) == (uint32_t)job && (pop & 0x200) != 0 && ((pop & 0x400) >> 10) == conquered) {
                                pops.push_back(i);
                            }
                        }
                    }
                }
            }
            return pops;
        }

        // COLDRAW::Calculate_Squish_Step for a job row: the distance between colonists. The farmer
        // row also leaves room for the shadow farmers it draws up to the farm limit.
        int16_t Colony_Pop_Step_(int16_t job, int16_t count) {
            int16_t icons = count;
            if (job == ECON_FOOD && Shown_Colony_()->max_farms != 0xFF) {
                icons = Shown_Colony_()->max_farms;
            }
            if (icons <= 1) {
                icons = 1;
            }
            int16_t available = (int16_t)(COLONY_POP_SPACING / -3 - COLONY_JOB_LEFT_X + COLONY_JOB_RIGHT_X);
            int16_t step = (int16_t)(available / icons);
            if (step <= 1) {
                step = 1;
            }
            return step < COLONY_POP_SPACING ? step : COLONY_POP_SPACING;
        }

        // A point inside the colonist's slot: Get_Selected_Pop takes the first slot whose right
        // edge is at or past the press.
        s_pointer_target Colony_Pop_Point_(int16_t job, int16_t slot, int16_t count) {
            int16_t step = Colony_Pop_Step_(job, count);
            return s_pointer_target{ (int16_t)(COLONY_JOB_LEFT_X + step * slot + (step + 1) / 2),
                (int16_t)(COLONY_JOB_TOP_Y + job * COLONY_JOB_ROW_HEIGHT + COLONY_JOB_ROW_HEIGHT / 2) };
        }

        std::string Colony_Pop_Text_(int16_t colony_idx, int16_t pop_idx) {
            char text[64] = {};
            COLMOVE::Colony_Pop_String(colony_idx, pop_idx, text, sizeof(text));
            return text::Normalize_Game_Text(text);
        }

        bool Colony_Holding_() {
            return COLONY::field_mode == 1 && COLMOVE::cluster_colony_n >= 0 &&
                COLMOVE::Get_First_Unemployed(COLMOVE::cluster_colony_n) != -1;
        }

        std::string Colony_Holding_Text_() {
            int16_t colony_idx = COLMOVE::cluster_colony_n;
            return vocab::Format(vocab::VOCAB_COLONY_HOLDING, std::to_string(COLMOVE::Get_Num_Unemployed(colony_idx)),
                Colony_Pop_Text_(colony_idx, COLMOVE::Get_First_Unemployed(colony_idx)));
        }

        std::string Colony_Job_Value_(int16_t job) {
            size_t count = Colony_Job_Pops_(job).size();
            std::string value = count == 1 ? std::string(vocab::Get(vocab::VOCAB_COLONY_JOB_COUNT_ONE))
                                           : vocab::Format(vocab::VOCAB_COLONY_JOB_COUNT, std::to_string(count));
            value += ", " + Colony_Resource_Text_(job);
            if (job == ECON_FOOD && Shown_Colony_()->max_farms == 0) {
                value += ", " + text::Normalize_Game_Text(ESTRINGS::E_Strings(ESTR_NO_FARMING));
            }
            return value;
        }

        // While colonists are held the job rows are plain drop targets; otherwise each row is a
        // scroll field, and a press picks up the colonist under the pointer together with the
        // identical colonists after it (COLONY::Check_Job_Fields). A slot of -1 is the last one.
        void Colony_Press_Job_(int16_t job, int16_t slot) {
            int16_t field_idx = COLONY::job_fields[job];
            if (!Is_Valid_Field_(field_idx)) {
                return;
            }
            if (fields::fields[field_idx].field_type == FIELD_TYPE_SCROLL) {
                std::vector<int16_t> pops = Colony_Job_Pops_(job);
                if (slot < 0) {
                    slot = (int16_t)(pops.size() - 1);
                }
                if (slot < 0 || slot >= (int16_t)pops.size()) {
                    return;
                }
                s_pointer_target point = Colony_Pop_Point_(job, slot, (int16_t)pops.size());
                platform::Force_Cursor_Pos(point.x, point.y);
            }
            navigator::Request_Field_Activation(field_idx);
        }

        // What Enter on a job row says once the game has acted. Picking up is spoken by the poll,
        // which also covers a colonist that left the row with focus on it.
        std::string Colony_Job_State_Text_(int16_t job) {
            return Colony_Holding_() ? std::string() : Colony_Job_Value_(job);
        }

        void Build_Colony_Jobs_(graph::GraphBuilder* builder, s_build_context* context) {
            const vocab::e_vocab_string job_names[3] = {
                vocab::VOCAB_COLONY_FARMERS, vocab::VOCAB_COLONY_WORKERS, vocab::VOCAB_COLONY_SCIENTISTS,
            };
            for (int16_t job = 0; job < 3; ++job) {
                std::vector<int16_t> pops = Colony_Job_Pops_(job);
                builder->StartRow();

                std::string header_key = "colony/job/" + std::to_string(job);
                graph::NodeVtable header;
                header.Type = &button_type;
                header.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(job_names[job])));
                header.Announcements.back().Kind = Label;
                header.Announcements.push_back(graph::NodeAnnouncement([job]() { return Colony_Job_Value_(job); }, true, Value));
                header.OnActivate = [job]() { Colony_Press_Job_(job, -1); };
                header.StateText = [job]() { return Colony_Job_State_Text_(job); };
                if (Is_Valid_Field_(COLONY::job_fields[job])) {
                    int16_t field_idx = COLONY::job_fields[job];
                    header.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };
                }
                if (!pops.empty()) {
                    context->node_points[header_key] = Colony_Pop_Point_(job, (int16_t)(pops.size() - 1), (int16_t)pops.size());
                }
                builder->AddItem(graph::ControlId::Structural(header_key), header);

                for (int16_t slot = 0; slot < (int16_t)pops.size(); ++slot) {
                    std::string key = header_key + "/" + std::to_string(slot);
                    graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ Colony_Pop_Text_(COLONY::colony_handle, pops[slot]) });
                    vtable.Type = &button_type;
                    vtable.OnActivate = [job, slot]() { Colony_Press_Job_(job, slot); };
                    vtable.StateText = [job]() { return Colony_Job_State_Text_(job); };
                    context->node_points[key] = Colony_Pop_Point_(job, slot, (int16_t)pops.size());
                    builder->AddItem(graph::ControlId::Structural(key), vtable);
                }
                builder->EndRow();
            }

            // Shown only while colonists are held: sends them to a colony in another system.
            int16_t transport = COLONY::colony_fields[11];
            if (Is_Valid_Field_(transport)) {
                Add_Field_Item_(builder, context, "colony/transport", transport, Help_Label_Or_(transport, vocab::VOCAB_COLONY_TRANSPORT));
            }
        }

        // A hidden field that opens a breakdown, read as the line it stands for.
        void Add_Colony_Readout_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            int16_t field_idx, std::function<std::string()> text) {
            graph::NodeVtable vtable;
            vtable.Type = &button_type;
            vtable.Announcements.push_back(graph::NodeAnnouncement(text, true, Label));
            if (Is_Valid_Field_(field_idx)) {
                vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
                vtable.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };
                context->node_fields[key] = field_idx;
            }
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        void Build_Colony_Summary_(graph::GraphBuilder* builder, s_build_context* context) {
            builder->AddItem(graph::ControlId::Structural("colony/title"), Info_Vtable_(std::vector<std::string>{ Colony_Name_() }));
            builder->AddItem(graph::ControlId::Structural("colony/population"),
                Info_Vtable_(std::vector<std::string>{ Colony_Population_Text_() }));
            std::string status = Colony_Status_Text_();
            if (!status.empty()) {
                builder->AddItem(graph::ControlId::Structural("colony/status"), Info_Vtable_(std::vector<std::string>{ status }));
            }
            Add_Colony_Readout_(builder, context, "colony/morale", COLONY::colony_fields[12], []() { return Colony_Morale_Text_(); });
            Add_Colony_Readout_(builder, context, "colony/income", COLONY::colony_fields[13], []() { return Colony_Resource_Text_(ECON_BC); });
            Add_Colony_Readout_(builder, context, "colony/food", COLONY::colony_fields[14], []() { return Colony_Resource_Text_(ECON_FOOD); });
            Add_Colony_Readout_(builder, context, "colony/industry", COLONY::colony_fields[15],
                []() { return Colony_Resource_Text_(ECON_INDUSTRY); });
            Add_Colony_Readout_(builder, context, "colony/research", COLONY::colony_fields[16],
                []() { return Colony_Resource_Text_(ECON_RESEARCH); });
        }

        // The rows Draw_Turn_Count prints under the production picture, from the mod-adjustable
        // TEXT_FORMAT templates.
        std::string Colony_Turns_Text_() {
            const s_colony* colony = Shown_Colony_();
            int32_t turns = COLONY::Calculate_Current_Production_Turn_Count(COLONY::colony_handle);
            int32_t cost = COLCALC::Colony_Producing_Product_Cost(const_cast<s_colony*>(colony));
            int32_t total = (int32_t)colony->production_spent + colony->bought_outright;
            const STRBUILD::s_named_argument arguments[] = {
                STRBUILD::Named_Argument("TURNS", turns),
                STRBUILD::Named_Argument("STORED_PROD", colony->production_spent),
                STRBUILD::Named_Argument("BOUGHT_PROD", colony->bought_outright),
                STRBUILD::Named_Argument("TOTAL_PROD", total),
                STRBUILD::Named_Argument("PRODUCT_COST", cost),
            };
            std::string text;
            const TEXT_FORMAT::e_row rows[] = { TEXT_FORMAT::ROW_PROD, TEXT_FORMAT::ROW_PROD2 };
            for (TEXT_FORMAT::e_row row : rows) {
                if (TEXT_FORMAT::format[row].template_text[0] == '\0') {
                    continue;
                }
                char line[512];
                STRBUILD::Named_Snprintf(line, sizeof(line), TEXT_FORMAT::format[row].template_text, arguments,
                    (uint32_t)(sizeof(arguments) / sizeof(arguments[0])));
                std::string normalized = text::Normalize_Game_Text(line);
                if (!normalized.empty()) {
                    text += (text.empty() ? "" : ", ") + normalized;
                }
            }
            return text;
        }

        std::string Colony_Producing_Text_() {
            char name[100] = {};
            strlcpy(name, COLBLDG::Selection_Name(Shown_Colony_()->producing[0]), sizeof(name));
            return text::Normalize_Game_Text(name);
        }

        // The auto build line Draw_Info_Build prints above the production picture while it is on.
        std::string Colony_Auto_Build_Text_() {
            const s_colony* colony = Shown_Colony_();
            int preset = BUILD_QUEUE::autobuild_settings.enabled != 0 ? BUILD_QUEUE::Decode_Autobuild_Preset(colony->auto_building) : -1;
            if (preset >= 0) {
                char line[40];
                snprintf(line, sizeof(line), "Auto Build Queue %d", preset);
                return line;
            }
            if (BUILD_QUEUE::Is_Legacy_Autobuild(colony->auto_building)) {
                return text::Normalize_Game_Text(ESTRINGS::E_Strings(0xbd));
            }
            return vocab::Get(vocab::VOCAB_STATE_OFF);
        }

        void Build_Colony_Production_(graph::GraphBuilder* builder, s_build_context* context) {
            graph::NodeVtable item;
            item.Announcements.push_back(graph::NodeAnnouncement([]() { return Colony_Producing_Text_(); }, true, Label));
            item.Announcements.push_back(graph::NodeAnnouncement([]() { return Colony_Turns_Text_(); }, true, Value));
            builder->AddItem(graph::ControlId::Structural("colony/producing"), item);

            int16_t change = COLONY::colony_fields[4];
            if (Is_Valid_Field_(change)) {
                Add_Field_Item_(builder, context, "colony/change", change, Help_Label_Or_(change, vocab::VOCAB_COLONY_CHANGE));
            }
            int16_t buy = COLONY::colony_fields[3];
            if (Is_Valid_Field_(buy)) {
                Add_Field_Item_(builder, context, "colony/buy", buy, Help_Label_Or_(buy, vocab::VOCAB_COLONY_BUY));
            }
            int16_t auto_build = COLONY::colony_fields[18];
            if (Is_Valid_Field_(auto_build)) {
                // Its help record is the production box's ("Current Production"), not the toggle's.
                graph::NodeVtable vtable = Field_Vtable_(auto_build, vocab::Get(vocab::VOCAB_COLONY_AUTO_BUILD));
                vtable.Type = &toggle_type;
                vtable.Announcements.push_back(graph::NodeAnnouncement([]() { return Colony_Auto_Build_Text_(); }, true, Value));
                context->node_fields["colony/auto_build"] = auto_build;
                builder->AddItem(graph::ControlId::Structural("colony/auto_build"), vtable);
            }
        }

        // The left panel, COLSYSDI::Draw_Col_Sys_Disp: one summary line per planet in the system.
        // A press on a planet opens that colony, or places held colonists there; a press on its
        // summary opens the planet's description (COLONY::Check_System_Display_Fields).
        void Build_Colony_Planets_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t star_idx = COLONY::sys_disp_star_handle;
            if (star_idx < 0 || star_idx >= MOX::NUM_STARS) {
                return;
            }
            for (int16_t i = 0; i < 5; ++i) {
                int16_t planet_idx = MOX::star[star_idx].planet_index[i];
                int16_t field_idx = COLONY::sys_disp_planet_fields[i];
                if (planet_idx < 0 || !Is_Valid_Field_(field_idx)) {
                    continue;
                }
                std::string key = "colony/planet/" + std::to_string(planet_idx);
                graph::NodeVtable vtable;
                vtable.Type = &button_type;
                vtable.Announcements.push_back(graph::NodeAnnouncement([planet_idx]() {
                    char summary[500];
                    COLSYSDI::Planet_Summary_String(summary, sizeof(summary), planet_idx, -1);
                    return text::Normalize_Game_Text(summary);
                }, true, Label));
                if (planet_idx == COLONY::planet_handle) {
                    vtable.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(vocab::VOCAB_STATE_CURRENT)));
                    vtable.Announcements.back().Kind = Selected;
                }
                vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
                int16_t summary_field = COLONY::sys_disp_summ_fields[i];
                if (MOX::planet[planet_idx].planet_type == PLANET_TYPE_PLANET && Is_Valid_Field_(summary_field)) {
                    vtable.OnSecondary = [summary_field]() { navigator::Request_Field_Activation(summary_field); };
                }
                vtable.OnTooltip = [field_idx]() { Speak_Field_Help_(field_idx); };
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        // A press scraps a building and a right click describes it (COLONY::Check_Bldg_Fields);
        // the starbases and gates drawn among them do neither, and the capitol can't be scrapped.
        bool Building_Is_Inert_(int32_t building_id) {
            return building_id == BUILDING_ARTEMIS_SYSTEM_NET || building_id == BUILDING_DIMENSIONAL_PORTAL ||
                building_id == BUILDING_STAR_BASE || building_id == BUILDING_STAR_FORTRESS || building_id == BUILDING_BATTLESTATION;
        }

        void Add_Colony_Building_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            int16_t field_idx, const char* name, bool can_scrap, bool can_describe) {
            graph::NodeVtable vtable = Info_Vtable_(std::vector<std::string>{ text::Normalize_Game_Text(name) });
            vtable.Type = &button_type;
            if (can_scrap) {
                vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
            }
            if (can_describe) {
                vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
            }
            context->node_fields[key] = field_idx;
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        void Build_Colony_Buildings_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int16_t row = 0; row < 6; ++row) {
                for (int16_t col = 0; col < 6; ++col) {
                    int32_t building_id = COLONY::colony_bldgs[row].buildings[col];
                    int16_t field_idx = COLONY::colony_bldg_fields[row * 6 + col];
                    if (building_id == 0 || !Is_Valid_Field_(field_idx)) {
                        continue;
                    }
                    bool inert = Building_Is_Inert_(building_id);
                    Add_Colony_Building_(builder, context, "colony/building/" + std::to_string(row * 6 + col), field_idx,
                        COLONY::CR_Building_Name((int16_t)building_id), !inert && building_id != BUILDING_CAPITOL, !inert);
                }
            }
            for (int16_t i = 0; i < 10; ++i) {
                int16_t building_id = COLONY::colony_satellites[i];
                int16_t field_idx = COLONY::colony_satellite_fields[i];
                if (building_id < 0 || !Is_Valid_Field_(field_idx)) {
                    continue;
                }
                Add_Colony_Building_(builder, context, "colony/satellite/" + std::to_string(building_id), field_idx,
                    COLONY::Real_Building_Name2(building_id), true, true);
            }
            // Print_Scanned_Military's line for each troop type; a press opens the troop popup.
            for (int16_t i = 0; i < 2; ++i) {
                int16_t field_idx = COLONY::military_fields[i];
                if (!Is_Valid_Field_(field_idx)) {
                    continue;
                }
                char line[200];
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_D_S), (int)Shown_Colony_()->military[i], TECHDATA::units[i + 1].name);
                Add_Field_Item_(builder, context, "colony/military/" + std::to_string(i), field_idx, text::Normalize_Game_Text(line));
            }
        }

        void Build_Colony_Controls_(graph::GraphBuilder* builder, s_build_context* context) {
            const struct {
                int32_t slot;
                const char* key;
                vocab::e_vocab_string fallback;
            } controls[] = {
                { 17, "colony/officer", vocab::VOCAB_COLONY_OFFICER },
                { 6, "colony/previous", vocab::VOCAB_COLONY_PREVIOUS },
                { 7, "colony/next", vocab::VOCAB_COLONY_NEXT },
                { 1, "colony/return", vocab::VOCAB_COLONY_RETURN },
            };
            for (const auto& control : controls) {
                int16_t field_idx = COLONY::colony_fields[control.slot];
                if (Is_Valid_Field_(field_idx)) {
                    std::string label = Is_Offscreen_(&fields::fields[field_idx]) ? std::string(vocab::Get(control.fallback))
                                                                                   : Help_Label_Or_(field_idx, control.fallback);
                    Add_Field_Item_(builder, context, control.key, field_idx, label);
                }
            }
        }

        void Colony_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            builder->BeginStop("colony/population");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_POPULATION));
            Build_Colony_Jobs_(builder, context);
            builder->PopContext();

            builder->BeginStop("colony/summary");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_SUMMARY));
            Build_Colony_Summary_(builder, context);
            builder->PopContext();

            builder->BeginStop("colony/production");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_PRODUCTION));
            Build_Colony_Production_(builder, context);
            builder->PopContext();

            builder->BeginStop("colony/planets");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_PLANETS));
            Build_Colony_Planets_(builder, context);
            builder->PopContext();

            builder->BeginStop("colony/buildings");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_BUILDINGS));
            Build_Colony_Buildings_(builder, context);
            builder->PopContext();

            builder->BeginStop("colony/controls");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_CONTROLS));
            Build_Colony_Controls_(builder, context);
            builder->PopContext();

            builder->SetStart(graph::ControlId::Structural("colony/job/0"));
        }

        // Picking colonists up removes them from their row, which can take focus with them, so
        // what is now held is spoken here rather than as the focused node's new state.
        int16_t polled_held = 0;

        void Colony_Poll_(bool just_attached) {
            int16_t held = Colony_Holding_() ? COLMOVE::Get_Num_Unemployed(COLMOVE::cluster_colony_n) : 0;
            if (!just_attached && held > 0 && held != polled_held) {
                speech::Say(Colony_Holding_Text_(), false);
            }
            polled_held = held;
        }

        // ---- Build queue ---------------------------------------------------------------------------

        // The popup's own loop only: refit, auto build, and message popups run nested loops from
        // Evaluate_Input with the screen unchanged.
        bool Build_Is_Active_() {
            return MOX::current_screen == SCREEN_QUEUE_POPUP && COLBLDG::awaiting_input != 0 &&
                COLONY::colony_handle >= 0 && COLONY::colony_handle < MOX::NUM_COLONIES;
        }

        std::string Build_Instance_Key_() {
            return "build/" + std::to_string(COLONY::colony_handle);
        }

        // The title Draw_Build_Queue_Popup prints over the queue.
        std::string Build_Name_() {
            char title[200];
            snprintf(title, sizeof(title), ESTRINGS::E_Strings(ESTR_BUILD_LIST_FOR_S),
                HACCESS::Get_Planet_Name(MOX::colony[COLONY::colony_handle].planet));
            return text::Normalize_Game_Text(title);
        }

        // COLBLDG::Selection_Name without its side effects: for a refit it formats into the game's
        // message buffer and resets the description cache the detail panel draws from.
        std::string Build_Product_Name_(int16_t product) {
            if (COLBLDG::Colony_Production_Is_Queued_Ship(product)) {
                const s_ship_data* ship = &MOX::ship[COLBLDG::Colony_Production_Ship_Index(product)];
                if (ship->status != 4) {
                    return text::Normalize_Game_Text(ship->d.name);
                }
                char name[200];
                snprintf(name, sizeof(name), ESTRINGS::E_Strings(ESTR_S_S), ESTRINGS::E_Strings(ESTR_REFIT_SHIP), ship->d.name);
                return text::Normalize_Game_Text(name);
            }
            return text::Normalize_Game_Text(COLBLDG::Selection_Name(product));
        }

        void Append_Game_Line_(std::string* text, const char* line) {
            std::string normalized = text::Normalize_Game_Text(line);
            if (!normalized.empty()) {
                *text += (text->empty() ? "" : ", ") + normalized;
            }
        }

        // The lines Draw_Cost_And_Time_Info prints under the name in the detail panel, for the
        // items Draw_Current_Selection prints them for.
        std::string Build_Cost_Text_(int16_t product) {
            switch (product) {
                case COLONY_PRODUCTION_NONE:
                case COLONY_PRODUCTION_HOUSING:
                case COLONY_PRODUCTION_TRADE_GOODS:
                case COLONY_PRODUCTION_REPEAT:
                case COLONY_PRODUCTION_REFIT:
                case COLONY_PRODUCTION_SEPARATOR:
                case COLONY_PRODUCTION_CUSTOM_DESIGN:
                case COLONY_PRODUCTION_DOOM_STAR:
                    return std::string();
                default:
                    break;
            }
            int16_t colony_idx = COLONY::colony_handle;
            s_colony* colony = &MOX::colony[colony_idx];
            std::string text;
            char line[200];
            snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_COST_HERE_D), (int32_t)(int16_t)COLCALC::Colony_Product_Cost(colony, product));
            Append_Game_Line_(&text, line);
            if (COLBLDG::Colony_Production_Is_Building(product)) {
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_MAINT_COST_D), (int32_t)TECHDATA::buildings[product].maintenance);
                Append_Game_Line_(&text, line);
            }
            snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_BUILD_TIME_D),
                (int32_t)COLONY::Calculate_Colony_Turn_Count_From_Scrap_For_Prod(colony_idx, product));
            Append_Game_Line_(&text, line);
            if (colony->producing[0] == product) {
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_TURNS_LEFT_D), (int32_t)COLONY::Calculate_Current_Production_Turn_Count(colony_idx));
                Append_Game_Line_(&text, line);
            } else if (product == BUILDING_GRAVITY_GENERATOR) {
                int8_t gravity = MOX::planet[colony->planet].gravity_class;
                if (gravity >= 0 && gravity < PLANET_GRAVITY_COUNT) {
                    snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_GRAVITY_HERE_S), MOX::planet_gravity_string[gravity]);
                    Append_Game_Line_(&text, line);
                }
            }
            return text;
        }

        // The paragraph the detail panel prints below the cost lines (Draw_Current_Selection).
        std::string Build_Description_(int16_t product) {
            int16_t app_id = -1;
            switch (product) {
                case COLONY_PRODUCTION_OUTPOST_SHIP: app_id = 109; break;
                case COLONY_PRODUCTION_FREIGHTERS: app_id = 69; break;
                case COLONY_PRODUCTION_COLONY_SHIP: app_id = 41; break;
                case COLONY_PRODUCTION_TRANSPORT_SHIP: app_id = 189; break;
                case COLONY_PRODUCTION_SPY: app_id = 166; break;
                case COLONY_PRODUCTION_SCIENTIST: app_id = 7; break;
                case COLONY_PRODUCTION_WORKER: app_id = 8; break;
                case COLONY_PRODUCTION_FARMER: app_id = 6; break;
                case COLONY_PRODUCTION_HOUSING:
                case COLONY_PRODUCTION_TRADE_GOODS: {
                    std::string text;
                    help::Read_Text_Record(ESTRINGS::E_Strings(ESTR_MAINTEXTLBX), product == COLONY_PRODUCTION_HOUSING ? 13 : 12, 0, &text);
                    return text;
                }
                default:
                    break;
            }
            s_ship_design* design = nullptr;
            if (COLBLDG::Colony_Production_Is_Building(product)) {
                app_id = (int16_t)TECHDATA::buildings[product].tech_app;
            } else if (COLBLDG::Colony_Production_Is_Queued_Ship(product)) {
                design = &MOX::ship[COLBLDG::Colony_Production_Ship_Index(product)].d;
            } else if (COLBLDG::Colony_Production_Is_Ship_Design(product)) {
                design = &MOX::player[MOX::PLAYER_NUM].ship_designs[COLBLDG::Colony_Production_Ship_Design_Index(product)];
            }
            if (design != nullptr) {
                char paragraph[20000];
                STRBUILD::s_builder builder = STRBUILD::Init(paragraph, sizeof(paragraph));
                COLBLDG::Build_Ship_Design_Info_Formatted_Paragraph(design, &builder);
                return text::Normalize_Game_Text(paragraph);
            }
            help::s_help_text help_text;
            return help::Read_Help_Text(app_id, &help_text) ? help_text.body : std::string();
        }

        // The designs Draw_Militarys_List draws dimmed: ones this colony can't build, and large
        // hulls without a starbase to build them at.
        bool Build_Ship_Is_Dim_(int16_t product) {
            if (!COLBLDG::Colony_Production_Is_Ship_Design(product) || COLBLDG::field_mode == 1) {
                return false;
            }
            s_colony* colony = &MOX::colony[COLONY::colony_handle];
            const s_ship_design* design = &MOX::player[MOX::PLAYER_NUM].ship_designs[COLBLDG::Colony_Production_Ship_Design_Index(product)];
            if (colony->buildings[BUILDING_STAR_BASE] == 0 && colony->buildings[BUILDING_STAR_FORTRESS] == 0 &&
                colony->buildings[BUILDING_BATTLESTATION] == 0 && design->size >= SHIP_SIZE_LARGE) {
                return true;
            }
            return !COLCALC::Colony_Can_Build_Product(colony, product);
        }

        int16_t Build_Held_Slot_() {
            return COLBLDG::active_prod[0] >= BUILDING_COUNT ? (int16_t)(COLBLDG::active_prod[0] - BUILDING_COUNT) : -1;
        }

        // A queue entry as the game draws it, with the Repeat slot after an item folded into it.
        struct s_build_entry {
            int16_t slot;
            int16_t product;
            bool repeating;
        };

        std::vector<s_build_entry> Build_Entries_(const int16_t* items) {
            std::vector<s_build_entry> entries;
            for (int16_t slot = 0; slot < 7; ++slot) {
                if (items[slot] == COLONY_PRODUCTION_NONE) {
                    continue;
                }
                if (items[slot] == COLONY_PRODUCTION_REPEAT) {
                    if (!entries.empty() && entries.back().slot == slot - 1) {
                        entries.back().repeating = true;
                    }
                    continue;
                }
                entries.push_back(s_build_entry{ slot, items[slot], false });
            }
            return entries;
        }

        // The queue as it was when a press was requested; the poll speaks what the press changed.
        struct s_build_snapshot {
            int16_t colony;
            int16_t items[7];
            std::string names[7];
            int16_t held_slot;
        };

        s_build_snapshot Build_Snapshot_() {
            s_build_snapshot snapshot;
            snapshot.colony = COLONY::colony_handle;
            snapshot.held_slot = Build_Held_Slot_();
            for (int16_t slot = 0; slot < 7; ++slot) {
                int16_t product = COLBLDG::current_item[slot];
                snapshot.items[slot] = product;
                if (product != COLONY_PRODUCTION_NONE && product != COLONY_PRODUCTION_REPEAT) {
                    snapshot.names[slot] = Build_Product_Name_(product);
                }
            }
            return snapshot;
        }

        enum e_build_removal {
            BUILD_REMOVAL_NONE,
            BUILD_REMOVAL_DROP,
            BUILD_REMOVAL_PICK,
        };

        s_build_snapshot build_before;
        bool build_change_pending = false;
        e_build_removal build_removal = BUILD_REMOVAL_NONE;
        int16_t build_removal_slot = -1;
        int16_t polled_build_held = -1;
        std::string polled_build_held_name;
        uint8_t polled_build_mode = 0;

        void Build_Press_(int16_t field_idx) {
            if (!Is_Valid_Field_(field_idx)) {
                return;
            }
            build_before = Build_Snapshot_();
            build_change_pending = true;
            navigator::Request_Field_Activation(field_idx);
        }

        // The game removes a queue entry with two presses, one to pick it up and one on it again;
        // anything already held, or a pending Design or Repeat choice, is let go first. The poll
        // makes each later press once the game has acted on the one before.
        void Build_Remove_(int16_t slot) {
            int16_t held = Build_Held_Slot_();
            build_removal = BUILD_REMOVAL_NONE;
            build_removal_slot = slot;
            if (held == slot && COLBLDG::field_mode == 0) {
                Build_Press_(COLBLDG::queue_fields[slot]);
            } else if (held >= 0 || COLBLDG::field_mode != 0) {
                build_removal = BUILD_REMOVAL_DROP;
                Build_Press_(COLBLDG::fields[0]);
            } else {
                build_removal = BUILD_REMOVAL_PICK;
                Build_Press_(COLBLDG::queue_fields[slot]);
            }
        }

        void Build_Advance_Removal_() {
            int16_t held = Build_Held_Slot_();
            int16_t field_idx = COLBLDG::queue_fields[build_removal_slot];
            if (build_removal == BUILD_REMOVAL_DROP && held < 0 && COLBLDG::field_mode == 0) {
                build_removal = BUILD_REMOVAL_PICK;
                navigator::Request_Field_Activation(field_idx);
                return;
            }
            // A pick-up the game refused, such as a bought item, leaves nothing held.
            if (build_removal == BUILD_REMOVAL_PICK && held == build_removal_slot) {
                navigator::Request_Field_Activation(field_idx);
            }
            build_removal = BUILD_REMOVAL_NONE;
        }

        std::string Build_Change_Text_(const s_build_snapshot& before) {
            std::vector<s_build_entry> old_entries = Build_Entries_(before.items);
            std::vector<s_build_entry> new_entries = Build_Entries_(COLBLDG::current_item);
            size_t common = std::min(old_entries.size(), new_entries.size());
            size_t first = common;
            for (size_t i = 0; i < common; ++i) {
                if (old_entries[i].product != new_entries[i].product || old_entries[i].repeating != new_entries[i].repeating) {
                    first = i;
                    break;
                }
            }
            if (new_entries.size() > old_entries.size()) {
                const s_build_entry& added = new_entries[first];
                std::string text = vocab::Format(vocab::VOCAB_BUILD_ADDED, Build_Product_Name_(added.product), std::to_string(first + 1));
                if (added.repeating) {
                    text += ", " + std::string(vocab::Get(vocab::VOCAB_BUILD_REPEATING));
                }
                return text;
            }
            if (new_entries.size() < old_entries.size()) {
                return vocab::Format(vocab::VOCAB_BUILD_REMOVED, before.names[old_entries[first].slot]);
            }
            if (first == common) {
                return std::string();
            }
            // A move shifts the entries between its two ends; the held one is what moved.
            int16_t moved = before.held_slot >= 0 ? before.items[before.held_slot] : old_entries[first].product;
            size_t position = new_entries.size();
            for (size_t i = 0; i < new_entries.size() && position == new_entries.size(); ++i) {
                if (new_entries[i].product == moved && old_entries[i].product != moved) {
                    position = i;
                }
            }
            if (position == new_entries.size()) {
                return std::string();
            }
            return vocab::Format(vocab::VOCAB_BUILD_MOVED, Build_Product_Name_(moved), std::to_string(position + 1));
        }

        void Speak_Build_Description_(int16_t product) {
            std::string description = Build_Description_(product);
            speech::Say(description.empty() ? std::string(vocab::Get(vocab::VOCAB_NO_TOOLTIP)) : description, true);
        }

        std::string Join_Build_Value_(const std::vector<std::string>& parts) {
            std::string text;
            for (const std::string& part : parts) {
                if (!part.empty()) {
                    text += (text.empty() ? "" : ", ") + part;
                }
            }
            return text;
        }

        // A product row: Enter presses its field, and the poll speaks what the press changed.
        graph::NodeVtable Build_Product_Vtable_(int16_t product, int16_t field_idx, std::function<std::string()> value) {
            graph::NodeVtable vtable;
            vtable.Type = &button_type;
            vtable.Announcements.push_back(graph::NodeAnnouncement::Static(Build_Product_Name_(product)));
            vtable.Announcements.back().Kind = Label;
            vtable.Announcements.push_back(graph::NodeAnnouncement(value, true, Value));
            vtable.OnActivate = [field_idx]() { Build_Press_(field_idx); };
            vtable.StateText = []() { return std::string(); };
            vtable.OnTooltip = [product]() { Speak_Build_Description_(product); };
            return vtable;
        }

        void Build_Queue_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            std::vector<s_build_entry> entries = Build_Entries_(COLBLDG::current_item);
            if (entries.empty()) {
                builder->AddItem(graph::ControlId::Structural("build/queue/empty"),
                    Info_Vtable_(std::vector<std::string>{ vocab::Get(vocab::VOCAB_BUILD_QUEUE_EMPTY) }));
                return;
            }
            for (const s_build_entry& entry : entries) {
                int16_t slot = entry.slot;
                int16_t product = entry.product;
                int16_t field_idx = COLBLDG::queue_fields[slot];
                if (!Is_Valid_Field_(field_idx)) {
                    continue;
                }
                std::string key = "build/queue/" + std::to_string(slot);
                graph::NodeVtable vtable = Build_Product_Vtable_(product, field_idx, [slot, product]() {
                    bool repeating = slot < 6 && COLBLDG::current_item[slot + 1] == COLONY_PRODUCTION_REPEAT;
                    return Join_Build_Value_({
                        Build_Held_Slot_() == slot ? std::string(vocab::Get(vocab::VOCAB_BUILD_HELD)) : std::string(),
                        repeating ? std::string(vocab::Get(vocab::VOCAB_BUILD_REPEATING)) : std::string(),
                        Build_Cost_Text_(product),
                    });
                });
                vtable.OnSecondary = [slot]() { Build_Remove_(slot); };
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        void Build_Buildings_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int16_t i = 0; i < COLBLDG::n_building_indexes; ++i) {
                int16_t product = COLBLDG::building_indexes[i];
                int16_t field_idx = COLBLDG::building_fields[i];
                if (!Is_Valid_Field_(field_idx)) {
                    continue;
                }
                std::string key = "build/building/" + std::to_string(product);
                graph::NodeVtable vtable = Build_Product_Vtable_(product, field_idx, [product]() {
                    return Join_Build_Value_({
                        COLBLDG::Has_Prod_In_Queue(product) ? std::string(vocab::Get(vocab::VOCAB_BUILD_QUEUED)) : std::string(),
                        Build_Cost_Text_(product),
                    });
                });
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        void Build_Ships_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int16_t i = 0; i < COLBLDG::n_military_indexes; ++i) {
                int16_t product = COLBLDG::military_indexes[i];
                int16_t field_idx = COLBLDG::military_fields[i];
                if (product == COLONY_PRODUCTION_SEPARATOR || !Is_Valid_Field_(field_idx)) {
                    continue;
                }
                std::string key = "build/ship/" + std::to_string(product);
                graph::NodeVtable vtable = Build_Product_Vtable_(product, field_idx, [product]() {
                    return Join_Build_Value_({
                        Build_Ship_Is_Dim_(product) ? std::string(vocab::Get(vocab::VOCAB_STATE_UNAVAILABLE)) : std::string(),
                        Build_Cost_Text_(product),
                    });
                });
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        void Build_Controls_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            // Buy and the colony switches are keyboard-only hotkeys, with no help box to name them.
            const struct {
                int32_t slot;
                const char* key;
                vocab::e_vocab_string fallback;
            } controls[] = {
                { 2, "build/auto_build", vocab::VOCAB_COLONY_AUTO_BUILD },
                { 6, "build/repeat", vocab::VOCAB_COUNT },
                { 4, "build/refit", vocab::VOCAB_COUNT },
                { 5, "build/design", vocab::VOCAB_COUNT },
                { 10, "build/buy", vocab::VOCAB_COLONY_BUY },
                { 8, "build/previous", vocab::VOCAB_COLONY_PREVIOUS },
                { 9, "build/next", vocab::VOCAB_COLONY_NEXT },
                { 0, "build/cancel", vocab::VOCAB_COUNT },
                { 3, "build/ok", vocab::VOCAB_COUNT },
            };
            for (const auto& control : controls) {
                int16_t field_idx = COLBLDG::fields[control.slot];
                if (!Is_Valid_Field_(field_idx)) {
                    continue;
                }
                std::string label = control.fallback != vocab::VOCAB_COUNT ? Help_Label_Or_(field_idx, control.fallback) : Field_Label_(field_idx);
                Add_Field_Item_(builder, context, control.key, field_idx, label);
            }
        }

        void Build_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            builder->BeginStop("build/queue");
            builder->PushContext(vocab::Get(vocab::VOCAB_BUILD_QUEUE));
            Build_Queue_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("build/buildings");
            builder->PushContext(vocab::Get(vocab::VOCAB_BUILD_BUILDINGS));
            Build_Buildings_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("build/ships");
            builder->PushContext(vocab::Get(vocab::VOCAB_BUILD_SHIPS));
            Build_Ships_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("build/controls");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_CONTROLS));
            Build_Controls_Stop_(builder, context);
            builder->PopContext();
        }

        // Speaks what a press changed once the game has acted on it, even after a popup it opened
        // has closed; otherwise what is held, and which choice Design or Repeat Build waits for.
        void Build_Poll_(bool just_attached) {
            if (navigator::Field_Activation_Pending()) {
                return;
            }
            int16_t held = Build_Held_Slot_();
            uint8_t mode = COLBLDG::field_mode;
            if (build_removal != BUILD_REMOVAL_NONE) {
                Build_Advance_Removal_();
            } else {
                std::string text;
                if (build_change_pending) {
                    build_change_pending = false;
                    if (build_before.colony == COLONY::colony_handle) {
                        text = Build_Change_Text_(build_before);
                    }
                }
                if (text.empty() && !just_attached) {
                    if (held != polled_build_held) {
                        if (held >= 0) {
                            text = vocab::Format(vocab::VOCAB_BUILD_HOLDING, Build_Product_Name_(COLBLDG::current_item[held]));
                        } else if (!polled_build_held_name.empty()) {
                            text = vocab::Format(vocab::VOCAB_BUILD_PUT_BACK, polled_build_held_name);
                        }
                    } else if (mode != polled_build_mode) {
                        if (mode == 1) {
                            text = vocab::Get(vocab::VOCAB_BUILD_CHOOSE_DESIGN);
                            navigator::Request_Focus_Stop("build/ships");
                        } else if (mode == 2) {
                            text = vocab::Get(vocab::VOCAB_BUILD_CHOOSE_REPEAT);
                        } else {
                            text = vocab::Get(vocab::VOCAB_BUILD_CANCELLED);
                        }
                    }
                }
                speech::Say(text, true);
            }
            polled_build_held = held;
            polled_build_held_name = held >= 0 ? Build_Product_Name_(COLBLDG::current_item[held]) : std::string();
            polled_build_mode = mode;
        }

        // ---- Officer level popup -------------------------------------------------------------------

        // MAINPUPS::Officer_Gains_Level_Popup: a leader gaining a level, or a marooned hero found in
        // a system who joins or has no room to (officer_gains_level_state). Any press closes it.
        bool Officer_Level_Is_Active_() {
            return MAINPUPS::level_popup_awaiting_input != 0 && MAINPUPS::g_officer >= 0;
        }

        std::string Officer_Level_Instance_Key_() {
            return "officer_level/" + std::to_string(MAINPUPS::g_officer) + "/" + std::to_string(MAINPUPS::officer_gains_level_state);
        }

        // The title Draw_Officer_Gains_Level_Popup prints across the top.
        std::string Officer_Level_Name_() {
            return text::Normalize_Game_Text(OFFICER::Leader_Name(MAINPUPS::g_officer, 1));
        }

        // The paragraph Draw_Officer_Gains_Level_Popup prints under the skills.
        std::string Officer_Level_Message_() {
            const int16_t formats[3] = {
                ESTR_S_GAINS_A_LEVEL,
                ESTR_YOU_FIND_S_WHO_JOINS_YOU_OUT_OF_GRATITUDE_FOR_BEING_RESCUED,
                ESTR_YOU_FIND_S_WHO_WOULD_JOIN_YOU_OUT_OF_GRATITUDE_FOR_BEING_RESCUED_BUT_YOU_HAVE_NO_ROOM,
            };
            int16_t state = MAINPUPS::officer_gains_level_state;
            if (state < 0 || state > 2) {
                return std::string();
            }
            char name[100] = {};
            strlcpy(name, OFFICER::Leader_Name(MAINPUPS::g_officer, 0), sizeof(name));
            char message[500];
            snprintf(message, sizeof(message), ESTRINGS::E_Strings(formats[state]), name);
            return text::Normalize_Game_Text(message);
        }

        void Officer_Level_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            std::string message = Officer_Level_Message_();
            if (!message.empty()) {
                builder->AddItem(graph::ControlId::Structural("officer_level/message"), Info_Vtable_(std::vector<std::string>{ message }));
            }
            // The popup adds no skill help fields, so a stale hire popup field must not stand in.
            Add_Officer_Skills_(builder, context, "officer_level", MAINPUPS::g_officer, false);
            int16_t close = MAINPUPS::mp_fields[0];
            if (Is_Valid_Field_(close)) {
                Add_Field_Item_(builder, context, "officer_level/continue", close, vocab::Get(vocab::VOCAB_CONTINUE));
            }
            if (!message.empty()) {
                builder->SetStart(graph::ControlId::Structural("officer_level/message"));
            }
        }

        // ---- Planet selection popup ----------------------------------------------------------------

        // MAINPUPS::New_Colony_Selection_Popup: a system display whose planets are pressed to choose
        // where a colony ship, colony base, outpost, troops, or artificial planet goes.
        bool Planet_Select_Is_Active_() {
            return MAINPUPS::selection_popup_awaiting_input != 0 && COLONY::sys_disp_star_handle >= 0 &&
                COLONY::sys_disp_star_handle < MOX::NUM_STARS;
        }

        // A system special notice Draw_Selection_Popup draws over the display until the next press.
        // A marooned hero draws nothing; the next press opens the officer level popup instead.
        bool Planet_Select_Special_Shown_() {
            return MAINPUPS::g_special != SYSTEM_SPECIAL_NO_SPECIAL && MAINPUPS::g_special != SYSTEM_SPECIAL_MAROONED_HERO &&
                MAINPUPS::g_special < SYSTEM_SPECIAL_COUNT;
        }

        std::string Planet_Select_Instance_Key_() {
            return "planet_select/" + std::to_string(COLONY::sys_disp_star_handle) + (Planet_Select_Special_Shown_() ? "/special" : "");
        }

        // The prompt Draw_Selection_Popup titles the display with.
        std::string Planet_Select_Name_() {
            int16_t string_id = -1;
            switch (MAINPUPS::selection_state_flag) {
                case 0:
                    string_id = MAINPUPS::g_using_colony_ship != 0 ? ESTR_SELECT_PLANET_FOR_COLONY_SHIP_IN_S_SYSTEM
                                                                   : ESTR_SELECT_PLANET_FOR_COLONY_BASE_IN_S_SYSTEM;
                    break;
                case 1: string_id = ESTR_SELECT_PLANET_FOR_AN_OUTPOST_IN_THE_S_SYSTEM; break;
                case 2: string_id = ESTR_SELECT_PLANET_FOR_TROOPS_IN_THE_S_SYSTEM; break;
                case 3: string_id = ESTR_SELECT_PLANET_FOR_ARTIFICIAL_PLANET_CONSTRUCTION_IN_THE_S_SYSTEM; break;
                default: break;
            }
            int16_t star_idx = MAINPUPS::g_star_n;
            if (string_id < 0 || star_idx < 0 || star_idx >= MOX::NUM_STARS) {
                return Star_Name_(COLONY::sys_disp_star_handle);
            }
            char prompt[300];
            snprintf(prompt, sizeof(prompt), ESTRINGS::E_Strings(string_id), MOX::star[star_idx].name);
            return text::Normalize_Game_Text(prompt);
        }

        void Planet_Select_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            if (Planet_Select_Special_Shown_()) {
                uint8_t special = MAINPUPS::g_special;
                builder->AddItem(graph::ControlId::Structural("planet_select/special"), Info_Vtable_(std::vector<std::string>{
                    text::Normalize_Game_Text(MOX::planet_special_string[special]),
                    text::Normalize_Game_Text(MAINPUPS::System_Special_Desc(special)) }));
                int16_t close = MAINPUPS::mp_fields[0];
                if (Is_Valid_Field_(close)) {
                    Add_Field_Item_(builder, context, "planet_select/continue", close, vocab::Get(vocab::VOCAB_CONTINUE));
                }
                return;
            }
            // Its close button is the popup's own cancel, not the galaxy display's.
            Build_System_Stop_(builder, context, "planet_select", COLONY::sys_disp_star_handle, MAINPUPS::mp_fields[2],
                vocab::Get(vocab::VOCAB_CANCEL));
        }

        // ---- Colony landing ------------------------------------------------------------------------

        // COLLAND::Colony_Landing_Screen: the pods landing on a newly settled planet. It waits for a
        // click, and prints the planet's details only once the animation has nearly finished.
        bool Landing_Is_Active_() {
            return MOX::current_screen == SCREEN_COLONY_LANDING && COLLAND::awaiting_input != 0 && COLLAND::l_anims != nullptr &&
                COLONY::planet_handle >= 0 && COLONY::planet_handle < MAX_PLANETS;
        }

        std::string Landing_Instance_Key_() {
            return "landing/" + std::to_string(COLONY::planet_handle);
        }

        // The title Draw_Colony_Landing_Screen prints across the top.
        std::string Landing_Name_() {
            char title[200];
            snprintf(title, sizeof(title), ESTRINGS::E_Strings(ESTR_COLONY_BUILT_ON_S), HACCESS::Get_Planet_Name(COLONY::planet_handle));
            return text::Normalize_Game_Text(title);
        }

        bool Landing_Details_Shown_() {
            return animate::Get_Animation_Frame_Count(COLLAND::l_anims) - 2 <= animate::Get_Current_Frame(COLLAND::l_anims);
        }

        // The lines COLLAND::Print_Planet_Data prints.
        std::vector<std::string> Landing_Details_() {
            int16_t planet_idx = COLONY::planet_handle;
            const s_planet_data* planet = &MOX::planet[planet_idx];
            std::vector<std::string> lines;
            char line[500];
            if (planet->colony_index == -1) {
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_S1), HACCESS::Get_Planet_Name(planet_idx));
            } else {
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_S__S_COLONY), HACCESS::Get_Planet_Name(planet_idx),
                    MOX::player[MOX::colony[planet->colony_index].owner].race_name);
            }
            lines.push_back(text::Normalize_Game_Text(line));

            const char* climate = nullptr;
            if (planet->colony_index == -1) {
                if (planet->planet_type != PLANET_TYPE_PLANET) {
                    lines.push_back(text::Normalize_Game_Text(ESTRINGS::E_Strings(ESTR_GAS_GIANT__UNINHABITABLE)));
                    return lines;
                }
                climate = MOX::planet_climate_string[planet->climate];
            } else {
                climate = MOX::planet_climate_string[(uint8_t)MOX::colony[planet->colony_index].climate];
            }
            const char* size_climate = MOX::settings.language == 4 ? "%s, %s" : ESTRINGS::E_Strings(ESTR_S_S);
            snprintf(line, sizeof(line), size_climate, MOX::planet_size_string[planet->size], climate);
            lines.push_back(text::Normalize_Game_Text(line));

            snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_D_TOTAL_POPULATION),
                (int)COLCALC::Planet_Max_Population_For_Player(const_cast<s_planet_data*>(planet), MOX::PLAYER_NUM));
            lines.push_back(text::Normalize_Game_Text(line));
            snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_MINERAL_S), MOX::mineral_class_string[planet->mineral_class]);
            lines.push_back(text::Normalize_Game_Text(line));
            lines.push_back(text::Normalize_Game_Text(MOX::planet_gravity_string[planet->gravity_class]));
            if (planet->planet_special != SYSTEM_SPECIAL_NO_SPECIAL) {
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_S1), MOX::planet_special_string[planet->planet_special]);
                lines.push_back(text::Normalize_Game_Text(line));
            }
            return lines;
        }

        void Landing_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            if (Landing_Details_Shown_()) {
                std::vector<std::string> lines = Landing_Details_();
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (!lines[i].empty()) {
                        builder->AddItem(graph::ControlId::Structural("landing/detail/" + std::to_string(i)),
                            Info_Vtable_(std::vector<std::string>{ lines[i] }));
                    }
                }
            }
            if (Is_Valid_Field_(COLLAND::fields)) {
                Add_Field_Item_(builder, context, "landing/continue", COLLAND::fields, vocab::Get(vocab::VOCAB_CONTINUE));
                builder->SetStart(graph::ControlId::Structural("landing/continue"));
            }
        }

        // The details appear without a keypress, so they are spoken when they do.
        bool polled_landing_details = false;

        void Landing_Poll_(bool just_attached) {
            bool shown = Landing_Details_Shown_();
            if (shown && (!polled_landing_details || just_attached)) {
                std::string text;
                for (const std::string& line : Landing_Details_()) {
                    if (!line.empty()) {
                        text += (text.empty() ? "" : ", ") + line;
                    }
                }
                speech::Say(text, false);
            }
            polled_landing_details = shown;
        }

        const s_screen_recipe recipes[] = {
            { "text_box", Text_Box_Is_Active_, Text_Box_Instance_Key_, Text_Box_Build_, Text_Box_Name_ },
            { "confirm", Confirmation_Is_Active_, Confirmation_Instance_Key_, Confirmation_Build_, Confirmation_Name_ },
            { "science_room", Science_Room_Is_Active_, Science_Room_Instance_Key_, Science_Room_Build_, Science_Room_Name_,
                Science_Room_Poll_ },
            { "officer_level", Officer_Level_Is_Active_, Officer_Level_Instance_Key_, Officer_Level_Build_, Officer_Level_Name_ },
            { "planet_select", Planet_Select_Is_Active_, Planet_Select_Instance_Key_, Planet_Select_Build_, Planet_Select_Name_ },
            { "officer_offer", Officer_Offer_Is_Active_, Officer_Offer_Instance_Key_, Officer_Offer_Build_, Officer_Offer_Name_ },
            { "turn_summary", Turn_Summary_Is_Active_, Turn_Summary_Instance_Key_, Turn_Summary_Build_, Turn_Summary_Name_,
                Turn_Summary_Poll_ },
            { "landing", Landing_Is_Active_, Landing_Instance_Key_, Landing_Build_, Landing_Name_, Landing_Poll_ },
            { "build", Build_Is_Active_, Build_Instance_Key_, Build_Build_, Build_Name_, Build_Poll_ },
            { "colony", Colony_Is_Active_, Colony_Instance_Key_, Colony_Build_, Colony_Name_, Colony_Poll_ },
            { "research", Research_Is_Active_, Research_Instance_Key_, Research_Build_, Research_Name_ },
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
