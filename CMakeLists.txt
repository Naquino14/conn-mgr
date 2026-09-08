cmake_minimum_required(VERSION 3.20.0)


if(CONFIG_MODULE_CONN_MGR)
    zephyr_include_directories(include)

    add_library(conn_mgr INTERFACE)

    target_include_directories(conn_mgr INTERFACE
        include/
    )

    target_sources(conn_mgr INTERFACE
        src/conn_mgr.c
    )

    target_link_libraries(conn_mgr INTERFACE zephyr_interface)
endif()