# detect_cache.cmake — auto-derive cache-tuning constants from /sys.
# Reads the host CPU's L2 and L3 cache sizes from sysfs and computes:
#   - QPLAYER_CACHE_BLOCK_SIZE: number of complex<double> pairs that fit in L2
#       Each element is 16 bytes (re + im); we leave 50 % headroom for control
#       state and OpenMP per-thread scratch.
#   (L3 size is probed and reported via message(STATUS ...) for diagnostics only;
#    it is NOT injected as a compile definition. The former
#    QPLAYER_L3_THRESHOLD_QUBITS existed solely for non-temporal stores, which
# dropped -- no code ever read it. See.)
#
# This is exposed as a target compile definition on qplayer_core so the kernels
# can pick it up at compile time. The script is Linux-only; on
# any other host it falls back to a conservative default and warns once.

function(_qplayer_read_sysfs_size FILE OUT)
    set(${OUT} 0 PARENT_SCOPE)
    if(NOT EXISTS "${FILE}")
        return()
    endif()
    file(READ "${FILE}" raw)
    string(STRIP "${raw}" raw)
    if(raw MATCHES "^([0-9]+)([KMG]?)$")
        set(value ${CMAKE_MATCH_1})
        set(unit ${CMAKE_MATCH_2})
        if(unit STREQUAL "K")
            math(EXPR value "${value} * 1024")
        elseif(unit STREQUAL "M")
            math(EXPR value "${value} * 1048576")
        elseif(unit STREQUAL "G")
            math(EXPR value "${value} * 1073741824")
        endif()
        set(${OUT} ${value} PARENT_SCOPE)
    endif()
endfunction()

function(qplayer_detect_cache TARGET)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(STATUS "[qplayer] Non-Linux host detected; using conservative cache defaults")
        target_compile_definitions(${TARGET} PRIVATE
            QPLAYER_CACHE_BLOCK_SIZE=4096)
        return()
    endif()

    set(L2_SIZE 0)
    set(L3_SIZE 0)

    # Scan cache indices and pick the largest L2 and L3 line on cpu0. Index
    # numbering varies across CPUs (some put L1d at index0, L1i at index1,
    # L2 at index2, L3 at index3 — but Zen/Skylake may differ).
    foreach(idx 0 1 2 3 4)
        set(level_file "/sys/devices/system/cpu/cpu0/cache/index${idx}/level")
        set(size_file  "/sys/devices/system/cpu/cpu0/cache/index${idx}/size")
        set(type_file  "/sys/devices/system/cpu/cpu0/cache/index${idx}/type")
        if(EXISTS "${level_file}" AND EXISTS "${size_file}" AND EXISTS "${type_file}")
            file(READ "${level_file}" level)
            file(READ "${type_file}"  type)
            string(STRIP "${level}" level)
            string(STRIP "${type}"  type)
            _qplayer_read_sysfs_size("${size_file}" size_bytes)
            # Skip instruction-only caches; we care about data/unified.
            if(NOT type STREQUAL "Instruction")
                if(level STREQUAL "2" AND size_bytes GREATER L2_SIZE)
                    set(L2_SIZE ${size_bytes})
                elseif(level STREQUAL "3" AND size_bytes GREATER L3_SIZE)
                    set(L3_SIZE ${size_bytes})
                endif()
            endif()
        endif()
    endforeach()

    # Fallback defaults if sysfs probe yielded nothing (containers, etc.).
    if(L2_SIZE EQUAL 0)
        set(L2_SIZE 524288)  # 512 KB
    endif()
    if(L3_SIZE EQUAL 0)
        set(L3_SIZE 16777216)  # 16 MB
    endif()

    # Each state-vector slot stores re+im = 16 bytes. Halve to keep room for
    # control structures and OpenMP per-thread scratch.
    math(EXPR CACHE_BLOCK_SIZE "${L2_SIZE} / (2 * 16)")

    # Find smallest n such that 2^n * 16 > L3_SIZE.
    set(THRESHOLD 0)
    set(state_bytes 16)
    foreach(n RANGE 0 35)
        if(state_bytes GREATER L3_SIZE)
            set(THRESHOLD ${n})
            break()
        endif()
        math(EXPR state_bytes "${state_bytes} * 2")
    endforeach()

    message(STATUS "[qplayer] Detected L2=${L2_SIZE}B L3=${L3_SIZE}B "
                   "-> CACHE_BLOCK_SIZE=${CACHE_BLOCK_SIZE} "
                   "(L3 crossover at ~${THRESHOLD} qubits, diagnostic only)")

    # Only CACHE_BLOCK_SIZE is injected -- the cache-blocked 1Q/2Q kernels consume it
    # (opt-). THRESHOLD stays local to the status message.
    target_compile_definitions(${TARGET} PRIVATE
        QPLAYER_CACHE_BLOCK_SIZE=${CACHE_BLOCK_SIZE})
endfunction()
