# build-pools.cmake: ограничить расход памяти при параллельной сборке AyuGram
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: сборка графических пакетов
# Первое изменение: 18.10.2026, последнее изменение: 19.10.2026
# Статус доступа: общедоступный, MIT

set_property(GLOBAL PROPERTY JOB_POOLS
    synapse_compile=4 synapse_heavy=3 synapse_pch=1 synapse_link=1)
set(CMAKE_JOB_POOL_COMPILE synapse_compile)
set(CMAKE_JOB_POOL_LINK synapse_link)
set(CMAKE_JOB_POOL_PRECOMPILE_HEADER synapse_pch)

# Общий пул не допускает одновременного запуска нескольких тяжёлых групп
function(ayugram_limit_memory)
    foreach(target Telegram lib_ui lib_base td_iv td_mtproto td_export)
        if(TARGET ${target})
            set_property(TARGET ${target} PROPERTY JOB_POOL_COMPILE synapse_heavy)
        endif()
    endforeach()

    if(TARGET lib_crl)
        target_compile_definitions(lib_crl PRIVATE SYNAPSE_CRL_QT_POOL=1)
        target_link_libraries(lib_crl PRIVATE Qt6::Core)

        add_executable(ayugram-async-probe /port/async-probe.cpp)
        target_link_libraries(ayugram-async-probe PRIVATE desktop-app::lib_crl Qt6::Core)
        target_compile_options(ayugram-async-probe PRIVATE -Wall -Wextra -Werror)
    endif()

    # Библиотека Alpine не содержит SONAME, поэтому линковать по имени
    if(TARGET external_tlottie AND UNIX)
        set_property(TARGET external_tlottie PROPERTY INTERFACE_LINK_LIBRARIES tlottie)
    endif()

    # Блокировать базу через каталог, как в отдельном пакете SQLite SynapseOS
    if(TARGET Telegram)
        set_property(SOURCE
            "${CMAKE_SOURCE_DIR}/Telegram/SourceFiles/ayu/libs/sqlite/sqlite3.c"
            TARGET_DIRECTORY Telegram APPEND PROPERTY COMPILE_DEFINITIONS
            "SQLITE_DEFAULT_UNIX_VFS=\"unix-dotfile\"")

        add_executable(ayugram-sqlite-probe
            /port/sqlite-probe.c
            "${CMAKE_SOURCE_DIR}/Telegram/SourceFiles/ayu/libs/sqlite/sqlite3.c")
        target_include_directories(ayugram-sqlite-probe PRIVATE
            "${CMAKE_SOURCE_DIR}/Telegram/SourceFiles/ayu/libs/sqlite")
        target_compile_definitions(ayugram-sqlite-probe PRIVATE
            "SQLITE_DEFAULT_UNIX_VFS=\"unix-dotfile\"")
        target_link_libraries(ayugram-sqlite-probe PRIVATE m dl pthread)
        target_link_options(ayugram-sqlite-probe PRIVATE -static)
        set_source_files_properties(/port/sqlite-probe.c PROPERTIES
            COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
    endif()
endfunction()

cmake_language(DEFER CALL ayugram_limit_memory)
