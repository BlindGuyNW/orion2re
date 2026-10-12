#include "pch.h"

#include "access/screens_common.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/speech.h"
#include "access/text.h"
#include "access/vocab.h"

#include <string>

namespace access::screens {

    namespace {

        // ---- Main menu -------------------------------------------------------------------------

        constexpr int16_t MAIN_MENU_BUTTON_X = 0x19F;
        constexpr int16_t MAIN_MENU_ABSENT_FIELD = -1000;


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
            return Is_Valid_Field(new_field) && fields::fields[new_field].x == MAIN_MENU_BUTTON_X &&
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

                std::string label = Help_Title(help_id);
                if (label.empty()) {
                    label = vocab::Get(button->fallback_label);
                }

                if (Is_Valid_Field(field_idx)) {
                    Add_Field_Item(builder, context, button->key, field_idx, label);
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

            if (Is_Valid_Field(MAINMENU::credits_field)) {
                Add_Field_Item(builder, context, "main_menu/credits", MAINMENU::credits_field,
                    vocab::Get(vocab::VOCAB_MAIN_MENU_CREDITS));
            }

            int16_t continue_field = MAINMENU::mainmenu->fields[MAIN_MENU_BTN_CONTINUE].field;
            builder->SetStart(graph::ControlId::Structural(Is_Valid_Field(continue_field) ? "main_menu/continue" : "main_menu/new"));
        }

        std::string Main_Menu_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_MAIN_MENU);
        }

        // ---- New Game --------------------------------------------------------------------------

        // Each setting is a hidden field over its picture plus a click-thru field over its label
        // strip; both cycle the same value forward, and the game draws the value's string itself.
        // ERICHELP's help list gives the tech level box the Prewarp record (550) whatever is
        // selected. HELP.LBX has one per level, in reverse order, and a general one (551) for
        // orion2re's Post-warp level, which has none. The buttons run Pre-warp, Average,
        // Post-warp, Advanced.
        int16_t Tech_Level_Help_Id_() {
            switch (NEWGAME::civ_button_variable) {
                case 0: return 550;
                case 1: return 549;
                case 3: return 548;
                default: return 551;
            }
        }

        struct s_new_game_setting {
            int32_t field_slot;
            const char* key;
            char** strings;
            int16_t* value;
            int16_t count;
            // Optional. The help record for the current value, where the help list's is wrong.
            int16_t (*Help_Id)();
        };

        const s_new_game_setting new_game_settings[] = {
            { 0, "new_game/difficulty", NEWGAME::diff_button_strings, &NEWGAME::diff_button_variable,
                (int16_t)(sizeof(NEWGAME::diff_button_strings) / sizeof(NEWGAME::diff_button_strings[0])), nullptr },
            { 1, "new_game/galaxy_size", NEWGAME::gal_size_button_strings, &NEWGAME::gal_size_button_variable,
                (int16_t)(sizeof(NEWGAME::gal_size_button_strings) / sizeof(NEWGAME::gal_size_button_strings[0])), nullptr },
            { 2, "new_game/galaxy_age", NEWGAME::gal_age_button_strings, &NEWGAME::gal_age_button_variable,
                (int16_t)(sizeof(NEWGAME::gal_age_button_strings) / sizeof(NEWGAME::gal_age_button_strings[0])), nullptr },
            { 3, "new_game/players", NEWGAME::opp_button_strings, &NEWGAME::opp_button_variable,
                (int16_t)(sizeof(NEWGAME::opp_button_strings) / sizeof(NEWGAME::opp_button_strings[0])), nullptr },
            { 5, "new_game/tech_level", NEWGAME::civ_button_strings, &NEWGAME::civ_button_variable,
                (int16_t)(sizeof(NEWGAME::civ_button_strings) / sizeof(NEWGAME::civ_button_strings[0])), Tech_Level_Help_Id_ },
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
            return Is_Valid_Field(difficulty_field) &&
                fields::fields[difficulty_field].field_type == FIELD_TYPE_HIDDEN &&
                fields::fields[difficulty_field].x == NEWGAME::g_x + NEW_GAME_DIFFICULTY_X;
        }

        std::string New_Game_Instance_Key_() {
            return "new_game";
        }

        std::string New_Game_Setting_Value_(const s_new_game_setting* setting) {
            int16_t value = *setting->value;
            if (value < 0 || value >= setting->count || !Is_Usable_String(setting->strings[value])) {
                return std::string();
            }
            return text::Normalize_Game_Text(setting->strings[value]);
        }

        // The tech level box's help title names one level ("Tech Level : Prewarp") whatever is
        // selected, so only the part before the colon labels the setting.
        std::string New_Game_Setting_Label_(int16_t field_idx) {
            std::string label = Field_Label(field_idx);
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
            std::string label = Help_Title(Field_Help_Id(&fields::fields[field_idx]));
            return label.empty() ? Field_Label(field_idx) : label;
        }

        void New_Game_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int32_t i = 0; i < (int32_t)(sizeof(new_game_settings) / sizeof(new_game_settings[0])); ++i) {
                const s_new_game_setting* setting = &new_game_settings[i];
                int16_t field_idx = NEWGAME::fields[setting->field_slot];
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }

                graph::NodeVtable vtable;
                vtable.Type = &choice_type;
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(New_Game_Setting_Label_(field_idx)));
                vtable.Announcements.back().Kind = Label;
                vtable.Announcements.push_back(graph::NodeAnnouncement([setting]() { return New_Game_Setting_Value_(setting); }, true, Value));
                vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
                if (setting->Help_Id != nullptr) {
                    vtable.OnTooltip = [setting]() { Speak_Help_Record(setting->Help_Id()); };
                } else {
                    vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
                }

                context->node_fields[setting->key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(setting->key), vtable);
            }

            for (int32_t i = 0; i < (int32_t)(sizeof(new_game_toggles) / sizeof(new_game_toggles[0])); ++i) {
                int16_t field_idx = Find_Radio_Field_(new_game_toggles[i].selection);
                if (Is_Valid_Field(field_idx)) {
                    Add_Field_Item(builder, context, new_game_toggles[i].key, field_idx, New_Game_Label_(field_idx));
                }
            }

            int16_t cancel_field = NEWGAME::fields[NEW_GAME_CANCEL_SLOT];
            if (Is_Valid_Field(cancel_field)) {
                Add_Field_Item(builder, context, "new_game/cancel", cancel_field, New_Game_Label_(cancel_field));
            }
            int16_t accept_field = NEWGAME::fields[NEW_GAME_ACCEPT_SLOT];
            if (Is_Valid_Field(accept_field)) {
                Add_Field_Item(builder, context, "new_game/accept", accept_field, New_Game_Label_(accept_field));
            }

            builder->SetStart(graph::ControlId::Structural("new_game/difficulty"));
        }

        std::string New_Game_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_NEW_GAME);
        }

    } // namespace

    const s_screen_recipe main_menu_recipe = { "main_menu", Main_Menu_Is_Active_, Main_Menu_Instance_Key_, Main_Menu_Build_, Main_Menu_Name_ };
    const s_screen_recipe new_game_recipe = { "new_game", New_Game_Is_Active_, New_Game_Instance_Key_, New_Game_Build_, New_Game_Name_ };

} // namespace access::screens
