# Provides the `doctest::doctest` target for host unit tests.
#
# Order of preference:
#   1) A doctest already installed on the system (find_package).
#   2) FetchContent from GitHub (requires network on first configure; cached
#      under build/_deps afterwards).
#
# doctest is a single-header framework with zero dependencies, which suits a
# portable C++17 library meant to also cross-compile for MCUs.

find_package(doctest QUIET)

if(NOT doctest_FOUND)
  include(FetchContent)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE)
  # doctest 2.4.11 still declares cmake_minimum_required(<3.5), which CMake 4.x
  # rejects. Relax the policy floor just for its subtree.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_MakeAvailable(doctest)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  # Make doctest_discover_tests() available (ships in doctest's cmake/ dir).
  list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
endif()

include(doctest)
