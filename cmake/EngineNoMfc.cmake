# Plan II.6: the engine never sees MFC. Fail configuration if the engine target
# carries the MFC DLL define or any engine source includes an MFC/ATL header.
function(lb_check_engine_has_no_mfc target source_dir)
  foreach(prop COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS COMPILE_OPTIONS)
    get_target_property(values ${target} ${prop})
    if(values AND "${values}" MATCHES "_AFXDLL|_AFX")
      message(FATAL_ERROR "${target} must not use MFC (found in ${prop}: ${values})")
    endif()
  endforeach()

  file(GLOB_RECURSE engine_sources "${source_dir}/*.h" "${source_dir}/*.hpp" "${source_dir}/*.cpp")
  foreach(src IN LISTS engine_sources)
    file(STRINGS "${src}" bad_includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](afx|atl)[^>\"]*[>\"]")
    if(bad_includes)
      message(FATAL_ERROR "MFC/ATL include in engine source ${src}: ${bad_includes}")
    endif()
  endforeach()
endfunction()
