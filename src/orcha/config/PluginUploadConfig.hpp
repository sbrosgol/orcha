//
// PluginUploadConfig.hpp - Settings for uploading plugins from the dashboard.
// Kept separate from YamlConfiguration.hpp (no yaml-cpp), like AdminConfig.hpp.
//
//   plugins:
//     upload:
//       enabled: false                 # off unless set (uploads run code in the server)
//       public_keys: ["<base64>", ...] # Ed25519 keys whose signatures are trusted
//       max_bytes: 33554432            # largest library accepted (32 MB)
//       directory: "./uploaded-plugins" # installed uploads (keep it persistent)
//
// Uploaded plugins live in their own directory, apart from the bundled ones in
// plugins.directory, and override a bundled plugin of the same name (see
// core/PluginSources.hpp). Staging and the install journal live there too, so
// installs are atomic renames. The Docker image sets it to /app/data/plugins
// (the data volume). Plugins already in it load even when uploads are off.
//
// public_keys entries are a key (may sign any plugin name, but not override
// a bundled plugin) or a mapping that scopes it:
//
//   public_keys:
//     - "g6lx...R4w="                 # any name; no bundled overrides
//     - key: "Zk1p...9Qs="
//       plugins: ["acme-*", "billing"] # names it may sign (* and ? globs)
//       override_bundled: true         # may replace bundled plugins
//
// public_keys may also be one string of keys separated by commas or spaces,
// which is how ORCHA_PLUGINS_UPLOAD_PUBLIC_KEYS sets it (plain keys only).
//

#pragma once

#include "IConfiguration.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Orcha::Config {

    struct PluginUploadKey {
        std::string key;                    ///< base64 Ed25519 public key
        std::vector<std::string> plugins;   ///< name globs it may sign; empty: any
        bool override_bundled = false;      ///< may replace a bundled plugin
    };

    struct PluginUploadConfig {
        bool enabled = false;
        std::vector<PluginUploadKey> public_keys;
        std::vector<std::string> errors;    ///< malformed public_keys entries (uploads stay off)
        int64_t max_bytes = 32ll * 1024 * 1024;
        std::string directory = "./uploaded-plugins";

        static PluginUploadConfig from_config(const IConfiguration& config) {
            PluginUploadConfig cfg;
            cfg.enabled = config.get_bool("plugins.upload.enabled", false);
            cfg.max_bytes = config.get_int("plugins.upload.max_bytes", cfg.max_bytes);
            cfg.directory = config.get_string("plugins.upload.directory", cfg.directory);
            if (auto joined = config.get_string("plugins.upload.public_keys")) {
                std::string list = *joined;
                for (auto& c : list) if (c == ',') c = ' ';
                std::istringstream in(list);
                for (std::string key; in >> key;) cfg.public_keys.push_back({key, {}, false});
            } else if (auto keys = config.get_json("plugins.upload.public_keys")) {
                if (!keys->is_array()) {
                    cfg.errors.push_back("plugins.upload.public_keys must be a list");
                    return cfg;
                }
                for (std::size_t i = 0; i < keys->size(); ++i) {
                    const auto& entry = (*keys)[i];
                    const auto where = "plugins.upload.public_keys[" + std::to_string(i) + "]";
                    if (entry.is_string()) {
                        cfg.public_keys.push_back({entry.get<std::string>(), {}, false});
                        continue;
                    }
                    if (!entry.is_object() || !entry.contains("key") || !entry.at("key").is_string()) {
                        cfg.errors.push_back(where + " must be a key, or a mapping with a \"key\" string");
                        continue;
                    }
                    PluginUploadKey k{entry.at("key").get<std::string>(), {}, false};
                    if (entry.contains("plugins")) {
                        const auto& globs = entry.at("plugins");
                        if (!globs.is_array() || globs.empty() ||
                            !std::all_of(globs.begin(), globs.end(), [](const Json& g) { return g.is_string() && !g.get<std::string>().empty(); })) {
                            cfg.errors.push_back(where + ".plugins must be a non-empty list of name patterns");
                            continue;
                        }
                        for (const auto& g : globs) k.plugins.push_back(g.get<std::string>());
                    }
                    if (entry.contains("override_bundled")) {
                        if (!entry.at("override_bundled").is_boolean()) {
                            cfg.errors.push_back(where + ".override_bundled must be true or false");
                            continue;
                        }
                        k.override_bundled = entry.at("override_bundled").get<bool>();
                    }
                    for (const auto& [field, _] : entry.items()) {
                        if (field != "key" && field != "plugins" && field != "override_bundled") {
                            cfg.errors.push_back(where + ": unknown setting \"" + field + "\"");
                        }
                    }
                    cfg.public_keys.push_back(std::move(k));
                }
            }
            return cfg;
        }
    };

} // namespace Orcha::Config
