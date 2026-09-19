# Transmitter Firmware (`tx_main.c`)
 
The "sending" board. Connects to the receiver's Wi-Fi network and sends it
a tiny packet 100 times/second, forever — this is what gives the receiver
something to measure. The packet content itself (`CSI_PING`) is irrelevant,
only its regular arrival matters.
 
## Sequence
1. Waits 2s at startup, then tries to connect to the receiver's network.
2. If not found yet, retries every 3s indefinitely where is no reboot, no error.
   (The receiver can take up to ~15s to finish scanning and start its
   network, so this is expected, not a fault.)
3. Once connected, sends one packet every 10ms (100 Hz) forever.
## Key settings (top of `tx_main.cpp`)
 
```cpp
#define AP_SSID  "CSI_SENSOR_AP"   // must match rx_main.cpp exactly
#define AP_PASSWORD "csi12345"     // must match rx_main.cpp exactly
#define RX_IP_ADDRESS "192.168.4.1"  // fixed address the receiver always uses
#define BEACON_RATE_HZ 100
```
 
## Build & flash
 
```bash
cd firmware/transmitter
idf.py set-target esp32 && idf.py build
idf.py -p PORT flash monitor    # different PORT than the receiver
```
Wait for `TX READY`. Once connected, it can be unplugged from the PC and
run on a power bank, it only needs power from here, no data connection.
 
## Why it never reboots on failure
 
The receiver's scan can take up to 15s, so an "reboot and retry"
strategy would prevent either board from ever getting a stable connection.
Waiting patiently is deliberate, not a missing feature.
