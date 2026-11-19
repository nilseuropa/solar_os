cmake_minimum_required(VERSION 3.16)
project(solaros_tinycc NONE)
include(../../cmake/solar_os_tinycc_module.cmake)
solar_os_tinycc_module(tinycc.library.elf typed_elf)
