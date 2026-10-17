#pragma once

// Standard library — heavy headers used across most TUs.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Third-party: nlohmann/json is pulled in by core/ICommand.hpp and ends up in
// virtually every TU (engine, agent, jobs, plugins). It's the single biggest
// parse cost in the project, so it earns its place in the PCH.
#include <nlohmann/json.hpp>
#include "core/Json.hpp"

// Project logging interface (pure declarations — safe for plugins that don't
// link against the logger library).
#include "utils/ILogger.hpp"
