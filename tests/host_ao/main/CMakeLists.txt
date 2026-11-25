# The suite is one component: the runner, the cases, and the sources under test -
# mesh_ao.c, #included by its case file so a case reaches its statics, and kode-sdk's own
# kqp_isr.c (the door the radio's interrupt posts through) and kqp_self.c (who is an Active
# Object, and who holds a level). What is faked is below them: klog's kode_log() and khal's
# khal_task_self() (fakes/).
#
# CONFIG_KODE_LOWPOWER=1: the image the owner is built for has kode-sdk's sleep rounds, and
# so has the suite; without it the owner's rest and wake would not be compiled.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KC "${KODE_SDK_DIR}/components")   # ../CMakeLists.txt fetches it

# kode_ao.h is kode.h, kqp.h and kaos.h: the public header of every layer.
# None of them includes an ESP-IDF one, so they all compile for `linux`.
file(GLOB KODE_INCLUDES LIST_DIRECTORIES true
     "${KC}/kdefs/include"
     "${KC}/kode/include"
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
         "${KC}/kqp/src/kqp_isr.c"
         "${KC}/kqp/src/kqp_self.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
    INCLUDE_DIRS "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/mesh_aos/mesh/src"
                 "${SDK_DIR}/components/mesh_aos/core/include"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 "${SDK_DIR}/components/mesh_drv/nmea/include"
                 ${KODE_INCLUDES}
    REQUIRES unity qpc freertos
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_KODE_LOWPOWER=1)

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
