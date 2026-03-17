cmake_minimum_required(VERSION 3.10)
project(vterm VERSION 0.3.3 LANGUAGES C)

set(VERSION_CURRENT 0)
set(VERSION_REVISION 0)
set(VERSION_AGE 0)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wpedantic)

if(CMAKE_SYSTEM_NAME MATCHES "SunOS")
    add_compile_definitions(__EXTENSIONS__ _XPG6 __XOPEN_OR_POSIX)
endif()

if(CMAKE_BUILD_TYPE MATCHES "Debug" OR DEBUG)
    add_compile_definitions(DEBUG)
endif()

option(PROFILE "Enable profiling" OFF)
if(PROFILE)
    add_compile_options(-pg)
    add_link_options(-pg)
endif()

find_package(Perl REQUIRED)

file(GLOB TBLFILES "src/encoding/*.tbl")
set(INCFILES "")
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src/encoding)

foreach(TBLFILE ${TBLFILES})
    get_filename_component(BASENAME ${TBLFILE} NAME_WE)
    set(INCFILE "${CMAKE_CURRENT_BINARY_DIR}/src/encoding/${BASENAME}.inc")
    add_custom_command(
        OUTPUT ${INCFILE}
        COMMAND ${PERL_EXECUTABLE} -CSD ${CMAKE_CURRENT_SOURCE_DIR}/tbl2inc_c.pl ${TBLFILE} > ${INCFILE}
        DEPENDS ${TBLFILE} ${CMAKE_CURRENT_SOURCE_DIR}/tbl2inc_c.pl
        VERBATIM
    )
    list(APPEND INCFILES ${INCFILE})
endforeach()

set(FULLWIDTH_INC "${CMAKE_CURRENT_BINARY_DIR}/src/fullwidth.inc")
add_custom_command(
    OUTPUT ${FULLWIDTH_INC}
    COMMAND ${PERL_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/find-wide-chars.pl > ${FULLWIDTH_INC}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/find-wide-chars.pl
    VERBATIM
)

file(GLOB CFILES "src/*.c")
add_library(vterm ${CFILES} ${INCFILES} ${FULLWIDTH_INC})
set_target_properties(vterm PROPERTIES
    VERSION ${VERSION_CURRENT}.${VERSION_REVISION}.${VERSION_AGE}
    SOVERSION ${VERSION_CURRENT}
)
target_include_directories(vterm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/src
)

#file(GLOB BINFILES_SRC "bin/*.c")
#foreach(BIN_SRC ${BINFILES_SRC})
#    get_filename_component(BIN_NAME ${BIN_SRC} NAME_WE)
#    add_executable(${BIN_NAME} ${BIN_SRC})
#    target_link_libraries(${BIN_NAME} vterm)
#endforeach()

#enable_testing()
#add_executable(harness t/harness.c)
#target_link_libraries(harness vterm)

#file(GLOB TESTFILES "t/*.test")
#foreach(TESTFILE ${TESTFILES})
#    get_filename_component(TESTNAME ${TESTFILE} NAME)
#    add_test(NAME ${TESTNAME} COMMAND ${PERL_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/t/run-test.pl ${TESTFILE})
#endforeach()

include(GNUInstallDirs)

install(TARGETS vterm LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
#install(TARGETS vterm-ctrl vterm-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(VERSION ${PROJECT_VERSION})
set(INCDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
set(LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/vterm.pc.in ${CMAKE_CURRENT_BINARY_DIR}/vterm.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vterm.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
