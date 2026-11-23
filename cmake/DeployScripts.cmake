if(NOT DEFINED CLIPPER_SCRIPT_SOURCE OR NOT DEFINED CLIPPER_SCRIPT_DESTINATION)
    message(FATAL_ERROR "Script source and destination are required")
endif()
file(MAKE_DIRECTORY "${CLIPPER_SCRIPT_DESTINATION}")
file(GLOB scripts "${CLIPPER_SCRIPT_SOURCE}/*.py")
foreach(script IN LISTS scripts)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${script}" "${CLIPPER_SCRIPT_DESTINATION}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot deploy script: ${script}")
    endif()
endforeach()
