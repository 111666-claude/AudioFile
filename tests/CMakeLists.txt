file (COPY test-audio DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file (MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/audio-write-tests)
file (MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/section-tests)

set (AUDIOFILE_TESTS Tests)

file (GLOB AUDIOFILE_TESTS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
source_group (TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${AUDIOFILE_TESTS_SOURCES})

add_executable (${AUDIOFILE_TESTS} ${AUDIOFILE_TESTS_SOURCES})
target_include_directories (${AUDIOFILE_TESTS} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries (${AUDIOFILE_TESTS} PUBLIC AudioFile)
if (UNIX AND NOT APPLE)
    target_link_libraries (${AUDIOFILE_TESTS} PRIVATE ${CMAKE_DL_LIBS})
endif ()
target_compile_features (${AUDIOFILE_TESTS} PRIVATE cxx_std_17)
target_compile_definitions (${AUDIOFILE_TESTS} PUBLIC
  -D_USE_MATH_DEFINES # needed for M_PI macro
  -DPROJECT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# A preloadable library that counts bytes actually pulled through read().
# Used by the range-loading tests to prove section loads do not read whole files.
add_library (audiofile_read_counter SHARED read_counter.c)
target_link_libraries (audiofile_read_counter PRIVATE ${CMAKE_DL_LIBS})

add_test (NAME ${AUDIOFILE_TESTS} COMMAND ${AUDIOFILE_TESTS})

# preload the read() counter wherever the platform supports LD_PRELOAD
if (UNIX AND NOT APPLE)
    set_tests_properties (${AUDIOFILE_TESTS} PROPERTIES
        ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:audiofile_read_counter>")
endif ()