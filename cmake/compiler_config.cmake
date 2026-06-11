if(NOT TARGET plexdb::compiler_config)
  add_library(plexdb_compiler_config INTERFACE)
  add_library(plexdb::compiler_config ALIAS plexdb_compiler_config)

  target_compile_options(plexdb_compiler_config INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/GR->

        $<$<CXX_COMPILER_ID:GNU>:-Wall>
        $<$<CXX_COMPILER_ID:GNU>:-Wextra>
        $<$<CXX_COMPILER_ID:GNU>:-Werror>
        $<$<CXX_COMPILER_ID:GNU>:-fno-rtti>

        $<$<CXX_COMPILER_ID:Clang>:-Wall>
        $<$<CXX_COMPILER_ID:Clang>:-Wextra>
        $<$<CXX_COMPILER_ID:Clang>:-Werror>
        $<$<CXX_COMPILER_ID:Clang>:-Wdangling>
        $<$<CXX_COMPILER_ID:Clang>:-fno-rtti>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${PLEXDB_FTIME_TRACE}>>:-ftime-trace>
    )

  if(PLEXDB_DISABLE_EXCEPTIONS)
    if(PLEXDB_ENABLE_TESTS)
      message(WARNING "Disabling exceptions AND compiling tests may lead to unexpected race conditions")
    endif()
    target_compile_options(plexdb_compiler_config INTERFACE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions>
            $<$<CXX_COMPILER_ID:MSVC>:/EHs-c->
        )
  endif()

  target_compile_definitions(plexdb_compiler_config INTERFACE
        $<$<BOOL:${PLEXDB_DEBUG}>:PLEXDB_DEBUG=1>
        $<$<BOOL:${PLEXDB_ENABLE_PLUGINS}>:PLEXDB_ENABLE_PLUGINS=1>
        $<$<BOOL:${PLEXDB_ENABLE_USER_KILL_SIGNAL}>:PLEXDB_ENABLE_USER_KILL_SIGNAL=1>
    )
  target_link_options(plexdb_compiler_config INTERFACE
        $<$<BOOL:${PLEXDB_ENABLE_PLUGINS}>:-rdynamic>
    )
endif()
