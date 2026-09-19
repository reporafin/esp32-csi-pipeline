# Dataset — Raw CSI Recordings
 
License: **CC BY 4.0** — free to use for anything, including commercially,
as long as you credit the source (see citation at the bottom) :).
 
## Files information
 
Wi-Fi splits its signal across 64 slightly different frequencies
("sub-carriers"). Each row in these CSVs records how much each of those 64
sub-carriers was disturbed by whatever was happening in the room at that
moment, a person breathing causes a small slow disturbance, walking a
bigger faster one, an empty room almost none.
 
| Column | Meaning |
|---|---|
| `Packet_Count` | Running packet counter (not a real timestamp — see limitations) |
| `Sub_0` … `Sub_63` | CSI amplitude on each Wi-Fi sub-carrier for that packet |
 
Packets arrive ~100/second, so 100 rows ≈ 1 second of real time.
 
## Folder structure
 
```
room1_quiet/   Room 1, receiver on its auto-selected quiet channel
room1_noisy/   Room 1, receiver forced onto the most congested channel
room2/         A different, unseen room (generalisation test)
```
 
## File naming
 
```
<state>_trial<n>.csv
```
- **empty** — room unoccupied
- **stationary** — person seated, breathing normally, minimal fidgeting
- **moving** — person walking continuously, ~1 m/s, varied paths
## Collection summary
 
Two ESP32 boards ~10 ft apart in Room 1 (26 ft in Room 2), receiver wired
to a laptop running the capture script. Each state was recorded across
several trials; the first/last few seconds of each raw recording were
trimmed for hardware settling. Full protocol: thesis Chapter 5.
 
## Loading the dataset
 
```python
import pandas as pd
df = pd.read_csv("room1_quiet/stationary_trial1.csv")
df["Sub_10"].plot()
```
 
## Limitations known
 
- No real timestamps, uniform 100 Hz sampling is assumed, not measured.
- Single test subject, results may not generalise to other people.
- Only two rooms used for data collection.
- A small fraction of rows may be missing/corrupted from ordinary packet
  loss which is filter non-numeric rows before analysis.
## Citation
 
```
Md. Rafiul Reza. (2026). ESP32 CSI Pipeline Dataset — Cognitive Radio
Presence Detection [Data set]. GitHub.
https://github.com/reporafin/esp32-csi-pipeline
