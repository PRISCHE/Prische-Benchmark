# Gstbenchmark 통합 아키텍처 및 파이프라인 구조

본 문서는 Gstbenchmark 프로젝트의 소스코드를 유지보수하거나 구조를 파악하기 위한 **내부 개발 및 아키텍처 문서**입니다. 외부 사용자 가이드는 `README.md`를 참고해 주세요.

---

## 📂 프로젝트 디렉토리 구조 (Directory Structure)

```text
Gstbenchmark/
├── README.md                     # 외부에 공개할 사용설명서 (입력, 옵션, 결과 안내 위주)
├── architecture.md               # [현재 파일] 내부 아키텍처 및 파이프라인 동작 원리 명세서
├── troubleshooting_debian12.md   # [내부 문서] 데비안 등 특정 OS 계열 VPU 에러 트러블슈팅
├── build.sh                      # 초기 1-click CMake C++ 빌드 쉘 스크립트
├── gst_benchmark_pipeline        # [빌드 결과물] 순수 C++ GStreamer 실행 바이너리
├── pipeline_gst/
│   ├── CMakeLists.txt            # CMake 빌드 설정 파일
│   └── main.cpp                  # 핵심 GStreamer C++ 파이프라인 (객체 인식 / 자세 추정 분기 포함)
├── model/
│   └── YoloV5N.dxnn              # DX-RT Model 경로 (NPU 추론을 위한 모델 가중치 파일)
├── result_all/                   # Python 벤치마크 래퍼가 자동 생성하는 타임스탬프 기반 리포트 디렉토리
└── run_pipeline_benchmark.py     # Python 모니터링 & 파이프라인 분기 래퍼 (자원 상태 로깅 및 바이너리 호출)
```

---

## 🔄 비동기 멀티스레드 파이프라인 아키텍처 (Async Multi-Threaded Architecture)

과거 영상 처리와 NPU 추론이 동기적으로(Synchronous) 결합되어 NPU 처리 지연력이 영상 전체 FPS를 낮추는 병목 현상을 해결하기 위해, 메인 프로세스를 **2개의 독립적인 비동기 스레드**로 완전히 분리하였습니다.

### Supervisor (Main Thread)
- GStreamer 디코드 파이프라인을 생성 및 Pre-roll (READY → PAUSED → PLAYING)
- Thread A (디스플레이 분기) 와 Thread B (추론 분기)를 생성
- SIGINT(Ctrl+C) 신호를 수신하여 하위 스레드들을 안전하게 종료(Join) 및 메모리 정리

### Thread A: 디스플레이 및 인코딩 (Display & Encoding Pipeline)
- **역할**: 카메라 입력 속도(예: 30 FPS)에 맞춰 끊김 없이 영상을 화면에 송출(또는 인코딩)
- **작동**: 
  1. `sink_full` 앱싱크에서 NV12 포맷의 원본 해상도 프레임을 수신
  2. 공유 Mutex 잠금을 통해 Thread B가 갱신한 **가장 최신 바운딩 박스 복사본(g_latest_boxes)**을 가져옴
  3. CPU 개입 없이 **NV12 버퍼의 Y, UV 플레인**에 직접 OSD 사각형을 드로잉 (`draw_rect_nv12`)
  4. 수정된 NV12 프레임을 하드웨어 인코더(`mpph264enc`)로 전달하여 스트리밍

### Thread B: NPU 비동기 추론 (Asynchronous Inference)
- **역할**: 설정된 추론 주기(`--interval`, 예: 5프레임당 1회)에 따라 백그라운드에서 NPU 연산
- **동작**:
  1. `sink_infer` 앱싱크에서 추론용 해상도(640x640)로 작아진 RGB 프레임 수신
  2. DX-RT SDK의 `RunAsync()` API를 호출하여 논블로킹(Non-blocking)으로 NPU에 작업 전달
  3. `Wait()` API로 NPU 연산을 대기한 후 `--task` 파라미터 값에 따라 Post-process 분기 진행
     - `det` (객체 인식): `[1, 25200, 85]` 텐서를 파싱하여 Bounding Box 도출
     - `pose` (자세 추정): `[1, 56, 8400]` 텐서를 파싱하여 17개의 Keypoint 추출 대상 도출
  4. 도출된 `YoloBox` 배열을 메인 Mutex `g_box_mutex`를 걸고 전역 변수 `g_latest_boxes`에 덮어쓰기

---

## 🎨 OSD 멀티-태스킹 아키텍처 (Zero-Copy 기반 분기)

**분기형(Branched) OSD 엔진**이 구축되어 있어, NPU 연산 결과에 따라 화면에 그리는 방식을 동적으로 변경합니다. `DxPostprocess` 와 같은 외부 GStreamer 엘리먼트에 의존하지 않고 순수 C++ 픽셀 연산을 수행하여 CPU 레이턴시를 최소화합니다.

### 지원하는 NPU Task와 렌더링 방식
1. **Object Detection (`--task det`)**:
   - 추출된 박스 좌표(x1, y1, x2, y2)를 기반으로 `draw_rect_nv12` 함수를 호출해 사각형 경계를 Y/UV 플레인에 직접 색칠(BT.601 YUV 색공간 변환).
2. **Pose Estimation (`--task pose`)**:
   - `post_process_yolov8_pose`에서 가져온 각각 17개의 키포인트(Keypoint)를 기반으로:
   - **`draw_line_nv12`**: 브레즌햄 선 긋기(Bresenham's algorithm) 변형 알고리즘을 사용하여 팔, 다리 등 뼈대를 이음새에 맞게 그림.
   - **`draw_point_nv12`**: 각 관절 포인트에 사각형 박스를 변형하여 포인트 마커 추가.

---

## 🔀 GStreamer 파이프라인 데이터 흐름도 및 최적화

이 프로젝트는 **CPU 사용량을 극단적으로 낮추는 NV12 패스스루 아키텍처**를 적용했습니다.

### 1단계: Decode (VPU 하드웨어 디코딩)
- RTSP / Video 소스 → `mppvideodec`
- **NV12 네이티브 포맷**을 그대로 출력 (과거처럼 RGB 변환을 위한 CPU 자원 소모 배제).

### 2단계: Tee (두 갈래 분기)
디코드 직후 `tee` 엘리먼트를 통해 파이프라인이 2가지 목적으로 나뉩니다.
- **분기 1 (Display/Encode)**: 
  - 큐 사이즈 2. `sink_full`로 입력. 
  - NV12 포맷 유지 (CPU 변환 0%)
- **분기 2 (Inference)**: 
  - 큐 사이즈 1. `sink_infer`로 입력.
  - 이 분기만 NPU를 위해 `videoconvert` (RGB 변환) 및 `videoscale` (640x640) 수행.
  - **Pad Probe Throttling**: NPU 큐가 비어있지 않거나 interval에 해당하지 않는 프레임은 `videoscale` 이전단(Probe)에서 원천 Drop시켜 CPU 부하 방어.

### 3단계: OSD (Zero-Copy 기반 NV12 YUV 드로잉)
- 추출된 Bounding Box를 원본 영상에 오버레이 합니다.
- **최적화**: RGB 변환 없이 GstBuffer를 맵핑한 후, BT.601 색공간 수식을 활용하여 Y 플레인과 UV 플레인에 직접 바운딩 박스를 덮어씁니다 (`draw_rect_nv12`). 이로 인해 GPU(RGA)나 CPU 병목이 사라집니다.

### 4단계: Encode & Output (VPU 하드웨어 인코딩)
- `appsrc` → `mpph264enc` → `rtph264pay` (스트리밍 시) 또는 `fdsink` (파이프 출력)
- NV12 포맷이므로 바로 하드웨어 인코더로 유입.

### 5단계: MediaMTX 연동 (Optional Streaming)
- 벤치마크 래퍼가 파이프라인의 stdout을 `ffmpeg`으로 파이핑하여, 로컬에 띄운 `MediaMTX` 서버로 실시간 WebRTC/RTSP 스트리밍 가능.
