idf_component_register(
  SRCS "snail.c" "swap.c" "./sha2/sha-256.c" "./picofeed/c/picofeed.c" "./monocypher/src/monocypher.c" "wrpc.c"
  INCLUDE_DIRS "." "./sha2" "./negentropy/cpp/" "./picofeed/c/" "./monocypher/src/"
)

