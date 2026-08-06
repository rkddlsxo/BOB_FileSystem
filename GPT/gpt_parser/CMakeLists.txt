cmake_minimum_required(VERSION 3.16)

project(gpt_parser LANGUAGES C)

add_executable(gpt_parser main.c)

include(GNUInstallDirs)
install(TARGETS gpt_parser
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
