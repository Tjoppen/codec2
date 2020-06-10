/*

git push pi tms_tun && ssh pi@raspberrypi.local "cd codec2/src/tun && (cd ../../build_linux/ && git pull && ninja)"

scp -p CMakeLists.txt pi@raspberrypi.local:codec2/ && scp -p src/CMakeLists.txt pi@raspberrypi.local:codec2/src/ && scp -rp src/tun/* pi@raspberrypi.local:codec2/src/tun/ && ssh pi@raspberrypi.local "cd codec2/src/tun && (cd ../../build_linux/ && git pull && ninja)"

*/

#include "../../rpitx/src/librpitx.h"
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "USAGE: %s f0 symbol_rate\n", argv[0]);
    return 1;
  }

  uint64_t f = atoll(argv[1]);
  float symbol_rate = atof(argv[2]);
  int channel = 14;
  int fifosize = 25000;
  fprintf(stderr, "%lli %f\n", f, symbol_rate);
  if (symbol_rate < 0) {
    return 1;
  }
  fskburst fsk(f, symbol_rate, symbol_rate, channel, fifosize);

  for (;;) {
    unsigned char buf[1600*8];
    int n = read(0, buf, sizeof(buf));
    fprintf(stderr, "n = %i\n", n);
    //int n = fread(buf, 1, sizeof(buf), stdin);
    if (n <= 0) {
      break;
    }
    /*int mask = 1;
    for (int i = 0; i < n; i++) {
      buf[i] &= mask;
    }
    for (int i = 0; i < 8*8; i++) {
      buf[i] = i & mask;
      buf[n-i-1] = i & mask;
    }*/
    fsk.SetSymbols(buf, n);
  }

  fsk.stop();

  return 0;
}