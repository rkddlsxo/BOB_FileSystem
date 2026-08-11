cmake_minimum_required(VERSION 3.16)

project(fat_parser LANGUAGES C)

add_executable(fat_parser main.c)

include(GNUInstallDirs)
install(TARGETS fat_parser
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
