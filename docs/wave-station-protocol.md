# WaveStation Binary Protocol (WSP1)

WaveStation(ESP32 DevKitC v4)과 C++ 백엔드 간 바이너리 통신 프로토콜 명세입니다.

- **Magic**: `WSP1` (`0x57535031`)
- **Protocol version**: `1`
- **Byte order**: Little-endian
- **Namespace**: `wsp`
- **TCP port**: `41737` (`wsp::kTcpPort`)
- **TCP 역할**: **ESP32 = 서버(listen)**, **백엔드(호스트) = 클라이언트(connect)**
- **헤더 파일**: `src/wave-server/device/protocol/wave_station.h`
- **ESP32 구현 가이드**: [wave-station-esp32-guide.md](./wave-station-esp32-guide.md)

---

## 1. 개요

WaveStation은 마이크(INMP441), 스피커(예정), IR, 조도/온도/습도 센서(예정)를 ESP32에 연결한 엣지 디바이스입니다. 본 프로토콜은 장치와 호스트(PC/서버) 간 **오디오 스트림**, **IR raw 타이밍**, **센서 값**, **구독 제어**를 단일 바이너리 포맷으로 주고받기 위한 것입니다.

### 1.1 Transport — TCP 41737

WaveStation ESP32는 공유기(WiFi)에 연결된 뒤 **TCP 41737 포트에서 listen** 합니다. 백엔드(wave-server)가 장치의 LAN IP로 **connect** 합니다.

```
[백엔드 :8500]  ──TCP connect──▶  [ESP32 :41737 listen]
         ◀──── WSP1 패킷 ────▶
```

| 항목 | 값 |
|------|-----|
| 포트 | **41737** |
| ESP32 역할 | TCP **서버** (`0.0.0.0:41737` bind + accept) |
| 백엔드 역할 | TCP **클라이언트** |
| 장치 주소 | 공유기 DHCP IP (고정 IP·DHCP 예약 권장) |
| 재접속 | 연결 끊김 시 **백엔드**가 재 connect (지수 백오프) |

각 패킷은 다음 형태입니다.

```
+--------+------------------+
| Header | Payload (Body)   |
| 16 B   | payloadSize bytes|
+--------+------------------+
```

- `payloadSize`는 **0 ~ kMaxPayload(4096)** 범위를 권장합니다.
- 한 TCP 메시지 = 한 WSP1 패킷으로 매핑합니다.

---



## 2. 공통 헤더

제어 메시지와 데이터 메시지 모두 **16바이트 고정 헤더**를 사용합니다.

### 2.1 ControlHeader (Subscribe, Unsubscribe, Ack, Error, Heartbeat)


| Offset | Size | Field       | Description              |
| ------ | ---- | ----------- | ------------------------ |
| 0      | 4    | magic       | `0x57535031` ("WSP1")    |
| 4      | 1    | version     | `1`                      |
| 5      | 1    | reserved    | `0`                      |
| 6      | 2    | type        | [Type](#3-message-types) |
| 8      | 4    | payloadSize | Body 길이 (바이트)            |
| 12     | 4    | requestId   | Ack/Error 매칭용 요청 ID      |




### 2.2 DataHeader (오디오, IR, 센서)


| Offset | Size | Field       | Description                        |
| ------ | ---- | ----------- | ---------------------------------- |
| 0      | 4    | magic       | `0x57535031` ("WSP1")              |
| 4      | 1    | version     | `1`                                |
| 5      | 1    | flags       | [HeaderFlagBits](#31-header-flags) |
| 6      | 2    | type        | [Type](#3-message-types)           |
| 8      | 4    | payloadSize | Payload 전체 길이 (바이트)                |
| 12     | 4    | sequence    | 타입별 순번 (손실/순서 검출)                  |




### 3.1 Header flags


| Bit | Name                       | Description                     |
| --- | -------------------------- | ------------------------------- |
| 0   | HeaderFlag_HasTimestampBit | Payload 앞에 `TimestampPrefix` 포함 |
| 1   | HeaderFlag_LastFrameBit    | 오디오/스트림 종료 프레임                  |
| 2   | HeaderFlag_KeyFrameBit     | 압축 오디오 디코더 리셋/동기용 키프레임          |


---



## 3. Message types


| Value  | Name         | Direction     | Header        | Body                 |
| ------ | ------------ | ------------- | ------------- | -------------------- |
| 0x0101 | MicPCM       | device → host | DataHeader    | AudioPCMBody + PCM   |
| 0x0102 | MicComp      | device → host | DataHeader    | AudioCompBody + Opus |
| 0x0111 | SpkPCM       | host → device | DataHeader    | AudioPCMBody + PCM   |
| 0x0112 | SpkComp      | host → device | DataHeader    | AudioCompBody + Opus |
| 0x0201 | IrReceive    | device → host | DataHeader    | IrReceiveBody + raw  |
| 0x0202 | IrTransmit   | host → device | DataHeader    | IrTransmitBody + raw |
| 0x0301 | AmbientLight | device → host | DataHeader    | SensorBody           |
| 0x0302 | Temperature  | device → host | DataHeader    | SensorBody           |
| 0x0303 | Humidity     | device → host | DataHeader    | SensorBody           |
| 0xF001 | Subscribe    | host → device | ControlHeader | SubscribeBody        |
| 0xF002 | Unsubscribe  | host → device | ControlHeader | UnsubscribeBody      |
| 0xF010 | Ack          | either        | ControlHeader | AckBody              |
| 0xF011 | Error        | either        | ControlHeader | ErrorBody + msg      |
| 0xF020 | Heartbeat    | either        | ControlHeader | (empty)              |


---



## 4. Payload 레이아웃 (DataHeader)

DataHeader 패킷의 Payload는 아래 순서로 구성됩니다.

```
[TimestampPrefix]   optional (flags & HasTimestamp)
[TypeBody]          fixed struct per type
[VariableData]      trailing bytes
```

**payloadSize = TimestampPrefix(optional) + TypeBody + VariableData**

### TimestampPrefix (8 bytes, optional)


| Field       | Type     | Description   |
| ----------- | -------- | ------------- |
| timestampUs | uint64_t | 장치 부팅 후 경과 μs |


---



## 5. 제어 평면



### 5.1 Subscribe

호스트가 특정 데이터 타입의 스트리밍/주기 전송을 요청합니다.

**SubscribeBody (8 bytes)**


| Field      | Type     | Description                        |
| ---------- | -------- | ---------------------------------- |
| targetType | uint16_t | 구독할 Type (MicComp, AmbientLight 등) |
| intervalMs | uint16_t | 센서: 전송 주기(ms). 오디오: `0` = 최대 속도    |
| options    | uint32_t | SubscribeOptionFlagBits            |


**SubscribeOptionFlagBits**


| Bit | Name                                 | Description             |
| --- | ------------------------------------ | ----------------------- |
| 0   | SubscribeOptionFlag_OnChangeOnlyBits | 센서: 값 변경 시에만 전송         |
| 1   | SubscribeOptionFlag_CompressedBits   | 오디오: MicComp/SpkComp 선호 |


**흐름 예시**

```
Host  -> Device : Subscribe(MicComp, intervalMs=0, Compressed)
Device -> Host  : Ack(requestId, status=0)
Device -> Host  : DataHeader(MicComp, seq=0) + AudioCompBody + opus...
Device -> Host  : DataHeader(MicComp, seq=1) + ...
```



### 5.2 Unsubscribe

**UnsubscribeBody (2 bytes)**


| Field      | Type     | Description |
| ---------- | -------- | ----------- |
| targetType | uint16_t | 해제할 Type    |


`payloadSize = 2`

### 5.3 Ack

**AckBody (8 bytes)**


| Field     | Type       | Description |
| --------- | ---------- | ----------- |
| requestId | uint32_t   | 원 요청 ID     |
| status    | uint8_t    | `0` = 성공    |
| reserved  | uint8_t[3] | `0`         |




### 5.4 Error

**ErrorBody (8 bytes + optional message)**


| Field     | Type     | Description |
| --------- | -------- | ----------- |
| requestId | uint32_t | 원 요청 ID     |
| code      | int32_t  | ErrorCode   |


**ErrorCode**


| Value | Name                | Description |
| ----- | ------------------- | ----------- |
| 0     | ErrorOk             | 성공          |
| -1    | ErrorUnsupported    | 미지원 타입/코덱   |
| -2    | ErrorBusy           | 장치 사용 중     |
| -3    | ErrorInvalidPayload | 잘못된 payload |


가변 메시지 길이:

```
messageLen = payloadSize - 8
```

메시지는 ErrorBody 직후 ASCII/UTF-8 바이트열로 이어집니다. null terminator는 사용하지 않습니다.

### 5.5 Heartbeat

- `ControlHeader.type = Heartbeat`
- `payloadSize = 0` (Body 없음)
- 연결 유지 및 장치 alive 확인용

---



## 6. 오디오



### 6.1 기본 파라미터 (권장)


| 항목            | 값                   |
| ------------- | ------------------- |
| Sample rate   | 16000 Hz            |
| Channels      | 1 (mono)            |
| PCM bit depth | 16-bit signed       |
| PCM frame     | 320 samples / 20 ms |
| Compression   | Opus, 20 ms frame   |
| Opus bitrate  | 24–32 kbps (협의)     |




### 6.2 AudioPCMBody (MicPCM, SpkPCM)


| Field         | Type      | Description        |
| ------------- | --------- | ------------------ |
| sampleRate    | uint32_t  | Hz                 |
| channels      | uint8_t   | 1 = mono           |
| bitsPerSample | uint8_t   | 16                 |
| sampleCount   | uint16_t  | 채널당 샘플 수           |
| pcmData       | uint8_t[] | trailing PCM bytes |


**PCM 데이터 크기**

```
pcmBytes = sampleCount * channels * (bitsPerSample / 8)
payloadSize = sizeof(TimestampPrefix?) + 8 + pcmBytes
```

20 ms @ 16 kHz mono 16-bit 예: `sampleCount=320`, `pcmBytes=640`

### 6.3 AudioCompBody (MicComp, SpkComp)


| Field           | Type      | Description    |
| --------------- | --------- | -------------- |
| codec           | uint8_t   | `1` = Opus     |
| sampleRate      | uint32_t  | Hz             |
| channels        | uint8_t   | 1 = mono       |
| frameDurationMs | uint8_t   | 20, 40, 60 ... |
| encodedSize     | uint16_t  | 인코딩 데이터 길이     |
| encodedData     | uint8_t[] | Opus 프레임       |


```
payloadSize = sizeof(TimestampPrefix?) + 9 + encodedSize
```

**SpkComp (PC → 장치)**: 호스트가 Opus로 인코딩한 프레임을 전송하면, ESP32에서 Opus 디코더로 PCM 변환 후 I2S 스피커 출력이 가능합니다. MicComp와 동일 코덱/파라미터를 사용합니다.

**KeyFrame**: `HeaderFlag_KeyFrameBit`가 설정된 프레임은 디코더 상태를 리셋하거나 동기화할 때 사용합니다.

---



## 7. IR

IRremoteESP8266 라이브러리의 raw 타이밍을 **마이크로초(μs)** 단위 `uint16_t` 배열로 주고받습니다. 배열은 **mark/space가 교대**하며, **첫 값은 mark**입니다.

### 7.1 IrReceive (device → host)

**IrReceiveBody (4 bytes + raw)**


| Field    | Type       | Description              |
| -------- | ---------- | ------------------------ |
| length   | uint16_t   | mark/space 개수            |
| overflow | uint8_t    | `1` = raw 버퍼 overflow 발생 |
| reserved | uint8_t    | `0`                      |
| rawData  | uint16_t[] | μs 단위 타이밍                |


```
rawBytes = length * 2
payloadSize = sizeof(TimestampPrefix?) + 4 + rawBytes
```



### 7.2 IrTransmit (host → device)

**IrTransmitBody (8 bytes + raw)**


| Field     | Type       | Description            |
| --------- | ---------- | ---------------------- |
| length    | uint16_t   | mark/space 개수          |
| carrierHz | uint32_t   | 반송파 주파수 (기본 38000)     |
| repeat    | uint16_t   | `0` = 1회, `N` = (N+1)회 |
| rawData   | uint16_t[] | μs 단위 타이밍              |


장치는 `rawData`를 IR LED로 송신합니다 (예: `IRsend::sendRaw`).

---



## 8. 센서

AmbientLight, Temperature, Humidity 모두 **SensorBody (8 bytes)** 를 사용합니다.


| Field    | Type     | Description             |
| -------- | -------- | ----------------------- |
| unit     | uint8_t  | SensorUnit              |
| quality  | uint8_t  | `0`=invalid, `255`=good |
| reserved | uint16_t | `0`                     |
| value    | float    | IEEE 754 little-endian  |


**SensorUnit**


| Value | Name        | Type         | Unit      |
| ----- | ----------- | ------------ | --------- |
| 1     | UnitLux     | AmbientLight | lux       |
| 2     | UnitCelsius | Temperature  | °C        |
| 3     | UnitPercent | Humidity     | % (0–100) |


센서는 Subscribe 후 `intervalMs` 주기(또는 OnChangeOnly)로 DataHeader + SensorBody를 전송합니다.

---



## 9. Hex 예시



### 9.1 Subscribe (MicComp)

```
ControlHeader:
  magic       31 50 53 57   ("WSP1")
  version     01
  reserved    00
  type        01 F0         (Subscribe = 0xF001)
  payloadSize 08 00 00 00   (8)
  requestId   01 00 00 00   (1)

SubscribeBody:
  targetType  02 01         (MicComp)
  intervalMs  00 00
  options     02 00 00 00   (Compressed)
```



### 9.2 MicComp data frame (Opus, no timestamp)

```
DataHeader:
  magic       31 50 53 57
  version     01
  flags       00
  type        02 01         (MicComp)
  payloadSize XX 00 00 00   (9 + encodedSize)
  sequence    00 00 00 00

AudioCompBody (9 bytes fixed):
  codec           01        (Opus)
  sampleRate      80 3E 00 00  (16000)
  channels        01
  frameDurationMs 14        (20)
  encodedSize     XX XX

encodedData: [encodedSize bytes]
```



### 9.3 IrReceive (4 marks/spaces = length 4)

```
IrReceiveBody:
  length     04 00
  overflow   00
  reserved   00
  rawData    90 00 E0 01 90 00 ...  (900us mark, 480us space, ...)
```

---



## 10. Payload size helpers

헤더 파일에 고정 body 크기 상수와 가변 tail 계산 헬퍼가 포함되어 있습니다.

| Symbol | Description |
| ------ | ----------- |
| `kTimestampPrefixSize` | 8 |
| `kSubscribeBodySize` | 8 |
| `kUnsubscribeBodySize` | 2 |
| `kAckBodySize` | 8 |
| `kErrorBodyFixedSize` | 8 |
| `kAudioPCMBodySize` | 8 |
| `kAudioCompBodySize` | 9 |
| `kIrReceiveBodySize` | 4 |
| `kIrTransmitBodySize` | 8 |
| `kSensorBodySize` | 8 |
| `pcm_data_size(body)` | PCM trailing byte length |
| `ir_raw_data_size(length)` | IR raw array byte length |
| `error_msg_size(payloadSize)` | Error optional message length |

---

## 11. 구현 체크리스트

- [ ] magic/version 검증 후 payloadSize ≤ 4096 확인
- [ ] ControlHeader.requestId ↔ Ack/Error 매칭
- [ ] DataHeader.sequence 타입별 독립 카운터
- [ ] HasTimestamp 플래그 시 payload 앞 8바이트를 TimestampPrefix로 파싱
- [ ] float / uint16_t / uint32_t 모두 little-endian
- [ ] IR raw: μs 단위, mark-first 교대 배열
- [ ] Error message: `payloadSize - 8` 길이로 파싱 (null terminator 없음)
- [ ] Heartbeat: payloadSize = 0

---



## 12. 변경 이력


| Version | Date       | Notes                                              |
| ------- | ---------- | -------------------------------------------------- |
| 1       | 2026-07-06 | ESP32 TCP server / backend client 역할 명시 |


