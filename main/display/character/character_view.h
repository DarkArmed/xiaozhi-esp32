#ifndef CHARACTER_VIEW_H
#define CHARACTER_VIEW_H

#include <functional>
#include <memory>
#include <string>

#include "character_pack.h"
#include "lvgl.h"

class Display;
class LvglGif;
class LvglCBinImage;

/**
 * LVGL layout for the character mode: light-blue stage with one character
 * image (static neutral or a playing GIF), a status hint and a subtitle area.
 * All methods must be called from the main task with the display lock already
 * held; the view itself never locks (lvgl_port_lock is not recursive).
 */
class CharacterView {
public:
    CharacterView(Display* display, const CharacterPackDef* pack, int top_inset);
    ~CharacterView();

    bool Create();  // Builds LVGL objects; false on failure
    void Destroy();

    void Show();
    void Hide();
    bool IsVisible() const { return visible_; }

    void SetStatusHint(const char* text);
    void SetSubtitle(const char* text);

    void ShowNeutral();

    // Plays one action with the given policy. `on_finished` is invoked exactly
    // once, from the LVGL timer context (no display lock held) - the caller
    // must marshal it to its own task before touching shared state.
    bool PlayAction(const CharacterActionDef* action, const CharacterPlaybackDef& policy,
                    std::function<void(bool error)> on_finished);
    void StopAction();

private:
    Display* display_;
    const CharacterPackDef* pack_;
    int top_inset_;
    bool visible_ = false;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* subtitle_label_ = nullptr;
    std::unique_ptr<LvglGif> gif_;
    std::unique_ptr<LvglCBinImage> neutral_image_;
};

#endif  // CHARACTER_VIEW_H
