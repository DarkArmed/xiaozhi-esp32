#ifndef CHARACTER_PACK_H
#define CHARACTER_PACK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Validated character pack description (character display design v1, section 12).
// Data pointers reference the assets partition mapping and stay valid while the
// pack is applied; the catalog never copies GIF payloads.

struct CharacterPlaybackDef {
    enum Mode { kOnce = 0, kRepeat = 1, kReply = 2 };

    Mode mode = kRepeat;
    int plays = 1;   // kRepeat: total cycle count including the first one
    int max_ms = 0;  // Deadline from play start on the monotonic clock
};

struct CharacterActionDef {
    std::string id;
    std::string file;
    const uint8_t* data = nullptr;
    size_t size = 0;
    int frames = 0;
    int loop_ms = 0;
    CharacterPlaybackDef default_playback;
    bool allow_once = false;
    bool allow_repeat = false;
    bool allow_reply = false;
    std::string fallback;

    bool Allows(CharacterPlaybackDef::Mode mode) const;
};

struct CharacterPackDef {
    bool valid = false;
    int version = 0;
    std::string pack_id;
    uint32_t background_color = 0xDCEFF8;  // #RRGGBB
    int canvas_w = 0;
    int canvas_h = 0;
    int ground_y = 0;
    int stage_x = 0;
    struct Neutral {
        std::string file;
        const uint8_t* data = nullptr;
        size_t size = 0;
        int width = 0;
        int height = 0;
    } neutral;
    std::vector<CharacterActionDef> actions;

    const CharacterActionDef* FindAction(const std::string& action_id) const;
};

#endif  // CHARACTER_PACK_H
