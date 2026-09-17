#ifndef CHARACTER_CATALOG_H
#define CHARACTER_CATALOG_H

#include <functional>
#include <memory>
#include <string>

#include "character_pack.h"
#include "cJSON.h"

/**
 * Holds a validated character pack: action descriptions, resource pointers and
 * default playback policies. Parsed once from the assets index; invalid packs
 * (wrong major version, broken metadata, missing files) resolve to nullptr and
 * the device keeps the built-in emoji path.
 */
class CharacterCatalog {
public:
    using AssetResolver =
        std::function<bool(const std::string& name, const uint8_t** data, size_t* size)>;

    // Parses `character_pack` from the assets index. Returns nullptr when the
    // pack core is invalid; individual invalid actions are dropped instead.
    static std::unique_ptr<CharacterCatalog> Parse(const cJSON* character_pack,
                                                   const AssetResolver& resolver);

    const CharacterPackDef* pack() const { return &pack_; }
    bool valid() const { return pack_.valid; }

private:
    CharacterPackDef pack_;
};

#endif  // CHARACTER_CATALOG_H
