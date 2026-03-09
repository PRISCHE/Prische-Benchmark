# Troubleshooting: Wavedyne (Debian 12) Rockchip GStreamer 플러그인 빌드 및 실행 가이드

> **Rockchip 기반 보드(RK3588 등) 중 순정 데비안(Debian 12) 환경**에서 GStreamer 하드웨어 가속(VPU 디코딩/인코딩) 플러그인이 없을 때의 해결 과정을 기록한 문서입니다.
> (예: Ubuntu 24.04가 기본 탑재된 보드에서는 `apt install gstreamer1.0-rockchip` 명령 하나로 해결되지만, 사설 OS나 Debian 12에서는 패키지가 없어 수동 설치가 필요합니다.)

---

## 📅 타임라인 및 발생한 문제들 요약

1. **문제 1: 패키지 미존재 (`gstreamer1.0-rockchip` 설치 불가)**
   - 데비안 12 Bookworm 공식 저장소에는 Rockchip 전용 플러그인이 없음.
2. **문제 2: 단순 라이브러리 파일(`libgstrockchipmpp.so`) 복사 실패**
   - Ubuntu(오렌지파이)에서 컴파일된 플러그인 파일을 그대로 데비안으로 복사했으나, **GLIBC 버전 불일치** (`GLIBC_2.38 not found`) 오류로 로드 실패.
3. **문제 3: 구형 소스코드 호환성 에러**
   - 구형 포크(`geekerlw/gstreamer-rockchip`)를 받아 소스 빌드하려 했으나, 최신 Rockchip MPP API 구조와 맞지 않아 C언어 컴파일 단계에서 문법 에러(`MppEncPrepCfg has no member named...`) 발생.
4. **문제 4: RGA (2D 가속기) 헤더 버전 문제**
   - 최신 소스코드(`BoxCloudIRL`)로 교체 후 빌드했으나, 데비안의 구형 `librga` 헤더가 최신 시스템의 특수 색상 픽셀 포맷(`RK_FORMAT_YCbCr_...`)을 미지원하여 컴파일 에러.
5. **문제 5: H.264 하드웨어 인코더(`mpph264enc`) 목록에서 증발**
   - 플러그인이 초기 로드될 때 하드웨어를 찔러보고 권한이 없으면 H.264 인코더를 리스트에서 빼버리는 로직 확인.
6. **문제 6: VPU 초기화 및 프레임 진행 멈춤 현상(0 FPS)**
   - 실행 시 `mpp_dma_heap` 메모리 접근 권한 에러 발생 (`/dev/mpp_service` 접근 불가).

---

## 🛠️ 최종 해결책 (처음부터 빌드하는 완벽 가이드)

다음 과정을 순서대로 진행하면 데비안 12 환경에서도 VPU 하드웨어 가속이 완벽하게 지원되는 GStreamer 플러그인을 활성화할 수 있습니다.

### 1단계: 선행 작업 (MPP 라이브러리 빌드)
GStreamer 플러그인이 VPU를 쓰기 위해서는 최하단에 **Rockchip MPP 기본 라이브러리**가 셋업되어야 합니다.
> *(기존에 오렌지파이에서 가져온 `librockchip_mpp.so` 파일이 잘못 남아있다면, 미리 `sudo rm /lib/aarch64-linux-gnu/librockchip_mpp.so*` 로 제거해야 충돌이 나지 않습니다.)*

소스코드를 컴파일하여 `/usr/local/lib/` 쪽에 MPP 라이브러리와 헤더를 정상 설치합니다. (본 문서에서는 이 과정이 완료되었다고 가정합니다.)

### 2단계: 필수 패키지 설치
빌드 툴(meson, ninja) 및 GStreamer 개발용 헤더들을 설치합니다.
```bash
sudo apt-get update
sudo apt-get install -y meson ninja-build pkg-config build-essential libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
```

### 3단계: 호환되는 소스코드 다운로드
반드시 최신 MPP API와 meson 빌드 시스템을 완벽하게 지원하는 `BoxCloudIRL` 저장소를 활용해야 합니다.
```bash
cd ~
git clone https://github.com/BoxCloudIRL/gstreamer-rockchip.git
cd gstreamer-rockchip
```

### 4단계: H.264 인코더 숨김 방지 패치 (C 코드 수정)
권한 부족 등으로 처음 `gst-inspect-1.0`을 실행할 때 초기화 테스트에 실패하면 `mpph264enc` 인코더가 리스트에서 감춰집니다. 이를 막기 위해 검사 루틴을 `무조건 통과(return TRUE)`하도록 C 코드를 강제 패치합니다.
```bash
sed -i 's/mpp_set_log_level (MPP_LOG_WARN);/return TRUE;/g' gst/rockchipmpp/gstmppenc.c
```

### 5단계: RGA 헤더 버전 불일치 문제 패치 (C 코드 수정)
구형 `librga`에 없는 특수 픽셀 포맷(10bit YUV, 생소한 RGB 포맷 등)을 에러 라인에서 지워버립니다. 벤치마크에서는 어차피 일반 NV12, RGB 계열만 사용하므로 무방합니다.
```bash
sed -i -e '/YCbCr_422_SP_10B/d' -e '/BGR_565/d' -e '/ARGB_8888/d' -e '/ABGR_8888/d' -e '/XRGB_8888/d' -e '/XBGR_8888/d' gst/rockchipmpp/gstmpp.c
```

### 6단계: 환경 변수 설정 및 빌드
GStreamer가 우리가 먼저 빌드해둔 MPP 기본 패키지를 찾을 수 있도록 경로(`PKG_CONFIG_PATH`)를 지정하고 빌드합니다.
```bash
# 1. 아까 컴파일해둔 경로 인식
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

# 2. 메이슨 설정 및 빌드
meson setup build
sudo ninja -C build install
```

### 7단계: 경로 인식 및 플러그인 로드 테스트
기본적으로 설치는 `/usr/local/lib/` 쪽으로 들어가는데, GStreamer가 쉽게 시스템 전역에서 찾도록 복사해주는 것이 마음 편합니다.
```bash
# 플러그인 복사
sudo cp -r /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/* /usr/lib/aarch64-linux-gnu/gstreamer-1.0/

# GStreamer 캐시 초기화 및 모듈 확인
rm -rf ~/.cache/gstreamer-1.0
gst-inspect-1.0 rockchipmpp
```

마지막 명령어의 출력 결과물 내에 아래 2가지가 보이면 **설치 대성공**입니다:
- `mppvideodec: Rockchip's MPP video decoder` (디코더)
- `mpph264enc: Rockchip Mpp H264 Encoder` (인코더)

---

## 🔴 중요: 실행 시 주의사항 (VPU Permission & 0 FPS 현상)
Wavedyne 사설 Debian OS 커널의 경우 Ubuntu와 달리 하드웨어 비디오 가속기 권한이 까다롭습니다.

일반 계정으로 벤치마크 스크립트 실행 시 화면에 `0 FPS`로 뜨면서 멈춰있고 내부 로그에 아래와 같이 뜬다면,
```
mpp_dma_heap: os_allocator_dma_heap_open open dma heap type 0 system failed!
```
이는 사용자가 `/dev/mpp_service` (VPU) 및 비디오/사진용 DMA 커널 메모리에 접근할 권한이 막혀있다는 뜻입니다.

**⭐️ 해결법:**
파이프라인 또는 프로그램 실행 시 반드시 **`sudo` 권한을 붙여서 루트 자격으로 실행**해야 하드웨어 장치를 원활하게 끌어다 쓸 수 있습니다.
```bash
sudo python3 run_pipeline_benchmark.py --duration 60
```
