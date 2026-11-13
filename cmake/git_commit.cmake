# Writes OUT (git_commit.h): GIT_COMMIT, the commit the source is at, short, with "+" when it has changes not committed.
# From git where the source is a checkout; else from build-commit.txt beside it (buildroot copies the tree without its
# .git: buildroot/local-build.sh writes that file first); else "unknown". Run at every build (src/CMakeLists.txt), the
# file rewritten only when it changes, so nothing rebuilds for nothing. SPDX-License-Identifier: LGPL-2.1-or-later
execute_process(COMMAND git -C "${SRC}" rev-parse --short=7 HEAD
                OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc ERROR_QUIET)
if(rc EQUAL 0 AND commit)
    execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE dirty ERROR_QUIET)
    if(dirty)
        set(commit "${commit}+")
    endif()
elseif(EXISTS "${SRC}/build-commit.txt")
    file(STRINGS "${SRC}/build-commit.txt" commit LIMIT_COUNT 1)
else()
    set(commit "unknown")
endif()
set(text "#define GIT_COMMIT \"${commit}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
