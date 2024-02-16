#!/bin/bash
set -eu

# Check if the first argument is provided
if [ -z "${1-}" ]; then
    echo "Usage: $0 <CHIP> # esp32|esp32s2|esp32c3|esp32s3 so on..."
    exit 1
fi

CHIP=$1
VER=$(git describe --tag)
OUTFILE=releases/SNAIL-${VER}-${CHIP}.bin
# Build binaries

idf.py set-target $CHIP
idf.py build

mkdir -p releases/

# Flash size is actually 4MB, but we'll use the last remaining as storage.
esptool.py --chip $CHIP \
  merge_bin --output $OUTFILE --format raw --flash_mode dio --flash_size 2MB \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/snail.bin

echo "flash command: "
echo ""
echo "	  $ esptool.py -b 300000 --before default_reset --after hard_reset write_flash -z 0x0 ${OUTFILE}"
echo ""
