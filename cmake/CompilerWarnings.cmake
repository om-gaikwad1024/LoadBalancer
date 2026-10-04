# Common MSVC options for every target in this repo.
function(lb_set_project_options target)
  target_compile_options(${target} PRIVATE
    /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc
    /external:anglebrackets /external:W0)
  target_compile_definitions(${target} PRIVATE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE)
endfunction()
