# WP-03 spike: throwaway end-to-end proof of the hot-reload pipeline (docs/wp/WP-03.md).
#
#   cml_spike   one sub-command per pipeline step (anchor, bind, reserve, load, retarget, hook).
#               spike/run_spike.py drives it from CI. Built with the repo's /W4 /WX helpers.
add_executable(cml_spike
    main.cpp
    common.cpp
    obj_tools.cpp
    externals.cpp
    win_remote.cpp)
target_link_libraries(cml_spike PRIVATE cml_coff cml_pe cml_pdb cml_util)
cml_apply_warnings(cml_spike)
cml_target_core_style(cml_spike)
set_target_properties(cml_spike PROPERTIES FOLDER "spike")
