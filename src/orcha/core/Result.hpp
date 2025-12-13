//
// Result.hpp - Monadic Result type for error handling
// Created as part of architectural improvements
//

#pragma once

#include <variant>
#include <string>
#include <functional>
#include <optional>

namespace Orcha::Core {

    struct ErrorInfo {
        std::string code;
        std::string message;
        std::optional<std::string> details;

        ErrorInfo(std::string msg) : code("ERROR"), message(std::move(msg)) {}
        ErrorInfo(std::string code, std::string msg)
            : code(std::move(code)), message(std::move(msg)) {}
        ErrorInfo(std::string code, std::string msg, std::string details)
            : code(std::move(code)), message(std::move(msg)), details(std::move(details)) {}
    };

    template<typename T, typename E = ErrorInfo>
    class Result {
    public:
        // Factory methods
        static Result Ok(T value) {
            Result r;
            r.data_ = std::move(value);
            r.is_success_ = true;
            return r;
        }

        static Result Err(E error) {
            Result r;
            r.data_ = std::move(error);
            r.is_success_ = false;
            return r;
        }

        // For void results
        static Result Ok() requires std::is_same_v<T, void> {
            Result r;
            r.is_success_ = true;
            return r;
        }

        // Query methods
        [[nodiscard]] bool is_ok() const noexcept { return is_success_; }
        [[nodiscard]] bool is_err() const noexcept { return !is_success_; }
        [[nodiscard]] explicit operator bool() const noexcept { return is_success_; }

        // Value access
        T& value() & {
            if (!is_success_) throw std::runtime_error("Attempted to access value of error Result");
            return std::get<T>(data_);
        }

        const T& value() const& {
            if (!is_success_) throw std::runtime_error("Attempted to access value of error Result");
            return std::get<T>(data_);
        }

        T&& value() && {
            if (!is_success_) throw std::runtime_error("Attempted to access value of error Result");
            return std::get<T>(std::move(data_));
        }

        // Error access
        E& error() & {
            if (is_success_) throw std::runtime_error("Attempted to access error of success Result");
            return std::get<E>(data_);
        }

        const E& error() const& {
            if (is_success_) throw std::runtime_error("Attempted to access error of success Result");
            return std::get<E>(data_);
        }

        // Value with default
        T value_or(T default_value) const& {
            return is_success_ ? std::get<T>(data_) : std::move(default_value);
        }

        // Monadic operations

        // map: Transform the success value
        template<typename F>
        auto map(F&& f) const -> Result<std::invoke_result_t<F, const T&>, E> {
            using U = std::invoke_result_t<F, const T&>;
            if (is_success_) {
                return Result<U, E>::Ok(std::forward<F>(f)(std::get<T>(data_)));
            }
            return Result<U, E>::Err(std::get<E>(data_));
        }

        // map_err: Transform the error value
        template<typename F>
        auto map_err(F&& f) const -> Result<T, std::invoke_result_t<F, const E&>> {
            using U = std::invoke_result_t<F, const E&>;
            if (!is_success_) {
                return Result<T, U>::Err(std::forward<F>(f)(std::get<E>(data_)));
            }
            return Result<T, U>::Ok(std::get<T>(data_));
        }

        // and_then: Chain operations that return Result
        template<typename F>
        auto and_then(F&& f) const -> std::invoke_result_t<F, const T&> {
            using ResultType = std::invoke_result_t<F, const T&>;
            if (is_success_) {
                return std::forward<F>(f)(std::get<T>(data_));
            }
            return ResultType::Err(std::get<E>(data_));
        }

        // or_else: Chain on error
        template<typename F>
        auto or_else(F&& f) const -> std::invoke_result_t<F, const E&> {
            using ResultType = std::invoke_result_t<F, const E&>;
            if (!is_success_) {
                return std::forward<F>(f)(std::get<E>(data_));
            }
            return ResultType::Ok(std::get<T>(data_));
        }

        // Match/visit pattern
        template<typename FnOk, typename FnErr>
        auto match(FnOk&& on_ok, FnErr&& on_err) const
            -> std::common_type_t<std::invoke_result_t<FnOk, const T&>,
                                  std::invoke_result_t<FnErr, const E&>> {
            if (is_success_) {
                return std::forward<FnOk>(on_ok)(std::get<T>(data_));
            }
            return std::forward<FnErr>(on_err)(std::get<E>(data_));
        }

    private:
        Result() = default;
        std::variant<T, E> data_;
        bool is_success_ = false;
    };

    // Specialization for void success type
    template<typename E>
    class Result<void, E> {
    public:
        static Result Ok() {
            Result r;
            r.is_success_ = true;
            return r;
        }

        static Result Err(E error) {
            Result r;
            r.error_ = std::move(error);
            r.is_success_ = false;
            return r;
        }

        [[nodiscard]] bool is_ok() const noexcept { return is_success_; }
        [[nodiscard]] bool is_err() const noexcept { return !is_success_; }
        [[nodiscard]] explicit operator bool() const noexcept { return is_success_; }

        E& error() & {
            if (is_success_) throw std::runtime_error("Attempted to access error of success Result");
            return *error_;
        }

        const E& error() const& {
            if (is_success_) throw std::runtime_error("Attempted to access error of success Result");
            return *error_;
        }

        template<typename F>
        auto and_then(F&& f) const -> std::invoke_result_t<F> {
            using ResultType = std::invoke_result_t<F>;
            if (is_success_) {
                return std::forward<F>(f)();
            }
            return ResultType::Err(*error_);
        }

    private:
        Result() = default;
        std::optional<E> error_;
        bool is_success_ = false;
    };

    // Type aliases for common cases
    using VoidResult = Result<void, ErrorInfo>;

} // namespace Orcha::Core
