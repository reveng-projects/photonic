cmake_minimum_required(VERSION 3.16)
project(fw_fake_camera C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Require Linux firewire kernel headers
include(CheckIncludeFile)
check_include_file("linux/firewire-cdev.h" HAVE_FIREWIRE_CDEV_H)
check_include_file("linux/firewire-constants.h" HAVE_FIREWIRE_CONSTANTS_H)

if(NOT HAVE_FIREWIRE_CDEV_H OR NOT HAVE_FIREWIRE_CONSTANTS_H)
    message(FATAL_ERROR
        "Linux firewire kernel headers not found.\n"
        "Install them with: apt install linux-headers-$(uname -r)")
endif()

add_executable(fw_fake_camera
    fake.c
    fwdev.c
    dcam.c
    video.c
    format7.c
    features.c
    i2c.c
    frame.c
    iso.c
    avc.c
    mailbox.c
    trigger.c
    log.c)

target_compile_options(fw_fake_camera PRIVATE -Wall -Wextra -Werror -Wno-unused-const-variable)

find_package(Threads REQUIRED)
target_link_libraries(fw_fake_camera PRIVATE Threads::Threads)

# Static build
target_link_options(fw_fake_camera PRIVATE -static)

# Passive isochronous receive verifier (packet-loss diagnostics)
add_executable(irlisten
    irlisten.c
    log.c)
target_compile_options(irlisten PRIVATE -Wall -Wextra -Werror)
target_link_options(irlisten PRIVATE -static)
