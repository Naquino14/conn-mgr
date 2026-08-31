cmake_minimum_required(VERSION 3.20.0)

add_library(conn_mgr INTERFACE)

target_sources(conn_mgr INTERFACE
    src/conn_mgr.c
)

target_include_directories(conn_mgr INTERFACE
    include/
)

target_link_directories(conn_mgr INTERFACE zephyr_interface)