//
// PluginUploadRoute.hpp - Upload, review and install plugins (admin API).
//
// Endpoints (under the admin auth gate; register before PluginAdminRoute,
// which owns the rest of /api/plugins):
//   GET    /api/plugins/_uploads               status + staged uploads
//   POST   /api/plugins/_uploads               stage an upload -> 201
//   POST   /api/plugins/_uploads/{id}/install  load it -> 200
//   DELETE /api/plugins/_uploads/{id}          discard it
//
// Upload body (JSON; files are base64):
//   { "library":   { "filename": "libfoo.so", "data": "<base64>" },
//     "manifest":  "<base64 manifest.json>" | null,
//     "signature": "<base64 of the 64-byte signature, or of the .sig file>" }
//
// The policy (off by default, signatures, staging, rollback) lives in
// Core::PluginInstaller; this route maps it onto HTTP.
//

#pragma once

#include "../AuthMiddleware.hpp"
#include "../IRouteHandler.hpp"
#include "../HttpJson.hpp"
#include "../../core/JsonCompat.hpp"
#include "../../core/PluginInstaller.hpp"
#include "../../core/PluginPackage.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace Orcha::Agent::Routes {

    class PluginUploadRoute : public IRouteHandler {
    public:
        static constexpr const char* kPath = "/api/plugins/_uploads";

        explicit PluginUploadRoute(std::shared_ptr<Core::PluginInstaller> installer)
            : installer_(std::move(installer)) {}

        [[nodiscard]] bool can_handle(const std::string& method, const std::string& path) const override {
            (void)method;
            return path == kPath || path_starts_with(path, std::string(kPath) + "/");
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            const auto seg = split_path(request.path);   // {"api","plugins","_uploads",...}
            if (seg.size() == 3) {
                if (request.method == "GET") return serve_status();
                if (request.method == "POST") return stage(request);
                return reply_error(status::MethodNotAllowed, "Use GET or POST /api/plugins/_uploads");
            }
            if (seg.size() == 4 && request.method == "DELETE") {
                auto r = installer_->discard(seg[3], user_of(request));
                if (r.is_err()) return reply_upload_error(r.error());
                Orcha::Json out = {{"success", true}};
                return reply_json(status::OK, out);
            }
            if (seg.size() == 5 && seg[4] == "install" && request.method == "POST") {
                auto r = installer_->install(seg[3], user_of(request));
                if (r.is_err()) return reply_upload_error(r.error());
                Orcha::Json out = {{"success", true}, {"name", r.value().name}, {"version", r.value().version},
                                   {"previous_version", r.value().previous_version
                                        ? Orcha::Json(*r.value().previous_version) : Orcha::Json(nullptr)}};
                return reply_json(status::OK, out);
            }
            return reply_error(status::NotFound, "Unknown plugin uploads endpoint");
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "GET",    .path = kPath, .description = "Plugin upload status and staged uploads"},
                {.method = "POST",   .path = kPath, .description = "Stage a signed plugin upload"},
                {.method = "POST",   .path = "/api/plugins/_uploads/{id}/install", .description = "Install a staged plugin"},
                {.method = "DELETE", .path = "/api/plugins/_uploads/{id}", .description = "Discard a staged plugin"}
            };
        }

        /// HTTP status for an installer error.
        [[nodiscard]] static int status_for(Core::PluginUploadErrorKind kind) {
            using K = Core::PluginUploadErrorKind;
            switch (kind) {
                case K::Disabled:     return status::Forbidden;
                case K::Invalid:      return status::BadRequest;
                case K::TooLarge:     return status::PayloadTooLarge;
                case K::BadSignature: return status::UnprocessableEntity;
                case K::NotAllowed:   return status::Forbidden;
                case K::NotFound:     return status::NotFound;
                case K::Conflict:     return status::Conflict;
                case K::LoadFailed:   return status::UnprocessableEntity;
                case K::Internal:     return status::InternalError;
            }
            return status::InternalError;
        }

    private:
        HttpResponse serve_status() const {
            auto out = installer_->status();
            Orcha::Json uploads = Orcha::Json::array();
            for (const auto& staged : installer_->list()) uploads.push_back(staged.to_json());
            out["uploads"] = uploads;
            return reply_json(status::OK, out);
        }

        HttpResponse stage(const HttpRequest& request) {
            if (!installer_->enabled()) {
                return reply_error(status::Forbidden, installer_->disabled_reason());
            }
            // One upload at a time: each holds the body, its JSON and the
            // decoded library (~3x the library size) in memory.
            if (staging_.exchange(true)) {
                return reply_error(status::Conflict, "Another plugin upload is in progress; try again shortly");
            }
            struct Done { std::atomic<bool>& flag; ~Done() { flag = false; } } done{staging_};
            Core::PluginUploadRequest upload;
            try {
                const auto body = Orcha::parse_json_bounded(request.body, 8);
                const auto& library = body.at("library");
                upload.filename = library.at("filename").get<std::string>();
                upload.library = decode(library.at("data"), "library.data");
                if (body.contains("manifest") && !body.at("manifest").is_null()) {
                    upload.manifest = decode(body.at("manifest"), "manifest");
                }
                upload.signature = decode(body.at("signature"), "signature");
                // A .sig file holds the signature as base64 text: decode once more.
                if (upload.signature.size() != 64) {
                    if (auto inner = Core::PluginPackage::base64_decode(upload.signature)) {
                        upload.signature = std::move(*inner);
                    }
                }
            } catch (const std::exception& ex) {
                return reply_error(status::BadRequest, std::string("Invalid upload body: ") + ex.what() +
                    ". Send {\"library\": {\"filename\", \"data\"}, \"manifest\"?, \"signature\"} with base64 files.");
            }
            upload.user = user_of(request);
            auto r = installer_->stage(upload);
            if (r.is_err()) return reply_upload_error(r.error());
            return reply_json(status::Created, r.value().to_json());
        }

        static std::string decode(const Orcha::Json& value, const char* field) {
            if (!value.is_string()) throw std::invalid_argument(std::string(field) + " must be a base64 string");
            auto bytes = Core::PluginPackage::base64_decode(value.get<std::string>());
            if (!bytes) throw std::invalid_argument(std::string(field) + " is not valid base64");
            return std::move(*bytes);
        }

        static std::string user_of(const HttpRequest& request) {
            if (auto header = request.header("Authorization")) {
                if (auto creds = detail::parse_basic_auth(*header)) return creds->user;
            }
            return "anonymous";
        }

        static HttpResponse reply_upload_error(const Core::PluginUploadError& error) {
            Orcha::Json body = {{"error", error.message}};
            if (error.kind == Core::PluginUploadErrorKind::LoadFailed) body["rolled_back"] = error.rolled_back;
            return reply_json(status_for(error.kind), body);
        }

        std::shared_ptr<Core::PluginInstaller> installer_;
        std::atomic<bool> staging_{false};
    };

} // namespace Orcha::Agent::Routes
