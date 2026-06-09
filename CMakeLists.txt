# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf_elron)

target_sources(app PRIVATE
  src/main.c
  src/schedule.c
  src/ble_svc.c
  src/ui.c
  src/battery.c
)

target_include_directories(app PRIVATE src)
