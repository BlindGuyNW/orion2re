#include "pch.h"

#include "access/screens_common.h"

#include "access/navigator.h"
#include "access/text.h"
#include "access/vocab.h"

#include <string>
#include <vector>

namespace access::screens {

    namespace {

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
                if (Is_Catch_All(&fields::fields[i])) {
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
                graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ line });
                if (Is_Valid_Field(dismiss)) {
                    vtable.OnActivate = [dismiss]() { navigator::Request_Field_Activation(dismiss); };
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
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
            if (Is_Valid_Field(yes)) {
                Add_Field_Item(builder, context, "confirm/yes", yes, vocab::Get(vocab::VOCAB_YES));
            }
            if (Is_Valid_Field(no)) {
                Add_Field_Item(builder, context, "confirm/no", no, vocab::Get(vocab::VOCAB_NO));
            }
            builder->SetStart(graph::ControlId::Structural("confirm/yes"));
        }

        // ---- Input box ---------------------------------------------------------------------------

        // namestar::Input_Box_Popup opens already typing into its one field, so every key goes to the
        // game and nothing can be navigated; the navigator's text-entry echo reads the field.
        bool Input_Box_Is_Active_() {
            return namestar::shown_prompt != nullptr;
        }

        std::string Input_Box_Instance_Key_() {
            return std::string("input_box/") + namestar::shown_prompt;
        }

        std::string Input_Box_Name_() {
            return text::Normalize_Game_Text(namestar::shown_prompt);
        }

        void Input_Box_Build_(graph::GraphBuilder*, s_build_context*) {
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
            return Is_Valid_Field(field_idx) ? field_idx : 0;
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
            if (Is_Valid_Field(button)) {
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
            if (++pending_turn_message_ticks >= PENDING_PRESS_MAX_TICKS) {
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
                graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ text::Normalize_Game_Text(text) });
                vtable.Type = &button_type;
                vtable.OnActivate = [i]() { Activate_Turn_Message_(i); };
                int16_t field_idx = Turn_Message_Field_(i);
                if (field_idx > 0) {
                    context->node_fields[key] = field_idx;
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
            if (Is_Valid_Field(TURNSUM::shown_ok_field)) {
                Add_Field_Item(builder, context, "turn_summary/ok", TURNSUM::shown_ok_field, vocab::Get(vocab::VOCAB_OK));
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
                if (help->skill_id[i] == skill_idx && Is_Valid_Field(help->skill_help_x[i])) {
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
            graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{
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
                builder->AddItem(graph::ControlId::Structural("officer_offer/message"), Info_Vtable(std::vector<std::string>{ message }));
            }

            Add_Officer_Skills_(builder, context, "officer_offer", officer, true);

            // The buttons carry only one-letter hotkeys. Before the choice they are Hire and
            // Reject; after it, any press closes the popup.
            int16_t hire = MAINPUPS::mp_fields[7];
            int16_t reject = MAINPUPS::mp_fields[8];
            std::string start;
            if (MAINPUPS::field_state == 0) {
                if (Is_Valid_Field(hire)) {
                    Add_Field_Item(builder, context, "officer_offer/hire", hire, vocab::Get(vocab::VOCAB_OFFICER_HIRE));
                    start = "officer_offer/hire";
                }
                if (Is_Valid_Field(reject)) {
                    Add_Field_Item(builder, context, "officer_offer/reject", reject, vocab::Get(vocab::VOCAB_OFFICER_REJECT));
                }
            } else if (Is_Valid_Field(reject)) {
                Add_Field_Item(builder, context, "officer_offer/ok", reject, vocab::Get(vocab::VOCAB_OK));
                start = "officer_offer/ok";
            }
            if (!message.empty()) {
                start = "officer_offer/message";
            }
            if (!start.empty()) {
                builder->SetStart(graph::ControlId::Structural(start));
            }
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
                builder->AddItem(graph::ControlId::Structural("officer_level/message"), Info_Vtable(std::vector<std::string>{ message }));
            }
            // The popup adds no skill help fields, so a stale hire popup field must not stand in.
            Add_Officer_Skills_(builder, context, "officer_level", MAINPUPS::g_officer, false);
            int16_t close = MAINPUPS::mp_fields[0];
            if (Is_Valid_Field(close)) {
                Add_Field_Item(builder, context, "officer_level/continue", close, vocab::Get(vocab::VOCAB_CONTINUE));
            }
            if (!message.empty()) {
                builder->SetStart(graph::ControlId::Structural("officer_level/message"));
            }
        }

    } // namespace

    const s_screen_recipe text_box_recipe = { "text_box", Text_Box_Is_Active_, Text_Box_Instance_Key_, Text_Box_Build_, Text_Box_Name_ };
    const s_screen_recipe confirm_recipe = { "confirm", Confirmation_Is_Active_, Confirmation_Instance_Key_, Confirmation_Build_, Confirmation_Name_ };
    const s_screen_recipe input_box_recipe = { "input_box", Input_Box_Is_Active_, Input_Box_Instance_Key_, Input_Box_Build_, Input_Box_Name_ };
    const s_screen_recipe officer_level_recipe = { "officer_level", Officer_Level_Is_Active_, Officer_Level_Instance_Key_, Officer_Level_Build_, Officer_Level_Name_ };
    const s_screen_recipe officer_offer_recipe = { "officer_offer", Officer_Offer_Is_Active_, Officer_Offer_Instance_Key_, Officer_Offer_Build_, Officer_Offer_Name_ };
    const s_screen_recipe turn_summary_recipe = { "turn_summary", Turn_Summary_Is_Active_, Turn_Summary_Instance_Key_, Turn_Summary_Build_, Turn_Summary_Name_, Turn_Summary_Poll_ };

} // namespace access::screens
