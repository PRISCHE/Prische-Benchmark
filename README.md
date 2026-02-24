# 🚀 DeepX NPU 벤치마크 사용 설명서

본 설명서는 다른 환경에서 **DeepX NPU (DX-M1 / V3)** 기반의 모델 추론 성능을 측정하고 시스템 상태(온도, CPU 사용률 등)를 모니터링하기 위한 가이드입니다.

---

## 📁 1. 디렉터리 구조 및 주요 파일

```text
benchmark/
├── run_dxbenchmark.sh   # NPU 벤치마크 실행 핵심 스크립트 (단일/병렬 모드 지원)
├── cpu_temp_monitor.py  # CPU 온도·사용률·RAM 실시간 모니터링 파이썬 스크립트
├── model_single/        # 기본 모델 디렉터리 (YOLOV5X_2.dxnn 모델 포함)
└── result/              # 실행 후 벤치마크 결과(로그, JSON)가 자동 저장되는 폴더
```

---

## 🛠️ 2. 벤치마크 실행 전 환경 설정 (사용자명 변경)

기본 스크립트는 기존 사용자명(`prische`) 기준으로 절대 경로가 설정되어 있습니다. 
다른 사용자 환경에서 실행하기 위해 `run_dxbenchmark.sh` 파일 내부의 경로를 본인의 환경에 맞게 수정해야 합니다.

### 수정해야 할 파일: `run_dxbenchmark.sh`

편집기(nano, vim 등)로 `run_dxbenchmark.sh` 파일을 열고 아래 변수들의 `/home/prische` 부분을 자신의 사용자 홈 디렉터리(예: `/home/user`)로 변경합니다.

```bash
# 수정 전 (예시)
BENCHMARK_DIR="/home/prische/benchmark"
DXBENCHMARK_PATH="/home/prische/dx-all-suite/dx-runtime/dx_rt/bin/dxbenchmark"
DEFAULT_MODEL_DIR="/home/prische/benchmark/model_single"

# 수정 후 (자신의 사용자명 '사용자ID'로 변경)
BENCHMARK_DIR="/home/사용자ID/benchmark"
DXBENCHMARK_PATH="/home/사용자ID/dx-all-suite/dx-runtime/dx_rt/bin/dxbenchmark"
DEFAULT_MODEL_DIR="/home/사용자ID/benchmark/model_single"
```

> **Tip:** `$HOME` 환경 변수를 활용하여 `BENCHMARK_DIR="$HOME/benchmark"` 등으로 변경해두면 다른 계정에서도 경로 수정 없이 편리하게 사용할 수 있습니다.

---

## 🏃 3. 벤치마크 실행 방법 (`model_single` 활용)

`model_single` 디렉터리에 포함된 모델(YOLOV5X 등)을 활용하여 NPU 추론 성능을 측정합니다. 

### 기본 실행 (단일 NPU 모드)
기본적으로 `model_single` 모델을 60초간 추론하며 성능을 측정합니다.
```bash
./run_dxbenchmark.sh
```

### 시간 및 모델 디렉터리 커스텀 실행
추론 시간(`-t`)이나 다른 모델이 있는 디렉터리(`--dir`)를 별도로 지정할 수 있습니다.
```bash
# 120초 동안 실행
./run_dxbenchmark.sh -t 120

# 다른 모델 디렉터리를 지정하여 실행
./run_dxbenchmark.sh --dir /path/to/other_model_dir -t 60
```

### 병렬 모드 실행 (NPU 3코어 동시 부하)
NPU의 3개 코어 전체에 부하를 주어 극한의 성능 타겟(예: 25 TOPS) 대비 실제 활용률을 측정합니다.
```bash
./run_dxbenchmark.sh --parallel
```

---

## 📊 4. 실행 로그 및 결과 저장 방식

벤치마크가 실행되면 `result/` 디렉터리가 자동으로 생성되며, 측정된 성능 지표 파일들이 저장됩니다.

1. **로그 파일 (`benchmark_YYYYMMDD_HHMMSS.log`)**
   - 벤치마크 실행 시 터미널에 출력되는 모든 표준 출력(stdout) 및 에러(stderr)가 기록됩니다.
   - NPU 처리 시간, CPU 후처리 소요 시간, 프레임 속도(FPS) 등의 텍스트 기록을 포함합니다.
2. **JSON 결과 파일 (`DXBENCHMARK_*.json`)**
   - NPU별 추론 시간, FPS 등 벤치마크 구조화 데이터가 저장되며, 스크립트 내부에서 이 JSON을 파싱하여 화면에 TOPS 등을 요약 출력합니다.
   - 병렬 모드(`--parallel`) 시에는 `result/npu0_...`, `result/npu1_...` 등 코어별 폴더에 분리되어 저장됩니다.

---

## 🌡️ 5. 실시간 시스템 모니터링 (`cpu_temp_monitor.py`)

벤치마크(NPU 부하) 실행 중 열 스로틀링이나 CPU 병목(후처리 등) 현상을 실시간 모니터링하기 위해 사용합니다. 가벼운 터미널 UI(TUI) 형태로 제공됩니다.

### 실행 방법
새로운 터미널 창을 하나 더 열어 아래 명령어를 실행합니다. (벤치마크와 동시 실행 권장)

```bash
# 기본 주기(2초) 갱신 모니터링
python3 cpu_temp_monitor.py

# 갱신 주기를 1초로 변경
python3 cpu_temp_monitor.py 1
```

### 모니터링 화면 보는 법
- **CPU Core Usage (사용률)**: 전체 및 각 코어별(0~N) 실시간 CPU 사용률이 바 형태로 나타납니다. `run_dxbenchmark.sh` 실행 시 CPU 후처리(Format Handler 등) 점유율이 높아지는지 확인할 때 유용합니다.
- **Thermal Zones (온도)**: SoC 전체, Big/Little 코어별, NPU의 현재 온도를 확인합니다. 85°C(🔴 Red) 이상이면 스로틀링 위험이 있습니다.
- **종료 방법**: `Ctrl + C` 누르면 원래 터미널 화면으로 깔끔하게 복구됩니다.

---

## 💡 요약 및 팁
* 벤치마크 테스트 전 반드시 **새로운 환경에 맞춘 경로(`/home/사용자명/...`) 수정**을 잊지 마세요.
* **터미널 2개**를 띄우고 한 곳에서는 `cpu_temp_monitor.py 1`을 실행해두고, 다른 곳에서 `./run_dxbenchmark.sh`을 실행하면 가장 이상적으로 시스템 성능을 확인할 수 있습니다.

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `DEFAULT_MODEL_DIR` | `model_single/` | 벤치마크 모델 디렉터리 |
| `DEFAULT_TIME` | `60` 초 | 벤치마크 실행 시간 |
| `MODEL_GFLOPS` | `180` | YOLOV5X 기준 연산량 |
| `TARGET_TOPS` | `25` | NPU 이론 최대 TOPS (RK3588 DeepX NPU) |
