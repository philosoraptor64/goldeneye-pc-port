# n64net client (external/n64net): the input ledger (lockstep/rollback) and the room-code rendezvous.
# Built as its own library so it sees the host's headers, not the decomp's include/ shadows.
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")
set(N64NET_DIR "${CMAKE_SOURCE_DIR}/external/n64net/client")
add_library(n64net STATIC
  "${N64NET_DIR}/src/n64net_input.cpp"
  "${N64NET_DIR}/src/n64net_rendezvous.cpp"
  "${CMAKE_SOURCE_DIR}/port/net/genet_transport.c")
target_include_directories(n64net PUBLIC "${N64NET_DIR}/include" "${CMAKE_SOURCE_DIR}/port/net")
set_target_properties(n64net PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
