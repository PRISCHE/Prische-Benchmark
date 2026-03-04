#!/bin/bash
# ============================================
# 빌드 스크립트 — GStreamer 파이프라인 벤치마크
# ============================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIPELINE_DIR="$SCRIPT_DIR/pipeline_gst"
BUILD_DIR="$PIPELINE_DIR/build"

echo "============================================"
echo "  GStreamer Pipeline Benchmark — 빌드"
echo "============================================"
echo ""

# ── GStreamer 의존성 확인 ─────────────────────
echo "의존성 확인 중..."
if ! pkg-config --exists gstreamer-1.0; then
    echo "  ❌ GStreamer 개발 패키지가 설치되어 있지 않습니다."
    echo "     sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev"
    exit 1
fi

GST_VER=$(pkg-config --modversion gstreamer-1.0)
echo "  ✅ GStreamer: ${GST_VER}"

# HW 플러그인 확인 (빌드에는 불필요하지만 정보성 출력)
if gst-inspect-1.0 mppvideodec >/dev/null 2>&1; then
    echo "  ✅ Rockchip MPP 플러그인: 설치됨"
else
    echo "  ⚠️  Rockchip MPP 플러그인: 미설치 (SW 디코더만 사용 가능)"
fi
echo ""

# ── gst_benchmark_pipeline 빌드 ─────────────────────────
echo "gst_benchmark_pipeline 빌드 중..."

# 이전 빌드 캐시 정리 (CMake 캐시 충돌 방지)
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 빌드 바이너리를 루트로 복사
cp "$BUILD_DIR/gst_benchmark_pipeline" "$SCRIPT_DIR/gst_benchmark_pipeline"
echo "  ✅ gst_benchmark_pipeline 빌드 완료"
echo ""


# ── 완료 ─────────────────────────────────────────
echo "============================================"
echo "  빌드 완료!"
echo "============================================"
echo ""
ls -lh "$SCRIPT_DIR/gst_benchmark_pipeline"
echo ""
echo "사용법:"
echo "  # HW 디코더 (기본, VPU 사용)"
echo "  python3 run_pipeline_benchmark.py --duration 60"
echo ""
echo "  # SW 디코더 (CPU 비교 벤치마크)"
echo "  python3 run_pipeline_benchmark.py --decoder sw --duration 60"
