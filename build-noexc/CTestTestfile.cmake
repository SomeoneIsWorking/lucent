# CMake generated Testfile for 
# Source directory: /home/bhamil/repo/lucent
# Build directory: /home/bhamil/repo/lucent/build-noexc
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(lucent_tests "/home/bhamil/repo/lucent/build-noexc/lucent_tests")
set_tests_properties(lucent_tests PROPERTIES  _BACKTRACE_TRIPLES "/home/bhamil/repo/lucent/CMakeLists.txt;56;add_test;/home/bhamil/repo/lucent/CMakeLists.txt;0;")
add_test(lucent_http_tests "/home/bhamil/repo/lucent/build-noexc/lucent_http_tests")
set_tests_properties(lucent_http_tests PROPERTIES  _BACKTRACE_TRIPLES "/home/bhamil/repo/lucent/CMakeLists.txt;60;add_test;/home/bhamil/repo/lucent/CMakeLists.txt;0;")
add_test(lucent_env_name_tests "/home/bhamil/repo/lucent/build-noexc/lucent_env_name_tests")
set_tests_properties(lucent_env_name_tests PROPERTIES  ENVIRONMENT "MYAPP_DEBUG=early" _BACKTRACE_TRIPLES "/home/bhamil/repo/lucent/CMakeLists.txt;70;add_test;/home/bhamil/repo/lucent/CMakeLists.txt;0;")
add_test(cpp_quality "/home/bhamil/repo/lucent/tools/check_cpp_quality.sh" "/home/bhamil/repo/lucent/build-noexc")
set_tests_properties(cpp_quality PROPERTIES  _BACKTRACE_TRIPLES "/home/bhamil/repo/lucent/CMakeLists.txt;76;add_test;/home/bhamil/repo/lucent/CMakeLists.txt;0;")
add_test(source_structure "/home/bhamil/.local/bin/python3.12" "/home/bhamil/repo/lucent/tools/check_source_structure.py")
set_tests_properties(source_structure PROPERTIES  _BACKTRACE_TRIPLES "/home/bhamil/repo/lucent/CMakeLists.txt;79;add_test;/home/bhamil/repo/lucent/CMakeLists.txt;0;")
add_test(source_structure_selftest "/home/bhamil/.local/bin/python3.12" "/home/bhamil/repo/lucent/tools/check_source_structure.py" "--selftest")
set_tests_properties(source_structure_selftest PROPERTIES  _BACKTRACE_TRIPLES "/home/bhamil/repo/lucent/CMakeLists.txt;81;add_test;/home/bhamil/repo/lucent/CMakeLists.txt;0;")
