# README_tun

A way to get Internet via the air using 2FSK and cheap hardware.
The main focus is low price, low power, low to moderate speed.
Think dialup speed over several 10s of kilometers using directional antennas and no more than 10-30 dBm.

tms_tun creates a TUN device and demodulates samples on a set of FIFOs, producing packets.
Outgoing packets are modulated onto another set of FIFOs.
Keep as much business logic out of the C code as possible, have it be hardware agnostic.
Do the rest in shell scripts or Python.

## Baudrates and power

Have the ability to specify a wide range of baudrates, from say 1 baud up to 1 Mbaud.
Rates may need to be non-integral, compare PSK31 which runs at 31.25 baud.
A few common rates should be enough for now, the main idea is that stations
can listen to way more channels than they can transmit on.
We want to support a heterogenous mix of speeds and TX power/complexity.

## Bands

All bands are of interest, but let's start with 2 meters.
I propose 144.700 to 144.775 MHz, assigning each baudrate a subset of it.
Can't go much higher than 775 without running into APRS.
Being close allows receiving APRS with the same RTL-SDR, which is useful.
RX bandwidth is cheap, sites are not.

## Possible hardware

### RTL-SDR

Ubiquitous, cheap, RX only. Covers 0 - 14.4 MHz in direct sampling mode. Osmocom and rtl_sdr.
Has some kind of AGC, which complicates things.
For now I deal with the AGC by transmitting 01010101 when idle.

### HackRF

Simplex TRX. About 10 dBm output power. Osmocom and hackrf_transfer.

### LimeSDR

Duplex TRX, 2x2 MIMO. Don't have one myself, might get one in the future.

### SSB radios

Rigs like my FT-817. Typically simplex, 2.5 kHz bandwidth. 20-60 dBm, depending on PA.
Having to deal with AGC and ALC stuff is annoying.

### NFM handhelds

Baofeng and gang. Simplex, 2.5 kHz baseband over NFM. 37 dBm at most.
I'd rather stay away from them, but it can't be denied that they are very common.

### Raspberry Pi

Ubiquitous. rpitx. Needs LPF on output, maybe a bit of amplification, 50 Ohm matching, CM choke.
Can also RX using an RTL-SDR or HackRF.

## Channelization

Can be done with Gnuradio.
Essential when combining say 1 baud with 1 Mbaud to keep memory footprint and CPU usage reasonable.

## Complications

### Simplex

Many rigs are simplex, which must be dealt with in some way.
HackRF's tools typically need to be terminated and relaunched whenever the hardware switches between RX and TX.

### Where to TX

How do we select which band(s) to TX on? Same for baudrate. Maybe configure it per port.
For example ssh and mosh might want to run at slower baudrates to guarantee that they work, albeit slowly.
Another way to choose is based on packet size. Smaller packets are probably fine to TX more slowly.

### Control channel

Having information about propagation is useful, compare ALE.
Send S-levels and positions periodically, using all available TX bands.
Stick to low baudrates, randomize channel.
Make sure there's enough bandwidth for many stations to TX at the same time.

## Packet format

Format is currently as follows:

  preamble | packet data | footer | preamble | [...]] | preamble | 0101010101[...]

Integers are sent in LSB order.
Preamble is "SA2TMS" in 8-bit ASCII.
Data is sent as-is, no FEC yet.
Footer is the length of the packet data , Golay-23 coded.
Valid values are 0 to 1600.
The length is sent twice, interleaved, first as-is and second as its 1's complement.
This effectively means it's sent Manchester coded.

## Packetizer and modulator

Packetization and modulation happens on the TX side, in the main thread.
select() is used to poll for new packets arriving at the TUN device's "outbox".
Packets are read() and then formatted according to the packet format described earlier.
They are then modulated for and sent to whatever TX is available.

## Demodulator and depacketizer

Input samples are demodulated and kept in a buffer large enough to hold two MTUs worth of bits.
The depacketizer looks for the preamble, using soft bits.
Whichever positions is most probable is further decoded.
The size of the packet is decoded after locating its position.
Size decoding is done in a maximum-likelyhood fashion.
The size and its 1's complement are compared, and only if they match is the packet considered valid.
Valid packets are turned into byte arrays and passed to the TUN device's "inbox" using write().

## TODO

* FEC, checksums
* Deal with missing bits. Look into burst mode, maybe add alignment words
* Think about regulatory compliance, especially station ID as per ITU regulations
* IPv4 vs IPv6. AMPRnet has no global allocation in IPv6 yet
* Give encryption some thought, don't worry too much about yankeeisms
* Signing things is useful and is not encryption. Consider it for the control channel, maybe Curve25519
* IP header compression, remove as many redundant bits as reasonable
* Testing, get decent performance measurements
* Maybe some formal verification (Frama-C), we are dealing with buffers and random data from the air
