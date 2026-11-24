# cmake/Sanitizers.cmake
# Configures Address, Undefined Behavior, and Thread Sanitizers

function(kvs_enable_sanitizers target_name)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        if(KVS_ENABLE_ASAN AND KVS_ENABLE_TSAN)
            message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined simultaneously.")
        endif()

        set(SANITIZER_FLAGS "")

        if(KVS_ENABLE_ASAN)
            list(APPEND SANITIZER_FLAGS
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -fno-optimize-sibling-calls
            )
            # ASan options for leak detection
            target_compile_definitions(${target_name} PRIVATE KVS_ASAN_ENABLED=1)
        endif()

        if(KVS_ENABLE_TSAN)
            list(APPEND SANITIZER_FLAGS
                -fsanitize=thread
                -fno-omit-frame-pointer
            )
            target_compile_definitions(${target_name} PRIVATE KVS_TSAN_ENABLED=1)
        endif()

        if(SANITIZER_FLAGS)
            target_compile_options(${target_name} PRIVATE ${SANITIZER_FLAGS})
            target_link_options(${target_name} PRIVATE ${SANITIZER_FLAGS})
        endif()
    endif()
endfunction()
