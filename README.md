# 🧥 Smart Jacket — IoT Worker Safety Monitoring System

An IoT-enabled wearable device designed to monitor worker safety in hazardous environments. The jacket uses integrated sensors to track physiological and environmental data in real-time, transmitting it to an edge server with built-in cybersecurity defences.

---

## 📐 System Architecture

![System Architecture](architecture.jpeg)

The system consists of three main nodes:
- **Raspberry Pi** — acts as the local server and central processing hub
- **ESP32-1 & ESP32-2** — microcontrollers embedded in two jackets, communicating with the RPi via HTTP GET/RESPONSE and with each other via **ESP-NOW protocol**
- **RFID Scanner** — connected to the RPi for identity verification

---

## 🔌 Circuit Diagrams

### Server (Raspberry Pi + RFID-RC522)
![RPI Server Circuit](RPI-server-circuit.png)

The RFID-RC522 module connects to the Raspberry Pi 5 via SPI pins (SDA, SCK, MOSI, MISO, RST) along with 3.3V power and GND.

### Jacket (ESP32 + Sensors)
![Jacket Circuit](jacket-circuit.png)

Each jacket's ESP32 is wired to:
- **DHT11** — temperature & humidity sensor
- **MQ-2** — gas/smoke sensor
- **Pulse sensor** — heart rate monitoring
- **Ultrasonic sensor (US-015)** — proximity detection
- **LCD display (I2C)** — local data display
- **Push button** — manual trigger
- **Buzzer + LED** — alerts and notifications

---

## 🧰 Hardware Requirements

| Component | Purpose |
|---|---|
| ESP32 (×2) | Microcontroller with Wi-Fi/Bluetooth for IoT communication |
| Raspberry Pi | Local edge server and processing hub |
| RFID-RC522 + Tags (×2) | Worker identity verification |
| DHT11 | Body temperature and humidity monitoring |
| MQ-2 Gas Sensor | Detection of hazardous gases |
| Pulse Sensor | Heart rate monitoring |
| Ultrasonic Sensor (US-015) | Proximity/distance detection |
| 16×2 I2C LCD | Local data display on jacket |
| Buzzer + LED | Alert indicators |
| Push Button | Manual input trigger |

---

## 💻 Software Requirements

| Software | Purpose |
|---|---|
| Arduino IDE | Programming the ESP32 microcontrollers |
| Python 3 | Backend scripting and data handling |
| Raspberry Pi OS | Server operating system |
| Flask | Python web framework for the local API server |
| `pycryptodome` | AES-128 encryption library |
| Metasploit (MSF) | DoS attack simulation (testing only) |
| Medusa | Brute-force SSH testing (testing only) |
| Fail2Ban | Intrusion prevention on the RPi |

---

## ⚙️ Setup Instructions

### 1. Flash the ESP32s

1. Open **Arduino IDE** and install the ESP32 board package.
2. Install required libraries: `DHT`, `MFRC522`, `LiquidCrystal_I2C`, `esp_now`, `WiFi`.
3. Flash **Jacket 1** code onto ESP32-1.
4. For **Jacket 2**, update only the **MAC address** in the code, then flash onto ESP32-2.

> ⚠️ Note the MAC address of each ESP32 before flashing — it's needed for ESP-NOW pairing.

### 2. Wire Up the Circuits

Follow the circuit diagrams above precisely.

- RFID-RC522 → Raspberry Pi via SPI (refer to RPI circuit diagram)
- All sensors → respective ESP32 GPIO pins (refer to jacket circuit diagram)

### 3. Set Up the Raspberry Pi Server

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install Python dependencies
pip install flask pycryptodome

# Clone this repository
git clone <your-repo-url>
cd <repo-folder>

# Run the Flask server
python server.py
```

The server will start listening for GET requests from both ESP32 jackets.

### 4. Connect ESP32s to Wi-Fi

In the ESP32 Arduino code, update the following with your local network credentials:

```cpp
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* serverURL = "http://<RPI_IP_ADDRESS>/data";
```

### 5. Test the System

Power both jackets. They should:
1. Connect to Wi-Fi
2. Read sensor values
3. Send data to the RPi server via HTTP GET
4. Communicate with each other via ESP-NOW

---

## 🔒 Security Implementation

### DoS Attack Defence — IP Tables (Firewall)

```bash
# Limit incoming TCP SYN packets to 1/sec with burst of 3
sudo iptables -A INPUT -p tcp --syn -m limit --limit 1/s --limit-burst 3 -j ACCEPT

# Drop all excess SYN packets
sudo iptables -A INPUT -p tcp --syn -j DROP
```

### R2L Attack Defence — Fail2Ban

```bash
# Install Fail2Ban
sudo apt-get install fail2ban

# Start the service
sudo systemctl start fail2ban

# Check SSH jail status
sudo fail2ban-client status sshd
```

### IP Theft Defence — AES-128 Encryption

```python
from Crypto.Cipher import AES
import base64

def encrypt_data(data, key):
    cipher = AES.new(key, AES.MODE_ECB)
    padded_data = pad(data)
    encoded = base64.b64encode(cipher.encrypt(padded_data))
    return encoded
```

All data packets are AES-128 encrypted before transmission between the ESP32 and Raspberry Pi.

---

## ⚠️ Important Notes

- **Version mismatch**: Always verify library and board package versions — ESP32 is sensitive to compatibility issues.
- **HTTP stability**: The ESP32 can occasionally bug out on HTTP transactions with Flask. Add retry logic or error handling in the code.
- **ESP-NOW vs RTOS**: The system uses the standard `setup()`/`loop()` method. FreeRTOS is an option but is memory-heavy and requires careful heap allocation.
- **Component quality**: Higher-quality sensors and components significantly reduce read errors and system instability.
- **Second jacket**: Jacket 2 is identical to Jacket 1 — only the MAC address constant needs to change.

---

## 📁 Project Structure

```
├── jacket_esp32/
│   ├── jacket1.ino       # Code for ESP32-1
│   └── jacket2.ino       # Code for ESP32-2 (same, different MAC)
├── server/
│   └── server.py         # Flask API running on Raspberry Pi
├── architecture.jpeg     # System architecture diagram
├── RPI-server-circuit.png
├── jacket-circuit.png
└── README.md
```

---

## 📚 References

1. M. Usama et al., *Chaos-based secure satellite imagery cryptosystem*, Computers and Mathematics with Applications, 2010.
2. Q. Zhang, *Study on Image Encryption Algorithm Based on Chaotic Theory*, International Conference on Information Science, 2013.
3. S. Tanwar et al., *Tactile Internet for Industrial IoT: A Survey on Security and Privacy*, IEEE IoT Journal, 2022.
4. Espressif, *ESP32 Technical Reference Manual*, 2024.
5. [Flask-Limiter Documentation](https://flask-limiter.readthedocs.io/)
