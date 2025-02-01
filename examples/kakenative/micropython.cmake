add_library(usermod_kakenative INTERFACE)

# Add our source files to the lib
target_sources(usermod_kakenative INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/kakenative.c
    ${CMAKE_CURRENT_LIST_DIR}/hxcmod.c
)

# Add the current directory as an include directory.
target_include_directories(usermod_kakenative INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_kakenative)
