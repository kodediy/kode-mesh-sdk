# The plan, built with CONFIG_MESH_LAB and CONFIG_MESH_LAB_PA_TABLE set. In a real
# build it comes from the owner's Kconfig (components/mesh_aos/Kconfig,
# `idf.py menuconfig`); the host suite has no mesh_aos component to carry that,
# so the same symbol is defined here, which is the name mesh_region.h tests.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/mesh_aos/mesh/src/mesh_plan.c"
         "${SDK_DIR}/components/mesh_drv/lora/src/mesh_drv_lora.c"
    INCLUDE_DIRS "."
                 "${SDK_DIR}/components/mesh_aos/mesh/src"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
    REQUIRES unity
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_MESH_LAB=1 CONFIG_MESH_LAB_PA_TABLE=1)

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
