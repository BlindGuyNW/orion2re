#include "pch.h"

#include "access/screens_common.h"

#include "access/navigator.h"
#include "access/text.h"
#include "access/vocab.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace access::screens {

    namespace {

        // The pickers the ship design screen opens run nested loops of their own, publishing which
        // one waits for input in DESBOX::shown_box. What a pick changed is spoken by the design
        // screen's poll once its loop is back.

        // Neither picker keeps its Cancel button's field index; it is the one button whose hotkey
        // is Escape.
        int16_t Picker_Cancel_Field_() {
            for (int16_t field_idx = 1; field_idx < fields::fields_count; ++field_idx) {
                const s_field* field = &fields::fields[field_idx];
                if (field->field_type == FIELD_TYPE_BUTTON && field->hotkey == '\x1B') {
                    return field_idx;
                }
            }
            return 0;
        }

        void Add_Picker_Cancel_(graph::GraphBuilder* builder, s_build_context* context, const char* key) {
            int16_t field_idx = Picker_Cancel_Field_();
            if (field_idx > 0) {
                Add_Field_Item(builder, context, key, field_idx, vocab::Get(vocab::VOCAB_CANCEL));
            }
        }

        std::string Column_(int16_t header_id, const std::string& value) {
            return H_Text(header_id) + " " + value;
        }

        // A picker row: Enter picks it, and Backspace shows its description, as a right-click does.
        graph::NodeVtable Picker_Row_Vtable_(const std::string& label, const std::string& details, std::function<void()> activate,
            std::function<void()> secondary) {
            graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ label, details });
            vtable.Type = &button_type;
            vtable.OnActivate = activate;
            vtable.OnSecondary = secondary;
            return vtable;
        }

        void Add_Picker_State_(graph::NodeVtable* vtable, vocab::e_vocab_string state, const char* kind) {
            vtable->Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(state)));
            vtable->Announcements.back().Kind = kind;
        }

        // ---- Shield or computer picker ---------------------------------------------------------------

        bool Generic_Picker_Is_Active_() {
            return DESBOX::shown_box == DESBOX::SHOWN_BOX_GENERIC && MOX::design != nullptr && MOX::scroll_bar != nullptr;
        }

        bool Picking_Shield_() {
            return MOX::design_screen_replacement_type == TECH_APPLICATION_TYPE_SHIELD;
        }

        std::string Generic_Picker_Instance_Key_() {
            return Picking_Shield_() ? "design_picker/shield" : "design_picker/computer";
        }

        std::string Generic_Picker_Name_() {
            return vocab::Get(Picking_Shield_() ? vocab::VOCAB_DESIGN_CHOOSE_SHIELD : vocab::VOCAB_DESIGN_CHOOSE_COMPUTER);
        }

        // The columns Print_Shield_Data draws: each value under its header.
        std::string Shield_Row_Details_(int16_t item) {
            int32_t cost = 0;
            int32_t space = 0;
            if (item > 0) {
                const s_tech_shield_data* shield = &TECHDATA::shields[item];
                int16_t tech_level = DESIGN::Tech_Level(MOX::PLAYER_NUM, shield->tech_app_id);
                cost = DESIGN::Cost_Given_M_Level(shield->cost_per_ship_type[MOX::design->ship_size], tech_level);
                space = (int16_t)DESIGN::Space_Given_M_Level(shield->space_per_ship_type[MOX::design->ship_size], tech_level, 0);
            }
            return Join_Values({ Column_(0x34, std::to_string(cost)), Column_(0x35, std::to_string(space)) });
        }

        // The columns Print_Computer_Data draws.
        std::string Computer_Row_Details_(int16_t item) {
            std::string bonus = "0";
            int32_t cost = 0;
            if (item > 0) {
                int16_t tech_level = DESIGN::Tech_Level(MOX::PLAYER_NUM, TECHDATA::computers[item].tech_app_id);
                cost = DESIGN::Cost_Given_M_Level(DESIGN::Computer_Cost(item), tech_level);
                bonus = "+" + std::to_string(TECHDATA::computers[item].bonus);
            }
            return Join_Values({ Column_(0x2A, bonus), Column_(0x2B, std::to_string(cost)) });
        }

        void Generic_Picker_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            bool shield = Picking_Shield_();
            for (int16_t row = 0; row < MOX::scroll_bar->total_rows; ++row) {
                int16_t item = MOX::design_choice_items[row];
                int16_t field_idx = MOX::repl_fields[row];
                if (item < 0 || !Is_Valid_Field(field_idx)) {
                    continue;
                }
                std::string label;
                if (item == 0) {
                    label = H_Text(shield ? 0x36 : 0x2D);
                } else {
                    label = text::Normalize_Game_Text(shield ? TECHDATA::shields[item].name : TECHDATA::computers[item].name);
                }
                graph::NodeVtable vtable = Picker_Row_Vtable_(label, shield ? Shield_Row_Details_(item) : Computer_Row_Details_(item),
                    [field_idx]() { navigator::Request_Field_Activation(field_idx); },
                    [field_idx]() { navigator::Request_Field_Secondary(field_idx); });
                if (DESBOX::field_item_chosen == row) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_SELECTED, Selected);
                }
                std::string key = "design_picker/item/" + std::to_string(item);
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
            Add_Picker_Cancel_(builder, context, "design_picker/cancel");
        }

        // ---- Special systems picker ------------------------------------------------------------------

        bool Specials_Picker_Is_Active_() {
            return DESBOX::shown_box == DESBOX::SHOWN_BOX_SPECIAL_SYSTEMS && MOX::design != nullptr && MOX::scroll_bar != nullptr;
        }

        std::string Specials_Picker_Instance_Key_() {
            return "design_specials/" + std::to_string(MOX::design_screen_replacement_slot);
        }

        std::string Specials_Picker_Name_() {
            return vocab::Get(vocab::VOCAB_DESIGN_CHOOSE_SPECIAL_SYSTEM);
        }

        int16_t Specials_Visible_Rows_() {
            return std::min<int16_t>(MOX::scroll_bar->total_rows, 10);
        }

        // The field of a row while it is scrolled into view, else 0.
        int16_t Specials_Row_Field_(int16_t item_idx) {
            int16_t visible = item_idx - MOX::scroll_bar->first_visible_row;
            if (visible < 0 || visible >= Specials_Visible_Rows_()) {
                return 0;
            }
            int16_t field_idx = MOX::repl_fields[visible];
            return Is_Valid_Field(field_idx) ? field_idx : 0;
        }

        // Print_Special_System_Data dims a system that won't fit in the space left, counting the
        // space of the one it replaces, or that conflicts with one already fitted.
        bool Special_Fits_(int16_t item_idx) {
            if (item_idx == 0) {
                return true;
            }
            int16_t cost;
            int16_t space;
            DESBOX::System_Added_Cost_And_Space(item_idx, &cost, &space);
            int16_t slot = MOX::design_screen_replacement_slot;
            if (slot > -1 && MOX::design->special_devices[slot] > 0) {
                space -= MOX::design->special_device_space[slot];
            }
            int16_t conflict;
            return MOX::design->space_used + space <= MOX::design->hull_space &&
                DESBOX::Special_System_Exclusion(MOX::design_choice_items[item_idx], &conflict) == 0;
        }

        std::string Special_Row_Details_(int16_t item_idx) {
            int16_t special = MOX::design_choice_items[item_idx];
            if (item_idx == 0) {
                return std::string();
            }
            int16_t cost;
            int16_t space;
            DESBOX::System_Added_Cost_And_Space(item_idx, &cost, &space);
            const char* description = MOX::special_desc_string[special];
            return Join_Values({
                Column_(0x30, std::to_string(space)),
                Column_(0x2B, std::to_string(cost)),
                description != nullptr ? text::Normalize_Game_Text(description) : std::string(),
            });
        }

        // A row scrolled out of view, waiting for the scroll buttons to bring it in.
        int16_t pending_special = -1;
        bool pending_special_secondary = false;
        int32_t pending_special_ticks = 0;
        int16_t pending_special_first_row = 0;

        void Scroll_Toward_Special_(int16_t item_idx) {
            bool up = item_idx < MOX::scroll_bar->first_visible_row;
            int16_t button = up ? MOX::special_scroll_up_button[0] : MOX::special_scroll_dn_button;
            pending_special_first_row = MOX::scroll_bar->first_visible_row;
            if (Is_Valid_Field(button)) {
                navigator::Request_Field_Activation(button);
            }
        }

        void Press_Special_(int16_t field_idx, bool secondary) {
            if (secondary) {
                navigator::Request_Field_Secondary(field_idx);
            } else {
                navigator::Request_Field_Activation(field_idx);
            }
        }

        // The player never has to scroll: a row out of view is scrolled to, then pressed.
        void Activate_Special_(int16_t item_idx, bool secondary) {
            int16_t field_idx = Specials_Row_Field_(item_idx);
            if (field_idx > 0) {
                pending_special = -1;
                Press_Special_(field_idx, secondary);
                return;
            }
            pending_special = item_idx;
            pending_special_secondary = secondary;
            pending_special_ticks = 0;
            Scroll_Toward_Special_(item_idx);
        }

        void Specials_Picker_Poll_(bool just_attached) {
            if (just_attached) {
                pending_special = -1;
                return;
            }
            if (pending_special < 0 || navigator::Field_Activation_Pending()) {
                return;
            }
            // Each scroll moves one row, so only a stall gives up.
            if (MOX::scroll_bar->first_visible_row != pending_special_first_row) {
                pending_special_ticks = 0;
            } else if (++pending_special_ticks >= PENDING_PRESS_MAX_TICKS) {
                pending_special = -1;
                return;
            }
            int16_t field_idx = Specials_Row_Field_(pending_special);
            if (field_idx > 0) {
                pending_special = -1;
                Press_Special_(field_idx, pending_special_secondary);
            } else if (MOX::scroll_bar->first_visible_row != pending_special_first_row) {
                Scroll_Toward_Special_(pending_special);
            }
        }

        // Every system, not just the ten rows in view: a sighted player scrolls through them all.
        void Specials_Picker_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t slot = MOX::design_screen_replacement_slot;
            int16_t fitted_here = slot > -1 ? MOX::design->special_devices[slot] : 0;
            for (int16_t item_idx = 0; item_idx < MOX::scroll_bar->total_rows; ++item_idx) {
                int16_t special = MOX::design_choice_items[item_idx];
                if (special < 0) {
                    continue;
                }
                std::string label = item_idx == 0 ? H_Text(0x39) : text::Normalize_Game_Text(TECHDATA::specials[special].name);
                graph::NodeVtable vtable = Picker_Row_Vtable_(label, Special_Row_Details_(item_idx),
                    [item_idx]() { Activate_Special_(item_idx, false); },
                    [item_idx]() { Activate_Special_(item_idx, true); });
                if (special == fitted_here) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_SELECTED, Selected);
                } else if (MOX::design_choice_item_selected[item_idx] != 0) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_DESIGN_FITTED, Value);
                }
                if (!Special_Fits_(item_idx)) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_UNAVAILABLE, Enabled);
                }
                std::string key = "design_specials/item/" + std::to_string(special);
                int16_t field_idx = Specials_Row_Field_(item_idx);
                if (field_idx > 0) {
                    context->node_fields[key] = field_idx;
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
            Add_Picker_Cancel_(builder, context, "design_specials/cancel");
        }

    } // namespace

    const s_screen_recipe design_generic_picker_recipe = { "design_generic_picker", Generic_Picker_Is_Active_,
        Generic_Picker_Instance_Key_, Generic_Picker_Build_, Generic_Picker_Name_ };
    const s_screen_recipe design_specials_picker_recipe = { "design_specials_picker", Specials_Picker_Is_Active_,
        Specials_Picker_Instance_Key_, Specials_Picker_Build_, Specials_Picker_Name_, Specials_Picker_Poll_ };

} // namespace access::screens
