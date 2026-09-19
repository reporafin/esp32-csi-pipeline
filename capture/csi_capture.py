import serial
import csv
import sys

# CONFIGURATION
SERIAL_PORT = 'COM3'  # Check with caution
BAUD_RATE = 460800
# Must match the ESP-IDF default console speed
OUTPUT_FILE = 'csi_raw_data.csv'
MAX_SUBCARRIERS = 64

def main():
    print(f"Attempting to connect to ESP32 on {SERIAL_PORT} at {BAUD_RATE} baud...")
    
    try:
        ser = serial.Serial()
        ser.port = SERIAL_PORT
        ser.baudrate = BAUD_RATE
        ser.timeout = 1
        
        ser.setDTR(False)
        ser.setRTS(False)
        ser.open()
        
    except serial.SerialException as e:
        print(f"\nError: {e}")
        print("CRITICAL: Make sure the ESP-IDF monitor is CLOSED! (Press Ctrl + ])")
        sys.exit(1)

    print(f"Connected! Saving data to '{OUTPUT_FILE}'")
    print("Press Ctrl+C to stop recording.\n")
    
    with open(OUTPUT_FILE, mode='w', newline='') as file:
        writer = csv.writer(file)
        headers = ['Packet_Count'] + [f'Sub_{i}' for i in range(MAX_SUBCARRIERS)]
        writer.writerow(headers)

        try:
            while True:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                
                if len(line) > 0 and not line.startswith("CSI_DATA"):
                    print(f"ESP32 Log: {line}")
                
                if line.startswith("CSI_DATA"):
                    data_parts = line.split(',')
                    
                    try:
                        if len(data_parts) > 1:
                            count = int(data_parts[1])
                            
                            writer.writerow(data_parts[1:])
                            
                            if count % 100 == 0:
                                print(f"Captured {count} packets...")
                                
                    except ValueError:
                        pass
            print("\nRecording stopped by user. File saved!")
        finally:
            if 'ser' in locals() and ser.is_open:
                ser.close()

if __name__ == '__main__':
    main()