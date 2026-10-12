#include "pch.h"

#include "access/screens_common.h"

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

        // ---- Ship design ---------------------------------------------------------------------------

        // The design screen's own loop only: the replacement pickers and message boxes it opens run
        // nested loops that leave DESIGN::awaiting_input clear.
        bool Design_Is_Active_() {
            return MOX::current_screen == SCREEN_DESIGN && DESIGN::awaiting_input != 0 && MOX::design != nullptr;
        }

        std::string Design_Instance_Key_() {
            return "design";
        }

        std::string Design_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_DESIGN);
        }

        // The stat lines under each system start with a '^', which the font draws as a bullet.
        std::string Design_Stat_Line_(const char* line) {
            while (*line == '^' || *line == ' ') {
                ++line;
            }
            return text::Normalize_Game_Text(line);
        }

        // Print_Current_Design formats its stat lines from H_Message printf formats.
        std::string Design_Stat_(int16_t message_id, int32_t value) {
            char line[200];
            snprintf(line, sizeof(line), HAROLD::H_Message(message_id), value);
            return Design_Stat_Line_(line);
        }

        std::string Design_Labelled_(int16_t message_id, const std::string& value) {
            return H_Text(message_id) + " " + value;
        }

        std::string Design_Shield_Name_() {
            int16_t shield = MOX::design->shield;
            return shield == SHIELD_NO_SHIELD ? H_Text(HESTR_036_NO_SHIELD) : text::Normalize_Game_Text(TECHDATA::shields[shield].name);
        }

        std::string Design_Shield_Details_() {
            if (MOX::design->shield == SHIELD_NO_SHIELD) {
                return Design_Stat_Line_(HAROLD::H_Message(HESTR_057_NO_STRENGTH)) + ", " +
                    Design_Stat_Line_(HAROLD::H_Message(HESTR_058_NO_DAMAGE_POINTS_BLOCK));
            }
            int32_t points = DESIGN::Current_Shield_Damage_Points_Blocked();
            char line[200];
            snprintf(line, sizeof(line), HAROLD::H_Message(HESTR_05A_D_DAMAGE_POINTS_BLOCKE), points, points == 1 ? "" : "s");
            return Design_Stat_(HESTR_059_D_STRENGTH, DESIGN::Shield_Strength()) + ", " + Design_Stat_Line_(line);
        }

        std::string Design_Computer_Name_() {
            return text::Normalize_Game_Text(TECHDATA::computers[MOX::design->computer_type].name);
        }

        struct s_design_bonuses {
            int16_t beam_attack;
            int16_t beam_defense;
            int16_t missile_evasion;
        };

        s_design_bonuses Design_Bonuses_() {
            s_ship_design local_design;
            DESIGN::Build_Design_Template(&local_design);
            s_design_bonuses bonuses;
            INITSHIP::Get_Design_Combat_Bonuses(local_design, &bonuses.beam_attack, &bonuses.beam_defense, &bonuses.missile_evasion);
            return bonuses;
        }

        // DESIGN::Weapon_Arc_String, read without JIM::Get_Text_Message's frame updates.
        std::string Design_Arc_Text_(uint16_t arc_flags) {
            int16_t message = -1;
            if (arc_flags & WEAPON_FIRING_ARC_FORWARD) {
                message = 3;
            } else if (arc_flags & WEAPON_FIRING_ARC_FORWARD_EXTENDED) {
                message = 4;
            } else if (arc_flags & WEAPON_FIRING_ARC_BACK_EXTENDED) {
                message = 6;
            } else if (arc_flags & WEAPON_FIRING_ARC_REAR) {
                message = 5;
            } else if (arc_flags & WEAPON_FIRING_ARC_360) {
                return "360";
            }
            std::string text;
            if (message >= 0) {
                help::Read_Text_Message("KENTEXT.LBX", message, &text);
            }
            return text;
        }

        // The count and name Print_Current_Design prints at the start of a weapon's row.
        std::string Design_Weapon_Name_(int16_t slot) {
            const s_current_design* design = MOX::design;
            int16_t type = design->weapon_type[slot];
            std::string name = text::Normalize_Game_Text(
                design->weapon_count[slot] < 2 ? TECHDATA::weapons[type].name : TECHDATA::weapons[type].name_plural);
            if (DESIGN::Weapon_Needs_Racks(type)) {
                name += " (" + std::to_string(design->weapon_ammo[slot]) + ")";
            }
            return std::to_string(design->weapon_count[slot]) + " " + name;
        }

        // DESIGN::Weapon_Mod_String draws the short forms, such as "Hv"; the full names say the same.
        std::string Design_Mods_Text_(uint16_t mods) {
            std::vector<std::string> names;
            for (int16_t i = 0; i < WEAPON_MOD_COUNT; ++i) {
                if ((mods >> i) & 1) {
                    names.push_back(text::Normalize_Game_Text(TECHDATA::weapon_modifications[i].name));
                }
            }
            return names.empty() ? H_Text(HESTR_064_NO_MODIFICATIONS) : Join_Values(names);
        }

        // The rest of the row: space and cost are drawn per weapon, never less than 1.
        std::string Design_Weapon_Details_(int16_t slot) {
            const s_current_design* design = MOX::design;
            char buffer[128];
            DESIGN::Design_Weapon_Damage_String(slot, buffer, sizeof(buffer), -1, -1);
            std::string damage = vocab::Format(vocab::VOCAB_DESIGN_DAMAGE, text::Normalize_Game_Text(buffer));
            std::string arc = Design_Arc_Text_((uint16_t)design->weapon_firing_arc[slot]);
            int32_t count = design->weapon_count[slot];
            int32_t space = count != 0 ? design->weapon_space[slot] / count : 0;
            int32_t cost = count != 0 ? design->weapon_cost[slot] / count : 0;
            return Join_Values({
                damage,
                arc.empty() ? std::string() : vocab::Format(vocab::VOCAB_DESIGN_ARC, arc),
                vocab::Format(vocab::VOCAB_DESIGN_SPACE_EACH, std::to_string(std::max<int32_t>(space, 1))),
                vocab::Format(vocab::VOCAB_DESIGN_COST_EACH, std::to_string(std::max<int32_t>(cost, 1))),
                Design_Mods_Text_(design->weapon_specials[slot]),
            });
        }

        // The prompt Print_Current_Design shows over the empty rows while the pointer is on them.
        std::string Design_Add_Text_(bool weapon) {
            if (weapon ? DESIGN::Space_For_Weapon_Replacement() : DESIGN::Space_For_System_Replacement()) {
                return H_Text(weapon ? HESTR_17F_ADD_NEW_WEAPON : HESTR_180_ADD_NEW_SPECIAL_SYSTEM);
            }
            if (DESIGN::printed_space_avail < 1) {
                return H_Text(weapon ? HESTR_01A_NOT_ENOUGH_SPACE_TO_AD : HESTR_01B_NOT_ENOUGH_SPACE_TO_AD);
            }
            return H_Text(weapon ? HESTR_018_NO_NEW_WEAPONS_CAN_BE_ : HESTR_019_NO_NEW_SPECIAL_SYSTEMS);
        }

        std::string Design_Picture_Text_() {
            if (MOX::design->ship_size == SHIP_SIZE_DOOM_STAR) {
                return std::string();
            }
            return vocab::Format(vocab::VOCAB_DESIGN_PICTURE, std::to_string(MOX::design->picture_type % 8 + 1));
        }

        std::string Design_Cost_Text_() {
            return Design_Labelled_(HESTR_185_COST, std::to_string(DESIGN::Printed_Design_Cost()));
        }

        std::string Design_Space_Text_() {
            return Design_Labelled_(HESTR_063_SPACE_AVAILABLE, std::to_string(DESIGN::printed_space_avail));
        }

        // The design as it was when a press was requested; the poll speaks what the press changed.
        struct s_design_item {
            std::string key;
            std::string text;
        };

        struct s_design_snapshot {
            std::string name;
            int16_t ship_size;
            int16_t picture;
            std::string shield;
            std::string computer;
            std::vector<s_design_item> weapons;
            std::vector<s_design_item> specials;
            std::string cost;
            std::string space;
            bool can_build;
        };

        s_design_snapshot Design_Snapshot_() {
            const s_current_design* design = MOX::design;
            s_design_snapshot snapshot;
            snapshot.name = text::Normalize_Game_Text(design->name);
            snapshot.ship_size = design->ship_size;
            snapshot.picture = design->picture_type;
            snapshot.shield = Design_Shield_Name_();
            snapshot.computer = Design_Computer_Name_();
            for (int16_t slot = 0; slot < 8; ++slot) {
                if (design->weapon_type[slot] > WEAPON_NO_WEAPONS) {
                    // A count change keeps the weapon; anything else makes it a different one.
                    std::string key = std::to_string(design->weapon_type[slot]) + "/" + std::to_string(design->weapon_firing_arc[slot]) +
                        "/" + std::to_string(design->weapon_specials[slot]) + "/" + std::to_string(design->weapon_ammo[slot]);
                    snapshot.weapons.push_back(s_design_item{ key, Design_Weapon_Name_(slot) });
                }
                int16_t device = design->special_devices[slot];
                if (device > SPECIAL_NO_SPECIAL) {
                    snapshot.specials.push_back(s_design_item{ std::to_string(device), text::Normalize_Game_Text(TECHDATA::specials[device].name) });
                }
            }
            snapshot.cost = Design_Cost_Text_();
            snapshot.space = Design_Space_Text_();
            // Add_Design_Buttons leaves Build out, drawing it dimmed, under this same test.
            snapshot.can_build = design->hull_space >= design->space_used;
            return snapshot;
        }

        void Design_Item_Changes_(const std::vector<s_design_item>& before, const std::vector<s_design_item>& after,
            std::vector<std::string>* parts) {
            std::vector<bool> matched(before.size(), false);
            for (const s_design_item& item : after) {
                size_t found = before.size();
                for (size_t i = 0; i < before.size() && found == before.size(); ++i) {
                    if (!matched[i] && before[i].key == item.key) {
                        found = i;
                    }
                }
                if (found == before.size()) {
                    parts->push_back(vocab::Format(vocab::VOCAB_DESIGN_ADDED, item.text));
                    continue;
                }
                matched[found] = true;
                if (before[found].text != item.text) {
                    parts->push_back(item.text);
                }
            }
            for (size_t i = 0; i < before.size(); ++i) {
                if (!matched[i]) {
                    parts->push_back(vocab::Format(vocab::VOCAB_DESIGN_REMOVED, before[i].text));
                }
            }
        }

        std::string Design_Change_Text_(const s_design_snapshot& before, const s_design_snapshot& after) {
            std::vector<std::string> parts;
            if (after.ship_size != before.ship_size) {
                parts.push_back(text::Normalize_Game_Text(TECHDATA::hull_data[after.ship_size].name));
            } else if (after.picture != before.picture) {
                parts.push_back(Design_Picture_Text_());
            }
            if (after.name != before.name) {
                parts.push_back(after.name);
            }
            if (after.shield != before.shield) {
                parts.push_back(after.shield);
            }
            if (after.computer != before.computer) {
                parts.push_back(after.computer);
            }
            Design_Item_Changes_(before.weapons, after.weapons, &parts);
            Design_Item_Changes_(before.specials, after.specials, &parts);
            if (after.cost != before.cost || after.space != before.space) {
                parts.push_back(after.cost);
                parts.push_back(after.space);
            }
            if (after.can_build != before.can_build) {
                parts.push_back(vocab::Get(after.can_build ? vocab::VOCAB_DESIGN_BUILD_AVAILABLE : vocab::VOCAB_DESIGN_BUILD_UNAVAILABLE));
            }
            return Join_Values(parts);
        }

        s_design_snapshot design_before;
        bool design_change_pending = false;
        bool design_expects_change = false;

        // Presses that open a picker or a message box expect nothing; the rest say so when the game
        // made no change, such as a picture arrow on a Doom Star.
        void Design_Press_(int16_t field_idx, bool expects_change) {
            if (!Is_Valid_Field(field_idx)) {
                return;
            }
            design_before = Design_Snapshot_();
            design_change_pending = true;
            design_expects_change = expects_change;
            navigator::Request_Field_Activation(field_idx);
        }

        // Add_Design_Buttons leaves out the plus button when one more would not fit, and the minus
        // button at a count of 1, drawing them dimmed instead.
        void Design_Adjust_Weapon_(int16_t slot, int sign) {
            int16_t field_idx = sign > 0 ? MOX::weapons_count_plus_button[slot] : MOX::weapons_count_minus_button[slot];
            if (!Is_Valid_Field(field_idx)) {
                speech::Say(vocab::Get(sign > 0 ? vocab::VOCAB_DESIGN_NO_ROOM : vocab::VOCAB_DESIGN_AT_MINIMUM), true);
                return;
            }
            Design_Press_(field_idx, true);
        }

        // A node over a design field whose press the poll reports on.
        graph::NodeVtable Design_Field_Vtable_(int16_t field_idx, const std::string& label, bool expects_change) {
            graph::NodeVtable vtable = Field_Vtable(field_idx, label);
            vtable.OnActivate = [field_idx, expects_change]() { Design_Press_(field_idx, expects_change); };
            vtable.StateText = []() { return std::string(); };
            return vtable;
        }

        void Add_Design_Field_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            graph::NodeVtable vtable, int16_t field_idx) {
            context->node_fields[key] = field_idx;
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        void Add_Design_Info_(graph::GraphBuilder* builder, const std::string& key, const std::string& text) {
            builder->AddItem(graph::ControlId::Structural(key), Info_Vtable(std::vector<std::string>{ text }));
        }

        // A fitted weapon or special system: Enter opens its picker, and Backspace shows its
        // description, as a right-click does.
        graph::NodeVtable Design_Row_Vtable_(int16_t field_idx, const std::string& label, const std::string& details) {
            graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ label, details });
            vtable.Type = &button_type;
            vtable.StateText = []() { return std::string(); };
            if (Is_Valid_Field(field_idx)) {
                vtable.OnActivate = [field_idx]() { Design_Press_(field_idx, false); };
                vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
                vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
            }
            return vtable;
        }

        void Design_Ship_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t name_field = MOX::design_name_field;
            if (Is_Valid_Field(name_field)) {
                Add_Field_Item(builder, context, "design/name", name_field, Help_Label_Or(name_field, vocab::VOCAB_DESIGN_NAME));
            }
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_HULL));
            for (int16_t size = 0; size < SHIP_SIZE_COUNT; ++size) {
                int16_t field_idx = MOX::design_ship_size_field[size];
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }
                // A size the game won't allow is a hidden field whose press explains why.
                bool allowed = DESIGN::add_size_button_flag[size] == 1;
                graph::NodeVtable vtable = Design_Field_Vtable_(field_idx, text::Normalize_Game_Text(TECHDATA::hull_data[size].name), allowed);
                vtable.Type = &choice_type;
                if (!allowed) {
                    vtable.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(vocab::VOCAB_STATE_UNAVAILABLE)));
                    vtable.Announcements.back().Kind = Enabled;
                }
                Add_Design_Field_(builder, context, "design/size/" + std::to_string(size), vtable, field_idx);
            }
            builder->PopContext();
            std::string picture = Design_Picture_Text_();
            if (!picture.empty()) {
                Add_Design_Info_(builder, "design/picture", picture);
            }
            if (Is_Valid_Field(MOX::ship_icon_lf_button)) {
                Add_Design_Field_(builder, context, "design/picture/previous",
                    Design_Field_Vtable_(MOX::ship_icon_lf_button, vocab::Get(vocab::VOCAB_DESIGN_PREVIOUS_PICTURE), true), MOX::ship_icon_lf_button);
            }
            if (Is_Valid_Field(MOX::ship_icon_rt_button)) {
                Add_Design_Field_(builder, context, "design/picture/next",
                    Design_Field_Vtable_(MOX::ship_icon_rt_button, vocab::Get(vocab::VOCAB_DESIGN_NEXT_PICTURE), true), MOX::ship_icon_rt_button);
            }
        }

        void Design_Systems_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            const s_current_design* design = MOX::design;
            Add_Design_Info_(builder, "design/drive", Join_Values({
                text::Normalize_Game_Text(TECHDATA::drives[design->ftl_type].name),
                Design_Stat_(HESTR_17E_D_PARSECS_TURN, TECHDATA::drives[design->ftl_type].warp_speed),
                Design_Stat_(HESTR_054_D_COMBAT_SPEED, DESIGN::Current_Design_Combat_Speed(MOX::design)),
            }));
            Add_Design_Info_(builder, "design/armor", Join_Values({
                text::Normalize_Game_Text(TECHDATA::armor[design->armor_type].name),
                Design_Stat_(HESTR_055_D_STRUCTURE_POINTS, DESIGN::Current_Design_Structural_Points()),
                Design_Stat_(HESTR_056_D_ARMOR_POINTS, DESIGN::Current_Design_Armor_Absorption()),
            }));
            // Enter on the shield or computer opens its picker, or a message saying why it can't.
            const struct {
                int16_t field_idx;
                const char* key;
                std::string label;
                std::string details;
            } systems[] = {
                { DESIGN::design_shield_replacement_field, "design/shield", Design_Shield_Name_(), Design_Shield_Details_() },
                { DESIGN::design_computer_replacement_field, "design/computer", Design_Computer_Name_(),
                    Design_Stat_(HESTR_05B_D_BEAM_ATTACK, Design_Bonuses_().beam_attack) },
            };
            for (const auto& system : systems) {
                if (!Is_Valid_Field(system.field_idx)) {
                    Add_Design_Info_(builder, system.key, system.label + ", " + system.details);
                    continue;
                }
                graph::NodeVtable vtable = Design_Field_Vtable_(system.field_idx, system.label, false);
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(system.details));
                vtable.Announcements.back().Kind = Value;
                Add_Design_Field_(builder, context, system.key, vtable, system.field_idx);
            }
        }

        // Left and Right on a weapon change its count. Every empty row opens the picker on the
        // first empty slot, so they are listed as one.
        void Design_Weapons_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            const s_current_design* design = MOX::design;
            int16_t add_field = -1;
            bool any = false;
            for (int16_t slot = 0; slot < 8; ++slot) {
                int16_t field_idx = MOX::weapons_field[slot];
                if (design->weapon_type[slot] <= WEAPON_NO_WEAPONS) {
                    if (add_field < 0 && Is_Valid_Field(field_idx)) {
                        add_field = field_idx;
                    }
                    continue;
                }
                any = true;
                graph::NodeVtable vtable = Design_Row_Vtable_(field_idx, Design_Weapon_Name_(slot), Design_Weapon_Details_(slot));
                vtable.OnAdjust = [slot](int sign, bool) { Design_Adjust_Weapon_(slot, sign); };
                Add_Design_Field_(builder, context, "design/weapon/" + std::to_string(slot), vtable, field_idx);
            }
            if (add_field >= 0) {
                any = true;
                Add_Design_Field_(builder, context, "design/weapon/add", Design_Field_Vtable_(add_field, Design_Add_Text_(true), false), add_field);
            }
            if (!any) {
                Add_Design_Info_(builder, "design/weapon/none", vocab::Get(vocab::VOCAB_DESIGN_NONE));
            }
        }

        void Design_Specials_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            const s_current_design* design = MOX::design;
            int16_t add_field = -1;
            bool any = false;
            for (int16_t slot = 0; slot < 8; ++slot) {
                int16_t field_idx = MOX::special_system_field[slot];
                int16_t device = design->special_devices[slot];
                if (device <= SPECIAL_NO_SPECIAL) {
                    if (add_field < 0 && Is_Valid_Field(field_idx)) {
                        add_field = field_idx;
                    }
                    continue;
                }
                any = true;
                const char* description = MOX::special_desc_string[device];
                graph::NodeVtable vtable = Design_Row_Vtable_(field_idx, text::Normalize_Game_Text(TECHDATA::specials[device].name),
                    description != nullptr ? text::Normalize_Game_Text(description) : std::string());
                Add_Design_Field_(builder, context, "design/special/" + std::to_string(slot), vtable, field_idx);
            }
            if (add_field >= 0) {
                any = true;
                Add_Design_Field_(builder, context, "design/special/add", Design_Field_Vtable_(add_field, Design_Add_Text_(false), false), add_field);
            }
            if (!any) {
                Add_Design_Info_(builder, "design/special/none", vocab::Get(vocab::VOCAB_DESIGN_NONE));
            }
        }

        void Design_Totals_Stop_(graph::GraphBuilder* builder) {
            s_design_bonuses bonuses = Design_Bonuses_();
            Add_Design_Info_(builder, "design/cost", Design_Cost_Text_());
            Add_Design_Info_(builder, "design/space", Design_Space_Text_());
            Add_Design_Info_(builder, "design/hull_space", vocab::Format(vocab::VOCAB_DESIGN_HULL_SPACE, std::to_string(MOX::design->hull_space)));
            Add_Design_Info_(builder, "design/beam_defense", Design_Labelled_(HESTR_05C_BEAM_DEFENSE,
                (bonuses.beam_defense >= 0 ? "+" : "") + std::to_string(bonuses.beam_defense)));
            Add_Design_Info_(builder, "design/missile_evasion", Design_Labelled_(HESTR_05D_MISSILE_EVASION,
                std::to_string(bonuses.missile_evasion) + "%"));
        }

        void Design_Controls_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            // Build is left out, and drawn dimmed, while the design uses more space than the hull has.
            if (Is_Valid_Field(MOX::design_build_button)) {
                Add_Field_Item(builder, context, "design/build", MOX::design_build_button, Field_Label(MOX::design_build_button));
            } else {
                graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ vocab::Get(vocab::VOCAB_DESIGN_BUILD) });
                vtable.Type = &button_type;
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(vocab::Get(vocab::VOCAB_STATE_UNAVAILABLE)));
                vtable.Announcements.back().Kind = Enabled;
                builder->AddItem(graph::ControlId::Structural("design/build"), vtable);
            }
            if (Is_Valid_Field(MOX::design_clear_button)) {
                Add_Design_Field_(builder, context, "design/clear",
                    Design_Field_Vtable_(MOX::design_clear_button, Field_Label(MOX::design_clear_button), true), MOX::design_clear_button);
            }
            if (Is_Valid_Field(MOX::design_cancel_button)) {
                Add_Field_Item(builder, context, "design/cancel", MOX::design_cancel_button, Field_Label(MOX::design_cancel_button));
            }
        }

        void Design_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            builder->BeginStop("design/ship");
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_SHIP));
            Design_Ship_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("design/systems");
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_SYSTEMS));
            Design_Systems_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("design/weapons");
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_WEAPONS));
            Design_Weapons_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("design/specials");
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_SPECIAL_SYSTEMS));
            Design_Specials_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("design/totals");
            builder->PushContext(vocab::Get(vocab::VOCAB_DESIGN_TOTALS));
            Design_Totals_Stop_(builder);
            builder->PopContext();

            builder->BeginStop("design/controls");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_CONTROLS));
            Design_Controls_Stop_(builder, context);
            builder->PopContext();
        }

        // Speaks what a press changed once the game has acted on it, including after a picker it
        // opened has closed, and that nothing changed when the game refused it silently.
        void Design_Poll_(bool just_attached) {
            if (!design_change_pending || navigator::Field_Activation_Pending()) {
                return;
            }
            design_change_pending = false;
            std::string text = Design_Change_Text_(design_before, Design_Snapshot_());
            if (text.empty() && design_expects_change) {
                text = vocab::Get(vocab::VOCAB_DESIGN_NO_CHANGE);
            }
            // Back from a picker the screen is announced again first, so the change queues after it.
            speech::Say(text, !just_attached);
        }

    } // namespace

    const s_screen_recipe design_recipe = { "design", Design_Is_Active_, Design_Instance_Key_, Design_Build_, Design_Name_, Design_Poll_ };

} // namespace access::screens
