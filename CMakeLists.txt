cmake_minimum_required(VERSION 3.10)
project(carousel C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# Expects SOEM cloned into ./SOEM  (git clone https://github.com/OpenEtherCATsociety/SOEM)
add_subdirectory(SOEM)

add_executable(carousel main.c bus.c)
target_link_libraries(carousel soem)

# If the SOEM target doesn't propagate its headers on your version, uncomment:
# target_include_directories(carousel PRIVATE
#   SOEM/soem SOEM/osal SOEM/osal/linux SOEM/oshw/linux)
