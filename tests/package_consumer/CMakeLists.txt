cmake_minimum_required(VERSION 3.25)

project(qlic_package_consumer LANGUAGES C)

find_package(qlic 0.5 CONFIG REQUIRED)

add_executable(qlic-package-consumer-static main.c)
target_link_libraries(qlic-package-consumer-static PRIVATE qlic::qlic_static)

add_executable(qlic-package-consumer-shared main.c)
target_link_libraries(qlic-package-consumer-shared PRIVATE qlic::qlic)
