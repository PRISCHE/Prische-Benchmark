1. 사전 요구사항: dx-all-suite SDK
본 벤치마크 프로젝트는 NPU 부하 및 성능 측정을 위해 내부적으로 DeepX에서 제공하는 SDK인 dx-all-suite 내의 dxbenchmark 바이너리를 호출합니다. 따라서 벤치마크를 실행할 보드에 해당 SDK가 먼저 클론(Clone) 및 설치되어 있어야 합니다.

dx-all-suite 설치
# 1. 원격 저장소에서 SDK 클론
git clone https://github.com/DEEPX-AI/dx-all-suite.git
cd dx-all-suite

# 2. 서브모듈(submodule) 초기화 및 업데이트 (dx-runtime 등 포함)
git submodule update --init --recursive

# 3. dx-runtime 디렉터리로 이동 후 전체 툴체인 및 드라이버 설치
cd dx-runtime
./install.sh --all
참고: 설치 스크립트 실행 시 권한이 필요할 수 있으며, NPU 드라이버 적용을 위해 재부팅이 요구될 수 있습니다. 설치 완료 후 아래 경로에 바이너리가 위치하는지 확인하세요.

벤치마크 도구 경로: /home/사용자명/dx-all-suite/dx-runtime/dx_rt/bin/dxbenchmark
이 스크립트는 dxbenchmark 바이너리를 가져와 활용합니다.


# GStreamer Pipeline Benchmark (DX-RT) 

이 프로젝트는 GStreamer와 DX-RT(DeepX NPU)를 연동하여 동작하는 **크로스플랫폼 AI 비디오 파이프라인 벤치마크 툴**입니다.  
다양한 하드웨어(Rockchip, NXP, NVIDIA, x86 등)에서 GStreamer 플러그인을 통해 **하드웨어 가속 디코딩 및 인코딩**을 수행하고, 파이프라인 각 단계별 처리 지연(Latency)과 시스템 리소스(CPU, RAM, 잔류 온도)를 측정할 수 있습니다.

> **주의 사항**: 이 Orange Pi 5 Plus (또는 Rockchip 기반 보드) 계열에서는 성능 극대화를 위해 **VPU 하드웨어 디코딩을 최우선으로 구동하여 CPU 병목을 방어**하도록 설계되어 있습니다.

---

## 🚀 주요 기능

- **크로스플랫폼 호환**: GStreamer의 `mppvideodec`, `v4l2slh264dec`, `nvv4l2decoder` 등 환경에 맞는 하드웨어 가속 플러그인을 **자동 감지하여 파이프라인을 구성**합니다.
- **HW / SW 성능 비교 벤치마크**: 명령줄 인자를 통해 강제로 소프트웨어(CPU) 디코더/인코더(`avdec_h264`, `x264enc`)를 선택하여 하드웨어 가속 적용 시의 성능 향상폭을 객관적으로 수치화하여 비교할 수 있습니다.
- **NV12 패스스루 아키텍처**: CPU 개입을 0%로 만들기 위해, 디코더의 NV12 출력을 색변환 없이 인코더까지 직결(Pass-through)하며, OSD도 NV12 버퍼(Y/UV 플레인)에 직접 작성합니다.
- **비동기 멀티스레드 (Async Inference)**: ভিডিও 디스플레이(30fps)와 NPU 추론 주기(예: 15fps)를 분리하기 위해, 독립된 스레드에서 `RunAsync/Wait` 패턴으로 텐서를 처리하고 Mutex를 통해 결과를 공유합니다.
- **파이프라인 통계 추출**: 각 프레임 단위로 `GstDecode`, `NPU Infer`, `PostProcess`, `OSD`, `GstEncode` 단계별 소요 시간을 정밀 측정하고, 종합 FPS 및 모델 TOPS(Tera Operations Per Second) 실효 성능을 예측합니다.
- **자동 리포트 생성**: 벤치마크 종료 시, `result_all/` 디렉토리에 시스템 정보, 설정 변수, 단계별 레이턴시 최소/최대/평균 정보가 포함된 **Markdown** 테이블 요약본과 **JSON** 파일이 타임스탬프 단위로 자동 저장됩니다.

---

## 🛠 시스템 요구사항 및 의존성

본 프로젝트는 아래와 같은 환경에서 개발 및 테스트되었습니다. 최적의 성능을 위해 권장 사양을 준수해 주세요.

*   **지원 및 테스트 하드웨어**: Rockchip RK3588 기반 보드 (Orange Pi 5 Plus, Wavedyne 등) 및 일반 PC
*   **운영체제 (OS)**: Ubuntu 22.04 LTS / 24.04 LTS (Armbian 포함) 또는 Debian 12 계열
*   **커널 버전 (Kernel)**: Linux Kernel 5.10 이상 (현재 주 테스트 환경 : `6.1.115-vendor-rk35xx` 등)
*   **DX-RT SDK**: DeepX NPU 추론을 위한 런타임 환경 (환경변수 `DXRT_DIR` 내지 기본 경로 `/home/prische/dx-all-suite/dx-runtime/dx_rt/build_aarch64` 요구)
*   **GStreamer 버전**: GStreamer 1.20 이상 권장 (현재 테스트 환경: Ubuntu 24.04 기준 1.24.2, Debian 12 기준 1.22.x)
    ```bash
    sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    ```
*   **Rockchip VPU 하드웨어 가속 패키지** (RK3588/Orange Pi 5 Plus 환경 사용 시 필수):
    *   **라이브러리 권장 버전**: `librockchip-mpp` 1.3.x 이상 (현재 테스트 환경: 1.5.0)
    *   **쉬운 설치 방안** (Ubuntu 24.04 기준): `sudo apt-get install gstreamer1.0-rockchip1 librockchip-mpp-dev`
    *   > **참고**: 공식 패키지 저장소를 미지원하는 사설 Debian 12 환경 등에서 VPU 초기화 실패(0 FPS 멈춤 등)가 발생할 경우, 소스코드 직접 빌드를 위한 별도의 내부 트러블슈팅 가이드를 참고

---

## ⚙️ 빌드 방법 (Build Instructions)

소스 코드를 다운로드 받은 후 `build.sh` 스크립트를 실행하면 CMake를 통해 C++ 파이프라인이 자동 빌드됩니다.

```bash
cd /home/prische/gstbenchmark
bash build.sh
```

성공적으로 빌드되면, 디렉토리 루트에 `gst_pipeline` 실행 파일이 생성됩니다.

---

## 🏃 사용 방법 (Usage)

직접 `gst_pipeline` C++ 바이너리를 실행할 수도 있지만, 환경 모니터링 및 결과 분석 리포트를 자동으로 생성해주는 **Python 래퍼 스크립트(`run_pipeline_benchmark.py`) 사용을 권장**합니다.

> **권한 안내**: 내부 NPU, VPU 하드웨어 가속 권한 및 DMA 접근을 위해 반드시 **루트 권한(`sudo`)**으로 파이썬 스크립트를 실행해 주시기 바랍니다.

### 1. 입력 소스 설정 (`rtsp_address.txt`)

프로젝트 최상단 디렉토리의 `rtsp_address.txt` 파일에 분석하고자 하는 타겟 RTSP 주소를 1줄로 기입해 주세요. 
파이썬 측정 스크립트를 실행할 때, 이 파일에 적힌 주소를 기본 스트림 소스로 가장 먼저 읽어들입니다.

### 2. 기본 실행 (하드웨어 디코더 + 하드웨어 인코더 권장사양 실행)

사용 가능한 최적의 하드웨어 플러그인(VPU/NPU)을 자동으로 찾아 실행합니다. 

```bash
sudo python3 run_pipeline_benchmark.py --duration 60
```

### 3. 특정 옵션을 지정하여 실행

입력 RTSP 주소, 모델 경로, 추론 간격, 동시 실행 채널 수 등을 수동으로 조절할 수 있습니다.

```bash
sudo python3 run_pipeline_benchmark.py \
    --input "rtsp://Id:password@192.168.0.200:554/Streaming/Channels/101" \
    --model "model/YoloV5N.dxnn" \
    --num-channels 4 \
    --interval 3 \
    --duration 60
```
*   `--input`: 분석할 대상 RTSP 주소입니다. (지정 시 `rtsp_address.txt`를 무시하고 이 옵션을 우선 적용합니다)
*   `--model`: NPU 추론에 사용할 `.dxnn` 모델 파일 경로입니다.
*   `--num-channels N`: 동시에 N개의 채널 파이프라인을 병렬로 실행하여 다중 채널 부하 테스트를 진행합니다. (기본값: 1)
*   `--interval N`: N프레임 당 1프레임만 NPU 추론을 수행하고 나머지는 하드웨어 디코딩/인코딩 병목을 분석합니다. (예: 3)
*   `--duration N`: 지정한 시간(초) 동안 벤치마크를 수행한 후 자동으로 종료하고 리포트를 생성합니다.
*   `--output DIR`: 벤치마크 결과 리포트를 저장할 디렉토리를 지정합니다. (기본값: `result/`)

### 4. HW vs SW 성능 비교 벤치마크 (소프트웨어 강제 지정)

하드웨어 VPU 기능을 끄고 순수 CPU 자원(SW)만으로 디코딩 혹은 인코딩을 수행하도록 강제할 수 있습니다. 정상적인 성능 테스트를 위해서는 사용을 지양합니다.

```bash
# SW 디코더만 강제로 사용 (인코더는 HW)
sudo python3 run_pipeline_benchmark.py --decoder sw --duration 60

# SW 인코더만 강제로 사용 (디코더는 HW)
sudo python3 run_pipeline_benchmark.py --encoder sw --duration 60

# 디코딩, 인코딩 모두 SW(CPU) 사용
sudo python3 run_pipeline_benchmark.py --decoder sw --encoder sw --duration 60
```

### 5. YOLOv8 Pose 모델 8채널 FHD 벤치마크 실행 (테스트 비디오 사용)

YOLOv8 Pose 모델을 사용하여 FHD 해상도의 8채널 비디오 파이프라인 성능을 테스트하는 명령어입니다. `--test-video` 옵션을 추가하면 로컬에 FHD 해상도의 테스트 비디오(`test_video.mp4`)를 자동으로 생성하여 스트리밍 소스로 활용합니다. (VPU 하드웨어 디코딩이 기본으로 자동 적용됩니다.)

```bash
sudo python3 run_pipeline_benchmark.py \
    --model "model/yolov8l_pose.dxnn" \
    --task pose \
    --num-channels 8 \
    --test-video \
    --duration 60
```

> **⚠️ 테스트 비디오 포맷 주의사항**
> `--test-video` 옵션으로 자동 생성되는 `test_video.mp4`는 반드시 **H.264 YUV420 8비트** 포맷이어야 합니다.
> Rockchip VPU 하드웨어 디코더(`mppvideodec`)는 H.264 High 4:4:4 프로필이나 10비트 YUV를 지원하지 않습니다.
> 만약 기존에 생성된 `test_video.mp4`가 호환되지 않는 포맷이라면, 삭제 후 재실행하면 VPU 호환 포맷으로 자동 재생성됩니다.
> ```bash
> rm -f test_video.mp4  # 기존 비호환 영상 삭제 후 재실행
> ```

---

## 📂 출력 결과 (Output)

실행이 완료되면 (또는 `Ctrl+C`로 중단한 즉시) 터미널에 요약 통계가 출력되며, `result/` 디렉터리에 다음 파일들이 생성됩니다.

*   `gst_benchmark_YYYYMMDD_HHMMSS.md`: Markdown 문법으로 작성된 종합 벤치마크 보고서 (사양, 옵션, FPS, 레이턴시 단계표 포함).
*   `gst_benchmark_YYYYMMDD_HHMMSS.json`: 프로그램이나 스크립트에서 자동 파싱할 수 있는 전체 원시 데이터.
*   `pipeline_ch0_YYYYMMDD_HHMMSS.csv`: 프레임별로 기록된 개별 레이턴시 통계.
