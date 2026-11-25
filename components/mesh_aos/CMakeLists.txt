# mesh_aos - the owner: one Active Object that finds the module, holds its
# positions and decides when everything happens. mesh_ao.c is the Active
# Object, on kode-sdk's QP/C (kqp.h); mesh_aos.c is what its signals do, and
# mesh_aos_events.c is the standard layer's events task.
#
# Laid out as kaos is: what the two layers include in core/include, and each AO
# in a folder of its own with its sources and private headers in src/. There
# is one AO here, AO_Mesh, so there is one folder.
#
# mesh_plan.c is plain C on purpose - no ESP-IDF, no FreeRTOS, no kode-sdk -
# because it is every rule of what the radio may do and tests/host runs it
# as it is.
idf_component_register(
    SRCS "mesh/src/mesh_aos.c"
         "mesh/src/mesh_aos_events.c"
         "mesh/src/mesh_aos_args.c"
         "mesh/src/mesh_ao.c"
         "mesh/src/mesh_plan.c"
    INCLUDE_DIRS "core/include"
    PRIV_INCLUDE_DIRS "mesh/src"
    REQUIRES kdefs mesh_drv mesh_bsp
    PRIV_REQUIRES kode_sys kqp klog freertos
)
