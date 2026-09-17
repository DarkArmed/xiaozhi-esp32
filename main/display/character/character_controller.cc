#include "character_controller.h"

#include <esp_log.h>

#include "../display.h"
#include "character_view.h"

#define TAG "CharacterCtrl"

// v1 status hints (zh-CN, character mode is a zh-CN feature in v1)
static constexpr const char* kHintConnecting[] = {"连接中", nullptr};
static constexpr const char* kHintListening[] = {"我在听", nullptr};
static constexpr const char* kHintWaiting[] = {"想一想", nullptr};

CharacterController::CharacterController(Display* display,
                                         std::shared_ptr<const CharacterCatalog> catalog,
                                         MainTaskPoster post)
    : display_(display), catalog_(std::move(catalog)), post_(std::move(post)) {
    if (display_ == nullptr || catalog_ == nullptr || !catalog_->valid() || !post_) {
        return;
    }
    if (!display_->SetCharacterMode(catalog_->pack())) {
        ESP_LOGW(TAG, "Display rejected character mode, controller disabled");
        return;
    }
    view_ = display_->GetCharacterView();
    if (view_ != nullptr) {
        DisplayLockGuard lock(display_);
        ShowBaseState();
    }
}

CharacterController::~CharacterController() {
    if (display_ != nullptr) {
        display_->SetCharacterMode(nullptr);
    }
    view_ = nullptr;
}

const char* CharacterController::DefaultHint() const {
    switch (state_) {
        case VisualState::kConnecting:
            return kHintConnecting[0];
        case VisualState::kListening:
            return kHintListening[0];
        case VisualState::kWaiting:
            return kHintWaiting[0];
        case VisualState::kStandby:
        case VisualState::kSpeaking:
        case VisualState::kSystem:
            return "";
    }
    return "";
}

void CharacterController::ShowBaseState() {
    if (view_ == nullptr || system_active_) {
        return;
    }
    if (!view_->IsVisible()) {
        view_->Show();
    }
    view_->ShowNeutral();
    view_->SetStatusHint(!hint_override_.empty() ? hint_override_.c_str() : DefaultHint());
}

void CharacterController::SetVisualState(VisualState state) {
    if (state_ == state) {
        return;  // Repeated state events must not rebuild the GIF (section 10.3)
    }
    state_ = state;
    hint_override_.clear();
    // Any non-speaking transition cancels a playing action: the device may go
    // back to listening before the action finished its plays (section 10.6/10.8)
    if (active_action_ != nullptr && state != VisualState::kSpeaking) {
        CancelAction();
    }
    if (state != VisualState::kSystem) {
        DisplayLockGuard lock(display_);
        ShowBaseState();
    }
}

void CharacterController::SetStatusHint(const char* text) {
    hint_override_ = text != nullptr ? text : "";
    DisplayLockGuard lock(display_);
    if (view_ != nullptr && !system_active_) {
        view_->SetStatusHint(!hint_override_.empty() ? hint_override_.c_str() : DefaultHint());
    }
}

bool CharacterController::StartAction(const std::string& action_id,
                                      const CharacterPlaybackDef* policy) {
    if (view_ == nullptr || system_active_) {
        return false;
    }
    const CharacterActionDef* action = catalog_->pack()->FindAction(action_id);
    if (action == nullptr) {
        ESP_LOGW(TAG, "Unknown action %s", action_id.c_str());
        return false;
    }
    CharacterPlaybackDef def = policy != nullptr ? *policy : action->default_playback;
    // Policy validation (section 9): mode must be allowed for this action and
    // fields must stay inside protocol bounds; invalid -> reject the request
    if (!action->Allows(def.mode)) {
        ESP_LOGW(TAG, "Action %s rejects mode %d", action_id.c_str(), def.mode);
        return false;
    }
    if (def.mode == CharacterPlaybackDef::kRepeat && (def.plays < 1 || def.plays > 2)) {
        return false;
    }
    if (def.max_ms < 500 || def.max_ms > 8000) {
        return false;
    }
    // Local per-action cap wins over the requested deadline
    if (def.max_ms > action->default_playback.max_ms) {
        def.max_ms = action->default_playback.max_ms;
    }

    generation_++;
    active_generation_ = generation_;
    active_action_ = action;
    {
        DisplayLockGuard lock(display_);
        bool ok = view_->PlayAction(action, def, [this, gen = active_generation_](bool error) {
            // LVGL timer context: marshal to the main task with the generation
            post_([this, gen, error]() { HandleActionFinished(gen, error); });
        });
        if (!ok) {
            active_action_ = nullptr;
            ShowBaseState();
            return false;
        }
    }
    ESP_LOGD(TAG, "Action %s started (generation %u)", action_id.c_str(), active_generation_);
    return true;
}

void CharacterController::HandleActionFinished(uint32_t gen, bool error) {
    if (gen != active_generation_ || active_action_ == nullptr) {
        return;  // Stale completion from an interrupted play instance
    }
    if (error) {
        ESP_LOGW(TAG, "Action %s failed to decode, falling back to neutral",
                 active_action_->id.c_str());
    }
    active_action_ = nullptr;
    DisplayLockGuard lock(display_);
    ShowBaseState();
}

void CharacterController::CancelAction() {
    if (active_action_ == nullptr) {
        return;
    }
    active_action_ = nullptr;
    generation_++;
    DisplayLockGuard lock(display_);
    view_->StopAction();
    ShowBaseState();
}

void CharacterController::OnReplyEnded() {
    // The turn is over even if the two plays are not (section 10.6)
    if (active_action_ != nullptr) {
        CancelAction();
    }
}

void CharacterController::OnUserInterrupt() {
    if (active_action_ != nullptr) {
        CancelAction();
    } else {
        generation_++;  // Invalidate completions still in flight
    }
}

void CharacterController::SetSystemViewActive(bool active) {
    if (system_active_ == active) {
        return;
    }
    system_active_ = active;
    if (view_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (active) {
        if (active_action_ != nullptr) {
            // Keep active_action_ so the stale completion is dropped by the
            // generation check when the system view exits
            view_->StopAction();
            active_action_ = nullptr;
            generation_++;
        }
        view_->Hide();
    } else {
        view_->Show();
        ShowBaseState();
    }
}
