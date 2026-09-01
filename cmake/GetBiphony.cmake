# CMake module to fetch and build Biphony (a QUIC-based multipath transport,
# https://github.com/NSF-Breaking-Low-Agora/biphony_quiche) for use by the
# biphony_network_backend plugin.
#
# Biphony is a Cargo workspace, not a CMake project, so this cannot use the
# fetch_git()+configure_target() pattern used by cmake/GetOpenVINS.cmake and
# friends (that pattern relies on FetchContent_MakeAvailable's implicit
# add_subdirectory(), which requires a CMakeLists.txt at the fetched source
# root). Instead, only the source is fetched via FetchContent, and the actual
# build is driven by `cargo build`, producing a static library that is wrapped
# in an IMPORTED CMake target.

find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "biphony_network_backend requires a Rust toolchain (the `cargo` command) "
        "to build the Biphony QUIC library, but `cargo` was not found in PATH. "
        "Install Rust (see https://rustup.rs) or remove biphony_network_backend "
        "from your profile.")
endif()
# Biphony's `quiche` dependency builds BoringSSL via the `cmake` crate at cargo
# build time, so a system `cmake` is also required -- ILLIXR itself already
# requires CMake to build, so this is not an additional burden.

include(FetchContent)

set(BIPHONY_REPO "git@github.com:NSF-Breaking-Low-Agora/biphony_quiche.git")
# Pinned to a specific commit (rather than a moving branch) for build
# reproducibility. Update this as Biphony's FFI stabilizes.
#
# This is `issues/24` (FFI + logging-init) with the one-shot-stream-retirement
# fix from `issues/26` (github.com/NSF-Breaking-Low-Agora/biphony/issues/26)
# cherry-picked on top locally, since the two fixes live on separate branches
# upstream and `issues/26` is still awaiting the Biphony author's review.
# Once `issues/26` is reviewed and merged into a branch that already carries
# the FFI, re-pin to that upstream commit instead of this local cherry-pick.
set(BIPHONY_TAG  "996be0b45d60ac3a9b485efb492fdf6d288d7e34")

FetchContent_Declare(Biphony
                      GIT_REPOSITORY ${BIPHONY_REPO}
                      GIT_TAG        ${BIPHONY_TAG}
                      GIT_SUBMODULES_RECURSE TRUE
                      GIT_PROGRESS   TRUE
)

FetchContent_GetProperties(Biphony)
if(NOT biphony_POPULATED)
    message(STATUS "Fetching Biphony (${BIPHONY_TAG}) ...")
    # Populate-only: deliberately not FetchContent_MakeAvailable(), since
    # Biphony has no top-level CMakeLists.txt to add_subdirectory() into.
    FetchContent_Populate(Biphony)
endif()

set(BIPHONY_TARGET_DIR "${biphony_SOURCE_DIR}/target/release")
set(BIPHONY_LIB "${BIPHONY_TARGET_DIR}/libquiche_apps.a")

add_custom_command(
    OUTPUT ${BIPHONY_LIB}
    COMMAND ${CARGO_EXECUTABLE} build --release --features ffi -p quiche_apps
    WORKING_DIRECTORY ${biphony_SOURCE_DIR}
    COMMENT "Building Biphony (quiche_apps, cargo build --release --features ffi) -- this can take several minutes the first time (BoringSSL build)"
    VERBATIM
    USES_TERMINAL
)
add_custom_target(biphony_cargo_build ALL DEPENDS ${BIPHONY_LIB})

# Prefer the static archive over the .so: BoringSSL is statically linked into
# it by quiche's own build.rs, so it's fully self-contained -- this avoids
# rpath/LD_LIBRARY_PATH complications for a dlopen()-loaded plugin .so.
add_library(biphony::quiche_apps STATIC IMPORTED GLOBAL)
set_target_properties(biphony::quiche_apps PROPERTIES
                       IMPORTED_LOCATION ${BIPHONY_LIB}
                       INTERFACE_INCLUDE_DIRECTORIES ${biphony_SOURCE_DIR}/apps/include
)
add_dependencies(biphony::quiche_apps biphony_cargo_build)

find_package(Threads REQUIRED)
target_link_libraries(biphony::quiche_apps INTERFACE Threads::Threads dl m)
