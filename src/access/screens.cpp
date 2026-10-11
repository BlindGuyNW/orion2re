#include "pch.h"

#include "access/screens.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/speech.h"
#include "access/text.h"
#include "access/vocab.h"

#include <string>

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

        const s_screen_recipe recipes[] = {
            { "main_menu", Main_Menu_Is_Active_, Main_Menu_Instance_Key_, Main_Menu_Build_, Main_Menu_Name_ },
            { "new_game", New_Game_Is_Active_, New_Game_Instance_Key_, New_Game_Build_, New_Game_Name_ },
            { "fields", Fields_Is_Active_, Fields_Instance_Key_, Fields_Build_, Fields_Name_ },
        };

    } // namespace

    const s_screen_recipe* Recipes(int32_t* count) {
        *count = (int32_t)(sizeof(recipes) / sizeof(recipes[0]));
        return recipes;
    }

} // namespace access::screens
