/*
  TUN/TAP 2FSK packet thingy
  Sets up a TUN or TAP device and modulates packets coming from it onto the air.
  It can also demodulate packets heard on the air and hand them over to the device.
  I have only tested the TUN stuff so far. TAP would be useful for ARP and other link-level stuff.
  We probably want our own link protocols.
*/

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include "fsk.h"
#include "golay23.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define MTU 1600
#define HEADER 0
// FOOTER == 46 ==> length is golay23 and manchester encoded
// FOOTER == 66 ==> length is repeated six times, manchester coded
#define FOOTER 46
// if 1 then use soft decoding
#define FOOTER_SD 1

// file descriptor for the TUN device. needed by all threads, therefore global
int tunfd;

// pre- and postamble is the same
// shorter than FSK_DEFAULT_NSYM
const char uw_str[] = "SA2TMS";
u8 uw[48];

#define MTU_BITS (sizeof(uw) + HEADER + MTU*8 + FOOTER + sizeof(uw))

typedef struct {
  char *filename;
  int fs, rs, f1, mtu, fd, p;
  int complex; //1 -> s16le, 2 -> 2float
  FILE *f;
  u8 out_bits[2*(MTU_BITS + FSK_DEFAULT_NSYM)]; // might want to use some other upper bound on Nsym
  int out_bits_left;
  struct FSK *fsk;
} channel_t;

// look for uw, return its offset if found else -1
int find_uw(u8 *bits, int nbits) {
  for (int x = 0; x <= nbits - sizeof(uw); x++) {
    int matches = 0;
    for (int y = 0; y < sizeof(uw); y++) {
      matches += uw[y] == bits[x+y];
    }
    // tolerate quite a few bit errors
    if (matches >= sizeof(uw) - 8) {
      return x;
    }
  }
  return -1;
}

// soft version of find_uw()
int find_uw_sd(float *sd_bits, int nbits) {
  int ret = -1;
  float best = 0;
  for (int x = 0; x <= nbits - sizeof(uw); x++) {
    float sum = 0;
    for (int y = 0; y < sizeof(uw); y++) {
      float sd = sd_bits[x+y];
      sum -= sd * (2.0f*uw[y] - 1.0f);
    }
    if (sum >= best || x == 0) {
      ret = x;
      best = sum;
    }
  }
  return ret;
}

// maximum-likelyhood decoding of packet length
int sd_g23_length_decode(float *sd_bits, int flip, int stride) {
  float best = -1;
  int best_x = -1;

  // possible values are 0..MTU
  for (int x = 0; x <= MTU; x++) {
    int y = golay23_encode(x ^ (flip ? 0xFFF : 0));
    float sum = 0;

    for (int i = 0; i < 23; i++) {
      int b = (y >> i) & 1;
      sum -= sd_bits[i*stride] * (2*b - 1);
    }

    if (x == 0 || sum > best) {
      best = sum;
      best_x = x;
    }
  }

  return best_x;
}

// this function is run for all RX threads
void* in_thread(void *arg) {
  const channel_t *ch = arg;
  FILE *f = fopen(ch->filename, "rb");
  if (!f) {
    exit(1);
  }
  
  //struct FSK *fsk = fsk_create(ch->fs, ch->rs, 2, ch->f1, ch->rs);
  struct FSK *fsk = fsk_create_hbr(ch->fs, ch->rs, 2, ch->p, FSK_DEFAULT_NSYM, ch->f1, ch->rs);
  
  int complex = 2;
  int mask = 1;
  int nin_max = fsk->N + fsk->Ts/2;
  // VLAs don't work on all compilers, but libcodec2 already use them so..
  COMP buf[nin_max];
  u8 bits[ch->mtu + fsk->Nbits];
  float sd_bits[ch->mtu + fsk->Nbits];

  memset(bits, 0, sizeof(bits));
  memset(sd_bits, 0, sizeof(sd_bits));

  //fsk_set_freq_est_limits(fsk, -ch->fs/2, ch->fs/2);
  fsk_set_freq_est_limits(fsk, 0, ch->fs/2);
  fsk_set_freq_est_alg(fsk, mask);

  //keep demodulating input, pass any detected packets to socket
  for (;;) {
    int nin = fsk_nin(fsk);
    if (nin > nin_max || fread(buf, complex * sizeof(float), nin, f) != nin) {
      break;
    }

    // this gets trickier when we were to try to demod multiple channels in the same sample stream
    //fsk_demod(fsk, &bits[ch->mtu], buf);
    fsk_demod_sd(fsk, &sd_bits[ch->mtu], buf);
    for (int x = 0; x < fsk->Nbits; x++) {
      float sd = sd_bits[ch->mtu+x];
      bits[ch->mtu+x] = sd < 1;
      sd_bits[ch->mtu+x] = logf(sd);
    }

    //  print demodulated bits, unless they're all zero
    /*u8 zeroes[fsk->Nbits];
    memset(zeroes, 0, fsk->Nbits);
    if (memcmp(&bits[ch->mtu], zeroes, fsk->Nbits)) {
      for (int x = 0; x < fsk->Nbits; x++) {
        fprintf(stderr, "%i", (int)bits[ch->mtu+x]);
      }
      fprintf(stderr, "\n");
    }*/

    // find valid packet
    //int ofs = find_uw(&bits[ch->mtu - fsk->Nbits], 2*fsk->Nbits);
    int ofs = find_uw_sd(&sd_bits[ch->mtu - fsk->Nbits], 2*fsk->Nbits);
    if (ofs >= 0) {
      // possibly valid packet
      u8 *footer = &bits[ch->mtu - fsk->Nbits + ofs - FOOTER];
      float *sd_footer = &sd_bits[ch->mtu - fsk->Nbits + ofs - FOOTER];
      int length = 0, length2 = 0;

#if FOOTER == 66
#if FOOTER_SD
      for (int x = 0; x < 11; x++) {
        float sd1 = 0;
        float sd2 = 0;
        sd1 += *sd_footer++;
        sd2 += *sd_footer++;
        sd1 += *sd_footer++;
        sd2 += *sd_footer++;
        sd1 += *sd_footer++;
        sd2 += *sd_footer++;
        length += (sd1 < 0) << x;
        length2+= (sd2 < 0) << x;
      }
#else
      int lengths[6] = {0};
      for (int x = 0; x < FOOTER/6; x++) {
        for (int y = 0; y < 6; y++) {
          lengths[y] += (*footer++) << x;
        }
      }
      // majority voting
      length = (lengths[0]&lengths[2]) | (lengths[2]&lengths[4]) | (lengths[0]&lengths[4]);
      length2= (lengths[1]&lengths[3]) | (lengths[1]&lengths[5]) | (lengths[1]&lengths[5]);*/
#endif
#elif FOOTER == 46
#if FOOTER_SD
      length = sd_g23_length_decode(sd_footer,   0, 2);
      length2= sd_g23_length_decode(sd_footer+1, 1, 2);
#else
#error G23 footer requires FOOTER_SD at the moment
#endif
#else
#error bad FOOTER
#endif


      if (length <= MTU && length == length2) {
        // print footer and uw bits, put a space between the two
        for (int x = 0; x < FOOTER+sizeof(uw); x++) {
          if (x == FOOTER) {
            fprintf(stderr, " ");
          }
          fprintf(stderr, "%i", footer[x]);
        }
        fprintf(stderr, " t=%li, length=%4i, EbN0=%.2f\n", time(NULL), length, fsk->EbNodB);

        // grab the data bits, descramble
        u8 *packet_bits = &bits[ch->mtu + ofs - FOOTER - length*8];
        u8 packet[MTU] = {0};

        for (int x = 0; x < length*8; x++) {
          packet[x/8] += packet_bits[x] << (x % 8);
        }
        for (int x = 0; x < length; x++) {
          packet[x] ^= 0x55;
        }

        // hand the packet data over to the TUN device
        write(tunfd, buf, length);

        /* // zero out bits in buffer up to the uw
        memset(bits, 0, ch->mtu - fsk->Nbits + ofs);
        memset(sd_bits, 0, (ch->mtu - fsk->Nbits + ofs)*sizeof(float)); */
      } else {
        // length == 0 is common at the start of a row of packets
        if (length != 0) {
          //fprintf(stderr, "false alarm, %i vs %i, EbN0=%.2f\n", length, length2, fsk->EbNodB);
        }
      }
    }

    // shift bits left
    // using a circular buffer would be faster. maybe later
    memmove(bits, &bits[fsk->Nbits], ch->mtu);
    memmove(sd_bits, &sd_bits[fsk->Nbits], ch->mtu*sizeof(float));
  }

  fclose(f);
  return NULL;
}

// finds an available TX
int find_tx(channel_t *tx, int num_tx, u8 *buf, int buf_sz) {
  int nbits = sizeof(uw) + 8*buf_sz + FOOTER + sizeof(uw);

  for (int x = 0; x < num_tx; x++) {
    //if (sizeof(tx[x].out_bits) - tx[x].out_bits_left >= nbits) {
    if (tx[x].out_bits_left <= sizeof(uw)) {
      //fprintf(stderr, "TX %i is free\n", x);
      u8 *bits = &tx[x].out_bits[tx[x].out_bits_left];

      int have_uw_already = tx[x].out_bits_left > 0;
      if (have_uw_already) {
        nbits -= sizeof(uw);
      }

      // encode packet
      if (!have_uw_already) {
        memcpy(bits, uw, sizeof(uw));
        bits += sizeof(uw);
      }
      for (int x = 0; x < buf_sz*8; x++) {
        *bits++ = ((buf[x/8]^0x55) >> (x%8)) & 1;
      }

#if FOOTER == 66
      int buf_sz2 = buf_sz ^ 0x7FF;
      for (int x = 0; x < 11; x++) {
        *bits++ = (buf_sz  >> x) & 1;
        *bits++ = (buf_sz2 >> x) & 1;
        *bits++ = (buf_sz  >> x) & 1;
        *bits++ = (buf_sz2 >> x) & 1;
        *bits++ = (buf_sz  >> x) & 1;
        *bits++ = (buf_sz2 >> x) & 1;
      }
#elif FOOTER == 46
      int buf_g23a = golay23_encode(buf_sz);
      int buf_g23b = golay23_encode(buf_sz^0xFFF);
      for (int x = 0; x < 23; x++) {
        *bits++ = (buf_g23a >> x) & 1;
        *bits++ = (buf_g23b >> x) & 1;
      }
#else
#error bad FOOTER
#endif
      memcpy(bits, uw, sizeof(uw));
      tx[x].out_bits_left += nbits;

      // signal that the packet was successfully queued
      return -1;
    }
  }
  return buf_sz;
}

void tx_loop(channel_t *tx, int num_tx) {
  // 01010101...
  for (int x = 0; x < num_tx; x++) {
    for (int i = 0; i < sizeof(tx[x].out_bits); i++) {
      tx[x].out_bits[i] = i % 2;
    }
  }

  // TODO: rearrange the loop body so that we don't have to keep buf and buf_sz around.
  //       poll the TX fd's before the polling tunfd.
  u8 buf[MTU];
  int buf_sz = -1; // >=0 if we have a packet to send but all TX are busy

  for (;;) {
    fd_set rfds, wfds;
    int nfds = tunfd + 1;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    // poll tunfd, but only if we don't have an unsent packet
    if (buf_sz < 0) {
      FD_SET(tunfd, &rfds);
    }

    for (int x = 0; x < num_tx; x++) {
      FD_SET(tx[x].fd, &wfds);
      if (tx[x].fd >= nfds) {
        nfds = tx[x].fd + 1;
      }
    }
    
    int ret = select(nfds, &rfds, &wfds, NULL, NULL);
    if (ret < 0) {
      fprintf(stderr, "select returned %i\n", ret);
      break;
    }
    
    if (FD_ISSET(tunfd, &rfds)) {
      if (buf_sz >= 0) {
        fprintf(stderr, "logic error, buf_sz = %i\n", buf_sz);
        exit(1);
      }
      // have packet
      buf_sz = read(tunfd, buf, sizeof(buf));
      if (buf_sz < 0) {
        fprintf(stderr, "read() = %i\n", buf_sz);
        exit(1);
      }
      /*if ((buf[0] & 0xF0) != 0x40) {
        // only IPv4
        buf_sz = -1;
        continue;
      }*/

      // print first 20 bytes (IPv4 header, or partial IPv6 header)
      fprintf(stderr, "%4i B packet: ", buf_sz);
      for (int x = 0; x < 5*4; x++) {
        fprintf(stderr, (x % 4 == 3 ? "%02X  " : "%02X "), buf[x]);
      }
      fprintf(stderr, "\n");

      // attempt to encode and queue it to an available TX
      buf_sz = find_tx(tx, num_tx, buf, buf_sz);
      continue;
    }

    // try to squeeze in any packet-to-be-sent
    if (buf_sz >= 0) {
      buf_sz = find_tx(tx, num_tx, buf, buf_sz);
    }

    // modulate bits for each TX that needs samples
    for (int x = 0; x < num_tx; x++) {
      if (FD_ISSET(tx[x].fd, &wfds)) {
        struct FSK *fsk = tx[x].fsk;
        int N = fsk->N;

        if (tx[x].complex == 1) {
          float fsk_out[N];
          u8 bytes_out[N*2];

          fsk_mod(fsk, fsk_out, tx[x].out_bits);

          for (int x = 0; x < N; x++) {
            // peak amplitude of fsk_out is 2
            int sample = fsk_out[x] * 16383;
            bytes_out[2*x+0] = sample & 0xFF;
            bytes_out[2*x+1] = sample / 256;
          }

          // TODO: unbuffered output. I only use one TX for now, so this works
          if (fwrite(bytes_out, 2*N, 1, tx[x].f) != 1) {
            fprintf(stderr, "fwrite() failed\n");
            exit(1);
          }
        } else {
          COMP fsk_out[N];
          fsk_mod_c(fsk, fsk_out, tx[x].out_bits);
          
          for (int x = 0; x < N; x++) {
            // peak amplitude of fsk_out is 2..
            fsk_out[x].real /= 2;
            fsk_out[x].imag /= 2;
          }

          if (fwrite(fsk_out, sizeof(COMP)*N, 1, tx[x].f) != 1) {
            fprintf(stderr, "fwrite() failed\n");
            exit(1);
          }
        }

        memmove(tx[x].out_bits, &tx[x].out_bits[fsk->Nbits], sizeof(tx[x].out_bits) - fsk->Nbits);
        // 10101010...
        for (int i = 0; i < fsk->Nbits; i++) {
          tx[x].out_bits[sizeof(tx[x].out_bits) - fsk->Nbits + i] = i % 2;
        }
       
        if (tx[x].out_bits_left >= fsk->Nbits) {
          tx[x].out_bits_left -= fsk->Nbits;
        } else {
          tx[x].out_bits_left = 0;
        }
      }
    }
  }
}

int tun_alloc(char *dev, int flags);
int main(int argc, char *argv[])
{
  golay23_init();

  if (argc < 2) {
    printf("Usage: bridge tap|tun\n");
    exit(1);
  }

  for (int x = 0; x < sizeof(uw); x++) {
    uw[x] = (uw_str[x/8] >> (x % 8)) & 1;
  }

  char dev[256] = "tun77";

  tunfd = tun_alloc(dev, strcmp(argv[1], "tun") == 0 ? IFF_TUN : IFF_TAP);
  fprintf(stderr, "tunfd = %i\n", tunfd);
  if (tunfd < 0) {
    perror(NULL);
    return 1;
  }
  ioctl(tunfd, TUNSETNOCSUM, 1); 

#define MAX_RX 16 //one thread per RX
#define MAX_TX 16
  pthread_t threads[MAX_RX];
  channel_t rx[MAX_RX];
  channel_t tx[MAX_TX];
  int num_rx = 0;
  int num_tx = 0;

  for (int x = 2; x < argc; x++) {
    if (!strcmp(argv[x], "rx")) {
      if (num_rx >= MAX_RX) {
        fprintf(stderr, "too many rx\n");
        return 1;
      }
      rx[num_rx].filename = argv[x+1];
      rx[num_rx].fs = atoi(argv[x+2]);
      rx[num_rx].rs = atoi(argv[x+3]);
      rx[num_rx].p = atoi(argv[x+4]);
      rx[num_rx].f1 = 1;
      rx[num_rx].mtu = MTU_BITS;
      if (pthread_create(&threads[num_rx], NULL, in_thread, &rx[num_rx]) < 0) {
        fprintf(stderr, "pthread_create\n");
        return 1;
      }
      num_rx++;
      x += 4;
    } else if (!strcmp(argv[x], "tx")) {
      if (num_tx >= MAX_TX) {
        fprintf(stderr, "too many tx\n");
        return 1;
      }
      tx[num_tx].filename = argv[x+1];
      tx[num_tx].fs = atoi(argv[x+2]);
      tx[num_tx].rs = atoi(argv[x+3]);
      tx[num_tx].f1 = atoi(argv[x+4]);
      tx[num_tx].p = atoi(argv[x+5]);
      tx[num_tx].complex = atoi(argv[x+6]);
      if (tx[num_tx].complex != 1 && tx[num_tx].complex != 2) {
        fprintf(stderr, "complex must be 1 or 2, not %i\n", tx[num_tx].complex);
        return 1;
      }
      tx[num_tx].mtu = MTU_BITS;
      //tx[num_tx].fd = open(tx[num_tx].filename, O_WRONLY | O_APPEND);
      tx[num_tx].f = fopen(tx[num_tx].filename, "wa");
      if (tx[num_tx].f == NULL) {
        fprintf(stderr, "failed to open tx file\n");
        return 1;
      }
      tx[num_tx].fd = fileno(tx[num_tx].f);
      tx[num_tx].out_bits_left = 0;
      //tx[num_tx].fsk = fsk_create(tx[num_tx].fs, tx[num_tx].rs, 2, tx[num_tx].f1, tx[num_tx].rs);
      tx[num_tx].fsk = fsk_create_hbr(tx[num_tx].fs, tx[num_tx].rs, 2, tx[num_tx].p, FSK_DEFAULT_NSYM, tx[num_tx].f1, tx[num_tx].rs);
      num_tx++;
      x += 6;
    } else {
      fprintf(stderr, "bad arg %s\n", argv[x]);
      return 1;
    }
  }

  if (num_tx > 0) {
    tx_loop(tx, num_tx);
  }

  for (int x = 0; x < num_rx; x++) {
    void *ret;
    pthread_join(threads[x], &ret);
  }

  for (int x = 0; x < num_tx; x++) {
    fclose(tx[x].f);
    fsk_destroy(tx[x].fsk);
  }
}

// https://piratelearner.com/en/bookmarks/tuntap-interface-tutorial/14/
// this needs to be rewritten
int tun_alloc(char *dev, int flags) {

  struct ifreq ifr;
  int fd, err;
  char *clonedev = "/dev/net/tun";

  /* Arguments taken by the function:
   *
   * char *dev: the name of an interface (or '\0'). MUST have enough
   *   space to hold the interface name if '\0' is passed
   * int flags: interface flags (eg, IFF_TUN etc.)
   */

   /* open the clone device */
   if( (fd = open(clonedev, O_RDWR)) < 0 ) {
     return fd;
   }

   /* preparation of the struct ifr, of type "struct ifreq" */
   memset(&ifr, 0, sizeof(ifr));

   ifr.ifr_flags = flags | IFF_NO_PI;   /* IFF_TUN or IFF_TAP, plus maybe IFF_NO_PI */

   if (*dev) {
     /* if a device name was specified, put it in the structure; otherwise,
      * the kernel will try to allocate the "next" device of the
      * specified type */
     strncpy(ifr.ifr_name, dev, IFNAMSIZ-1);
     ifr.ifr_name[IFNAMSIZ-1] = 0;
   }

   /* try to create the device */
   if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ) {
     close(fd);
     return err;
   }

  /* if the operation was successful, write back the name of the
   * interface to the variable "dev", so the caller can know
   * it. Note that the caller MUST reserve space in *dev (see calling
   * code below) */
  strcpy(dev, ifr.ifr_name);

  /* this is the special file descriptor that the caller will use to talk
   * with the virtual interface */
  return fd;
}