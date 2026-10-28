//
// PluginPackage.cpp - see PluginPackage.hpp.
//

#include "PluginPackage.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace Orcha::Core::PluginPackage {

    namespace {

        std::uint32_t read_u32(std::string_view b, std::size_t at, bool big_endian) {
            std::uint32_t v = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                const auto byte = static_cast<std::uint8_t>(b[at + (big_endian ? i : 3 - i)]);
                v = (v << 8) | byte;
            }
            return v;
        }

        std::uint16_t read_u16_le(std::string_view b, std::size_t at) {
            return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[at]) |
                                              (static_cast<std::uint8_t>(b[at + 1]) << 8));
        }

        std::string macho_arch(std::uint32_t cputype) {
            switch (cputype) {
                case 0x01000007: return "x86_64";
                case 0x0100000c: return "arm64";
                case 0x00000007: return "i386";
                case 0x0000000c: return "arm";
                default:         return "cputype " + std::to_string(cputype);
            }
        }

        std::string elf_arch(std::uint16_t machine) {
            switch (machine) {
                case 62:  return "x86_64";
                case 183: return "arm64";
                case 3:   return "i386";
                case 40:  return "arm";
                case 243: return "riscv";
                default:  return "machine " + std::to_string(machine);
            }
        }

        BinaryInfo finish(BinaryInfo info) {
            const auto host = host_platform();
            if (!info.problem.empty()) return info;
            if (host.format.empty()) {
                info.problem = "This platform does not support plugin uploads";
            } else if (info.format.rfind("Mach-O", 0) == 0 ? host.format != "Mach-O"
                                                           : host.format != info.format) {
                info.problem = "This is a " + info.format + " library; this server (" +
                               host.format + " " + host.arch + ") cannot load it";
            } else if (info.arch.find(host.arch) == std::string::npos) {
                info.problem = "Built for " + info.arch + "; this server is " + host.arch;
            } else {
                info.loadable_here = true;
            }
            return info;
        }

        struct EvpPkeyFree { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
        struct EvpMdCtxFree { void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); } };

    } // namespace

    BinaryInfo host_platform() {
        BinaryInfo host;
#if defined(__APPLE__)
        host.format = "Mach-O";
#elif defined(__linux__)
        host.format = "ELF";
#endif
#if defined(__x86_64__) || defined(_M_X64)
        host.arch = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__)
        host.arch = "arm64";
#else
        host.format.clear();
#endif
        host.loadable_here = !host.format.empty();
        return host;
    }

    BinaryInfo inspect_library(std::string_view b) {
        BinaryInfo info;
        if (b.size() >= 64 && b.substr(0, 4) == "\x7f" "ELF") {
            info.format = "ELF";
            if (b[4] != 2) info.problem = "Only 64-bit ELF libraries are supported";
            else if (b[5] != 1) info.problem = "Only little-endian ELF libraries are supported";
            else if (read_u16_le(b, 16) != 3) info.problem = "Not a shared library (ELF type is not ET_DYN)";
            info.arch = elf_arch(read_u16_le(b, 18));
            return finish(info);
        }
        if (b.size() >= 32) {
            const auto magic_le = read_u32(b, 0, false);
            if (magic_le == 0xfeedfacf) {   // MH_MAGIC_64, little-endian
                info.format = "Mach-O";
                info.arch = macho_arch(read_u32(b, 4, false));
                const auto filetype = read_u32(b, 12, false);
                if (filetype != 6 && filetype != 8) {   // MH_DYLIB, MH_BUNDLE
                    info.problem = "Not a dynamic library or bundle (Mach-O file type " +
                                   std::to_string(filetype) + ")";
                }
                return finish(info);
            }
            const auto magic_be = read_u32(b, 0, true);
            if (magic_be == 0xcafebabe || magic_be == 0xcafebabf) {   // FAT_MAGIC(_64)
                const bool wide = magic_be == 0xcafebabf;
                const auto count = read_u32(b, 4, true);
                const std::size_t entry = wide ? 32 : 20;
                if (count == 0 || count > 16 || 8 + count * entry > b.size()) {
                    info.problem = "Unrecognised universal binary header";
                    return info;
                }
                info.format = "Mach-O universal";
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (i) info.arch += ", ";
                    info.arch += macho_arch(read_u32(b, 8 + i * entry, true));
                }
                return finish(info);
            }
        }
        info.problem = "Not a shared library this server recognises (expected ELF or Mach-O)";
        return info;
    }

    std::optional<std::string> base64_decode(std::string_view text) {
        static constexpr auto kTable = [] {
            std::array<std::int8_t, 256> t{};
            t.fill(-1);
            const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(alphabet[i])] = static_cast<std::int8_t>(i);
            return t;
        }();
        std::string out;
        out.reserve(text.size() / 4 * 3);
        std::uint32_t acc = 0;
        int bits = 0;
        std::size_t padding = 0;
        for (const char c : text) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            if (c == '=') { ++padding; continue; }
            if (padding) return std::nullopt;   // data after padding
            const auto v = kTable[static_cast<unsigned char>(c)];
            if (v < 0) return std::nullopt;
            acc = (acc << 6) | static_cast<std::uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((acc >> bits) & 0xff));
            }
        }
        // Leftover bits must be zero padding (strict: one encoding per input).
        if (padding > 2 || bits >= 6 || (acc & ((1u << bits) - 1)) != 0) return std::nullopt;
        return out;
    }

    std::string base64_encode(std::string_view bytes) {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        std::size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3) {
            const auto n = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                           (static_cast<std::uint8_t>(bytes[i + 1]) << 8) |
                           static_cast<std::uint8_t>(bytes[i + 2]);
            out += kAlphabet[(n >> 18) & 63];
            out += kAlphabet[(n >> 12) & 63];
            out += kAlphabet[(n >> 6) & 63];
            out += kAlphabet[n & 63];
        }
        if (i < bytes.size()) {
            const bool two = i + 1 < bytes.size();
            const auto n = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                           (two ? static_cast<std::uint8_t>(bytes[i + 1]) << 8 : 0);
            out += kAlphabet[(n >> 18) & 63];
            out += kAlphabet[(n >> 12) & 63];
            out += two ? kAlphabet[(n >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    std::string sha256_hex(std::string_view bytes) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
        unsigned int len = 0;
        if (EVP_Digest(bytes.data(), bytes.size(), md.data(), &len, EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("SHA-256 failed");
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(len * 2);
        for (unsigned int i = 0; i < len; ++i) {
            out += kHex[md[i] >> 4];
            out += kHex[md[i] & 15];
        }
        return out;
    }

    std::string signing_message(std::string_view library_filename,
                                std::string_view library_sha256_hex,
                                std::string_view manifest_sha256_hex) {
        std::string m = "orcha-plugin-signature-v2\n";
        m += library_filename;
        m += '\n';
        m += library_sha256_hex;
        m += '\n';
        m += manifest_sha256_hex;
        m += '\n';
        return m;
    }

    bool verify_ed25519(std::string_view public_key, std::string_view signature,
                        std::string_view message) {
        if (public_key.size() != 32 || signature.size() != 64) return false;
        std::unique_ptr<EVP_PKEY, EvpPkeyFree> key(EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(public_key.data()),
            public_key.size()));
        std::unique_ptr<EVP_MD_CTX, EvpMdCtxFree> ctx(EVP_MD_CTX_new());
        if (!key || !ctx) return false;
        if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return false;
        return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(signature.data()),
                                signature.size(),
                                reinterpret_cast<const unsigned char*>(message.data()),
                                message.size()) == 1;
    }

    std::string key_id(std::string_view public_key) {
        return sha256_hex(public_key).substr(0, 16);
    }

    bool is_valid_plugin_name(std::string_view name) {
        if (name.empty() || name.size() > 64) return false;
        const auto alnum = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        };
        if (!alnum(name.front())) return false;
        for (const char c : name) {
            if (!alnum(c) && c != '_' && c != '-' && c != '.') return false;
        }
        if (name.find("..") != std::string_view::npos) return false;
        for (const std::string_view suffix : {".so", ".dylib", ".dll", ".part", ".tmp"}) {
            if (name.ends_with(suffix)) return false;
        }
        return true;
    }

    std::string plugin_name_from_filename(std::string_view filename) {
        // Same rule as PluginDiscoveryService::create_basic_metadata.
        std::string stem = std::filesystem::path(std::string(filename)).stem().string();
        if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
        return stem;
    }

} // namespace Orcha::Core::PluginPackage
