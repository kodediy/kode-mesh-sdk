# mesh_sys - the extended layer: what an Active Object includes (mesh_sys.h).
#
# The calls of an AO never wait: a command is posted, a request is posted with
# its answer already taken from the pool, and the news come as signals. The
# owner (mesh_aos) is private; so are its types. QP/C reaches this component
# through kode_sys.h (tools/check_layers.sh, rules 7 and 13).
idf_component_register(
    SRCS "src/mesh_sys.c"
    INCLUDE_DIRS "include"
    REQUIRES kdefs kode_sys mesh_bsp
    PRIV_REQUIRES mesh_aos klog
)
