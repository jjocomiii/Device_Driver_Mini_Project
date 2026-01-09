# 📟 RPi Embedded Linux Monitor System
> **Kernel-to-User Full Stack Implementation** on Raspberry Pi 4B

![Generic Badge](https://img.shields.io/badge/Platform-Raspberry_Pi_4B-C51A4A.svg) ![Generic Badge](https://img.shields.io/badge/Kernel-Linux_Device_Driver-F34B7D.svg) ![Generic Badge](https://img.shields.io/badge/Language-C-00599C.svg)

<p align="center">
  <img src="docs/videos/fulldemo.gif" width="85%" alt="Main Demonstration">
</p>

본 프로젝트는 라즈베리파이 환경에서 **리눅스 커널 디바이스 드라이버부터 유저 공간의 데몬, 그리고 Systemd 기반의 자동화**까지 임베디드 리눅스 시스템의 전 과정을 밑바닥부터 구축한 결과물입니다.

하드웨어 제어(OLED, RTC, Sensor, Actuator)를 위한 커널 모듈을 직접 작성하고, 이를 통합 관리하는 상태 머신(State Machine) 기반의 어플리케이션을 구현하여 **부팅 즉시 동작하는 완성형 임베디드 시스템**을 목표로 했습니다.

---

## 🛠️ System Architecture

![architecture](https://github.com/user-attachments/assets/b4ad44a8-a466-4034-b669-7dc62746757d)

단순한 라이브러리 활용이 아닌, **OS 커널 영역과 사용자 영역의 명확한 역할 분리**를 통해 시스템 안정성과 확장성을 확보했습니다.


### 1. State Machine Design (UI Logic)

Rotary Encoder 입력 인터럽트에 따라 화면 모드와 RTC 편집 모드를 유기적으로 전환하기 위해 **FSM(Finite State Machine)** 구조를 적용했습니다.
<img width="624" height="1068" alt="FSM" src="https://github.com/user-attachments/assets/c0c4944e-efd9-4f71-be93-fcfde97d3dbf" />



---

## 📸 Demonstration & Features

시스템의 주요 동작 시나리오입니다. FSM 기반의 모드 전환과 인터럽트 제어를 통한 즉각적인 반응성을 확인하실 수 있습니다.

| 1. Mode Switching (FSM) | 2. RTC Time Edit (Rotary) | 3. Humi-Gauge (Kernel) |
| --- | --- | --- |
| <img src="docs/videos/switching_mode.gif" width="100%"> | <img src="docs/videos/edit_time.gif" width="100%"> | <img src="docs/videos/humidity_change.gif" width="100%"> |
**Rotary 회전:**<br> | <br>Clock ↔ Sensor 페이지 전환 
**Button 클릭:**<br> | <br>필드 이동 및 RTC 값 수정
**Sensor 감지:**<br> | <br>습도값에 따른 LED 자동 제어

### Hardware Setup

<p align="center">
<img src="docs/hardware/setup.png" width="60%" alt="Hardware Setup">
</p>

---

## 🔩 Hardware Specifications

### Pinout Configuration (BCM 기준)

| Component | Interface | GPIO Pins | Description |
| --- | --- | --- | --- |
| **SSD1306 OLED** | I2C | `GPIO 2 (SDA)`, `GPIO 3 (SCL)` | 128x64 Display |
| **DHT11 Sensor** | 1-Wire | `GPIO 4` | Temp/Humi Sensing |
| **DS1302 RTC** | 3-Wire | `GPIO 5 (CLK)`, `GPIO 6 (DAT)`, `GPIO 13 (RST)` | Real-Time Clock |
| **Rotary Encoder** | GPIO | `GPIO 17 (A)`, `GPIO 27 (B)`, `GPIO 22 (SW)` | User Input |
| **LED Bar** | GPIO | `GPIO 12, 16, 20, 21, 23, 24, 25, 26` | Humidity Visualizer |

> **Note:** 모든 핀 번호는 물리적 핀 번호가 아닌 **BCM(Broadcom SOC Channel)** 번호를 기준으로 매핑되었습니다.

---

## 🚀 Key Implementation Details

### 1. 커널 레벨의 실시간 제어 (Kernel Modules)

* **Direct Hardware Access:** `/dev/ssd1306`, `/dev/rtc0` 등 리눅스 표준 인터페이스인 캐릭터 디바이스 노드를 생성하여 하드웨어를 추상화했습니다.
* **Hardware-driven Automation:** DHT11 센서값에 따라 LED Bar가 점등되는 로직을 유저 공간이 아닌 **커널 드라이버(`dht11_ledbar.c`) 내부에서 직접 처리**하여 반응 속도와 신뢰성을 높였습니다.

### 2. 유저 공간 데몬 (Main Application)

* **env-oled Daemon:** `poll()` 시스템 콜을 활용하여 Rotary Encoder의 인터럽트 이벤트를 비동기로 감지합니다.
* **Graphic Handling:** 128x64 픽셀 프레임버퍼를 직접 드로잉하여 RTC 시간을 표시하고, 편집 모드 진입 시 직관적인 필드 이동 UI를 제공합니다.

### 3. 부팅 자동화 (Systemd & Udev)

* **udev Rules:** 디바이스 노드 생성 시 권한 문제(Permission denied)를 방지하기 위해 `99-mini-dev.rules`를 작성, 자동으로 `mode=0666` 권한을 부여했습니다.
* **Systemd Service:** `mini-kmods.service`(모듈 로드)와 `env-oled.service`(앱 실행)를 등록하여 전원 인가 시 별도의 조작 없이 시스템이 구동됩니다.

---

## 🔧 Installation & Build

본 프로젝트는 `Raspberry Pi OS (32-bit)` 환경에서 테스트되었습니다.

### 1. Kernel Modules Build

```bash
# 리눅스 커널 헤더 경로 지정
export KDIR=/home/ubuntu/linux 
make

```

### 2. User Daemon Compilation

```bash
gcc -O2 -Wall -o env-oled env-oled.c
sudo install -m 0755 env-oled /usr/local/bin/env-oled

```

### 3. Deploy Automation Scripts

```bash
# udev 룰 적용 (디바이스 권한 설정)
sudo cp 99-mini-dev.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# systemd 서비스 등록
sudo cp *.service /etc/systemd/system/
sudo systemctl daemon-reload

# 서비스 시작 및 부팅 등록
sudo systemctl enable --now mini-kmods.service
sudo systemctl enable --now env-oled.service

```

---

## 💡 Troubleshooting & Analysis

프로젝트 진행 중 발생한 하드웨어 신호 무결성 문제와 해결 과정입니다.

### Case 1: 1-Wire 통신 불안정 (DHT11)

* **현상:** 드라이버 로드 후 `read()` 시 지속적인 Checksum Error 또는 Timeout 발생.
* **분석:** 소프트웨어 타이밍 문제인지 하드웨어 문제인지 판별하기 위해 **오실로스코프**로 DATA 핀 파형 측정.
* **해결:** High 신호가 충분한 전압 레벨에 도달하지 못하는 현상을 확인, **Pull-up 저항 배선 보강 및 접점 재연결**을 통해 깨끗한 펄스 파형을 확보함.

<p align="center">
<img src="docs/hardware/trouble_shooting.png" width="70%" alt="Oscilloscope Analysis">
</p>

### Case 2: Rotary Encoder 바운싱(Bouncing)

* **현상:** 한 번의 회전 동작에 다수의 이벤트가 트리거되는 현상.
* **해결:** 하드웨어 필터(Capacitor) 대신 커널 드라이버 내에서 **소프트웨어 디바운싱(Debouncing) 로직**을 추가하여 5ms 이내의 중복 인터럽트를 무시하도록 구현.

<p align="center">
<img src="docs/hardware/trouble_shooting2.png" width="70%" alt="Debouncing Logic">
</p>

---

## 📂 File Structure

```text
.
├── kernel_modules/
│   ├── ssd1306_i2c.c        # OLED Framebuffer Driver
│   ├── dht11_ledbar.c       # Sensor & Actuator Driver
│   ├── rotary_device.c      # Input Subsystem Driver
│   └── ds1302_rpi_rtc.c     # RTC Protocol Driver
├── user_app/
│   └── env-oled.c           # Main Control Daemon
├── config/
│   ├── 99-mini-dev.rules    # Udev Rule
│   └── *.service            # Systemd Units
└── docs/                    # Schematics & Datasheets

```
