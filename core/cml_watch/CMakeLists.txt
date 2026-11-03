cml_add_core_library(cml_watch
    SOURCES src/watch.cpp src/directory_watcher.cpp
    LIBS cml_util)
if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(cml_watch PUBLIC Threads::Threads)
endif()
