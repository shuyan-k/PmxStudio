# CMake generated Testfile for 
# Source directory: /workspace
# Build directory: /workspace/build-headless
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[core]=] "/workspace/build-headless/pmx_tests")
set_tests_properties([=[core]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;169;add_test;/workspace/CMakeLists.txt;0;")
add_test([=[anim]=] "/workspace/build-headless/pmx_tests_anim")
set_tests_properties([=[anim]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;182;add_test;/workspace/CMakeLists.txt;0;")
add_test([=[bake]=] "/workspace/build-headless/pmx_tests_bake")
set_tests_properties([=[bake]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;182;add_test;/workspace/CMakeLists.txt;0;")
add_test([=[script]=] "/workspace/build-headless/pmx_tests_script")
set_tests_properties([=[script]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;182;add_test;/workspace/CMakeLists.txt;0;")
