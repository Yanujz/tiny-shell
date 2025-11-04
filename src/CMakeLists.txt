set(LIB_SOURCES
    shell.c
    shell.h
)

add_library(tiny-shell ${LIB_SOURCES})

target_include_directories(tiny-shell
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)