//
// PluginPackage.hpp - Checks on an uploaded plugin that never execute it.
//
// Loading a plugin runs its code (dlopen runs static initialisers before any
// ABI check), so everything decided before the admin clicks Install has to be
// decided from the bytes: the binary format and CPU architecture from the
// ELF/Mach-O header, SHA-256 digests, and an Ed25519 signature over both
// files. OpenSSL stays inside PluginPackage.cpp.
//
// Signature (v2): Ed25519 over signing_message(library file name,
// sha256(library), sha256(manifest)), where a missing manifest hashes as
// empty. The file name is signed because it names a plugin that has no
// manifest; the manifest names (and versions) the others. So the signature
// decides which plugin an upload becomes. scripts/orcha-plugin-sign.sh
// produces it with the openssl CLI; keep the two in step.
//

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Orcha::Core::PluginPackage {

    /// What the library's header says, and whether this host can load it.
    struct BinaryInfo {
        std::string format;          ///< "ELF", "Mach-O", "Mach-O universal", or "" if unknown.
        std::string arch;            ///< "x86_64", "arm64", ... ("x86_64, arm64" for universal).
        bool loadable_here = false;
        std::string problem;         ///< Why not, when !loadable_here.
    };

    /// Inspect a shared library's header (no loading).
    [[nodiscard]] BinaryInfo inspect_library(std::string_view bytes);

    /// This host, e.g. {"Mach-O", "arm64"}; format is empty on unsupported platforms.
    [[nodiscard]] BinaryInfo host_platform();

    /// Strict base64 (standard alphabet, padding optional, whitespace ignored).
    [[nodiscard]] std::optional<std::string> base64_decode(std::string_view text);
    [[nodiscard]] std::string base64_encode(std::string_view bytes);

    /// Lower-case hex SHA-256.
    [[nodiscard]] std::string sha256_hex(std::string_view bytes);

    /// The exact bytes a plugin signature covers.
    [[nodiscard]] std::string signing_message(std::string_view library_filename,
                                              std::string_view library_sha256_hex,
                                              std::string_view manifest_sha256_hex);

    /// Ed25519 verification; @p public_key is the raw 32-byte key, @p signature 64 bytes.
    [[nodiscard]] bool verify_ed25519(std::string_view public_key, std::string_view signature,
                                      std::string_view message);

    /// Short, stable id for a public key (first 16 hex digits of its SHA-256).
    [[nodiscard]] std::string key_id(std::string_view public_key);

    /// Plugin names become directory names: [A-Za-z0-9][A-Za-z0-9._-]{0,63},
    /// no "..", and not ending like a library or temporary file (.so, .part, ...).
    [[nodiscard]] bool is_valid_plugin_name(std::string_view name);

    /// The name discovery gives a library without a manifest ("libfoo.so" -> "foo").
    [[nodiscard]] std::string plugin_name_from_filename(std::string_view filename);

} // namespace Orcha::Core::PluginPackage
