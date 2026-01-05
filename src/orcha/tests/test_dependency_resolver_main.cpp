//
// test_dependency_resolver_main.cpp - Test runner for dependency resolver
//

#include "DependencyResolverTests.hpp"

int main() {
    try {
        Orcha::Tests::run_dependency_resolver_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[FAIL] Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
