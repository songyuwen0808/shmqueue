cmake_minimum_required(VERSION 3.6)
project(shmqueue)

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++11")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Ofast -std=c++11 -g")

set(SOURCE_FILES main.cpp
        shmmqueue.h
        shm_rwlock.h)
add_executable(shmqueue ${SOURCE_FILES})
target_link_libraries(shmqueue pthread)
