#!/bin/bash


idf.py build || exit 1
BAUD=300000
for dev in /dev/ttyUSB*; do
    if [ -e "$dev" ]; then
        echo "Flashing device at $dev"
        idf.py app-flash -b $BAUD -p "$dev" &
	# "$@"
    fi
done

wait

if [ "$1" = "purge" ]; then
    for dev in /dev/ttyUSB*; do
        if [ -e "$dev" ]; then
            echo "Erasing region for device at $dev"
            esptool.py --port "$dev" erase_region 0x200000 0x200000 &
        fi
    done
    wait
fi
# node tools/multimon.js

# Original
#idf.py build \
#  && ( \
#    idf.py flash -b 300000 -p /dev/ttyUSB0 \
#    & idf.py flash -b 300000 -p /dev/ttyUSB1 \
#    & wait \
#  )
