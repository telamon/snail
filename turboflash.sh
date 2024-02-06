#!/bin/bash

# Original
#idf.py build \
#  && ( \
#    idf.py flash -b 300000 -p /dev/ttyUSB0 \
#    & idf.py flash -b 300000 -p /dev/ttyUSB1 \
#    & wait \
#  )

# Made modular by gpt
idf.py build || exit 1

BAUD=300000

for dev in /dev/ttyUSB*; do
    if [ -e "$dev" ]; then
        echo "Flashing device at $dev"
        idf.py flash -b $BAUD -p "$dev" "$@" &
    fi
done

wait
# node tools/multimon.js
