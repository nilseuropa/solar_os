cmake_minimum_required(VERSION 3.16)
project(solaros_cc NONE)
include(../../cmake/solar_os_tinycc_module.cmake)
solar_os_tinycc_module(cc.app.elf elf)
