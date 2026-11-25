# mesh - the standard layer: what a task includes (mesh.h). It also brings mesh_sys, the extended
# layer, so that an app's `REQUIRES mesh` reaches either header.
#
# mesh_bsp is public because mesh.h names its positions and the region's
# limits, and mesh_types.h its types; the owner is private, so no app ever sees
# its types (tools/check_layers.sh, rule 6). The calls of an Active Object are
# in mesh_sys.
idf_component_register(
    SRCS "src/mesh.c"
    INCLUDE_DIRS "include"
    REQUIRES kdefs mesh_bsp mesh_sys
    PRIV_REQUIRES mesh_aos kode
)
