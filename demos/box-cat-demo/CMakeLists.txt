cmake_minimum_required(VERSION 4.4)

project(box_cat_demo VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

huxerui_add_app(box_cat_demo
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        ${HUXERUI_APP_RESOURCE_OUTPUT_ARGUMENTS}
        BUNDLE_NAME
            "box-cat-demo"
        BUNDLE_IDENTIFIER
            "dev.farna.clashflux.boxcatdemo"
)

huxerui_configure_project_app(box_cat_demo)
if (MSVC AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    target_compile_options(box_cat_demo PRIVATE /utf-8)
endif ()
