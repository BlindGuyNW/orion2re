#include "pch.h"

#include "access/screens_common.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/speech.h"
#include "access/text.h"
#include "access/vocab.h"

#include <string>
#include <vector>

namespace access::screens {

    namespace {

        constexpr int16_t SCREEN_WIDTH = 640;
        constexpr int16_t SCREEN_HEIGHT = 480;

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

    } // namespace

    const graph::ControlType button_type = Make_Type_("button", vocab::VOCAB_ROLE_BUTTON);
    const graph::ControlType toggle_type = Make_Type_("toggle", vocab::VOCAB_ROLE_TOGGLE);
    const graph::ControlType choice_type = Make_Type_("choice", vocab::VOCAB_ROLE_CHOICE);

    namespace {

        const graph::ControlType slider_type = Make_Type_("slider", vocab::VOCAB_ROLE_SLIDER);
        const graph::ControlType list_item_type = Make_Type_("list_item", vocab::VOCAB_ROLE_LIST_ITEM);
        const graph::ControlType text_field_type = Make_Type_("text_field", vocab::VOCAB_ROLE_TEXT_FIELD);
        const graph::ControlType map_area_type = Make_Type_("map_area", vocab::VOCAB_ROLE_MAP_AREA);

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

        // The toggle, slider, or text field value line; empty for fields without one.
        std::string Field_Value_(int16_t field_idx) {
            if (!Is_Valid_Field(field_idx)) {
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
                case FIELD_TYPE_CONTINUOUS_INPUT:
                {
                    const char* buffer = fields::Field_Data_Continuous_Input(field)->buffer;
                    std::string text = buffer != nullptr ? text::Normalize_Game_Text(buffer) : std::string();
                    return text.empty() ? std::string(vocab::Get(vocab::VOCAB_TEXT_BLANK)) : text;
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

    } // namespace

    bool Is_Valid_Field(int16_t field_idx) {
        return field_idx > 0 && field_idx < fields::fields_count;
    }

    bool Is_Usable_String(const char* text) {
        return text != nullptr && text != fields::invalid_field_string_ptr && text[0] != '\0';
    }

    std::string Field_Own_Text(const s_field* field) {
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
        return Is_Usable_String(text) ? text::Normalize_Game_Text(text) : std::string();
    }

    int16_t Field_Help_Id(const s_field* field) {
        return help::Find_Help_Id(field->x, field->y, field->x_end, field->y_end);
    }

    std::string Help_Title(int16_t help_id) {
        help::s_help_text help_text;
        return help::Read_Help_Text(help_id, &help_text) ? help_text.title : std::string();
    }

    std::string Hotkey_Text(const s_field* field) {
        unsigned char key = (unsigned char)field->hotkey;
        if (key <= ' ' || key >= 0x7F) {
            return std::string();
        }
        if (key >= 'a' && key <= 'z') {
            key = (unsigned char)(key - 'a' + 'A');
        }
        return vocab::Format(vocab::VOCAB_HOTKEY, std::string(1, (char)key));
    }

    std::string Field_Label(int16_t field_idx) {
        const s_field* field = &fields::fields[field_idx];
        std::string label = Field_Own_Text(field);
        if (label.empty()) {
            label = Help_Title(Field_Help_Id(field));
        }
        if (label.empty()) {
            label = Hotkey_Text(field);
        }
        if (label.empty()) {
            label = vocab::Format(vocab::VOCAB_UNLABELED, std::to_string(field_idx));
        }
        return label;
    }

    void Speak_Help_Record(int16_t help_id) {
        help::s_help_text help_text;
        bool found = help::Read_Help_Text(help_id, &help_text) && !help_text.body.empty();
        speech::Say(found ? help_text.title + ". " + help_text.body : std::string(vocab::Get(vocab::VOCAB_NO_TOOLTIP)), true);
    }

    void Speak_Field_Help(int16_t field_idx) {
        Speak_Help_Record(Is_Valid_Field(field_idx) ? Field_Help_Id(&fields::fields[field_idx]) : -1);
    }

    graph::NodeVtable Field_Vtable(int16_t field_idx, const std::string& label) {
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
        vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
        return vtable;
    }

    void Add_Field_Item(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
        int16_t field_idx, const std::string& label) {
        context->node_fields[key] = field_idx;
        builder->AddItem(graph::ControlId::Structural(key), Field_Vtable(field_idx, label));
    }

    std::string Screen_Name(int16_t screen) {
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

    bool Is_Offscreen(const s_field* field) {
        return field->x >= SCREEN_WIDTH || field->y >= SCREEN_HEIGHT || field->x_end < field->x ||
            field->y_end < field->y;
    }

    bool Is_Catch_All(const s_field* field) {
        return field->field_type == FIELD_TYPE_HIDDEN && field->x <= 0 && field->y <= 0 &&
            field->x_end >= SCREEN_WIDTH - 1 && field->y_end >= SCREEN_HEIGHT - 1;
    }

    bool Field_Is_Listed(int16_t field_idx, bool has_specific_field) {
        const s_field* field = &fields::fields[field_idx];
        if (field->field_type == FIELD_TYPE_MULTI_HOTKEY || Is_Offscreen(field)) {
            return false;
        }
        return !(Is_Catch_All(field) && has_specific_field);
    }

    graph::NodeVtable Info_Vtable(const std::vector<std::string>& lines) {
        graph::NodeVtable vtable;
        for (size_t i = 0; i < lines.size(); ++i) {
            vtable.Announcements.push_back(graph::NodeAnnouncement::Static(lines[i]));
            vtable.Announcements.back().Kind = i == 0 ? Label : Value;
        }
        return vtable;
    }

    std::string Help_Label_Or(int16_t field_idx, vocab::e_vocab_string fallback) {
        std::string label = Help_Title(Field_Help_Id(&fields::fields[field_idx]));
        return label.empty() ? std::string(vocab::Get(fallback)) : label;
    }

    std::string H_Text(int16_t message_id) {
        return text::Normalize_Game_Text(HAROLD::H_Message(message_id));
    }

    std::string Join_Values(const std::vector<std::string>& parts) {
        std::string text;
        for (const std::string& part : parts) {
            if (!part.empty()) {
                text += (text.empty() ? "" : ", ") + part;
            }
        }
        return text;
    }

} // namespace access::screens
