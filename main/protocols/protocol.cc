#include "protocol.h"
#include "assets.h"

#include <esp_log.h>

#define TAG "Protocol"

void Protocol::AddTextFontCapabilities(cJSON* root) {
    auto capability = Assets::GetInstance().text_font_capability();
    cJSON* features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsObject(features)) {
        cJSON_AddBoolToObject(features, "glyph_push", capability.glyph_push);
    }

    if (!capability.glyph_push) {
        return;
    }
    cJSON* font = cJSON_CreateObject();
    cJSON_AddStringToObject(font, "bundle", capability.bundle.c_str());
    cJSON_AddStringToObject(font, "charset", capability.charset.c_str());
    cJSON_AddNumberToObject(font, "size", capability.size);
    cJSON_AddNumberToObject(font, "bpp", capability.bpp);
    cJSON_AddItemToObject(root, "text_font", font);
}

void Protocol::AddCharacterAnimationHello(cJSON* root) {
#if CONFIG_ENABLE_CHARACTER_DISPLAY
    auto catalog = Assets::GetInstance().character_catalog();
    if (catalog == nullptr || !catalog->valid()) {
        return;
    }
    const CharacterPackDef* pack = catalog->pack();
    cJSON* features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsObject(features)) {
        cJSON_AddBoolToObject(features, "character_animation", true);
    }
    cJSON* animation = cJSON_CreateObject();
    cJSON_AddNumberToObject(animation, "version", 1);
    cJSON_AddStringToObject(animation, "pack_id", pack->pack_id.c_str());
    cJSON_AddNumberToObject(animation, "epoch", 1);
    cJSON* actions = cJSON_AddArrayToObject(animation, "actions");
    for (const auto& action : pack->actions) {
        cJSON_AddItemToArray(actions, cJSON_CreateString(action.id.c_str()));
    }
    cJSON_AddItemToObject(root, "character_animation", animation);
#endif
}

void Protocol::ParseCharacterAnimationHello(const cJSON* root) {
    // The server confirms by echoing version 1; without confirmation the
    // device stays on local state + neutral and never expects action metadata
    auto animation = cJSON_GetObjectItem(root, "character_animation");
    if (!cJSON_IsObject(animation)) {
        character_animation_confirmed_ = false;
        return;
    }
    auto version = cJSON_GetObjectItem(animation, "version");
    character_animation_confirmed_ =
        cJSON_IsNumber(version) && version->valueint == 1;
    ESP_LOGI(TAG, "Server character animation %s",
             character_animation_confirmed_ ? "confirmed" : "not confirmed");
}

void Protocol::AddCharacterAnimationContext(cJSON* root) {
#if CONFIG_ENABLE_CHARACTER_DISPLAY
    auto catalog = Assets::GetInstance().character_catalog();
    if (catalog == nullptr || !catalog->valid()) {
        return;
    }
    cJSON* animation = cJSON_CreateObject();
    cJSON_AddNumberToObject(animation, "version", 1);
    cJSON_AddNumberToObject(animation, "epoch", character_epoch_);
    cJSON_AddItemToObject(root, "character_animation", animation);
#endif
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) { on_connected_ = callback; }

void Protocol::OnDisconnected(std::function<void()> callback) { on_disconnected_ = callback; }

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SetError(const std::string& message, const std::string& detail) {
    if (detail.empty()) {
        SetError(message);
        return;
    }
    SetError(message + "\n" + detail);
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    // An abort interrupts the current turn: publish a new capture epoch with it
    character_epoch_++;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "abort");
    if (reason == kAbortReasonWakeWordDetected) {
        cJSON_AddStringToObject(root, "reason", "wake_word_detected");
    }
    AddCharacterAnimationContext(root);
    auto json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "detect");
    cJSON_AddStringToObject(root, "text", wake_word.c_str());
    AddCharacterAnimationContext(root);
    auto json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void Protocol::SendStartListening(ListeningMode mode) {
    // A new capture window: publish a fresh epoch before the audio starts
    // (character design, section 9)
    character_epoch_++;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "start");
    if (mode == kListeningModeRealtime) {
        cJSON_AddStringToObject(root, "mode", "realtime");
    } else if (mode == kListeningModeAutoStop) {
        cJSON_AddStringToObject(root, "mode", "auto");
    } else {
        cJSON_AddStringToObject(root, "mode", "manual");
    }
    AddCharacterAnimationContext(root);
    auto json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void Protocol::SendCharacterReset() {
    // Local VAD interrupt without a listen message (continuous listening):
    // invalidate the current visual turn on the server side
    character_epoch_++;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "character");
    cJSON_AddStringToObject(root, "state", "reset");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "epoch", character_epoch_);
    auto json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void Protocol::SendStopListening() {
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
