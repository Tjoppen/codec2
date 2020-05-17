# this file is sourced by run_tms_tun*.sh
FIFO1=/tmp/channel0.iq
FIFO2=/tmp/tx0.s16le

rm -f $FIFO1 $FIFO2
mkfifo $FIFO1 $FIFO2
PIDS=

cleanup() {
  echo killing $PIDS
  kill $PIDS
  rm $FIFO1 $FIFO2
}

trap cleanup INT
