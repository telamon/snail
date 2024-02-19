idf_component_register(
  SRCS "snail.c" "swap.c" "pico_repo_flash_rb.c" "./picofeed/c/picofeed.c" "./monocypher/src/monocypher.c" "wrpc.c" "recon_sync.cpp"
  INCLUDE_DIRS "." "./negentropy/cpp/" "./picofeed/c/" "./monocypher/src/"
)

