#!/bin/bash
# This sets up the TX side. Runs on a Raspberry Pi 3.
set -e
source common.sh

RATE=192000
# aplay -D "plughw:CARD=Set,DEV=0" -f S16_LE -r $RATE -t raw < $FIFO2 &
# aplay -f S16_LE -r $RATE -t raw < $FIFO2 &
# APLAY_PID=$!
killall -9 ping || true
killall -9 sendiq || true
sendiq -i $FIFO2 -s $RATE -f 144700000 -t float &
PIDS="$PIDS $!"

#../../build_linux/src/tms_tun tun rx $FIFO1 200000 800 25 tx $FIFO2 $RATE 800 234 6 2
#../../build_linux/src/tms_tun tun rx $FIFO1 200000 800 25
#../../build_linux/src/tms_tun tun tx $FIFO2 $RATE 800 1000 6
../../build_linux/src/tms_tun tun tx $FIFO2 $RATE 16000 234 3 2 &
#../../build_linux/src/tms_tun tun tx $FIFO2 $RATE 16 234 12000 2 &
PIDS="$PIDS $!"
sleep 1

# Bring the TUN device up and ping it
ip link set tun77 up
ip addr add 10.0.0.1/24 dev tun77
ping -s 1450 10.0.0.2
# ping 10.0.0.2
