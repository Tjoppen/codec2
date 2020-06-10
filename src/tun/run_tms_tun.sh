#!/bin/bash
# This sets up the RX side. RTL-SDR connector to my laptop.
set -e
source common.sh

./top_block.py &
PIDS="$PIDS $!"

# RATE=48000
# aplay -D "plughw:CARD=Set,DEV=0" -f S16_LE -r $RATE -t raw < $FIFO2 &
# aplay -f S16_LE -r $RATE -t raw < $FIFO2 &
# PIDS="$PIDS $!"

../../build_linux/src/tms_tun tun rx $FIFO1 160000 $BAUD

# We could do something similar to run_tms_tun_pi.sh here,
# and maybe set up a route so that incoming packets can be sent to the internets

kill $PIDS

