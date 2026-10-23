idf_component_register(
    SRCS "mesh.c" "dedup.c" "neighbors.c"
    INCLUDE_DIRS "include"
    REQUIRES link
    PRIV_REQUIRES esp_timer
)
