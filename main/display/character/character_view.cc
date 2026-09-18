#include "character_view.h"

#include <cstring>

#include <esp_log.h>

#include "../display.h"
#include "../lvgl_display/lvgl_image.h"
#include "../lvgl_display/lvgl_theme.h"
#include "../lvgl_display/gif/lvgl_gif.h"

#define TAG "CharacterView"

namespace {

constexpr int kSubtitleHeight = 48;   // Subtitle area at the bottom (design section 4)
constexpr int kMaxActionFrames = 64;  // Runtime safety net for canvas checks

// Minimal descriptor so LvglGif can open GIF data straight from the assets map
lv_img_dsc_t MakeRawDescriptor(const uint8_t* data, size_t size) {
    lv_img_dsc_t dsc = {};
    dsc.data = data;
    dsc.data_size = size;
    return dsc;
}

bool IsGifData(const uint8_t* data, size_t size) {
    return data != nullptr && size >= 6 &&
           (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0);
}

}  // namespace

CharacterView::CharacterView(Display* display, const CharacterPackDef* pack, int top_inset)
    : display_(display), pack_(pack), top_inset_(top_inset) {}

CharacterView::~CharacterView() {
    Destroy();
}

bool CharacterView::Create() {
    if (root_ != nullptr || pack_ == nullptr || !pack_->valid) {
        return false;
    }

    auto theme = static_cast<LvglTheme*>(display_->GetTheme());
    const lv_font_t* text_font =
        theme != nullptr && theme->text_font() != nullptr ? theme->text_font()->font() : nullptr;

    int screen_w = display_->width();
    int screen_h = display_->height();
    int subtitle_h = kSubtitleHeight < screen_h / 4 ? kSubtitleHeight : screen_h / 4;

    // Root covers everything below the status bar so the chat content and the
    // floating emoji layer stay hidden while character mode is on.
    root_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root_, screen_w, screen_h - top_inset_);
    lv_obj_set_pos(root_, 0, top_inset_);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root_, lv_color_hex(pack_->background_color), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    if (text_font != nullptr) {
        lv_obj_set_style_text_font(root_, text_font, 0);
    }

    // Character image: canvas centered horizontally, resting on the stage
    // bottom with the subtitle area below it (design section 4)
    image_ = lv_image_create(root_);
    int stage_h = screen_h - top_inset_ - subtitle_h;
    int excess = stage_h - pack_->canvas_h;
    if (excess < 0) {
        excess = 0;
    }
    int image_x = (screen_w - pack_->canvas_w) / 2;
    if (image_x < 0) {
        image_x = pack_->stage_x;
    }
    int image_y = excess / 2;
    lv_obj_set_pos(image_, image_x, image_y);
    lv_obj_set_size(image_, pack_->canvas_w, pack_->canvas_h < stage_h ? pack_->canvas_h : stage_h);

    // Canvas taller than the stage: shrink uniformly, anchored bottom-center
    // so the feet stay on the ground line and nothing essential is clipped
    int fit_w = (256 * screen_w) / pack_->canvas_w;
    int fit_h = (256 * stage_h) / pack_->canvas_h;
    int fit = fit_w < fit_h ? fit_w : fit_h;
    if (fit < 256) {
        lv_image_set_pivot(image_, pack_->canvas_w / 2, pack_->canvas_h);
        lv_image_set_scale(image_, static_cast<uint16_t>(fit));
        ESP_LOGW(TAG, "canvas %dx%d exceeds stage height %d, scaled to %d%%", pack_->canvas_w,
                 pack_->canvas_h, stage_h, fit * 100 / 256);
    }

    // Status hint on top of the stage
    hint_label_ = lv_label_create(root_);
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint_label_,
                                theme != nullptr ? theme->text_color() : lv_color_black(), 0);
    lv_label_set_text(hint_label_, "");
    lv_obj_align(hint_label_, LV_ALIGN_TOP_MID, 0, 2);

    // Subtitle area at the bottom, up to two lines
    subtitle_label_ = lv_label_create(root_);
    lv_obj_set_width(subtitle_label_, screen_w - 8);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(subtitle_label_,
                                theme != nullptr ? theme->text_color() : lv_color_black(), 0);
    lv_label_set_text(subtitle_label_, "");
    lv_obj_align(subtitle_label_, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Static neutral, kept resident so animation failures can fall back
    neutral_image_ = std::make_unique<LvglCBinImage>(const_cast<uint8_t*>(pack_->neutral.data));
    if (neutral_image_->image_dsc() == nullptr) {
        ESP_LOGE(TAG, "Failed to parse neutral image %s", pack_->neutral.file.c_str());
        Destroy();
        return false;
    }
    lv_image_set_src(image_, neutral_image_->image_dsc());

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void CharacterView::Destroy() {
    // Detach LVGL from the GIF canvas before releasing the decoder (section 11)
    gif_.reset();
    neutral_image_.reset();
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    image_ = nullptr;
    hint_label_ = nullptr;
    subtitle_label_ = nullptr;
}

void CharacterView::Show() {
    if (root_ != nullptr) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        visible_ = true;
    }
}

void CharacterView::Hide() {
    if (root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        visible_ = false;
    }
}

void CharacterView::SetStatusHint(const char* text) {
    if (hint_label_ != nullptr) {
        lv_label_set_text(hint_label_, text != nullptr ? text : "");
    }
}

void CharacterView::SetSubtitle(const char* text) {
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, text != nullptr ? text : "");
    }
}

void CharacterView::ShowNeutral() {
    // Detach the GIF canvas before releasing the decoder (section 11)
    if (image_ != nullptr && neutral_image_ != nullptr) {
        lv_image_set_src(image_, neutral_image_->image_dsc());
    }
    gif_.reset();
}

bool CharacterView::PlayAction(const CharacterActionDef* action,
                               const CharacterPlaybackDef& policy,
                               std::function<void(bool error)> on_finished) {
    if (image_ == nullptr || action == nullptr || action->data == nullptr) {
        return false;
    }
    if (!IsGifData(action->data, action->size)) {
        ESP_LOGE(TAG, "Action %s is not GIF data", action->id.c_str());
        return false;
    }

    // Runtime size check: canvas must match the pack stage (section 12)
    if (action->size < 13) {
        return false;
    }
    uint16_t gif_w = static_cast<uint16_t>(action->data[6] | (action->data[7] << 8));
    uint16_t gif_h = static_cast<uint16_t>(action->data[8] | (action->data[9] << 8));
    if (gif_w != static_cast<uint16_t>(pack_->canvas_w) ||
        gif_h != static_cast<uint16_t>(pack_->canvas_h)) {
        ESP_LOGE(TAG, "Action %s canvas %dx%d does not match pack %dx%d", action->id.c_str(),
                 gif_w, gif_h, pack_->canvas_w, pack_->canvas_h);
        return false;
    }
    if (action->frames > kMaxActionFrames) {
        ESP_LOGE(TAG, "Action %s frame count %d exceeds cap", action->id.c_str(), action->frames);
        return false;
    }

    // Detach LVGL from the previous canvas, then release the old decoder
    if (neutral_image_ != nullptr) {
        lv_image_set_src(image_, neutral_image_->image_dsc());
    }
    gif_.reset();

    lv_img_dsc_t dsc = MakeRawDescriptor(action->data, action->size);
    auto gif = std::make_unique<LvglGif>(&dsc);
    if (!gif->IsLoaded()) {
        ESP_LOGE(TAG, "Failed to open GIF for action %s", action->id.c_str());
        return false;
    }

    LvglGifPlayPolicy play_policy;
    switch (policy.mode) {
        case CharacterPlaybackDef::kOnce:
            play_policy.mode = LvglGifPlayPolicy::kOnce;
            break;
        case CharacterPlaybackDef::kReply:
            play_policy.mode = LvglGifPlayPolicy::kReply;
            break;
        case CharacterPlaybackDef::kRepeat:
        default:
            play_policy.mode = LvglGifPlayPolicy::kRepeat;
            play_policy.plays = policy.plays;
            break;
    }
    play_policy.max_ms = policy.max_ms;

    // Own the decoder BEFORE wiring callbacks: the frame callback reads the
    // gif_ member, and Start() renders the first frame immediately
    gif_ = std::move(gif);
    gif_->SetPlayPolicy(play_policy);
    gif_->SetFrameCallback([this]() { lv_image_set_src(image_, gif_->image_dsc()); });
    gif_->SetFinishedCallback(std::move(on_finished));

    lv_image_set_src(image_, gif_->image_dsc());
    gif_->Start();
    return true;
}

void CharacterView::StopAction() {
    ShowNeutral();
}
