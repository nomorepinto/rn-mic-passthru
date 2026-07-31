cmake_minimum_required(VERSION 3.22.1)
project("micmonitor" LANGUAGES CXX)

# ── Oboe (consumed via Prefab from the AAR in build.gradle) ──────────
# The Gradle plugin + prefab feature expose Oboe headers and the
# pre-built shared library so we can simply find_package + link.
find_package(oboe REQUIRED CONFIG)

# ── Our native library ───────────────────────────────────────────────
add_library(micmonitor SHARED
    PassthroughEngine.cpp
    jni_bridge.cpp
)

target_include_directories(micmonitor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# C++17 for structured bindings, std::optional, etc.
target_compile_features(micmonitor PRIVATE cxx_std_17)

# Link dependencies:
#   oboe::oboe  — Oboe audio library
#   log         — Android __android_log_print
target_link_libraries(micmonitor PRIVATE
    oboe::oboe
    log
)
