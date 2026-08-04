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
# NOTE: this does NOT include the one-shot-stream-retirement fix
# (github.com/NSF-Breaking-Low-Agora/biphony/issues/26, fix on the
# `issues/26` branch there, based on `main`). That fix isn't on this FFI
# branch yet, so a connection sending many independent one-shot messages
# will permanently exhaust initial_max_streams_bidi (500) and stop making
# progress after that many messages. Update this pin once issues/26 is
# reviewed and its fix is available on a branch that also has the FFI.
set(BIPHONY_TAG  "1f90b96498ef0fdc56a260be10312da782b70c17")

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
