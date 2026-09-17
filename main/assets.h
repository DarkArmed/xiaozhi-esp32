#ifndef ASSETS_H
#define ASSETS_H

#include <functional>
#include <memory>
#include <string>

#include <esp_partition.h>
#include <cJSON.h>
#include <model_path.h>
#include <map>
#include <string>

#include "display/character/character_catalog.h"

#if HAVE_LVGL
#include <spi_flash_mmap.h>
#endif

struct Asset {
    size_t size;
    size_t offset;
};

struct TextFontCapability {
    bool glyph_push = false;
    std::string bundle;
    std::string charset;
    int size = 0;
    int bpp = 0;
};

class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets();

    bool Download(std::string url,
                  std::function<void(int progress, size_t speed)> progress_callback);
    bool Apply(bool refresh_display_theme = true);
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size);

    inline bool partition_valid() const { return partition_valid_; }
    inline std::string default_assets_url() const { return default_assets_url_; }
    inline TextFontCapability text_font_capability() const { return text_font_capability_; }
    // Optional character pack parsed from the assets index; null when absent
    // or invalid. Resource pointers stay valid until the next Apply/UnApply;
    // holders keep the shared_ptr so a concurrent re-apply cannot dangle.
    inline CharacterCatalog* character_catalog() const { return character_catalog_.get(); }
    inline std::shared_ptr<CharacterCatalog> character_catalog_shared() const {
        return character_catalog_;
    }

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    bool InitializePartition();
    void UnApplyPartition();
    static bool FindPartition(Assets* assets);
    static bool LoadSrmodelsFromIndex(Assets* assets, cJSON* root = nullptr);
    void UseBuiltInTextFontCapability();
    void DisableTextFontGlyphPush();

    class AssetStrategy {
    public:
        virtual ~AssetStrategy() = default;
        virtual bool Apply(Assets* assets, bool refresh_display_theme = true) = 0;
        virtual bool InitializePartition(Assets* assets) = 0;
        virtual void UnApplyPartition(Assets* assets) = 0;
        virtual bool GetAssetData(Assets* assets, const std::string& name, void*& ptr,
                                  size_t& size) = 0;
    };

    class LvglStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets, bool refresh_display_theme = true) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr,
                          size_t& size) override;

    private:
        static uint32_t CalculateChecksum(const char* data, uint32_t length);
        std::map<std::string, Asset> assets_;
        esp_partition_mmap_handle_t mmap_handle_ = 0;
        const char* mmap_root_ = nullptr;
        bool checksum_valid_ = false;
    };

    class EmoteStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets, bool refresh_display_theme = true) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr,
                          size_t& size) override;
    };

    // Strategy instance
    std::unique_ptr<AssetStrategy> strategy_;

protected:
    const esp_partition_t* partition_ = nullptr;
    bool partition_valid_ = false;
    std::string default_assets_url_;
    TextFontCapability text_font_capability_;
    std::shared_ptr<CharacterCatalog> character_catalog_;
    srmodel_list_t* models_list_ = nullptr;
};

#endif
