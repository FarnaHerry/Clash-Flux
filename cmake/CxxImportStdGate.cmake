# CxxImportStdGate.cmake — enable CMake's experimental `import std` support.
#
# `import std;`（本仓库所有 apitab.* 模块都在用）在 CMake 里仍是 experimental
# feature：必须在 `project()` 之前把 CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 设成
# 该 CMake 版本文档指定的 UUID，否则扫描器不认识 std 模块，配置直接失败。
# UUID 随 CMake 版本变（每个版本都不一样！见各版本源码
# Help/dev/experimental.rst 的 "C++ import std support" 一节）——升级 CMake
# 后如果 configure 报 "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD is set to incorrect
# value"，去那里查新值并补一行。已核对：3.30/3.31、4.0–4.2、4.3、4.4。
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.4")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
else()
    # 3.30 – 3.31
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
endif()
