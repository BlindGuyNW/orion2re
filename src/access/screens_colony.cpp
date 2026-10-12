#include "pch.h"

#include "access/screens_common.h"

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
            if (!Is_Valid_Field(field_idx)) {
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
                if (Is_Valid_Field(COLONY::job_fields[job])) {
                    int16_t field_idx = COLONY::job_fields[job];
                    header.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
                }
                if (!pops.empty()) {
                    context->node_points[header_key] = Colony_Pop_Point_(job, (int16_t)(pops.size() - 1), (int16_t)pops.size());
                }
                builder->AddItem(graph::ControlId::Structural(header_key), header);

                for (int16_t slot = 0; slot < (int16_t)pops.size(); ++slot) {
                    std::string key = header_key + "/" + std::to_string(slot);
                    graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ Colony_Pop_Text_(COLONY::colony_handle, pops[slot]) });
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
            if (Is_Valid_Field(transport)) {
                Add_Field_Item(builder, context, "colony/transport", transport, Help_Label_Or(transport, vocab::VOCAB_COLONY_TRANSPORT));
            }
        }

        // A hidden field that opens a breakdown, read as the line it stands for.
        void Add_Colony_Readout_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            int16_t field_idx, std::function<std::string()> text) {
            graph::NodeVtable vtable;
            vtable.Type = &button_type;
            vtable.Announcements.push_back(graph::NodeAnnouncement(text, true, Label));
            if (Is_Valid_Field(field_idx)) {
                vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
                vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
                context->node_fields[key] = field_idx;
            }
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        void Build_Colony_Summary_(graph::GraphBuilder* builder, s_build_context* context) {
            builder->AddItem(graph::ControlId::Structural("colony/title"), Info_Vtable(std::vector<std::string>{ Colony_Name_() }));
            builder->AddItem(graph::ControlId::Structural("colony/population"),
                Info_Vtable(std::vector<std::string>{ Colony_Population_Text_() }));
            std::string status = Colony_Status_Text_();
            if (!status.empty()) {
                builder->AddItem(graph::ControlId::Structural("colony/status"), Info_Vtable(std::vector<std::string>{ status }));
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
            if (Is_Valid_Field(change)) {
                Add_Field_Item(builder, context, "colony/change", change, Help_Label_Or(change, vocab::VOCAB_COLONY_CHANGE));
            }
            int16_t buy = COLONY::colony_fields[3];
            if (Is_Valid_Field(buy)) {
                Add_Field_Item(builder, context, "colony/buy", buy, Help_Label_Or(buy, vocab::VOCAB_COLONY_BUY));
            }
            int16_t auto_build = COLONY::colony_fields[18];
            if (Is_Valid_Field(auto_build)) {
                // Its help record is the production box's ("Current Production"), not the toggle's.
                graph::NodeVtable vtable = Field_Vtable(auto_build, vocab::Get(vocab::VOCAB_COLONY_AUTO_BUILD));
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
                if (planet_idx < 0 || !Is_Valid_Field(field_idx)) {
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
                if (MOX::planet[planet_idx].planet_type == PLANET_TYPE_PLANET && Is_Valid_Field(summary_field)) {
                    vtable.OnSecondary = [summary_field]() { navigator::Request_Field_Activation(summary_field); };
                }
                vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
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
            graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ text::Normalize_Game_Text(name) });
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
                    if (building_id == 0 || !Is_Valid_Field(field_idx)) {
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
                if (building_id < 0 || !Is_Valid_Field(field_idx)) {
                    continue;
                }
                Add_Colony_Building_(builder, context, "colony/satellite/" + std::to_string(building_id), field_idx,
                    COLONY::Real_Building_Name2(building_id), true, true);
            }
            // Print_Scanned_Military's line for each troop type; a press opens the troop popup.
            for (int16_t i = 0; i < 2; ++i) {
                int16_t field_idx = COLONY::military_fields[i];
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }
                char line[200];
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_D_S), (int)Shown_Colony_()->military[i], TECHDATA::units[i + 1].name);
                Add_Field_Item(builder, context, "colony/military/" + std::to_string(i), field_idx, text::Normalize_Game_Text(line));
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
                if (Is_Valid_Field(field_idx)) {
                    std::string label = Is_Offscreen(&fields::fields[field_idx]) ? std::string(vocab::Get(control.fallback))
                                                                                   : Help_Label_Or(field_idx, control.fallback);
                    Add_Field_Item(builder, context, control.key, field_idx, label);
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
                            Info_Vtable(std::vector<std::string>{ lines[i] }));
                    }
                }
            }
            if (Is_Valid_Field(COLLAND::fields)) {
                Add_Field_Item(builder, context, "landing/continue", COLLAND::fields, vocab::Get(vocab::VOCAB_CONTINUE));
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

    } // namespace

    const s_screen_recipe landing_recipe = { "landing", Landing_Is_Active_, Landing_Instance_Key_, Landing_Build_, Landing_Name_, Landing_Poll_ };
    const s_screen_recipe colony_recipe = { "colony", Colony_Is_Active_, Colony_Instance_Key_, Colony_Build_, Colony_Name_, Colony_Poll_ };

} // namespace access::screens
