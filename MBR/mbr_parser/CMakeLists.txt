cmake_minimum_required(VERSION 3.16)

project(mbr_parser LANGUAGES C)

add_executable(mbr_parser main.c)

include(GNUInstallDirs)
install(TARGETS mbr_parser
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
