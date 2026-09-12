cmake_minimum_required(VERSION 3.13)
project(lcorehttp C)

file(GLOB lcorehttp_sources src/*.c)

add_library(lcorehttp ${lcorehttp_sources})