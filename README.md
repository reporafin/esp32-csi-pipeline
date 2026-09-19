# esp32-csi-pipeline
Firmware, Capture Script and dataset to figure out if a room is empty, if someone is sitting still, or if they are walking around. It only uses Wi-Fi signals— no cameras or wearables needed.
> This is part of my undergraduate research project titled *"Cognitive Radio CSI Signal Based Human Presence Detection Framework Using Edge Computing Devices."*

## How it functions

When a person is near a Wi-Fi signal, their body actually disturbs it a little bit. We can measure these changes using something called *Channel State Information* (CSI).

The setup is pretty straightforward: two ESP32 boards constantly send packets back and forth. The receiving board looks at how much the signal got messed up along the way and streams that data to a PC as a CSV file.

You can clearly see what's going on just by looking at the data: an empty room shows almost zero change, someone just sitting and breathing makes small slow waves, and someone walking around creates big, fast spikes.

To keep the data clean, the receiver automatically scans all 13 Wi-Fi channels *(MAC address)* on startup and picks the quietest one so your normal home Wi-Fi doesn't interfere.

## Repository layout

```
firmware/receiver/      ESP32: channel scan, CSI capture, sends over USB
firmware/transmitter/   ESP32: connects to receiver, pings it 100×/sec
capture/                PC script that saves incoming CSI data to CSV
dataset/                Raw recordings — see dataset/README.md
```

## Hardware needed
2× ESP32 DevKit boards, 2× USB cables, a PC. (Optional: 3 LEDs + resistors
for status lights — see `firmware/receiver/README.md`.)

## ESP32 Setup

```bash
# 1. Flash the receiver
cd firmware/receiver
idf.py set-target esp32 && idf.py build
idf.py -p PORT flash monitor        # wait for "=== SYSTEM READY ==="
                                     # then Ctrl+] to exit
 
# 2. Flash the transmitter (different PORT)
cd firmware/transmitter
idf.py set-target esp32 && idf.py build
idf.py -p PORT flash monitor        # wait for "TX READY"
 
# 3. Capture data on your PC (close any open idf.py monitor first)
cd capture
pip install -r requirements.txt
python capture_csi.py               # Ctrl+C to stop
```

Edit `SERIAL_PORT` in `capture_csi.py` to match your receiver's port first.
Output is saved as `csi_raw_data.csv` — one row per packet, columns
`Packet_Count, Sub_0 ... Sub_63`.

## Settings to be noted

| Setting | File | Notes |
|---|---|---|
| `USE_QUIET_CHANNEL` | `rx_main.cpp` | `true`=quietest channel, `false`=most congested |
| `AP_SSID` / `AP_PASSWORD` | both firmware files | Must match exactly on both boards |
| `BAUD_RATE` | `capture_csi.py` | Must match `UART_BAUD` in `rx_main.cpp` (460800) |

## Key problems solutions

- **Port busy / permission denied** : close any open `idf.py monitor` first.
- **Board keeps resetting during capture** → already handled in the script
  via `setDTR(False)`/`setRTS(False)`; don't remove that if you edit it.
- **Transmitter retries forever** : normal while the receiver is still
  scanning (~15s); check `AP_SSID`/`AP_PASSWORD` match if it never connects.
- **Garbled or missing rows in the CSV** : occasional packet loss is normal;
  the script already filters out non-numeric lines.

  ## License
Code: MIT (`LICENSE`). Dataset: CC BY 4.0 (`dataset/README.md`).

## Citation
See `CITATION.cff`.

## Acknowledgements
CSI capture follows the pattern of Espressif's ESP-IDF Wi-Fi CSI examples.

## Note
`AP_PASSWORD` is a fixed test credential for linking the two boards to each
other — not meant as real security practice.

To figure out which channels are congested, the ESP32 does a quick scan of nearby MAC addresses. It basically counts how many devices are active on each channel and automatically picks the quietest one to use.
