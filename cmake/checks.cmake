# The static checks of the project's own targets: the warnings both compilers get, as errors when this is the top-level
# project, and clang-tidy over every translation unit when MINIVERSE_LINT is on. Consumers of the installed package get none
# of it: the options are PRIVATE to each target, and no target is exported with them.
#
# Left off, each for a reason:
#   -Wmissing-field-initializers    fires on a designated initializer that leaves a member to its default (world/push.hpp)
#   -Wswitch-default                a default label would silence -Wswitch, which wants every enum value handled
#   -Wfloat-equal                   grid coordinates are exact by construction and are compared as such, above all in tests
#   -Weffc++, -Wnoexcept            thousands of sites inside libstdc++ itself
#   -Wshadow-all (clang)            flags the `[x = std::move(x)]` init-capture; -Wshadow does not
#   -Wctad-maybe-unsupported        `Miniverse world(layers...)` deduces its models from the constructor by design
#   -Wundefined-func-template       the headers declare templates that one source file defines for each type (developer.md)
#   -Wunused-member-function        the five special members that every class declares
#   -Wglobal-constructors,          tests' constants
#   -Wexit-time-destructors
#   -Wweak-vtables                  the one header-only exception class, TablesDiffer
#   -fanalyzer                      GCC 13 calls its C++ support experimental
set(MINIVERSE_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wcast-align
    -Wcast-qual
    -Woverloaded-virtual
    -Wnon-virtual-dtor
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wundef
    -Wextra-semi
    -Wzero-as-null-pointer-constant
    -Wmissing-declarations
    -Wredundant-decls
    -Wsuggest-override
    -Wdeprecated-copy-dtor
    -Walloca
    -Wcast-function-type
    -Wno-missing-field-initializers
)
set(MINIVERSE_WARNINGS_GNU -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast -Wsign-promo)
set(MINIVERSE_WARNINGS_CLANG -Wmissing-prototypes -Wunused-lambda-capture -Wdocumentation)

if(MINIVERSE_LINT)
  if(NOT MINIVERSE_CLANG_TIDY)
    find_program(MINIVERSE_CLANG_TIDY NAMES clang-tidy-18 clang-tidy DOC "clang-tidy, run over every translation unit when MINIVERSE_LINT is on")
  endif()
  if(NOT MINIVERSE_CLANG_TIDY)
    message(FATAL_ERROR "MINIVERSE_LINT is ON, but clang-tidy was not found: set MINIVERSE_CLANG_TIDY to it (a ;-separated list may add arguments).")
  endif()
  # The GCC-only warnings above are unknown to clang-tidy's clang, and its driver would report each as an error.
  set(MINIVERSE_CLANG_TIDY_COMMAND ${MINIVERSE_CLANG_TIDY} --extra-arg=-Wno-unknown-warning-option)
endif()

# Gives `target` the project's checks.
function(miniverse_checks target)
  target_compile_options(
    ${target}
    PRIVATE ${MINIVERSE_WARNINGS} "$<$<CXX_COMPILER_ID:GNU>:${MINIVERSE_WARNINGS_GNU}>"
            "$<$<CXX_COMPILER_ID:Clang,AppleClang>:${MINIVERSE_WARNINGS_CLANG}>"
  )
  # The property, not CMAKE_COMPILE_WARNING_AS_ERROR, so that schemacht's targets are left alone when it is built from its
  # tree. A configure with `--compile-no-warning-as-error` turns it off for that build tree.
  set_property(TARGET ${target} PROPERTY COMPILE_WARNING_AS_ERROR ${MINIVERSE_WARNINGS_AS_ERRORS})
  if(MINIVERSE_LINT)
    set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY "${MINIVERSE_CLANG_TIDY_COMMAND}")
  endif()
endfunction()
