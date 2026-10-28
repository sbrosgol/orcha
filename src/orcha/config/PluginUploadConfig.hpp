//
// PluginUploadConfig.hpp - Settings for uploading plugins from the dashboard.
// Kept separate from YamlConfiguration.hpp (no yaml-cpp), like AdminConfig.hpp.
//
//   plugins:
//     upload:
//       enabled: false                 # off unless set (uploads run code in the server)
//       public_keys: ["<base64>", ...] # Ed25519 keys whose signatures are trusted
//       max_bytes: 33554432            # largest library accepted (32 MB)
//
// Uploads are staged in <plugins.directory>/.orcha-uploads (same filesystem
// as the plugins, so installs are atomic renames; see PluginInstaller.hpp).
//
// public_keys may also be one string of keys separated by commas or spaces,
// which is how ORCHA_PLUGINS_UPLOAD_PUBLIC_KEYS sets it.
//

#pragma once

#include "IConfiguration.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Orcha::Config {

    struct PluginUploadConfig {
        bool enabled = false;
        std::vector<std::string> public_keys;
        int64_t max_bytes = 32ll * 1024 * 1024;

        static PluginUploadConfig from_config(const IConfiguration& config) {
            PluginUploadConfig cfg;
            cfg.enabled = config.get_bool("plugins.upload.enabled", false);
            cfg.max_bytes = config.get_int("plugins.upload.max_bytes", cfg.max_bytes);
            if (auto joined = config.get_string("plugins.upload.public_keys")) {
                std::string list = *joined;
                for (auto& c : list) if (c == ',') c = ' ';
                std::istringstream in(list);
                for (std::string key; in >> key;) cfg.public_keys.push_back(key);
            } else {
                cfg.public_keys = config.get_string_list("plugins.upload.public_keys");
            }
            return cfg;
        }
    };

} // namespace Orcha::Config
