#ifndef CHARACTER_CONTROLLER_H
#define CHARACTER_CONTROLLER_H

#include <functional>
#include <memory>
#include <string>

#include "character_catalog.h"
#include "character_pack.h"

class Display;
class CharacterView;

/**
 * The only visual decision maker for the character mode (design section 6).
 * Receives lightweight events, owns state, priority, dedup and the action
 * lifetime. Every method must be called from the main task; LVGL object
 * changes happen under the display lock held here (the view never locks).
 * Callbacks posted from the LVGL timer context carry a generation counter so
 * stale completions can never restore a newer visual era (section 10.7).
 */
class CharacterController {
public:
    enum class VisualState {
        kStandby,
        kConnecting,
        kListening,
        kWaiting,
        kSpeaking,
        kSystem,  // Provisioning / upgrade / alert views take over
    };

    using MainTaskPoster = std::function<void(std::function<void()>)>;

    CharacterController(Display* display,
                        std::shared_ptr<const CharacterCatalog> catalog, MainTaskPoster post);
    ~CharacterController();

    bool valid() const { return view_ != nullptr; }

    // Visual base state; unmapped device states map to kSystem/kStandby by the caller
    void SetVisualState(VisualState state);
    VisualState visual_state() const { return state_; }

    // Overrides the per-state hint text until the next state change
    void SetStatusHint(const char* text);

    // Starts one action; `policy` may be null to use the pack default.
    // The policy is validated against the pack before playback starts.
    bool StartAction(const std::string& action_id, const CharacterPlaybackDef* policy);

    // TTS stop received and the playback queue drained (section 10.5)
    void OnReplyEnded();

    // User interruption / new capture window / channel loss (section 10.7)
    void OnUserInterrupt();

    // Dedicated system views (provisioning, upgrade, alert popups) take over
    void SetSystemViewActive(bool active);

    bool IsActionPlaying() const { return active_action_ != nullptr; }

private:
    void HandleActionFinished(uint32_t generation, bool error);
    void CancelAction();
    void ShowBaseState();  // display lock held
    const char* DefaultHint() const;

    Display* display_;
    std::shared_ptr<const CharacterCatalog> catalog_;
    CharacterView* view_ = nullptr;
    MainTaskPoster post_;

    VisualState state_ = VisualState::kStandby;
    const CharacterActionDef* active_action_ = nullptr;
    uint32_t generation_ = 1;
    uint32_t active_generation_ = 0;
    std::string hint_override_;
    bool system_active_ = false;
};

#endif  // CHARACTER_CONTROLLER_H
