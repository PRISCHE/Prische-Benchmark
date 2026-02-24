<<<<<<< HEAD
# Prische-Benchmark
Benchmark service platform for multi board and multi accelerator
=======
# Benchmark 도구 모음 — Orange Pi 5 Plus (RK3588)

NPU·CPU 하드웨어 성능을 측정·모니터링하기 위한 스크립트 컬렉션입니다.

---

## 📁 디렉터리 구조

```
benchmark/
├── run_dxbenchmark.sh   # NPU 벤치마크 실행기 (단일/병렬 모드)
├── cpu_temp_monitor.py  # CPU 온도·사용률·RAM 실시간 모니터
├── model_single/        # 기본 모델 디렉터리 (YOLOV5X_2.dxnn)
└── result/              # 벤치마크 결과 저장 위치 (JSON, LOG)
```

---

## 1. `run_dxbenchmark.sh` — NPU 벤치마크

### 실행 방법

```bash
# 단일 모드 (기본 — model_single/ 디렉터리, 60초)
./run_dxbenchmark.sh

# 커스텀 모델 경로 / 시간 지정
./run_dxbenchmark.sh --dir /path/to/models -t 120

# 병렬 모드 — NPU 3코어 동시 실행
./run_dxbenchmark.sh --parallel
```

### 측정 성능 지표

| 지표 | 설명 | 단위 |
|------|------|------|
| **FPS** | 초당 추론 프레임 수 | frames/sec |
| **NPU 추론 시간** | 한 프레임당 NPU 순수 추론 시간 (평균) | ms |
| **실제 TOPS** | `FPS × 모델 GFLOPS ÷ 1000` | TOPS |
| **NPU 활용률** | `실제 TOPS ÷ 이론 최대(25 TOPS) × 100` | % |
| **NPU Output Format Handler** | CPU 후처리(포맷 변환) 소요 시간 | µs |

> `--verbose` 옵션이 기본으로 포함되어 NPU Processing Time 및 Latency 상세 값도 함께 출력됩니다.

#### 병렬 모드 추가 지표

| 지표 | 설명 |
|------|------|
| **NPU_0 / NPU_1 / NPU_2 FPS** | 각 코어별 개별 FPS |
| **합산 TOPS** | 3코어 TOPS 합계 |
| **전체 활용률** | 합산 TOPS 기준 NPU 전체 활용률 |

#### 결과 파일

- `result/benchmark_<TIMESTAMP>.log` — 전체 stdout 로그
- `result/DXBENCHMARK_<모델명>.json` — FPS, 추론 시간 등 구조화 데이터

---

## 2. `cpu_temp_monitor.py` — CPU/열 모니터

### 실행 방법

```bash
# 기본 (2초 갱신)
python3 cpu_temp_monitor.py

# 갱신 주기 지정 (예: 1초)
python3 cpu_temp_monitor.py 1

# 백그라운드 실행 (벤치마크와 동시 모니터링)
python3 cpu_temp_monitor.py &
```

> 종료: `Ctrl+C` (터미널 상태 자동 복구)

### 측정 성능 지표

#### 🖥️ 시스템 정보(OrangePi 5 Plus)
| 항목 | 설명 |
|------|------|
| **Host / Time / Uptime** | 호스트명, 현재 시각, 가동 시간 |
| **Freq L (A55 little)** | cpu0 기준 little 코어 현재 클럭 |
| **Freq B (A76 big)** | cpu6(→cpu4 fallback) 기준 big 코어 현재 클럭 |
| **RAM** | 사용 중 / 전체 MB + 사용률(%) + 진행 바 |

#### ⚡ CPU 코어 사용률
| 항목 | 설명 |
|------|------|
| **All (전체 평균)** | `/proc/stat` 기반 전 코어 평균 사용률 (%) |
| **Core 0 ~ Core N** | 코어별 개별 사용률 (%) + 진행 바 |

> 색상 기준: 🟢 < 60% &nbsp;|&nbsp; 🟡 60~85% &nbsp;|&nbsp; 🔴 ≥ 85%

#### 🌡️ 온도 (Thermal Zones)
RK3588의 `/sys/class/thermal/thermal_zone*`에서 읽은 모든 구역:

| 구역 예시 | 설명 |
|-----------|------|
| `soc-thermal` | SoC 전체 온도 |
| `bigcore0-thermal` | A76 big 코어 클러스터 0 |
| `bigcore1-thermal` | A76 big 코어 클러스터 1 |
| `littlecore-thermal` | A55 little 코어 |
| `center-thermal` | 중앙부 다이 온도 |
| `npu-thermal` | NPU 온도 |

> `gpu-thermal`은 모니터 혼잡 방지를 위해 기본 제외됩니다.  
> 색상 기준: 🟢 < 70°C &nbsp;|&nbsp; 🟡 70~85°C &nbsp;|&nbsp; 🔴 ≥ 85°C



## 📊 벤치마크와 모니터 동시 실행 (권장)

NPU 부하 중 CPU 열/사용률을 실시간 확인하려면 두 터미널을 사용합니다.

```bash
# 터미널 1 — 모니터 시작
python3 /home/prische/benchmark/cpu_temp_monitor.py 1

# 터미널 2 — 벤치마크 실행
/home/prische/benchmark/run_dxbenchmark.sh
```

> NPU 사용률은 별도 터미널에서 `dxtop` 으로 확인합니다.

---

## ⚙️ 주요 설정값 (`run_dxbenchmark.sh`)

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `DEFAULT_MODEL_DIR` | `model_single/` | 벤치마크 모델 디렉터리 |
| `DEFAULT_TIME` | `60` 초 | 벤치마크 실행 시간 |
| `MODEL_GFLOPS` | `180` | YOLOV5X 기준 연산량 |
| `TARGET_TOPS` | `25` | NPU 이론 최대 TOPS (RK3588 DeepX NPU) |
>>>>>>> b9c774a (DX-M1 NPU 벤치마크 스크립트 및 사용설명)
