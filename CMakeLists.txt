cmake_minimum_required(VERSION 3.16)

project(AuctionService LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(libpqxx REQUIRED)

add_executable(auction_service
    src/main.cpp
    src/database.cpp
)

target_include_directories(auction_service
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(auction_service
    PRIVATE
        libpqxx::pqxx
        Threads::Threads
)

