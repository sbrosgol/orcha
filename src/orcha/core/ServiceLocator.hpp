//
// ServiceLocator.hpp - Dependency injection container
// Created as part of architectural improvements
//

#pragma once

#include <memory>
#include <unordered_map>
#include <typeindex>
#include <functional>
#include <stdexcept>
#include <any>
#include <mutex>

namespace Orcha::Core {

    /**
     * @class ServiceLocator
     * @brief Dependency injection container for managing application services.
     *
     * Supports:
     * - Singleton services (shared across the application)
     * - Transient services (new instance per request)
     * - Factory-based registration
     * - Thread-safe access
     *
     * Example usage:
     * @code
     * ServiceLocator locator;
     *
     * // Register singleton
     * locator.register_singleton<ILogger>(std::make_shared<Logger>());
     *
     * // Register factory
     * locator.register_factory<ICommandRegistry>([]() {
     *     return std::make_shared<CommandRegistry>();
     * });
     *
     * // Resolve service
     * auto logger = locator.get<ILogger>();
     * @endcode
     */
    class ServiceLocator {
    public:
        ServiceLocator() = default;
        ~ServiceLocator() = default;

        // Prevent copying
        ServiceLocator(const ServiceLocator&) = delete;
        ServiceLocator& operator=(const ServiceLocator&) = delete;

        // Cannot move due to std::recursive_mutex
        ServiceLocator(ServiceLocator&&) = delete;
        ServiceLocator& operator=(ServiceLocator&&) = delete;

        /**
         * @brief Register a singleton service instance.
         *
         * The same instance is returned for all get() calls.
         */
        template<typename Interface>
        void register_singleton(std::shared_ptr<Interface> instance) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto key = std::type_index(typeid(Interface));
            singletons_[key] = instance;
            // Clear any factory for this type
            factories_.erase(key);
        }

        /**
         * @brief Register a factory function for transient services.
         *
         * A new instance is created for each get() call.
         */
        template<typename Interface>
        void register_factory(std::function<std::shared_ptr<Interface>()> factory) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto key = std::type_index(typeid(Interface));
            factories_[key] = [factory]() -> std::any {
                return factory();
            };
            // Clear any singleton for this type
            singletons_.erase(key);
        }

        /**
         * @brief Register an implementation type that will be constructed.
         *
         * The Implementation must have a default constructor or take
         * dependencies that can be resolved from the locator.
         */
        template<typename Interface, typename Implementation>
        void register_type() {
            register_factory<Interface>([]() {
                return std::make_shared<Implementation>();
            });
        }

        /**
         * @brief Register a singleton with lazy initialization.
         */
        template<typename Interface>
        void register_lazy_singleton(std::function<std::shared_ptr<Interface>()> factory) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto key = std::type_index(typeid(Interface));
            lazy_singletons_[key] = [factory]() -> std::any {
                return factory();
            };
        }

        /**
         * @brief Get a service instance.
         *
         * @throws std::runtime_error if service is not registered.
         */
        template<typename Interface>
        std::shared_ptr<Interface> get() const {
            auto key = std::type_index(typeid(Interface));

            std::lock_guard<std::recursive_mutex> lock(mutex_);

            // Check singletons first
            auto singleton_it = singletons_.find(key);
            if (singleton_it != singletons_.end()) {
                return std::any_cast<std::shared_ptr<Interface>>(singleton_it->second);
            }

            // Check lazy singletons
            auto lazy_it = lazy_singletons_.find(key);
            if (lazy_it != lazy_singletons_.end()) {
                auto instance = std::any_cast<std::shared_ptr<Interface>>(lazy_it->second());
                // Promote to singleton (safe: singletons_ is mutable)
                singletons_[key] = instance;
                lazy_singletons_.erase(key);
                return instance;
            }

            // Check factories
            auto factory_it = factories_.find(key);
            if (factory_it != factories_.end()) {
                return std::any_cast<std::shared_ptr<Interface>>(factory_it->second());
            }

            throw std::runtime_error(
                std::string("Service not registered: ") + typeid(Interface).name());
        }

        /**
         * @brief Try to get a service, returning nullptr if not found.
         */
        template<typename Interface>
        std::shared_ptr<Interface> try_get() const noexcept {
            try {
                return get<Interface>();
            } catch (...) {
                return nullptr;
            }
        }

        /**
         * @brief Check if a service is registered.
         */
        template<typename Interface>
        [[nodiscard]] bool has() const {
            auto key = std::type_index(typeid(Interface));
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            return singletons_.contains(key) ||
                   factories_.contains(key) ||
                   lazy_singletons_.contains(key);
        }

        /**
         * @brief Unregister a service.
         */
        template<typename Interface>
        void unregister() {
            auto key = std::type_index(typeid(Interface));
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            singletons_.erase(key);
            factories_.erase(key);
            lazy_singletons_.erase(key);
        }

        /**
         * @brief Clear all registered services.
         */
        void clear() {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            singletons_.clear();
            factories_.clear();
            lazy_singletons_.clear();
        }

    private:
        mutable std::recursive_mutex mutex_;
        mutable std::unordered_map<std::type_index, std::any> singletons_;
        std::unordered_map<std::type_index, std::function<std::any()>> factories_;
        mutable std::unordered_map<std::type_index, std::function<std::any()>> lazy_singletons_;
    };

    /**
     * @class ServiceScope
     * @brief RAII scope for temporary service overrides.
     *
     * Useful for testing where you want to temporarily replace
     * a service with a mock.
     */
    template<typename Interface>
    class ServiceScope {
    public:
        ServiceScope(ServiceLocator& locator, std::shared_ptr<Interface> temporary)
            : locator_(locator)
            , had_previous_(locator.has<Interface>()) {
            if (had_previous_) {
                previous_ = locator.try_get<Interface>();
            }
            locator_.register_singleton<Interface>(std::move(temporary));
        }

        ~ServiceScope() {
            if (had_previous_) {
                locator_.register_singleton<Interface>(previous_);
            } else {
                locator_.template unregister<Interface>();
            }
        }

        ServiceScope(const ServiceScope&) = delete;
        ServiceScope& operator=(const ServiceScope&) = delete;

    private:
        ServiceLocator& locator_;
        bool had_previous_;
        std::shared_ptr<Interface> previous_;
    };

} // namespace Orcha::Core
