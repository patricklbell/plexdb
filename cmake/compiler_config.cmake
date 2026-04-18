if(NOT TARGET plexdb::compiler_config)
    add_library(project_compiler_config INTERFACE)
    add_library(plexdb::compiler_config ALIAS project_compiler_config)

    target_compile_options(project_compiler_config INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/EHs-c->
        $<$<CXX_COMPILER_ID:MSVC>:/GR->

        $<$<CXX_COMPILER_ID:GNU>:-Wall>
        $<$<CXX_COMPILER_ID:GNU>:-Wextra>
        $<$<CXX_COMPILER_ID:GNU>:-fno-exceptions>
        $<$<CXX_COMPILER_ID:GNU>:-fno-rtti>

        $<$<CXX_COMPILER_ID:Clang>:-Wall>
        $<$<CXX_COMPILER_ID:Clang>:-Wextra>
        $<$<CXX_COMPILER_ID:Clang>:-Wdangling>
        $<$<CXX_COMPILER_ID:Clang>:-fno-exceptions>
        $<$<CXX_COMPILER_ID:Clang>:-fno-rtti>
    )

    if (PLEXDB_DISABLE_EXCEPTIONS)
        target_compile_options(project_compiler_config INTERFACE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions>
            $<$<CXX_COMPILER_ID:MSVC>:/EHs-c->
        )
    endif()

    target_compile_definitions(project_compiler_config INTERFACE
        $<$<BOOL:${PLEXDB_DEBUG}>:PLEXDB_DEBUG=1>
        $<$<BOOL:${PLEXDB_ENABLE_LOGGING}>:PLEXDB_ENABLE_LOGGING=1>
    )
    target_link_options(project_compiler_config INTERFACE
        $<$<BOOL:${PLEXDB_ENABLE_LOGGING}>:-rdynamic>
    )
endif()