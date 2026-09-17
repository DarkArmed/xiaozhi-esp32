#include "character_catalog.h"

#include <cstdlib>
#include <cstring>

#include <esp_log.h>

namespace {

constexpr const char* TAG = "CharacterPack";

constexpr int kMaxNameLen = 64;
constexpr int kMaxActionLen = 32;
constexpr int kMaxActions = 16;
constexpr int kMinPlays = 1;
constexpr int kMaxPlays = 2;
constexpr int kMinMaxMs = 500;
constexpr int kMaxMaxMs = 8000;

// Strict integer extraction: rejects floats instead of truncating them into
// seemingly valid values (protocol contract, section 9).
bool GetInt(const cJSON* obj, const char* key, int* out) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble != static_cast<double>(item->valueint)) {
        return false;
    }
    *out = item->valueint;
    return true;
}

bool GetBoundedInt(const cJSON* obj, const char* key, int* out, int lo, int hi) {
    int value = 0;
    if (!GetInt(obj, key, &value) || value < lo || value > hi) {
        return false;
    }
    *out = value;
    return true;
}

std::string GetStringBounded(const cJSON* obj, const char* key, bool* ok, int max_len) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(item) || strlen(item->valuestring) == 0 ||
        strlen(item->valuestring) > static_cast<size_t>(max_len)) {
        *ok = false;
        return "";
    }
    *ok = true;
    return item->valuestring;
}

bool ParsePlayback(const cJSON* playback, CharacterPlaybackDef* out, bool* mode_allowed_flags,
                   bool default_mode_must_be_allowed) {
    cJSON* mode = cJSON_GetObjectItem(playback, "mode");
    if (!cJSON_IsString(mode)) {
        return false;
    }
    CharacterPlaybackDef def;
    if (strcmp(mode->valuestring, "once") == 0) {
        def.mode = CharacterPlaybackDef::kOnce;
    } else if (strcmp(mode->valuestring, "repeat") == 0) {
        def.mode = CharacterPlaybackDef::kRepeat;
    } else if (strcmp(mode->valuestring, "reply") == 0) {
        def.mode = CharacterPlaybackDef::kReply;
    } else {
        return false;
    }
    switch (def.mode) {
        case CharacterPlaybackDef::kOnce:
        case CharacterPlaybackDef::kReply:
            break;
        case CharacterPlaybackDef::kRepeat:
            if (!GetBoundedInt(playback, "plays", &def.plays, kMinPlays, kMaxPlays)) {
                return false;
            }
            break;
    }
    if (!GetBoundedInt(playback, "max_ms", &def.max_ms, kMinMaxMs, kMaxMaxMs)) {
        return false;
    }
    if (default_mode_must_be_allowed && mode_allowed_flags != nullptr &&
        !mode_allowed_flags[def.mode]) {
        return false;
    }
    *out = def;
    return true;
}

}  // namespace

bool CharacterActionDef::Allows(CharacterPlaybackDef::Mode mode) const {
    switch (mode) {
        case CharacterPlaybackDef::kOnce:
            return allow_once;
        case CharacterPlaybackDef::kRepeat:
            return allow_repeat;
        case CharacterPlaybackDef::kReply:
            return allow_reply;
    }
    return false;
}

const CharacterActionDef* CharacterPackDef::FindAction(const std::string& action_id) const {
    for (const auto& action : actions) {
        if (action.id == action_id) {
            return &action;
        }
    }
    return nullptr;
}

std::unique_ptr<CharacterCatalog> CharacterCatalog::Parse(const cJSON* character_pack,
                                                          const AssetResolver& resolver) {
    if (character_pack == nullptr || !cJSON_IsObject(character_pack) || resolver == nullptr) {
        return nullptr;
    }

    CharacterPackDef pack;
    int version = 0;
    if (!GetBoundedInt(character_pack, "version", &version, 1, 1)) {
        ESP_LOGW(TAG, "character_pack version missing or unsupported, ignoring pack");
        return nullptr;
    }
    pack.version = version;

    bool ok = false;
    pack.pack_id = GetStringBounded(character_pack, "pack_id", &ok, kMaxNameLen);
    if (!ok) {
        ESP_LOGW(TAG, "character_pack pack_id invalid, ignoring pack");
        return nullptr;
    }

    cJSON* background = cJSON_GetObjectItem(character_pack, "background");
    if (!cJSON_IsString(background) || strlen(background->valuestring) != 7 ||
        background->valuestring[0] != '#') {
        ESP_LOGW(TAG, "character_pack background invalid, ignoring pack");
        return nullptr;
    }
    pack.background_color = strtoul(background->valuestring + 1, nullptr, 16);

    cJSON* canvas = cJSON_GetObjectItem(character_pack, "canvas");
    if (!cJSON_IsObject(canvas) ||
        !GetBoundedInt(canvas, "width", &pack.canvas_w, 16, 1024) ||
        !GetBoundedInt(canvas, "height", &pack.canvas_h, 16, 1024) ||
        !GetBoundedInt(canvas, "ground_y", &pack.ground_y, 0, pack.canvas_h) ||
        !GetBoundedInt(canvas, "stage_x", &pack.stage_x, 0, 1024)) {
        ESP_LOGW(TAG, "character_pack canvas invalid, ignoring pack");
        return nullptr;
    }

    cJSON* neutral = cJSON_GetObjectItem(character_pack, "neutral");
    if (!cJSON_IsObject(neutral)) {
        ESP_LOGW(TAG, "character_pack neutral missing, ignoring pack");
        return nullptr;
    }
    pack.neutral.file = GetStringBounded(neutral, "file", &ok, kMaxNameLen);
    if (!ok || !GetBoundedInt(neutral, "width", &pack.neutral.width, 16, 1024) ||
        !GetBoundedInt(neutral, "height", &pack.neutral.height, 16, 1024)) {
        ESP_LOGW(TAG, "character_pack neutral metadata invalid, ignoring pack");
        return nullptr;
    }
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!resolver(pack.neutral.file, &data, &size) || data == nullptr || size < 16) {
        ESP_LOGW(TAG, "character_pack neutral file %s missing, ignoring pack",
                 pack.neutral.file.c_str());
        return nullptr;
    }
    pack.neutral.data = data;
    pack.neutral.size = size;

    cJSON* actions = cJSON_GetObjectItem(character_pack, "actions");
    if (!cJSON_IsArray(actions)) {
        ESP_LOGW(TAG, "character_pack actions missing, ignoring pack");
        return nullptr;
    }
    int action_count = cJSON_GetArraySize(actions);
    if (action_count > kMaxActions) {
        ESP_LOGW(TAG, "character_pack has %d actions, capping to %d", action_count, kMaxActions);
        action_count = kMaxActions;
    }
    for (int i = 0; i < action_count; i++) {
        cJSON* item = cJSON_GetArrayItem(actions, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        CharacterActionDef action;
        action.id = GetStringBounded(item, "id", &ok, kMaxActionLen);
        if (!ok) {
            ESP_LOGW(TAG, "action[%d] id invalid, dropped", i);
            continue;
        }
        action.file = GetStringBounded(item, "file", &ok, kMaxNameLen);
        if (!ok) {
            ESP_LOGW(TAG, "action %s file invalid, dropped", action.id.c_str());
            continue;
        }
        if (!GetBoundedInt(item, "frames", &action.frames, 1, 4096) ||
            !GetBoundedInt(item, "loop_ms", &action.loop_ms, 100, 60000)) {
            ESP_LOGW(TAG, "action %s timing invalid, dropped", action.id.c_str());
            continue;
        }
        cJSON* playback = cJSON_GetObjectItem(item, "default_playback");
        cJSON* allowed = cJSON_GetObjectItem(item, "allowed_modes");
        if (!cJSON_IsObject(playback) || !cJSON_IsArray(allowed)) {
            ESP_LOGW(TAG, "action %s playback metadata invalid, dropped", action.id.c_str());
            continue;
        }
        bool mode_allowed[3] = {false, false, false};
        int allowed_count = 0;
        cJSON* mode_item = nullptr;
        cJSON_ArrayForEach (mode_item, allowed) {
            if (!cJSON_IsString(mode_item)) {
                continue;
            }
            if (strcmp(mode_item->valuestring, "once") == 0) {
                action.allow_once = true;
                mode_allowed[CharacterPlaybackDef::kOnce] = true;
                allowed_count++;
            } else if (strcmp(mode_item->valuestring, "repeat") == 0) {
                action.allow_repeat = true;
                mode_allowed[CharacterPlaybackDef::kRepeat] = true;
                allowed_count++;
            } else if (strcmp(mode_item->valuestring, "reply") == 0) {
                action.allow_reply = true;
                mode_allowed[CharacterPlaybackDef::kReply] = true;
                allowed_count++;
            }
        }
        if (allowed_count == 0) {
            ESP_LOGW(TAG, "action %s allowed_modes empty, dropped", action.id.c_str());
            continue;
        }
        if (!ParsePlayback(playback, &action.default_playback, mode_allowed, true)) {
            ESP_LOGW(TAG, "action %s default_playback invalid, dropped", action.id.c_str());
            continue;
        }
        cJSON* fallback = cJSON_GetObjectItem(item, "fallback");
        if (cJSON_IsString(fallback) && strlen(fallback->valuestring) > 0 &&
            strlen(fallback->valuestring) <= static_cast<size_t>(kMaxActionLen)) {
            action.fallback = fallback->valuestring;
        } else {
            action.fallback = "neutral";
        }
        const uint8_t* action_data = nullptr;
        size_t action_size = 0;
        if (!resolver(action.file, &action_data, &action_size) || action_data == nullptr ||
            action_size < 16) {
            ESP_LOGW(TAG, "action %s file %s missing, dropped", action.id.c_str(),
                     action.file.c_str());
            continue;
        }
        action.data = action_data;
        action.size = action_size;
        pack.actions.push_back(std::move(action));
    }
    if (pack.actions.empty()) {
        ESP_LOGW(TAG, "character_pack has no valid actions, ignoring pack");
        return nullptr;
    }

    pack.valid = true;
    auto catalog = std::unique_ptr<CharacterCatalog>(new CharacterCatalog());
    catalog->pack_ = std::move(pack);
    ESP_LOGI(TAG, "character pack %s loaded: %u actions, canvas %dx%d",
             catalog->pack_.pack_id.c_str(),
             static_cast<unsigned>(catalog->pack_.actions.size()), catalog->pack_.canvas_w,
             catalog->pack_.canvas_h);
    return catalog;
}
