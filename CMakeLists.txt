cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBAOM_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_av1.c
)

SET(LIBAOM_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libaom.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libaom_version.a
)

SET(LIBAOM_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libaom
        "${LIBAOM_SRC}"
        "${LIBAOM_LIB}"
        []
        ""
        "${LIBAOM_INC}"
        ""
        "1")
