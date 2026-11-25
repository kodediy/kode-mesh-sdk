# The suite is one component: the runner, the cases, and the real SDK
# sources under test (the owner, and the plain C it stands on).
#
# The cases are globbed and the sources under test are listed, on purpose:
# adding a case is adding a file, but pulling a production source into the
# host build is a decision about what it drags in, and a decision should cost
# a line somebody reads. Three of them are plain C (mesh_plan.c, mesh_drv_lora.c
# and mesh_drv_nmea.c) and need only Unity. The fourth is the owner, mesh_aos.c;
# everything it stands on is a fake in fakes/: the radio (fake_lr2021.c), the
# GNSS, the pins, kode.h and the log (fake_misc.c) and its Active Object
# (fake_mesh_ao.c, at mesh_ao.h). tests/host_drv is where the radio driver is
# tested for real, and tests/host_ao where mesh_ao.c is.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/mesh_aos/mesh/src/mesh_aos.c"
    "${SDK_DIR}/components/mesh_aos/mesh/src/mesh_aos_events.c"
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
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_lr2021.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_mesh_ao.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_misc.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/owner_harness.c"
    INCLUDE_DIRS "."
                 "${KODE_SDK_DIR}/components/kdefs/include"
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/mesh_aos/core/include"
                 "${SDK_DIR}/components/mesh_aos/mesh/src"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
                 "${SDK_DIR}/components/mesh_drv/gnss/include"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 "${SDK_DIR}/components/mesh_drv/lr2021/include"
                 "${SDK_DIR}/components/mesh_drv/nmea/include"
                 "${SDK_DIR}/components/mesh_drv/pins/include"
    REQUIRES unity freertos
    WHOLE_ARCHIVE
)

# sdkconfig.h has no CONFIG_KODEOS_* here, with kode-sdk faked, so the image's
# setting is spelled out as an app's would be: the C5 link is there, and a denied
# kode_sys_exp_power() is a refusal, not an image with J3 always powered.
# A send that began and is stuck waits MESH_AOS_SEND_GRACE_MS for the owner; a
# case that stalls the owner would wait two minutes, so the suite shortens it.
target_compile_definitions(${COMPONENT_LIB} PRIVATE "CONFIG_KODEOS_RADIO=1" "MESH_AOS_SEND_GRACE_MS=3000U")

# The test signal is a lab build's (CONFIG_MESH_LAB), and this suite's build keeps the
# region's limits, as an app's does. What the owner does with a test signal - its deadline, a
# stop, a restart - is checked here all the same, so the plan's one question, "may this build
# put a test signal on air?", is the suite's to answer (g_plan_test_allowed, fakes/fake_misc.c).
# mesh_plan.c's own answer is built under another name, and a case reads it.
set_source_files_properties("${SDK_DIR}/components/mesh_aos/mesh/src/mesh_plan.c"
    PROPERTIES COMPILE_DEFINITIONS "mesh_plan_test_allowed=mesh_plan_test_allowed_as_built")

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
