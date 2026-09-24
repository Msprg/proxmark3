#!/usr/bin/env bash
make clean
make -j PLATFORM_FILE=$PLTFRM
./pm3-flash-all
