#pragma once

#include "../lvgl_image.h"
#include "gifdec.h"
#include <lvgl.h>
#include <memory>
#include <functional>

/**
 * Application-level playback policy for one GIF play instance.
 * Resource loop markers are format information only; this policy decides
 * when the play instance finishes (design: character display v1, section 11).
 */
struct LvglGifPlayPolicy {
    enum Mode {
        kNative,  // No policy: keep legacy behavior (loop per resource markers)
        kOnce,    // Exactly one cycle
        kRepeat,  // `plays` cycles in total (includes the first one)
        kReply,   // Loop until stopped or max_ms deadline
    };
    Mode mode = kNative;
    int plays = 1;          // kRepeat: total cycle count
    uint32_t max_ms = 0;    // Monotonic deadline from Start(); 0 = unlimited
};

/**
 * C++ implementation of LVGL GIF widget
 * Provides GIF animation functionality using gifdec library
 */
class LvglGif {
public:
    explicit LvglGif(const lv_img_dsc_t* img_dsc);
    virtual ~LvglGif();

    // LvglImage interface implementation
    virtual const lv_img_dsc_t* image_dsc() const;

    /**
     * Start/restart GIF animation (rewinds and re-applies the current policy)
     */
    void Start();

    /**
     * Pause GIF animation
     */
    void Pause();

    /**
     * Resume GIF animation
     */
    void Resume();

    /**
     * Stop GIF animation and rewind to first frame
     */
    void Stop();

    /**
     * Check if GIF is currently playing
     */
    bool IsPlaying() const;

    /**
     * Check if GIF was loaded successfully
     */
    bool IsLoaded() const;

    /**
     * Get loop count
     */
    int32_t GetLoopCount() const;

    /**
     * Set loop count
     */
    void SetLoopCount(int32_t count);

    /**
     * Get loop delay in milliseconds (delay between loops)
     */
    uint32_t GetLoopDelay() const;

    /**
     * Set loop delay in milliseconds (delay between loops)
     * @param delay_ms Delay in milliseconds before starting next loop. 0 means no delay.
     */
    void SetLoopDelay(uint32_t delay_ms);

    /**
     * Get GIF dimensions
     */
    uint16_t width() const;
    uint16_t height() const;

    /**
     * Set frame update callback
     */
    void SetFrameCallback(std::function<void()> callback);

    /**
     * Set the playback policy for the next Start(). Changing it while playing
     * only takes effect after the next Start() (rewind re-applies the policy).
     */
    void SetPlayPolicy(const LvglGifPlayPolicy& policy);

    /**
     * Callback invoked ONCE when this play instance finishes: policy satisfied
     * (cycles completed / deadline reached / stopped) or a decode error occurred.
     * `error` is true only when the GIF data could not be decoded further.
     * Called from the LVGL timer context; the owner must marshal to its own
     * task before touching shared state.
     */
    void SetFinishedCallback(std::function<void(bool error)> callback);

private:
    // GIF decoder instance
    gd_GIF* gif_;

    // LVGL image descriptor
    lv_img_dsc_t img_dsc_;

    // Animation timer
    lv_timer_t* timer_;

    // Last frame update time
    uint32_t last_call_;

    // Animation state
    bool playing_;
    bool loaded_;

    // Loop delay configuration
    uint32_t loop_delay_ms_;      // Delay between loops in milliseconds
    bool loop_waiting_;           // Whether we're waiting for the next loop
    uint32_t loop_wait_start_;    // Timestamp when loop wait started

    // Frame update callback
    std::function<void()> frame_callback_;

    // Playback policy state
    LvglGifPlayPolicy policy_;
    int cycles_done_ = 0;              // Completed cycles of the current instance
    uint32_t instance_start_ = 0;      // Tick when the current instance started
    bool finished_notified_ = false;

    // Instance finished callback (see SetFinishedCallback)
    std::function<void(bool error)> finished_callback_;

    /**
     * Update to next frame
     */
    void NextFrame();

    /**
     * Finish the current play instance (idempotent). Stops the timer and
     * notifies the finished callback once.
     */
    void FinishInstance(bool error);

    /**
     * Cleanup resources
     */
    void Cleanup();
};
