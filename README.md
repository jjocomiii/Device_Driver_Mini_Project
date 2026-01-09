# RPi4 On-Boot Device Driver Pack (Mini)

Raspberry Pi 4B에서 **커널 모듈 기반 디바이스 드라이버 4개**와  
**유저스페이스 데몬 1개**, 그리고 **systemd + udev 자동 실행/권한 설정**까지 한 번에 묶은 미니 패키지입니다.

부팅 후 별도 조작 없이,
- 커널 모듈 로드
- /dev 노드 권한 설정
- 데몬 실행  
까지 자동으로 완료되도록 구성했습니다.

---

## What you get

### 1) OLED UI (SSD1306)
- 시간/날짜 페이지
- 온/습도 페이지
- 페이지 전환 및 RTC 편집 상태 표시

### 2) Input (Rotary + Key)
- 페이지 전환
- RTC 시간 편집/저장(필드 단위)

### 3) Sensor + Output (DHT11 + LED Bar)
- 주기 샘플링 기반 온습도 캐시
- 습도 값을 0~8단계로 매핑해 LED Bar 자동 갱신

---

## Demo

> 아래 gif/png는 예시 경로입니다. 실제 저장 위치에 맞춰 수정하세요.

<p align="center">
  <img src="docs/videos/demo.gif" width="520">
</p>

<details>
<summary><b>More</b></summary>

<br/>

- Page switching  
  <img src="docs/videos/switching_mode.gif" width="520">

- RTC edit & save  
  <img src="docs/videos/edit_time.gif" width="520">

- Humidity → LED bar  
  <img src="docs/videos/humidity_change.gif" width="520">

</details>

---

## Hardware

- Setup  
  <img src="docs/hardware/setup.png" width="520">

- Schematic  
  <img src="docs/hardware/schematic.png" width="520">

<details>
<summary><b>UI screenshots</b></summary>

<br/>

- Time edit  
  <img src="docs/hardware/ui_time_edit.png" width="520">

- Sensor page  
  <img src="docs/hardware/ui_sensor.png" width="520">

</details>

---

## System Overview

### Device nodes
- `/dev/ssd1306` : OLED framebuffer write
- `/dev/dht11`   : DHT11 read (driver 내부에서 캐시 갱신)
- `/dev/rotary`  : rotary/key event read/poll
- `/dev/rtc0`    : RTC read/set (DS1302)

### Boot automation
- `mini-kmods.service` : 부팅 시 `.ko` 모듈 로드
- `env-oled.service`   : OLED UI 데몬 자동 실행
- `99-mini-dev.rules`  : /dev 노드 권한 세팅

---

## Functional Details (Implementation)

### OLED daemon: env-oled
- `/dev/ssd1306`에 **128x64 framebuffer (1024 bytes)**를 주기적으로 write
- 2 pages
  - **Clock**: `YYYY-MM-DD` + `HH:MM:SS`
  - **Sensor**: `HUMI xx%`(큰 글씨) + `TEMP xxC`

### Rotary/Key behavior
- Normal mode  
  - Rotary: Clock ↔ Sensor 전환  
  - Key(K): Clock → Edit 진입 / Sensor → Clock 복귀

- Edit mode  
  - Key(K): 필드 이동 `YEAR → MON → DAY → HOUR → MIN → SEC → EXIT`
  - Rotary: 선택 필드 값 증감
  - EXIT에서 Key(K): `RTC_SET_TIME` 저장 + "SAVED" 토스트 출력

### DHT11 + LED Bar
- DHT11 주기 샘플링(기본 2초)으로 값 캐시
- 습도 0~100% → 0~8 단계로 변환
- LED Bar는 드라이버에서 자동 갱신

### RTC (DS1302)
- `/dev/rtc0` 제공
- daemon이 `RTC_RD_TIME / RTC_SET_TIME`로 읽기/설정

---

## Wiring (Fixed Pinout)

> **BCM(GPIO 번호)** 기준 / 괄호는 **Physical Pin(물리 핀)**

### OLED (SSD1306 / I2C)
| Signal | BCM | Physical | Note |
|---|---:|---:|---|
| SDA | GPIO2 | 3 | I2C Data |
| SCL | GPIO3 | 5 | I2C Clock |
| VCC | - | 1 or 17 | 3.3V |
| GND | - | Any | |

### DHT11
| Signal | BCM | Physical | Note |
|---|---:|---:|---|
| DATA | GPIO4 | 7 | 1-Wire |
| VCC | - | - | 3.3V |
| GND | - | - | |

### DS1302 (RTC)
| Signal | BCM | Physical | Note |
|---|---:|---:|---|
| CLK | GPIO5 | 29 | |
| DAT | GPIO6 | 31 | |
| RST/CE | GPIO13 | 33 | Chip Enable |
| VCC | - | - | 3.3V |
| GND | - | - | |

### Rotary Encoder
| Signal | BCM | Physical | Note |
|---|---:|---:|---|
| S1 | GPIO17 | 11 | Phase A |
| S2 | GPIO27 | 13 | Phase B |
| KEY | GPIO22 | 15 | Push Button |
| GND | - | - | Common |

### LED Bar (8ch)
| CH | BCM | Physical |
|---:|---:|---:|
| 1 | GPIO23 | 16 |
| 2 | GPIO24 | 18 |
| 3 | GPIO25 | 22 |
| 4 | GPIO12 | 32 |
| 5 | GPIO16 | 36 |
| 6 | GPIO20 | 38 |
| 7 | GPIO21 | 40 |
| 8 | GPIO26 | 37 |

---

## Permissions

udev rule(`99-mini-dev.rules`)로 다음 노드 권한을 `0666`으로 설정합니다.
- `/dev/ssd1306`
- `/dev/dht11`
- `/dev/rotary`
- `/dev/rtc0`

---

## Architecture

```mermaid
flowchart LR
  subgraph Boot["Boot & OS Integration"]
    KMOD["mini-kmods.service<br/>insmod modules"]
    RULE["99-mini-dev.rules<br/>device permissions"]
    SVC["env-oled.service<br/>start daemon"]
  end

  subgraph Kernel["Kernel Space"]
    M[".ko drivers<br/>ssd1306 | dht11_ledbar | rotary | ds1302_rtc"]
    N["/dev nodes<br/>ssd1306 dht11 rotary rtc0"]
  end

  subgraph User["User Space"]
    D["env-oled daemon"]
  end

  KMOD --> M --> N
  RULE --> N
  SVC --> D
  D <--> N
