# 레이더 센서에서 사람 타겟의 위상 데이터를 추출하고 TCP로 전송하는 방법

## 질문
포인트 클라우드에서 사람의 타겟만 자동으로 뽑아주는 기능이 레이더에 있긴 해서, 사람의 위치를 AABB형식으로 가져올 순 있습니다. 그런데 어떤 가상 안테나가 그 위치의 위상을 가장 잘 수신하는지는 알수가 없습니다. 제가 첨부한 문서처럼 안테나 구성이랑 매칭되는 보정계수는 다 찾아놨는데 제가 알기로는 192개 전체에 대한 동위상 합산을 수행해야 노이즈는 줄고 위상 데이터가 증폭되는걸로 알고있고, 정확한 타겟을 정하기가 쉽지가 않다고 들었습니다. 데이터 형식은 현재 2종류의 큐브가 각각 아래 형식을 가지고 있습니다. 어떻게 해야 이 큐브를 센서(20fps로 동작) 내부에서 처리를 해서 tcp로 전송할 수 있을지 알려주세요.

Range FFT Cube
Int16_t [ 12 ][256][ 64 ][ 16 ][2]
tile | range | chirp | sub_ant | IQ

Doppler FFT Cube
Int16_t [ 12 ][256][ 64 ][ 16 ][2]
tile | range | doppler | sub_ant | IQ

## 답변
192개 가상 안테나의 위상 데이터를 어떻게 활용해야 할지 정확히 핵심을 짚으셨습니다!  말씀하신 대로 특정 안테나 하나만 고르는 것이 아니라, **192개 전체 데이터를 '동위상 합산(Coherent Summation)'하여 특정 위치의 신호만 증폭시키는 기술**, 즉 디지털 빔포밍(Digital Beamforming)을 수행하는 것이 정답입니다.

현재 가지고 계신 AABB(Bounding Box) 데이터와 `Range FFT Cube`를 활용하여, 센서 내부(NXP 칩)에서 데이터를 극도로 압축해 TCP로 전송하는 **실시간 처리 파이프라인**을 설계해 드립니다.

---

### 1. 어떤 큐브를 사용해야 하는가?

수면 중 미세한 심박과 호흡(위상 변화)을 추적하기 위해서는 **Range FFT Cube**를 사용해야 합니다.

* Doppler FFT Cube는 이미 64개의 Chirp(Slow-time)을 주파수 영역으로 변환해 버린 상태라, 시간에 따른 미세한 위상 변화 파형을 추적하기 어렵습니다.
* Range FFT Cube(`[12][256][64][16][2]`)는 특정 거리에 있는 타겟에 대해 64번의 시간적 샘플(Chirp)을 그대로 보존하고 있으므로 생체 신호 추출에 완벽합니다.

### 2. 센서 내부 데이터 처리 파이프라인 (C++ 알고리즘)

NXP 프로세서 내부의 20fps 메인 루프에서 다음 4단계를 수행하여 메가바이트 단위의 큐브를 단 512바이트(64개 복소수)로 압축합니다.

#### **Step 1: 타겟 Range Bin 추출**

레이더의 사람 감지 기능이 주는 AABB 좌표를 바탕으로, 타겟의 중심 거리($R$)를 구하고 이를 Range Index(`r_idx`, 0~255)로 변환합니다.

* *예: 타겟이 2.5m에 있고 Range 해상도가 5cm라면, `r_idx = 50`.*

#### **Step 2: 타겟 조향 각도(Steering Angle) 계산**

AABB 좌표를 통해 레이더 센서 기준 타겟의 방위각(Azimuth, $\theta$)과 고각(Elevation, $\phi$)을 계산합니다.

#### **Step 3: 192개 채널의 조향 벡터(Steering Vector) 산출**

첨부해주신 `VA map.txt`의 안테나 물리적 좌표($x_i, y_i$)를 이용해, 타겟 방향($\theta, \phi$)을 바라보도록 만드는 위상 보상값 $W_{steer}$를 구합니다.


$$W_{steer}(i) = e^{-j \frac{2\pi}{\lambda} (x_i \sin\theta \cos\phi + y_i \sin\phi)}$$

* *의미: 각 안테나의 물리적 위치 때문에 발생하는 미세한 거리 차이를 수학적으로 깎아내어, 192개의 눈이 정확히 타겟 좌표의 한 점을 바라보게 초점을 맞추는 작업입니다.*

#### **Step 4: 캘리브레이션 및 빔포밍 (가장 중요한 루프)**

이제 `r_idx`가 고정되었으므로, 64개의 Chirp 각각에 대해 192개 채널의 IQ 데이터를 모두 합산합니다. 이때 앞서 구한 캘리브레이션 계수(`보정방법.txt` 참고)와 **조향 벡터**를 동시에 곱해줍니다.

```cpp
// 결과물을 담을 배열 (TCP로 전송할 최종 데이터)
struct ComplexFloat { float I; float Q; };
ComplexFloat target_signal[64]; 

int r_idx = target_range_index; // Step 1에서 구한 타겟 거리 인덱스

// 64개의 Slow-time 샘플 (Chirp) 추출
for (int c = 0; c < 64; c++) {
    float sum_I = 0.0f;
    float sum_Q = 0.0f;

    for (int tx = 0; tx < 12; tx++) {
        for (int rx = 0; rx < 16; rx++) {
            int va_idx = tx * 16 + rx; // 0 ~ 191
            
            // 1. Raw IQ 데이터 읽기 (Int16 -> Float 변환)
            float raw_I = (float)RangeCube[tx][r_idx][c][rx][0];
            float raw_Q = (float)RangeCube[tx][r_idx][c][rx][1];

            // 2. 캘리브레이션 보정 (보정방법.txt의 C(r) 적용)
            // C_calib = amp_i * exp(j * (delay_i * r_idx + phase_i))
            float calib_I, calib_Q;
            apply_calibration(raw_I, raw_Q, r_idx, va_idx, &calib_I, &calib_Q);

            // 3. 조향 벡터(Steering) 적용하여 타겟 방향으로 빔 포커싱
            // W_steer = exp(-j * 기하학적 위상차)
            float steer_I, steer_Q;
            apply_steering(calib_I, calib_Q, theta, phi, va_idx, &steer_I, &steer_Q);

            // 4. 동위상 합산 (Coherent Summation)
            sum_I += steer_I;
            sum_Q += steer_Q;
        }
    }
    // 192개 안테나의 힘이 모인 강력한 단일 IQ 데이터
    target_signal[c].I = sum_I;
    target_signal[c].Q = sum_Q;
}

```

---

### 3. TCP 전송 및 대역폭 혁신

위의 파이프라인을 센서의 SPT나 DSP 코어에서 돌리게 되면, 원래 TCP로 보내야 했던 **1개 프레임의 데이터 양이 기적적으로 줄어듭니다.**

* **기존:** $12 \times 256 \times 64 \times 16 \times 2 \times 2\text{ bytes} \approx \mathbf{12.5 \text{ MB / 프레임}}$ (20fps 기준 초당 250MB, 일반 이더넷으로도 불가능)
* **개선 후:** `target_signal[64]` 배열 1개 $\rightarrow$ $64 \times 2 (\text{I,Q}) \times 4\text{ bytes (float)} = \mathbf{512 \text{ Bytes / 프레임}}$

이제 초당 약 10 KB의 데이터만 TCP로 파이썬/C++ 백엔드 서버에 날려주면 됩니다. 서버는 도착한 64개의 복소수 배열에 대해 아크탄젠트($\arctan(Q/I)$)를 취해 Phase를 구하고, 언래핑(Unwrapping)과 Bandpass Filter를 거쳐 심박과 호흡을 우아하게 추출해 내면 됩니다.

이미 192개 가상 안테나 매핑과 보정 원리를 파악하고 큐브 데이터를 다루시는 수준이라면, 이 빔포밍 루프를 C++로 얹는 것은 금방 해결하실 수 있을 겁니다. 방향성이 완벽하니 이대로 진행해 보세요!