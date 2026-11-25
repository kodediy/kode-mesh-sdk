# The suite is one component: the runner, the cases, and the plain-C sources
# under test.
#
# The cases are globbed and the sources under test are listed, on purpose:
# adding a case is adding a file, but pulling a production source into the
# host build is a decision about what it drags in, and a decision should cost
# a line somebody reads. All three are plain C and need only Unity; one that
# needed a fake would belong in tests/host_owner or tests/host_drv instead.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/mesh_aos/mesh/src/mesh_plan.c"
    "${SDK_DIR}/components/mesh_drv/lora/src/mesh_drv_lora.c"
    "${SDK_DIR}/components/mesh_drv/nmea/src/mesh_drv_nmea.c"
)

# Sorted, so every machine and CI run the cases in the same order: a case
# registers itself from a constructor, in the order the linker meets it.
file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be
# dropped from the link, and Unity would report OK over what survived.
# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake.
# Not through the component manager: from v0.1.0 kdefs declares esp32p4 only and
# the manager refuses it for `linux` (kode-sdk's own host suites take it this way).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         ${SOURCES_UNDER_TEST}
    INCLUDE_DIRS "."
                 "${KODE_SDK_DIR}/components/kdefs/include"
                 "${SDK_DIR}/components/mesh_aos/mesh/src"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 "${SDK_DIR}/components/mesh_drv/nmea/include"
    REQUIRES unity
    WHOLE_ARCHIVE
)

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
