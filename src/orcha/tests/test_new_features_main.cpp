//
// test_new_features_main.cpp - Test runner for new architecture features
//

#include "NewFeaturesTests.hpp"

int main() {
    try {
        Orcha::Tests::run_new_features_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[FAIL] Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
