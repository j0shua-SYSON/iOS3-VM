if(NOT DEFINED BOOTKERNEL OR NOT EXISTS "${BOOTKERNEL}")
    message(FATAL_ERROR "BOOTKERNEL must name the built bootkernel executable")
endif()

function(expect_rejected name expected)
    execute_process(
        COMMAND "${BOOTKERNEL}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    set(combined "${stdout}${stderr}")
    if(result EQUAL 0)
        message(FATAL_ERROR "${name}: invalid invocation returned success")
    endif()
    string(FIND "${combined}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "${name}: expected diagnostic '${expected}', got:\n${combined}")
    endif()
endfunction()

# Unlike the parser-only cases below, this reaches bootkernel's invariant
# self-checks before the expected missing-kernel open failure. In particular,
# the external-md layout check requires framebuffer PA 0x0885c000 and
# topOfKernelData 0x088f4000, while the diagnostic classifiers keep their truth
# tables exercised in public CI without requiring Apple firmware.
expect_rejected(startup_selfchecks "open"
    absent-kernel)
expect_rejected(requires_tree "--external-md requires -d"
    absent-kernel --external-md absent-source new-work)
expect_rejected(legacy_ramdisk "cannot be combined with -r"
    absent-kernel -d absent-tree -r absent-ramdisk
    --external-md absent-source new-work)
expect_rejected(snapshot "cold-boot only"
    absent-kernel -d absent-tree --external-md absent-source new-work
    --snapshot-at 1 absent-snapshot)
expect_rejected(wrong_ram "effective -R 128"
    absent-kernel -d absent-tree --external-md absent-source new-work -R 127)
expect_rejected(wrong_root "exact rd=md0"
    absent-kernel -d absent-tree --external-md absent-source new-work
    -c rd=disk0)
