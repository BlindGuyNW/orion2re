#include "pch.h"

#include "access/screens_common.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/text.h"
#include "access/vocab.h"

#include <string>
#include <vector>

namespace access::screens {

    namespace {

        // ---- Research selection ------------------------------------------------------------------

        // TECH::_Tech_Select publishes its eight areas while it waits for input; _Tech_List, the
        // browse list for one area, opens over it without removing its fields.
        bool Research_Is_Active_() {
            return TECH::shown_entries != nullptr && TECH::shown_list_entry == nullptr &&
                Is_Valid_Field(TECH::g_first_tech_field) && Is_Valid_Field(TECH::g_last_tech_field);
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
                    graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ text::Normalize_Game_Text(entry->app_names[app]) });
                    if (Research_App_Is_Current_(entry, app)) {
                        vtable.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(vocab::VOCAB_STATE_CURRENT)));
                        vtable.Announcements.back().Kind = Selected;
                    }
                    if (Is_Valid_Field(field_idx)) {
                        vtable.OnActivate = [field_idx]() { Choose_Research_(field_idx); };
                        // A right click shows the technology's description.
                        vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
                        vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
                        context->node_fields[key] = field_idx;
                    }
                    builder->AddItem(graph::ControlId::Structural(key), vtable);
                    if (start.empty()) {
                        start = key;
                    }
                }

                if (TECH::allow_future_technology_browsing && Is_Valid_Field(entry->button_field_id)) {
                    std::string label = vocab::Format(vocab::VOCAB_RESEARCH_BROWSE, area.empty() ? heading : area);
                    Add_Field_Item(builder, context, "research/" + std::to_string(i) + "/browse", entry->button_field_id, label);
                }
                builder->PopContext();
            }

            if (Is_Valid_Field(TECH::shown_accept_field)) {
                Add_Field_Item(builder, context, "research/accept", TECH::shown_accept_field,
                    Help_Label_Or(TECH::shown_accept_field, vocab::VOCAB_ACCEPT));
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
                if (Is_Catch_All(&fields::fields[i])) {
                    click = i;
                    break;
                }
            }
            auto add_line = [builder, click](const std::string& key, const std::string& text) {
                graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ text });
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
                Add_Field_Item(builder, context, base + "continue", click, vocab::Get(vocab::VOCAB_CONTINUE));
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

    } // namespace

    const s_screen_recipe science_room_recipe = { "science_room", Science_Room_Is_Active_, Science_Room_Instance_Key_, Science_Room_Build_, Science_Room_Name_, Science_Room_Poll_ };
    const s_screen_recipe research_recipe = { "research", Research_Is_Active_, Research_Instance_Key_, Research_Build_, Research_Name_ };

} // namespace access::screens
