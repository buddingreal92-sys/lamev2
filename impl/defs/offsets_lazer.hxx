#pragma once

#include <cstdint>
#include <string>

namespace offsets::lazer {

    inline constexpr const char* anchor_pattern =
        "00 00 80 44 00 00 40 44 00 00 00 00 ? ? ? ? 00 00 00 00";

    struct table_t {
        std::string osu_version = "2026.804.2";

        uint32_t api_access_game = 784;
        uint32_t ext_link_opener_api = 536;
        uint32_t game_base_beatmap_clock = 1232;
        uint32_t game_base_beatmap = 1104;
        uint32_t game_base_api = 1080;
        uint32_t game_screen_stack = 1552;
        uint32_t screen_stack_stack = 800;
        uint32_t framed_clock_final_source = 528;
        uint32_t framed_clock_current_time = 48;
        uint32_t working_map_info = 8;
        uint32_t working_map_set_info = 16;
        uint32_t map_info_online_id = 140;
        uint32_t map_info_hash = 80;
        uint32_t map_info_difficulty = 40;
        uint32_t set_info_online_id = 48;
        uint32_t submitting_player_api = 1264;
        uint32_t player_api = 1008;
        uint32_t player_drawable_ruleset = 1112;
        uint32_t bindable_value = 0x20;
        uint32_t bindable_number_value = 0x40;
        uint32_t drawable_osu_beatmap = 896;
        uint32_t beatmap_hit_objects = 48;
        uint32_t list_items = 8;
        uint32_t list_size = 0x10;
        uint32_t array_first_element = 0x10;
        uint32_t hit_object_start_time_bindable = 0x10;
        uint32_t osu_hit_object_position_xy = 0x58;
        uint32_t hit_object_nested_objects = 40;
        uint32_t hit_object_has_duration = 0xd0;
        uint32_t slider_path_wrapper = 0xe0;
        uint32_t path_ctrl_points_list = 0x20;

        [[nodiscard]] bool has_hitobject_offsets( ) const {
            return drawable_osu_beatmap != 0 && beatmap_hit_objects != 0 &&
                list_items != 0 && list_size != 0 && hit_object_start_time_bindable != 0 &&
                bindable_value != 0;
        }
    };

    inline bool load_from_file( table_t& out, const std::wstring& path ) {
        return true;
    }

    inline std::wstring default_json_path( ) {
        return L"";
    }

}