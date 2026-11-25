# The runner, the cases, the fakes they drive, and kodeOS's console registry
# with its rules. examples/console_actions/main/console_actions_ao.c is the
# source under test, #included by its case file so a case reaches its state
# handler: it is not listed here. mesh_sys.h is the real one, over a fake
# kode_sys.h (fakes/) that keeps kode-sdk's console tables and klog's macros as
# they ship and fakes only the QP/C the example touches; what answers
# mesh_sys.h is fakes/fake_mesh_sys.c. See tests/host/main for why the cases
# are globbed, sorted and linked whole.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KC "${KODE_SDK_DIR}/components")   # ../CMakeLists.txt fetches it

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_console.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_mesh_sys.c"
    INCLUDE_DIRS "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/mesh_sys/include"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 "${SDK_DIR}/components/mesh_drv/nmea/include"
                 "${SDK_DIR}/components/mesh_aos/core/include"
                 "${KC}/kdefs/include"
                 "${KC}/kaos/core/include"
                 "${KC}/klog/include"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
                 "${SDK_DIR}/examples/console_actions/main"
    REQUIRES unity
    WHOLE_ARCHIVE
)

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
