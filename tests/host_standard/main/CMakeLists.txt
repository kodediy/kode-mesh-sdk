# The runner, the cases, the standard layer under test (mesh.c), the argument checks both layers
# share (mesh_aos_args.c, plain C), and the owner they are given. The owner is the mock, so that
# the one request each call makes can be read back; kode.h is kode-sdk's own header, with
# kode_init() faked.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KC "${KODE_SDK_DIR}/components")

file(GLOB KODE_INCLUDES LIST_DIRECTORIES true
     "${KC}/kdefs/include"
     "${KC}/kode/include"
     "${KC}/kode/internal/include"
     "${KC}/kode_sys/include"
     "${KC}/kqp/include"
     "${KC}/klog/include"
     "${KC}/kaos/*/include"
     "${KC}/kdrv/*/include"
     "${KC}/khal/*/include"
     "${KC}/kbsp/*/include")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/mesh/src/mesh.c"
         "${SDK_DIR}/components/mesh_aos/mesh/src/mesh_aos_args.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_mesh_aos.c"
    INCLUDE_DIRS "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/mesh/include"
                 "${SDK_DIR}/components/mesh_aos/mesh/src"
                 "${SDK_DIR}/components/mesh_aos/core/include"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
                 "${SDK_DIR}/components/mesh_drv/nmea/include"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 ${KODE_INCLUDES}
    REQUIRES unity
    WHOLE_ARCHIVE
)

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
