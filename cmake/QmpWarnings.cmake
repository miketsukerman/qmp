# SPDX-License-Identifier: Apache-2.0
# Warning sets from .github/copilot-instructions.md:
#   C:   -std=c11 -Wall -Wextra -Wpedantic -Werror
#   C++: -std=c++17 -Wall -Wextra -Werror
# The -std flags come from CMAKE_<LANG>_STANDARD / EXTENSIONS OFF at the top level.

function(qmp_set_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Werror>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
endfunction()
