#include "pch.h"

#include "access/screens_common.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/speech.h"
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

        // help_id is the record the picker opens when Cancel is right-clicked.
        void Add_Picker_Cancel_(graph::GraphBuilder* builder, s_build_context* context, const char* key, int16_t help_id) {
            int16_t field_idx = Picker_Cancel_Field_();
            if (field_idx <= 0) {
                return;
            }
            graph::NodeVtable vtable = Field_Vtable(field_idx, vocab::Get(vocab::VOCAB_CANCEL));
            vtable.OnTooltip = [help_id]() { Speak_Help_Record(help_id); };
            context->node_fields[key] = field_idx;
            builder->AddItem(graph::ControlId::Structural(key), vtable);
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

        // Space speaks the description the game's right-click opens in a text box: the help record
        // of the item's technology application, or the game's own record for the empty choice.
        void Set_Picker_Tooltip_(graph::NodeVtable* vtable, int16_t help_id) {
            vtable->OnTooltip = [help_id]() { Speak_Help_Record(help_id); };
        }

        void Add_Picker_State_(graph::NodeVtable* vtable, vocab::e_vocab_string state, const char* kind) {
            vtable->Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(state)));
            vtable->Announcements.back().Kind = kind;
        }

        // ---- Scrolling pickers ------------------------------------------------------------------------

        // The weapon and special systems pickers show ten rows of MOX::scroll_bar at a time, each
        // visible row a field in MOX::repl_fields, with scroll buttons that move one row per press.

        int16_t Picker_Scroll_Button_(bool up) {
            if (DESBOX::shown_box == DESBOX::SHOWN_BOX_WEAPONS) {
                return up ? DESBOX::replacement_scroll_up_button : DESBOX::replacement_scroll_dn_button;
            }
            return up ? MOX::special_scroll_up_button[0] : MOX::special_scroll_dn_button;
        }

        // The field of a row while it is scrolled into view, else 0.
        int16_t Picker_Row_Field_(int16_t row) {
            int16_t visible = row - MOX::scroll_bar->first_visible_row;
            if (visible < 0 || visible >= std::min<int16_t>(MOX::scroll_bar->total_rows, 10)) {
                return 0;
            }
            int16_t field_idx = MOX::repl_fields[visible];
            return Is_Valid_Field(field_idx) ? field_idx : 0;
        }

        // A row scrolled out of view, waiting for the scroll buttons to bring it in.
        int16_t pending_picker_row = -1;
        bool pending_picker_secondary = false;
        int32_t pending_picker_ticks = 0;
        int16_t pending_picker_first_row = 0;

        void Scroll_Toward_Picker_Row_(int16_t row) {
            int16_t button = Picker_Scroll_Button_(row < MOX::scroll_bar->first_visible_row);
            pending_picker_first_row = MOX::scroll_bar->first_visible_row;
            if (Is_Valid_Field(button)) {
                navigator::Request_Field_Activation(button);
            }
        }

        void Press_Picker_Row_(int16_t field_idx, bool secondary) {
            if (secondary) {
                navigator::Request_Field_Secondary(field_idx);
            } else {
                navigator::Request_Field_Activation(field_idx);
            }
        }

        // The player never has to scroll: a row out of view is scrolled to, then pressed.
        void Activate_Picker_Row_(int16_t row, bool secondary) {
            int16_t field_idx = Picker_Row_Field_(row);
            if (field_idx > 0) {
                pending_picker_row = -1;
                Press_Picker_Row_(field_idx, secondary);
                return;
            }
            pending_picker_row = row;
            pending_picker_secondary = secondary;
            pending_picker_ticks = 0;
            Scroll_Toward_Picker_Row_(row);
        }

        bool Picker_Row_Pending_() {
            return pending_picker_row >= 0;
        }

        void Poll_Pending_Picker_Row_(bool just_attached) {
            if (just_attached) {
                pending_picker_row = -1;
                return;
            }
            if (pending_picker_row < 0 || navigator::Field_Activation_Pending()) {
                return;
            }
            // Each scroll moves one row, so only a stall gives up.
            if (MOX::scroll_bar->first_visible_row != pending_picker_first_row) {
                pending_picker_ticks = 0;
            } else if (++pending_picker_ticks >= PENDING_PRESS_MAX_TICKS) {
                pending_picker_row = -1;
                return;
            }
            int16_t field_idx = Picker_Row_Field_(pending_picker_row);
            if (field_idx > 0) {
                pending_picker_row = -1;
                Press_Picker_Row_(field_idx, pending_picker_secondary);
            } else if (MOX::scroll_bar->first_visible_row != pending_picker_first_row) {
                Scroll_Toward_Picker_Row_(pending_picker_row);
            }
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
                // Draw_Generic_Replacement_Box_Help's records.
                if (item == 0) {
                    Set_Picker_Tooltip_(&vtable, shield ? 0x2B4 : 0x2B5);
                } else {
                    Set_Picker_Tooltip_(&vtable, (int16_t)(shield ? TECHDATA::shields[item].tech_app_id : TECHDATA::computers[item].tech_app_id));
                }
                if (DESBOX::field_item_chosen == row) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_SELECTED, Selected);
                }
                std::string key = "design_picker/item/" + std::to_string(item);
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
            Add_Picker_Cancel_(builder, context, "design_picker/cancel", shield ? 0x2AB : 0x2AE);
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

        void Specials_Picker_Poll_(bool just_attached) {
            Poll_Pending_Picker_Row_(just_attached);
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
                    [item_idx]() { Activate_Picker_Row_(item_idx, false); },
                    [item_idx]() { Activate_Picker_Row_(item_idx, true); });
                Set_Picker_Tooltip_(&vtable, item_idx == 0 ? 691 : (int16_t)TECHDATA::specials[special].tech_app_id);
                if (special == fitted_here) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_SELECTED, Selected);
                } else if (MOX::design_choice_item_selected[item_idx] != 0) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_DESIGN_FITTED, Value);
                }
                if (!Special_Fits_(item_idx)) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_UNAVAILABLE, Enabled);
                }
                std::string key = "design_specials/item/" + std::to_string(special);
                int16_t field_idx = Picker_Row_Field_(item_idx);
                if (field_idx > 0) {
                    context->node_fields[key] = field_idx;
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
            Add_Picker_Cancel_(builder, context, "design_specials/cancel", 689);
        }

        // ---- Weapon picker ---------------------------------------------------------------------------

        // DESBOX::Weapons_Replacement_Box: the first press on a weapon selects it, setting up its
        // arc or racks and modifications below the list; a second press on it, or Accept, fits it.
        // The first row empties the slot.

        bool Weapons_Picker_Is_Active_() {
            return DESBOX::shown_box == DESBOX::SHOWN_BOX_WEAPONS && MOX::design != nullptr && MOX::scroll_bar != nullptr &&
                MOX::weapon_filter != nullptr;
        }

        std::string Weapons_Picker_Instance_Key_() {
            return "design_weapons/" + std::to_string(MOX::design_screen_replacement_slot);
        }

        std::string Weapons_Picker_Name_() {
            return vocab::Get(vocab::VOCAB_DESIGN_CHOOSE_WEAPON);
        }

        // The selected weapon, or -1 while none is.
        int16_t Chosen_Weapon_() {
            int16_t row = DESBOX::field_item_chosen;
            if (row <= 0 || row >= MOX::scroll_bar->total_rows) {
                return -1;
            }
            int16_t weapon = DESBOX::Weapon_Index(row);
            return weapon > 0 ? weapon : -1;
        }

        std::string Weapon_Name_(int16_t weapon) {
            return text::Normalize_Game_Text(TECHDATA::weapons[weapon].name);
        }

        // Print_Main_Weapon_Box prices the selected row as set up below the list, and every other
        // row with a forward arc, the default racks, and no modifications.
        std::string Weapon_Row_Cost_Space_(int16_t row) {
            int16_t weapon = DESBOX::Weapon_Index(row);
            int32_t cost = 0;
            int32_t space = 0;
            if (row > 0 && weapon > 0) {
                bool racks = DESIGN::Weapon_Needs_Racks(weapon) != 0;
                if (DESBOX::field_item_chosen == row) {
                    int16_t ammo = racks ? DESIGN::Missile_Rack_Quantity((int16_t)DESBOX::weapon_replacement_rack) : 0;
                    cost = DESIGN::Weapon_Cost(weapon, 1, DESBOX::weapon_replacement_arcs, ammo, (int16_t)DESBOX::weapon_replacement_mods);
                    space = DESIGN::Weapon_Space(MOX::PLAYER_NUM, weapon, 1, DESBOX::weapon_replacement_arcs, ammo,
                        (int16_t)DESBOX::weapon_replacement_mods);
                } else {
                    int16_t ammo = racks ? DESIGN::Missile_Rack_Quantity(DESIGN::missile_default_x2_rack ? 0 : 1) : 0;
                    cost = DESIGN::Weapon_Cost(weapon, 1, WEAPON_FIRING_ARC_FORWARD, ammo, 0);
                    space = DESIGN::Weapon_Space(MOX::PLAYER_NUM, weapon, 1, WEAPON_FIRING_ARC_FORWARD, ammo, 0);
                }
            }
            return Join_Values({ Column_(0x2B, std::to_string(cost)), Column_(0x30, std::to_string(space)) });
        }

        std::string Weapon_Row_Details_(int16_t row) {
            int16_t weapon = DESBOX::Weapon_Index(row);
            if (row <= 0 || weapon <= 0) {
                return std::string();
            }
            char damage[80];
            DESIGN::Design_Weapon_Damage_String(weapon, damage, sizeof(damage), DESBOX::field_item_chosen, row);
            const char* note = MOX::weapon_notes_string[weapon];
            return Join_Values({
                Column_(0x2F, text::Normalize_Game_Text(damage)),
                Weapon_Row_Cost_Space_(row),
                note != nullptr ? text::Normalize_Game_Text(note) : std::string(),
            });
        }

        // Draw_Weapons_Arc_Box lights the first of these arcs set, else Forward.
        int16_t Current_Arc_() {
            int16_t arcs = DESBOX::weapon_replacement_arcs;
            if (arcs & WEAPON_FIRING_ARC_FORWARD_EXTENDED) {
                return 1;
            }
            if (arcs & WEAPON_FIRING_ARC_BACK_EXTENDED) {
                return 2;
            }
            if (arcs & WEAPON_FIRING_ARC_REAR) {
                return 3;
            }
            if (arcs & WEAPON_FIRING_ARC_360) {
                return 4;
            }
            return 0;
        }

        // The names Weapons_Replacement_Box uses for the arcs in its messages.
        std::string Arc_Name_(int16_t arc) {
            return arc < 4 ? H_Text((int16_t)(0x46 + arc)) : std::string("360");
        }

        std::string Rack_Name_(int16_t rack) {
            return vocab::Format(vocab::VOCAB_DESIGN_SHOTS, std::to_string(DESIGN::Missile_Rack_Quantity(rack)));
        }

        std::string Mod_Name_(int16_t mod) {
            return text::Normalize_Game_Text(TECHDATA::weapon_modifications[mod].name);
        }

        struct s_weapon_filter_toggle {
            int16_t* field;
            uint8_t* status;
            vocab::e_vocab_string fallback;
        };

        const s_weapon_filter_toggle weapon_filter_toggles[] = {
            { &MOX::beam_filter_button, &DESBOX::beam_filter_button_status, vocab::VOCAB_DESIGN_FILTER_BEAM },
            { &MOX::missile_filter_button, &DESBOX::missile_filter_button_status, vocab::VOCAB_DESIGN_FILTER_MISSILE },
            { &MOX::bomb_filter_button, &DESBOX::bomb_filter_button_status, vocab::VOCAB_DESIGN_FILTER_BOMB },
            { &MOX::special_filter_button, &DESBOX::special_filter_button_status, vocab::VOCAB_DESIGN_FILTER_SPECIAL },
        };

        std::string Filter_Name_(const s_weapon_filter_toggle& filter) {
            return Is_Valid_Field(*filter.field) ? Help_Label_Or(*filter.field, filter.fallback) : std::string(vocab::Get(filter.fallback));
        }

        // Print_Weapon_Mod_Help_Description: the general modifications paragraph, then the one for
        // this modification, under its title.
        void Speak_Mod_Help_(int16_t mod) {
            help::s_help_text general;
            help::s_help_text specific;
            if (!help::Read_Help_Text((int16_t)(400 + mod), &specific)) {
                speech::Say(vocab::Get(vocab::VOCAB_NO_TOOLTIP), true);
                return;
            }
            std::string body = help::Read_Help_Text(400, &general) ? general.body + " " + specific.body : specific.body;
            speech::Say(specific.title + ". " + body, true);
        }

        std::string On_Off_(bool on) {
            return vocab::Get(on ? vocab::VOCAB_STATE_ON : vocab::VOCAB_STATE_OFF);
        }

        // The picker as it was when a press was requested; the poll speaks what the press changed.
        struct s_weapons_snapshot {
            int16_t weapon;
            int16_t arc;
            int16_t rack;
            uint16_t mods;
            uint8_t filters[4];
            std::string cost_space;
        };

        s_weapons_snapshot Weapons_Snapshot_() {
            s_weapons_snapshot snapshot;
            snapshot.weapon = Chosen_Weapon_();
            snapshot.arc = Current_Arc_();
            snapshot.rack = (int16_t)DESBOX::weapon_replacement_rack;
            snapshot.mods = DESBOX::weapon_replacement_mods;
            for (int32_t i = 0; i < 4; ++i) {
                snapshot.filters[i] = *weapon_filter_toggles[i].status;
            }
            snapshot.cost_space = snapshot.weapon > 0 ? Weapon_Row_Cost_Space_(DESBOX::field_item_chosen) : std::string();
            return snapshot;
        }

        std::string Weapons_Change_Text_(const s_weapons_snapshot& before, const s_weapons_snapshot& after) {
            std::vector<std::string> parts;
            bool racks = after.weapon > 0 && DESIGN::Weapon_Needs_Racks(after.weapon) != 0;
            if (after.weapon != before.weapon) {
                if (after.weapon > 0) {
                    parts.push_back(vocab::Format(vocab::VOCAB_DESIGN_WEAPON_SELECTED, Weapon_Name_(after.weapon)));
                }
            } else {
                if (racks && after.rack != before.rack) {
                    parts.push_back(Rack_Name_(after.rack));
                } else if (!racks && after.arc != before.arc) {
                    parts.push_back(Arc_Name_(after.arc));
                }
                for (int16_t mod = 1; mod < WEAPON_MOD_COUNT; ++mod) {
                    bool was_on = ((before.mods >> mod) & 1) != 0;
                    bool is_on = ((after.mods >> mod) & 1) != 0;
                    if (was_on != is_on) {
                        parts.push_back(Mod_Name_(mod) + " " + On_Off_(is_on));
                    }
                }
            }
            for (int32_t i = 0; i < 4; ++i) {
                if (after.filters[i] != before.filters[i]) {
                    parts.push_back(Filter_Name_(weapon_filter_toggles[i]) + " " + On_Off_(after.filters[i] != 0));
                }
            }
            if (after.weapon > 0 && (!parts.empty() || after.cost_space != before.cost_space)) {
                parts.push_back(after.cost_space);
            }
            return Join_Values(parts);
        }

        s_weapons_snapshot weapons_before;
        bool weapons_change_pending = false;

        // Presses that close the picker are left to the design screen's poll.
        void Note_Weapons_Press_() {
            weapons_before = Weapons_Snapshot_();
            weapons_change_pending = true;
        }

        void Weapons_Press_(int16_t field_idx) {
            Note_Weapons_Press_();
            navigator::Request_Field_Activation(field_idx);
        }

        void Activate_Weapon_Row_(int16_t row) {
            // The first row, or the selected one again, fits the choice and closes the picker.
            if (row != 0 && row != DESBOX::field_item_chosen) {
                Note_Weapons_Press_();
            }
            Activate_Picker_Row_(row, false);
        }

        void Weapons_Picker_Poll_(bool just_attached) {
            Poll_Pending_Picker_Row_(just_attached);
            if (!weapons_change_pending || Picker_Row_Pending_() || navigator::Field_Activation_Pending()) {
                return;
            }
            weapons_change_pending = false;
            // Back from a message box the screen is announced again first, so the change queues after it.
            speech::Say(Weapons_Change_Text_(weapons_before, Weapons_Snapshot_()), !just_attached);
        }

        graph::NodeVtable Weapons_Field_Vtable_(int16_t field_idx, const std::string& label, const graph::ControlType* type) {
            graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ label });
            vtable.Type = type;
            vtable.StateText = []() { return std::string(); };
            vtable.OnActivate = [field_idx]() { Weapons_Press_(field_idx); };
            vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
            return vtable;
        }

        void Add_Weapons_Node_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            const graph::NodeVtable& vtable, int16_t field_idx) {
            if (field_idx > 0) {
                context->node_fields[key] = field_idx;
            }
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        // Every weapon, not just the ten rows in view: a sighted player scrolls through them all.
        void Weapons_List_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            std::string first_weapon_key;
            for (int16_t row = 0; row < MOX::scroll_bar->total_rows; ++row) {
                int16_t weapon = DESBOX::Weapon_Index(row);
                if (weapon < 0) {
                    continue;
                }
                std::string label = weapon == 0 ? H_Text(0x32) : Weapon_Name_(weapon);
                graph::NodeVtable vtable = Picker_Row_Vtable_(label, Weapon_Row_Details_(row),
                    [row]() { Activate_Weapon_Row_(row); },
                    [row]() { Activate_Picker_Row_(row, true); });
                vtable.StateText = []() { return std::string(); };
                Set_Picker_Tooltip_(&vtable, weapon == 0 ? 0x2B2 : (int16_t)TECHDATA::weapons[weapon].tech_app_id);
                if (DESBOX::field_item_chosen == row && row > 0) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_SELECTED, Selected);
                }
                // Print_Main_Weapon_Box dims a weapon one of which won't fit in the space left.
                if (row > 0 && MOX::design->replacement_item_unlocked[row] == 0) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_UNAVAILABLE, Enabled);
                }
                std::string key = "design_weapons/weapon/" + std::to_string(weapon);
                if (weapon > 0 && first_weapon_key.empty()) {
                    first_weapon_key = key;
                }
                Add_Weapons_Node_(builder, context, key, vtable, Picker_Row_Field_(row));
            }
            // With nothing selected, start on the first weapon rather than the row that empties the slot.
            if (Chosen_Weapon_() < 0 && !first_weapon_key.empty()) {
                builder->SetStart(graph::ControlId::Structural(first_weapon_key));
            }
        }

        // Beams take a firing arc and missiles a rack size; other weapons have neither to choose.
        void Weapons_Mount_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t weapon = Chosen_Weapon_();
            if (weapon < 0) {
                return;
            }
            bool racks = DESIGN::Weapon_Needs_Racks(weapon) != 0;
            const int16_t* fields = racks ? MOX::weapon_rack_field : MOX::weapon_arc_field;
            builder->BeginStop("design_weapons/mount");
            builder->PushContext(vocab::Get(racks ? vocab::VOCAB_DESIGN_RACKS : vocab::VOCAB_DESIGN_FIRING_ARC));
            int16_t current = racks ? (int16_t)DESBOX::weapon_replacement_rack : Current_Arc_();
            for (int16_t i = 0; i < 5; ++i) {
                if (!Is_Valid_Field(fields[i])) {
                    continue;
                }
                graph::NodeVtable vtable = Weapons_Field_Vtable_(fields[i], racks ? Rack_Name_(i) : Arc_Name_(i), &choice_type);
                if (i == current) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_SELECTED, Selected);
                }
                Add_Weapons_Node_(builder, context, "design_weapons/mount/" + std::to_string(i), vtable, fields[i]);
            }
            builder->PopContext();
        }

        // Add_Replacement_Weapon_Fields gives only the modifications the selected weapon can take a
        // field; Backspace on one shows its description.
        void Weapons_Mods_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            bool any = false;
            for (int16_t mod = 1; mod < WEAPON_MOD_COUNT; ++mod) {
                int16_t field_idx = MOX::weapon_mod_fields[mod];
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }
                if (!any) {
                    builder->BeginStop("design_weapons/mods");
                    builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_MODIFICATIONS));
                    any = true;
                }
                graph::NodeVtable vtable = Weapons_Field_Vtable_(field_idx, Mod_Name_(mod), &toggle_type);
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(On_Off_(MOX::weapon_mod_field_status[mod] == 1)));
                vtable.Announcements.back().Kind = Value;
                vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
                vtable.OnTooltip = [mod]() { Speak_Mod_Help_(mod); };
                Add_Weapons_Node_(builder, context, "design_weapons/mod/" + std::to_string(mod), vtable, field_idx);
            }
            if (any) {
                builder->PopContext();
            }
        }

        void Weapons_Filters_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int32_t i = 0; i < 4; ++i) {
                const s_weapon_filter_toggle& filter = weapon_filter_toggles[i];
                int16_t field_idx = *filter.field;
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }
                graph::NodeVtable vtable = Weapons_Field_Vtable_(field_idx, Filter_Name_(filter), &toggle_type);
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(On_Off_(*filter.status != 0)));
                vtable.Announcements.back().Kind = Value;
                Add_Weapons_Node_(builder, context, "design_weapons/filter/" + std::to_string(i), vtable, field_idx);
            }
        }

        void Weapons_Controls_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            // Accept is a hidden field while the choice isn't valid; pressing it says why.
            int16_t accept = MOX::replacement_box_accept_button;
            if (Is_Valid_Field(accept)) {
                graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ vocab::Get(vocab::VOCAB_ACCEPT) });
                vtable.Type = &button_type;
                vtable.OnActivate = [accept]() { navigator::Request_Field_Activation(accept); };
                if (fields::fields[accept].field_type != FIELD_TYPE_BUTTON) {
                    Add_Picker_State_(&vtable, vocab::VOCAB_STATE_UNAVAILABLE, Enabled);
                }
                Add_Weapons_Node_(builder, context, "design_weapons/accept", vtable, accept);
            }
            Add_Picker_Cancel_(builder, context, "design_weapons/cancel", 0x2B0);
        }

        void Weapons_Picker_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            builder->BeginStop("design_weapons/list");
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_WEAPONS));
            Weapons_List_Stop_(builder, context);
            builder->PopContext();

            Weapons_Mount_Stop_(builder, context);
            Weapons_Mods_Stop_(builder, context);

            builder->BeginStop("design_weapons/filters");
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_FILTERS));
            Weapons_Filters_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("design_weapons/controls");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_CONTROLS));
            Weapons_Controls_Stop_(builder, context);
            builder->PopContext();
        }

    } // namespace

    const s_screen_recipe design_generic_picker_recipe = { "design_generic_picker", Generic_Picker_Is_Active_,
        Generic_Picker_Instance_Key_, Generic_Picker_Build_, Generic_Picker_Name_ };
    const s_screen_recipe design_specials_picker_recipe = { "design_specials_picker", Specials_Picker_Is_Active_,
        Specials_Picker_Instance_Key_, Specials_Picker_Build_, Specials_Picker_Name_, Specials_Picker_Poll_ };
    const s_screen_recipe design_weapons_picker_recipe = { "design_weapons_picker", Weapons_Picker_Is_Active_,
        Weapons_Picker_Instance_Key_, Weapons_Picker_Build_, Weapons_Picker_Name_, Weapons_Picker_Poll_ };

} // namespace access::screens
