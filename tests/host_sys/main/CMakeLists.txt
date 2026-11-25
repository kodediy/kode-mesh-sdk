# The suite is one component: the runner, the cases, the extended layer under test
# (mesh_sys.c), the argument checks both layers share (mesh_aos_args.c, plain C), and
# a stand-in for the owner. What is faked is below them: klog's kode_log() and the
# owner (fakes/).
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KC "${KODE_SDK_DIR}/components")   # ../CMakeLists.txt fetches it

# kode_sys.h is the public header of kodeOS's extended layer: kode.h, kqp.h and kaos.h.
# None of them includes an ESP-IDF one, so they all compile for `linux`.
file(GLOB KODE_INCLUDES LIST_DIRECTORIES true
     "${KC}/kdefs/include"
     "${KC}/kode/include"
     "${KC}/kode_sys/include"
     "${KC}/kqp/include"
     "${KC}/klog/include"
     "${KC}/kaos/*/include"
     "${KC}/kdrv/*/include"
     "${KC}/khal/*/include"
     "${KC}/kbsp/*/include")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be
# dropped from the link, and Unity would report OK over what survived.
idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/mesh_sys/src/mesh_sys.c"
         "${SDK_DIR}/components/mesh_aos/mesh/src/mesh_aos_args.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_owner.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
    INCLUDE_DIRS "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/mesh_sys/include"
                 "${SDK_DIR}/components/mesh_aos/mesh/src"
                 "${SDK_DIR}/components/mesh_aos/core/include"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 "${SDK_DIR}/components/mesh_drv/nmea/include"
                 ${KODE_INCLUDES}
    REQUIRES unity qpc freertos
    WHOLE_ARCHIVE
)

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
