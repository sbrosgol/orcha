#include "../../core/ICommand.hpp"
#include <cpprest/json.h>
#include <string>
#include <sstream>
#include <iostream>
#include <regex>

namespace {
    // Validates PostgreSQL identifier to prevent SQL injection
    // Allows alphanumeric, underscores, and must start with letter or underscore
    bool is_valid_pg_identifier(const std::string& name) {
        if (name.empty() || name.length() > 63) {
            return false;
        }
        static const std::regex valid_identifier(R"(^[a-zA-Z_][a-zA-Z0-9_]*$)");
        return std::regex_match(name, valid_identifier);
    }
}

class PostgresCreator final : public Orcha::Core::ICommand {
public:
    [[nodiscard]] std::string name() const override { return "create_pg_db"; }

    web::json::value execute(const web::json::value &params) override {
        using namespace web;
        using namespace utility;

        try {
            json::value result;
            auto get_str = [&](const char* key, const std::string& def = std::string()) -> std::string {
                if (params.has_field(conversions::to_string_t(key))) {
                    return conversions::to_utf8string(params.at(conversions::to_string_t(key)).as_string());
                }
                return def;
            };
            auto get_bool = [&](const char* key, bool def) -> bool {
                if (params.has_field(conversions::to_string_t(key))) {
                    const auto& v = params.at(conversions::to_string_t(key));
                    if (v.is_boolean()) return v.as_bool();
                    if (v.is_string()) {
                        auto s = conversions::to_utf8string(v.as_string());
                        return s == "1" || s == "true" || s == "TRUE" || s == "yes";
                    }
                }
                return def;
            };

            const std::string dbname = get_str("dbname");
            if (dbname.empty()) {
                throw std::runtime_error("'dbname' parameter is required");
            }
            if (!is_valid_pg_identifier(dbname)) {
                throw std::runtime_error("'dbname' contains invalid characters (use alphanumeric and underscores only)");
            }
            const std::string host = get_str("host", "localhost");
            const std::string port = get_str("port", "5432");
            const std::string user = get_str("user", "");
            const std::string password = get_str("password", "");
            const std::string owner = get_str("owner", "");
            const std::string template_db = get_str("template", "");
            const bool if_not_exists = get_bool("if_not_exists", true);

            // Validate optional identifiers
            if (!owner.empty() && !is_valid_pg_identifier(owner)) {
                throw std::runtime_error("'owner' contains invalid characters (use alphanumeric and underscores only)");
            }
            if (!template_db.empty() && !is_valid_pg_identifier(template_db)) {
                throw std::runtime_error("'template' contains invalid characters (use alphanumeric and underscores only)");
            }

            // Prepare base psql command
            std::ostringstream psqlBase;
            psqlBase << "psql";
            if (!host.empty()) psqlBase << " -h '" << host << "'";
            if (!port.empty()) psqlBase << " -p '" << port << "'";
            if (!user.empty()) psqlBase << " -U '" << user << "'";
            psqlBase << " -t -A -q"; // table only, unaligned, quiet

            // Export password to environment for psql
            std::string old_pgpassword;
            if (const char* prev = std::getenv("PGPASSWORD")) {
                old_pgpassword = prev;
            }
            if (!password.empty()) {
#ifdef _WIN32
                _putenv_s("PGPASSWORD", password.c_str());
#else
                setenv("PGPASSWORD", password.c_str(), 1);
#endif
            }

            auto restore_password = [&]() {
#ifdef _WIN32
                if (!password.empty()) {
                    if (!old_pgpassword.empty()) _putenv_s("PGPASSWORD", old_pgpassword.c_str());
                    else _putenv_s("PGPASSWORD", "");
                }
#else
                if (!password.empty()) {
                    if (!old_pgpassword.empty()) setenv("PGPASSWORD", old_pgpassword.c_str(), 1);
                    else unsetenv("PGPASSWORD");
                }
#endif
            };

            // Check if DB exists
            std::ostringstream checkCmd;
            checkCmd << psqlBase.str() << " -d postgres -c \"SELECT 1 FROM pg_database WHERE datname='" << dbname << "';\"";
            std::cout << "Checking if database exists: " << dbname << '\n';
            const int checkExit = std::system(checkCmd.str().c_str());

            if (const bool exists = (checkExit == 0); exists) {
                if (if_not_exists) {
                    result[U("success")] = json::value(true);
                    result[U("dbname")] = json::value::string(conversions::to_string_t(dbname));
                    result[U("created")] = json::value(false);
                    restore_password();
                    return result;
                }
                restore_password();
                throw std::runtime_error("Database already exists: " + dbname);
            }

            std::ostringstream createSql;
            createSql << "CREATE DATABASE \"" << dbname << "\"";
            if (!owner.empty()) createSql << " OWNER \"" << owner << "\"";
            if (!template_db.empty()) createSql << " TEMPLATE \"" << template_db << "\"";
            createSql << ";";

            std::ostringstream createCmd;
            createCmd << psqlBase.str() << " -d postgres -c \"" << createSql.str() << "\"";
            std::cout << "Creating database: " << dbname << '\n';
            const int createExit = std::system(createCmd.str().c_str());

            restore_password();

            if (createExit != 0) {
                throw std::runtime_error("Failed to create database. Exit code: " + std::to_string(createExit));
            }

            result[U("success")] = json::value(true);
            result[U("dbname")] = json::value::string(conversions::to_string_t(dbname));
            result[U("created")] = json::value(true);
            return result;
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << '\n';
            json::value err;
            err[U("success")] = web::json::value(false);
            err[U("error")] = web::json::value::string(ex.what());
            return err;
        }
    }
};

extern "C" Orcha::Core::ICommand* create_command() {
    return new PostgresCreator();
}
