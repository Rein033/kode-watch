# The requirement on bt is unconditional: requirements are fixed in an early
# pass where CONFIG_ values are not reliable; only the sources depend on NimBLE.
set(srcs "")
if(CONFIG_BT_NIMBLE_ENABLED)
    set(srcs "misc.c" "peer.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt)
