# ⚡ IoT Smart Energy Meter with MQTT & Alerts

An IoT-based Smart Energy Meter built using **ESP32** and **PZEM-004T** to monitor real-time electrical parameters such as voltage, current, power, energy, frequency, and power factor.  
The system supports **MQTT-based dashboards** and **real-time alerts via SMS and Telegram**.

---

## 🚀 Features

- 📡 Real-time energy monitoring
- 📊 Web-based MQTT dashboard
- 🔔 Alerts for:
  - Power cut & restoration
  - Over-voltage / under-voltage
  - Over-current & overload
  - Frequency abnormalities
  - Sensor communication errors
- 📱 Telegram & SMS notifications
- 💡 LCD display for local monitoring

---

## 🛠️ Hardware Components

| Component | Description |
|--------|-------------|
| ESP32 | DOIT ESP32 DEVKIT V1 |
| PZEM-004T v4.0 | Energy measurement module |
| CT Clamp | Current measurement |
| LCD | 16x2 I2C LCD |
| Breadboard & Jumpers | Prototyping |
| Power Supply | USB |

---

## 🧠 System Architecture

![Block Diagram](hardware/block_diagram.png)
![Circuit Diagram](hardware/circuit_diagram.png)

---

## 💻 Firmware Variants

| Version | Description |
|------|-------------|
| MQTT Basic | Real-time MQTT publishing |
| SMS Alert | High-voltage SMS alerts |
| Telegram Alert | Full alert system with Telegram |

Source codes are available in the `firmware/` directory.

---

## 🌐 Web Dashboard

- Built using **HTML, CSS, JavaScript**
- Uses **MQTT over WebSockets**
- Real-time visualisation of electrical parameters

![Dashboard Preview](dashboard/preview.png)

---

## 📄 Documentation

Full project documentation including theory, circuit design, and code explanation:

📘 [View Documentation](docs/Smart_Energy_Meter_Documentation.pdf)

---

## 🎥 Demo

📹 Project demo video available in the `media/` folder.

---

## 📌 Future Improvements

- Cloud data logging
- Mobile app integration
- Energy consumption analytics
- Smart billing integration

---

## 👤 Author

**Abdul Rahman (Abu)**  
Biomedical Engineering Undergraduate  
ESP32 | IoT | Embedded Systems

---

## 📜 License

This project is licensed under the MIT License.
