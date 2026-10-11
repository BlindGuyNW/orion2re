#include "pch.h"

#include "access/help_text.h"

#include "access/text.h"

#include <map>

namespace access::help {

    namespace {

        struct s_cached_help_text {
            bool found;
            s_help_text text;
        };

        std::string cached_lbx_name;
        std::map<int16_t, s_cached_help_text> cache;

        // Reads directly through the VFS so lookups never touch the game's allocator blocks.
        bool Load_Help_Record_(const char* lbx_name, int16_t help_id, s_help_record* record) {
            s_vfs_file* file = vfs::fopen_lbx(lbx_name, 0);
            if (file == nullptr) {
                return false;
            }

            uint16_t record_count = 0;
            uint16_t record_size = 0;
            bool ok = vfs::fread(&record_count, sizeof(record_count), 1, file) == 1 &&
                vfs::fread(&record_size, sizeof(record_size), 1, file) == 1 &&
                record_size == sizeof(s_help_record) && help_id < (int32_t)record_count;

            if (ok) {
                int32_t offset = (int32_t)(sizeof(record_count) + sizeof(record_size)) + (int32_t)record_size * help_id;
                ok = vfs::fseek(file, offset, SEEK_SET) == 0 && vfs::fread(record, sizeof(*record), 1, file) == 1;
            }

            vfs::fclose(file);
            return ok;
        }

    } // namespace

    int16_t Find_Help_Id(int16_t x, int16_t y, int16_t x_end, int16_t y_end) {
        if (fields::help_list_active == 0 || fields::help_struct_pointer == nullptr) {
            return -1;
        }

        int16_t center_x = (int16_t)(x + (x_end - x) / 2);
        int16_t center_y = (int16_t)(y + (y_end - y) / 2);

        for (int16_t i = 0; i < fields::help_list_count; ++i) {
            const s_help_box* box = &fields::help_struct_pointer[i];
            if (box->help_id < 0) {
                continue;
            }
            if (center_x >= box->x1 && center_x <= box->x2 && center_y >= box->y1 && center_y <= box->y2) {
                return box->help_id;
            }
        }

        return -1;
    }

    bool Read_Help_Text(int16_t help_id, s_help_text* help_text) {
        if (help_id < 0) {
            return false;
        }

        char lbx_name[20];
        TEXTBOX::Get_Help_Lbx_Name(lbx_name, sizeof(lbx_name));
        if (cached_lbx_name != lbx_name) {
            cache.clear();
            cached_lbx_name = lbx_name;
        }

        auto it = cache.find(help_id);
        if (it == cache.end()) {
            s_cached_help_text entry = {};
            s_help_record record;
            if (Load_Help_Record_(lbx_name, help_id, &record)) {
                record.title[sizeof(record.title) - 1] = '\0';
                record.body[sizeof(record.body) - 1] = '\0';
                entry.found = true;
                entry.text.title = text::Normalize_Game_Text(record.title);
                entry.text.body = text::Normalize_Game_Text(record.body);
            }
            it = cache.emplace(help_id, entry).first;
        }

        if (!it->second.found) {
            return false;
        }
        *help_text = it->second.text;
        return true;
    }

} // namespace access::help
