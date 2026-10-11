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

        std::map<std::string, std::string> message_cache;

        // Reads one record of a records-type LBX entry directly through the VFS, so lookups never
        // touch the game's allocator blocks. A record larger than the buffer is truncated to it.
        bool Load_Lbx_Record_(const char* lbx_name, int16_t entry, int32_t record_idx, void* buffer, uint16_t size, bool exact) {
            s_vfs_file* file = vfs::fopen_lbx(lbx_name, entry);
            if (file == nullptr) {
                return false;
            }

            uint16_t record_count = 0;
            uint16_t record_size = 0;
            bool ok = vfs::fread(&record_count, sizeof(record_count), 1, file) == 1 &&
                vfs::fread(&record_size, sizeof(record_size), 1, file) == 1 &&
                (exact ? record_size == size : record_size > 0) && record_idx >= 0 && record_idx < (int32_t)record_count;

            if (ok) {
                int32_t offset = (int32_t)(sizeof(record_count) + sizeof(record_size)) + (int32_t)record_size * record_idx;
                uint16_t read_size = record_size < size ? record_size : size;
                ok = vfs::fseek(file, offset, SEEK_SET) == 0 && vfs::fread(buffer, read_size, 1, file) == 1;
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
            if (Load_Lbx_Record_(lbx_name, 0, help_id, &record, (uint16_t)sizeof(record), true)) {
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

    bool Read_Text_Message(const char* lbx_name, int16_t message_index, std::string* text) {
        uint16_t language = (uint8_t)MOX::settings.language;
        if (language > 5) {
            language = 0;
        }
        std::string key = std::string(lbx_name) + "/" + std::to_string(message_index) + "/" + std::to_string(language);
        auto it = message_cache.find(key);
        if (it == message_cache.end()) {
            char buffer[100] = {};
            std::string found;
            if (Load_Lbx_Record_(lbx_name, (int16_t)(message_index * 6 + language), 0, buffer, (uint16_t)(sizeof(buffer) - 1), false)) {
                found = text::Normalize_Game_Text(buffer);
            }
            it = message_cache.emplace(key, found).first;
        }
        *text = it->second;
        return !text->empty();
    }

} // namespace access::help
