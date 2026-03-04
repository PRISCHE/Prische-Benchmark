#!/usr/bin/env python3
"""
GStreamer Pipeline Benchmark — 크로스플랫폼 파이프라인 성능 측정

GStreamer 기반 파이프라인의 Decode → NPU Infer → PostProcess → OSD → Encode 성능을 측정.
HW/SW 디코더/인코더 비교 벤치마크 지원.

사용법:
  python3 run_pipeline_benchmark.py [옵션]

옵션:
  --input URL       RTSP 입력 (기본: rtsp_address.txt 파일 내용 참조)
  --test-video      RTSP 대신 로컬 FHD 30fps 30초 테스트 영상을 생성하여 사용
  --model PATH      모델 파일 경로 (기본: model/YoloV5N.dxnn)
  --duration N      실행 시간(초) (기본: 30)
  --interval N      추론 간격 — N프레임마다 1회 추론 (기본: 3)
  --decoder hw|sw   디코더 선택 (기본: hw)
  --encoder hw|sw   인코더 선택 (기본: hw)
  --output DIR      결과 저장 디렉터리 (기본: result/)
"""

import os
import sys
import time
import json
import signal
import argparse
import subprocess
import threading
import re
import glob
from datetime import datetime

# ============================================
# 기본 설정
# ============================================

def get_default_rtsp():
    rtsp_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rtsp_address.txt")
    if os.path.exists(rtsp_file):
        with open(rtsp_file, "r") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    return line
    return "rtsp://admin:password@192.168.0.200:554/Streaming/Channels/101"

DEFAULT_INPUT = get_default_rtsp()
DEFAULT_DURATION = 30
DEFAULT_INTERVAL = 3

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULT_DIR = os.path.join(SCRIPT_DIR, "result_all")
TEST_VIDEO_PATH = os.path.join(SCRIPT_DIR, "test_video.mp4")
DEFAULT_MODEL = os.path.join(SCRIPT_DIR, "model", "YoloV5N.dxnn")
PIPELINE_BIN = os.path.join(SCRIPT_DIR, "gst_benchmark_pipeline")

# ANSI 색상
RESET  = "\033[0m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RED    = "\033[91m"
GREEN  = "\033[92m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
WHITE  = "\033[97m"

# 제외할 온도 구역
EXCLUDED_THERMAL_ZONES = {"gpu-thermal"}

# 파이프라인 단계 이름 (GStreamer 버전)
STAGE_NAMES = ["Decode&Preproc", "NPU Infer", "PostProcess", "OSD", "GstEncode", "Frame Total"]

# 모델별 FLOPs (GFLOPs 단위) — TOPS 역산용
MODEL_GFLOPS = {
    "YoloV5N":     3.8,
    "YoloV5S":    16.4,
    "YoloV5M":    49.0,
    "YoloV5L":   109.3,
    "YoloV5X":   205.8,
    "YOLOV5X_2": 205.8,
}


def get_model_gflops(model_name):
    """모델 이름에서 GFLOPs 조회."""
    base = os.path.splitext(model_name)[0]
    if base in MODEL_GFLOPS:
        return MODEL_GFLOPS[base]
    for key, val in MODEL_GFLOPS.items():
        if key.lower() == base.lower():
            return val
    return None


def compute_tops(gflops, inference_frames, elapsed_sec):
    """TOPS = GFLOPs × 추론_FPS / 1000"""
    if elapsed_sec <= 0 or inference_frames <= 0:
        return None
    infer_fps = inference_frames / elapsed_sec
    tops = gflops * infer_fps / 1000.0
    return tops, infer_fps


# ============================================
# 시스템 정보 수집
# ============================================

def get_board_model():
    try:
        with open("/proc/device-tree/model") as f:
            return f.read().strip().rstrip('\x00')
    except OSError:
        return "Unknown"


def get_cpu_info():
    model = "Unknown"
    cores = os.cpu_count() or 0
    try:
        out = subprocess.check_output(["lscpu"], text=True, stderr=subprocess.DEVNULL)
        for line in out.splitlines():
            if "Model name" in line:
                model = line.split(":", 1)[1].strip()
                break
    except Exception:
        pass
    max_mhz = None
    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq") as f:
            max_mhz = int(f.read().strip()) // 1000
    except OSError:
        pass
    return model, cores, max_mhz


def get_ram_info():
    info = {}
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    info[parts[0].rstrip(":")] = int(parts[1])
    except OSError:
        return None, None
    total = info.get("MemTotal", 0) // 1024
    avail = info.get("MemAvailable", 0) // 1024
    return total, avail


def get_soc_temp():
    base = "/sys/class/thermal"
    best = 0.0
    try:
        for entry in sorted(os.listdir(base)):
            if not entry.startswith("thermal_zone"):
                continue
            type_file = os.path.join(base, entry, "type")
            zone_type = entry
            if os.path.exists(type_file):
                with open(type_file) as f:
                    zone_type = f.read().strip()
            if zone_type in EXCLUDED_THERMAL_ZONES:
                continue
            with open(os.path.join(base, entry, "temp")) as f:
                temp = int(f.read().strip()) / 1000.0
            if "soc" in zone_type.lower():
                return temp
            best = max(best, temp)
    except (OSError, ValueError):
        pass
    return best


# ============================================
# CPU 사용률 측정
# ============================================

_prev_cpu_stat = None

def _read_cpu_total():
    try:
        with open("/proc/stat") as f:
            line = f.readline()
        vals = [int(x) for x in line.split()[1:]]
        idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
        total = sum(vals)
        return idle, total
    except (OSError, ValueError):
        return 0, 0


def get_cpu_usage_percent():
    global _prev_cpu_stat
    idle, total = _read_cpu_total()
    if _prev_cpu_stat is None:
        _prev_cpu_stat = (idle, total)
        return None
    prev_idle, prev_total = _prev_cpu_stat
    _prev_cpu_stat = (idle, total)
    d_total = total - prev_total
    d_idle = idle - prev_idle
    if d_total == 0:
        return 0.0
    return max(0.0, min(100.0, (1.0 - d_idle / d_total) * 100.0))


# ============================================
# 시스템 모니터 스레드
# ============================================

MONITOR_INTERVAL = 1.0

class SystemMonitor:
    """별도 스레드에서 CPU/온도/RAM을 주기적으로 기록"""
    def __init__(self, interval=MONITOR_INTERVAL):
        self.interval = interval
        self._running = False
        self._thread = None
        self.cpu_samples = []
        self.temp_samples = []
        self.ram_samples = []

    def start(self):
        self._running = True
        get_cpu_usage_percent()  # 초기 스냅샷
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=3)

    def _loop(self):
        while self._running:
            time.sleep(self.interval)
            if not self._running:
                break
            cpu = get_cpu_usage_percent()
            if cpu is not None:
                self.cpu_samples.append(cpu)
            self.temp_samples.append(get_soc_temp())
            total, avail = get_ram_info()
            if total is not None:
                self.ram_samples.append(total - avail)

    def summary(self):
        def stats(arr):
            if not arr:
                return {"avg": 0, "min": 0, "max": 0, "peak": 0}
            return {
                "avg": sum(arr) / len(arr),
                "min": min(arr),
                "max": max(arr),
                "peak": max(arr),
            }
        return {
            "cpu_percent": stats(self.cpu_samples),
            "soc_temp_c": stats(self.temp_samples),
            "ram_used_mb": stats(self.ram_samples),
            "sample_count": len(self.cpu_samples),
        }


# ============================================
# 로그 파싱 (GStreamer 버전)
# ============================================

def parse_pipeline_log(log_path):
    """
    gst_pipeline이 생성하는 .log 파일을 파싱하여
    각 단계별 통계를 딕셔너리로 반환한다.
    """
    result = {
        "total_frames": 0,
        "inference_frames": 0,
        "elapsed_sec": 0.0,
        "overall_fps": 0.0,
        "ram_current_mb": 0.0,
        "ram_peak_mb": 0.0,
        "decoder_type": "Unknown",
        "encoder_type": "Unknown",
        "stages": {},
    }

    if not os.path.isfile(log_path):
        return result

    with open(log_path) as f:
        content = f.read()

    # 글로벌 통계 파싱
    m = re.search(r'Total Frames:\s*(\d+)', content)
    if m: result["total_frames"] = int(m.group(1))

    m = re.search(r'Inference Frames:\s*(\d+)', content)
    if m: result["inference_frames"] = int(m.group(1))

    m = re.search(r'Elapsed Time:\s*([\d.]+)', content)
    if m: result["elapsed_sec"] = float(m.group(1))

    m = re.search(r'Overall FPS:\s*([\d.]+)', content)
    if m: result["overall_fps"] = float(m.group(1))

    m = re.search(r'RAM Current.*?:\s*([\d.]+)', content)
    if m: result["ram_current_mb"] = float(m.group(1))

    m = re.search(r'RAM Peak.*?:\s*([\d.]+)', content)
    if m: result["ram_peak_mb"] = float(m.group(1))

    m = re.search(r'Decoder:\s*(.+)', content)
    if m: result["decoder_type"] = m.group(1).strip()

    m = re.search(r'Encoder:\s*(.+)', content)
    if m: result["encoder_type"] = m.group(1).strip()

    # 단계별 통계 파싱
    stage_pattern = re.compile(
        r'\[(\w[\w\s]*?)\]\s*\n'
        r'\s*Count:\s*(\d+)\s*\n'
        r'\s*Mean:\s*([\d.]+)\s*ms.*?\n'
        r'\s*SD:\s*([\d.]+)\s*ms.*?\n'
        r'.*?\n'  # CV 줄
        r'\s*Min:\s*([\d.]+)\s*ms.*?\n'
        r'\s*Max:\s*([\d.]+)\s*ms',
        re.MULTILINE
    )

    for m in stage_pattern.finditer(content):
        name = m.group(1).strip()
        result["stages"][name] = {
            "count": int(m.group(2)),
            "mean_ms": float(m.group(3)),
            "sd_ms": float(m.group(4)),
            "min_ms": float(m.group(5)),
            "max_ms": float(m.group(6)),
        }

    return result


# ============================================
# 결과 출력
# ============================================

def print_pipeline_results(parsed, sys_stats, board_model, model_name, interval, decoder_mode, encoder_mode):
    """파이프라인 벤치마크 결과를 콘솔에 출력"""
    print(f"\n{'='*70}")
    print(f"  {BOLD}{CYAN}📊 GStreamer Pipeline Benchmark 결과 — {board_model}{RESET}")
    print(f"{'='*70}")
    print(f"  모델: {model_name}  |  추론 간격: {interval}프레임")
    print(f"  디코더: {parsed.get('decoder_type', decoder_mode)}  |  인코더: {parsed.get('encoder_type', encoder_mode)}")
    print(f"  총 프레임: {parsed['total_frames']}  |  추론 프레임: {parsed['inference_frames']}")
    print(f"  경과 시간: {parsed['elapsed_sec']:.1f}초  |  FPS: {GREEN}{BOLD}{parsed['overall_fps']:.2f}{RESET}")
    print(f"  RAM: {parsed['ram_current_mb']:.1f}MB (현재)  /  {parsed['ram_peak_mb']:.1f}MB (최대)")

    # TOPS 역산
    gflops = get_model_gflops(model_name)
    if gflops is not None:
        tops_result = compute_tops(gflops, parsed['inference_frames'], parsed['elapsed_sec'])
        if tops_result:
            tops, infer_fps = tops_result
            print(f"\n  {BOLD}{YELLOW}⚡ NPU 성능 추정{RESET}")
            print(f"  모델 FLOPs   : {gflops} GFLOPs")
            print(f"  추론 FPS     : {infer_fps:.2f}")
            print(f"  추정 TOPS    : {BOLD}{tops:.4f} TOPS{RESET}")
    else:
        print(f"\n  {DIM}⚠️  TOPS 계산 불가: '{model_name}'의 FLOPs 정보 없음{RESET}")
    print()

    # 단계별 테이블
    print(f"  {'단계':<15s} │ {'Count':>6s} │ {'Avg(ms)':>9s} │ {'SD(ms)':>8s} │ {'Min(ms)':>8s} │ {'Max(ms)':>8s}")
    print(f"  {'─'*15}─┼─{'─'*6}─┼─{'─'*9}─┼─{'─'*8}─┼─{'─'*8}─┼─{'─'*8}")

    for stage in STAGE_NAMES:
        s = parsed["stages"].get(stage, None)
        if s is None:
            continue
        prefix = BOLD if stage == "Frame Total" else ""
        suffix = RESET if stage == "Frame Total" else ""
        print(f"  {prefix}{stage:<15s}{suffix} │ {s['count']:>6d} │ {s['mean_ms']:>9.2f} │ {s['sd_ms']:>8.2f} │ {s['min_ms']:>8.2f} │ {s['max_ms']:>8.2f}")

    print(f"  {'─'*70}")

    # 시스템 리소스
    print(f"\n  {DIM}시스템 리소스 (벤치마크 중 모니터링){RESET}")
    cpu = sys_stats["cpu_percent"]
    temp = sys_stats["soc_temp_c"]
    ram = sys_stats["ram_used_mb"]
    print(f"  CPU 사용률 : avg={cpu['avg']:.1f}%  peak={cpu['peak']:.1f}%")
    print(f"  SoC 온도   : avg={temp['avg']:.1f}°C  peak={temp['peak']:.1f}°C")
    print(f"  RAM 사용량 : avg={ram['avg']:.0f}MB  peak={ram['peak']:.0f}MB")
    print()


# ============================================
# 결과 저장 (JSON + Markdown)
# ============================================

def save_pipeline_results(parsed, sys_stats, board_model, model_name, interval,
                          decoder_mode, encoder_mode, output_dir):
    """결과를 JSON과 Markdown으로 저장"""
    os.makedirs(output_dir, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    cpu_model, cpu_cores, cpu_max_mhz = get_cpu_info()
    ram_total, _ = get_ram_info()

    # TOPS 계산
    gflops = get_model_gflops(model_name)
    tops_data = {}
    if gflops is not None:
        tops_result = compute_tops(gflops, parsed['inference_frames'], parsed['elapsed_sec'])
        if tops_result:
            tops, infer_fps = tops_result
            tops_data = {
                "model_gflops": gflops,
                "inference_fps": round(infer_fps, 2),
                "estimated_tops": round(tops, 4),
            }

    combined = {
        "timestamp": timestamp,
        "board_model": board_model,
        "pipeline_type": "GStreamer",
        "decoder_mode": decoder_mode,
        "encoder_mode": encoder_mode,
        "decoder_type": parsed.get("decoder_type", decoder_mode),
        "encoder_type": parsed.get("encoder_type", encoder_mode),
        "system_info": {
            "cpu_model": cpu_model,
            "cpu_cores": cpu_cores,
            "cpu_max_mhz": cpu_max_mhz,
            "ram_total_mb": ram_total,
        },
        "benchmark_config": {
            "model": model_name,
            "interval": interval,
        },
        "npu_tops": tops_data,
        "pipeline": parsed,
        "system_monitor": sys_stats,
    }

    # JSON
    json_path = os.path.join(output_dir, f"gst_benchmark_{timestamp}.json")
    with open(json_path, "w") as f:
        json.dump(combined, f, indent=2, ensure_ascii=False)
    print(f"  📁 JSON: {json_path}")

    # Markdown
    md_path = os.path.join(output_dir, f"gst_benchmark_{timestamp}.md")
    with open(md_path, "w") as f:
        f.write(f"# GStreamer Pipeline Benchmark — {board_model}\n\n")
        f.write(f"**날짜**: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")

        f.write(f"## 설정\n\n")
        f.write(f"| 항목 | 값 |\n|------|----|")
        f.write(f"\n| 보드 | {board_model} |")
        f.write(f"\n| CPU | {cpu_model} ({cpu_cores}코어) |")
        if cpu_max_mhz:
            f.write(f"\n| CPU 최대 주파수 | {cpu_max_mhz} MHz |")
        if ram_total:
            f.write(f"\n| RAM | {ram_total} MB |")
        f.write(f"\n| 파이프라인 | GStreamer |")
        f.write(f"\n| 디코더 | {parsed.get('decoder_type', decoder_mode)} |")
        f.write(f"\n| 인코더 | {parsed.get('encoder_type', encoder_mode)} |")
        f.write(f"\n| 모델 | {model_name} |")
        f.write(f"\n| 추론 간격 | {interval} 프레임 |")
        f.write(f"\n| 총 프레임 | {parsed['total_frames']} |")
        f.write(f"\n| 추론 프레임 | {parsed['inference_frames']} |")
        f.write(f"\n| FPS | **{parsed['overall_fps']:.2f}** |")
        f.write(f"\n| 경과 시간 | {parsed['elapsed_sec']:.1f}초 |")
        if tops_data:
            f.write(f"\n| 추정 TOPS | **{tops_data['estimated_tops']:.4f}** |")
            f.write(f"\n| 추론 FPS | {tops_data['inference_fps']:.2f} |")
            f.write(f"\n| 모델 FLOPs | {tops_data['model_gflops']} GFLOPs |")
        f.write(f"\n\n")

        f.write(f"## 단계별 성능\n\n")
        f.write(f"| 단계 | Count | Avg(ms) | SD(ms) | Min(ms) | Max(ms) |\n")
        f.write(f"|------|-------|---------|--------|---------|--------|\n")
        for stage in STAGE_NAMES:
            s = parsed["stages"].get(stage, None)
            if s is None:
                continue
            f.write(f"| {stage} | {s['count']} | {s['mean_ms']:.2f} | {s['sd_ms']:.2f} | {s['min_ms']:.2f} | {s['max_ms']:.2f} |\n")

        cpu = sys_stats["cpu_percent"]
        temp = sys_stats["soc_temp_c"]
        ram = sys_stats["ram_used_mb"]
        f.write(f"\n## 시스템 리소스\n\n")
        f.write(f"| 항목 | Avg | Peak |\n|------|-----|------|\n")
        f.write(f"| CPU 사용률 | {cpu['avg']:.1f}% | {cpu['peak']:.1f}% |\n")
        f.write(f"| SoC 온도 | {temp['avg']:.1f}°C | {temp['peak']:.1f}°C |\n")
        f.write(f"| RAM 사용량 | {ram['avg']:.0f}MB | {ram['peak']:.0f}MB |\n")

    print(f"  📁 Markdown: {md_path}")
    return json_path, md_path


# ============================================
# 메인 실행
# ============================================

def main():
    parser = argparse.ArgumentParser(
        description="GStreamer Pipeline Benchmark — 크로스플랫폼 파이프라인 성능 측정",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--input", default=DEFAULT_INPUT,
                        help=f"RTSP 입력 소스")
    parser.add_argument("--model", default=DEFAULT_MODEL,
                        help=f"모델 파일 경로")
    parser.add_argument("--test-video", action="store_true",
                        help="RTSP 입력 대신 로컬 FHD 30fps 60초 테스트 영상을 자동 생성 및 사용")
    parser.add_argument("--duration", type=int, default=DEFAULT_DURATION,
                        help=f"실행 시간(초) [기본: {DEFAULT_DURATION}]")
    parser.add_argument("--interval", type=int, default=DEFAULT_INTERVAL,
                        help=f"추론 간격 [기본: {DEFAULT_INTERVAL}]")
    parser.add_argument("--num-channels", type=int, default=1,
                        help=f"동시 실행할 채널 수 [기본: 1]")
    parser.add_argument("--decoder", default="hw", choices=["hw", "sw"],
                        help="디코더 선택: hw(기본, VPU) / sw(CPU 비교용)")
    parser.add_argument("--encoder", default="hw", choices=["hw", "sw"],
                        help="인코더 선택: hw(기본, VPU) / sw(CPU 비교용)")
    parser.add_argument("--stream", action="store_true",
                        help="웹에서 스트리밍을 확인하기 위해 MediaMTX와 웹서버 구동 (성능에 영향을 줄 수 있음)")
    parser.add_argument("--output", default=RESULT_DIR,
                        help=f"결과 저장 디렉터리")
    args = parser.parse_args()

    # ── 사전 검증 ──────────────────────────────────
    if not os.path.isfile(PIPELINE_BIN):
        print(f"  ❌ gst_pipeline 바이너리를 찾을 수 없습니다: {PIPELINE_BIN}")
        print(f"     먼저 빌드하세요: bash build.sh")
        return 1

    if not os.path.isfile(args.model):
        print(f"  ❌ 모델 파일을 찾을 수 없습니다: {args.model}")
        return 1

    # 테스트 비디오 옵션 처리
    if args.test_video:
        if not os.path.isfile(TEST_VIDEO_PATH):
            print(f"  🎬 로컬 테스트 영상이 없습니다. FHD 30fps 60초 영상을 생성합니다...")
            generate_cmd = [
                "gst-launch-1.0", "-e", "videotestsrc", "num-buffers=1800",
                "!", "video/x-raw,width=1920,height=1080,framerate=30/1",
                "!", "v4l2h264enc", "!", "h264parse", "!", "mp4mux",
                "!", "filesink", f"location={TEST_VIDEO_PATH}"
            ]
            try:
                # v4l2h264enc (HW) 인코더 우선 시도
                subprocess.run(generate_cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                print(f"  ✅ 테스트 영상 생성 완료: {TEST_VIDEO_PATH}")
            except Exception:
                print(f"  ⚠️ HW 인코더 기반 영상 생성 실패. SW 인코더(x264enc)로 재시도합니다...")
                generate_cmd_sw = [
                    "gst-launch-1.0", "-e", "videotestsrc", "num-buffers=1800",
                    "!", "video/x-raw,width=1920,height=1080,framerate=30/1",
                    "!", "x264enc", "bitrate=2000", "!", "h264parse", "!", "mp4mux",
                    "!", "filesink", f"location={TEST_VIDEO_PATH}"
                ]
                try:
                    subprocess.run(generate_cmd_sw, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    print(f"  ✅ 테스트 영상 생성 완료: {TEST_VIDEO_PATH}")
                except Exception as e:
                    print(f"  ❌ 테스트 영상 생성 실패: {e}")
                    return 1
        args.input = f"file://{TEST_VIDEO_PATH}"
        print(f"  🎥 테스트 영상 (60초) 소스를 사용합니다: {args.input}")

    # 누적 저장을 위해 이전 로그를 지우지 않습니다.
    # 이번 실행을 위한 고유의 서브 폴더를 생성합니다.
    run_timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = os.path.join(args.output, f"run_{run_timestamp}")
    os.makedirs(run_dir, exist_ok=True)

    board_model = get_board_model()
    cpu_model, cpu_cores, cpu_max_mhz = get_cpu_info()
    ram_total, ram_avail = get_ram_info()
    model_name = os.path.basename(args.model)

    # ── 헤더 출력 ──────────────────────────────────
    decoder_label = f"HW (VPU)" if args.decoder == "hw" else f"SW (CPU) ⚠️"
    encoder_label = f"HW (VPU)" if args.encoder == "hw" else f"SW (CPU) ⚠️"

    print(f"\n{'='*60}")
    print(f"  {BOLD}{CYAN}GStreamer Pipeline Benchmark{RESET}")
    print(f"  {DIM}GstDecode → NPU Infer → PostProcess → OSD → GstEncode{RESET}")
    print(f"{'='*60}")
    print(f"\n  {DIM}보드     :{RESET} {board_model}")
    print(f"  {DIM}CPU      :{RESET} {cpu_model} ({cpu_cores}코어)")
    if cpu_max_mhz:
        print(f"  {DIM}CPU Max  :{RESET} {cpu_max_mhz} MHz")
    if ram_total:
        print(f"  {DIM}RAM      :{RESET} {ram_total}MB (사용 가능: {ram_avail}MB)")
    print(f"  {DIM}디코더   :{RESET} {decoder_label}")
    print(f"  {DIM}인코더   :{RESET} {encoder_label}")
    print(f"  {DIM}모델     :{RESET} {model_name}")
    print(f"  {DIM}입력     :{RESET} {args.input}")
    print(f"  {DIM}시간     :{RESET} {args.duration}초")
    print(f"  {DIM}추론 간격:{RESET} {args.interval}프레임마다 1회")
    print(f"  {DIM}채널 수  :{RESET} {args.num_channels}채널 동시 실행")
    print(f"  {DIM}현재 온도:{RESET} {get_soc_temp():.1f}°C")
    print()

    # ── 파이프라인 실행 ────────────────────────────
    procs = []
    stream_procs = []
    
    if args.stream:
        print(f"  {BOLD}{GREEN}▶ 스트리밍 서비스 시작 (MediaMTX & WebServer){RESET}")
        try:
            mtx_log = open(os.path.join(SCRIPT_DIR, "mediamtx.log"), "w")
            p_mtx = subprocess.Popen(["./mediamtx"], cwd=SCRIPT_DIR, stdout=mtx_log, stderr=subprocess.STDOUT, preexec_fn=os.setsid)
            stream_procs.append(p_mtx)
            
            web_log = open(os.path.join(SCRIPT_DIR, "webserver.log"), "w")
            p_web = subprocess.Popen([sys.executable, "webserver.py"], cwd=SCRIPT_DIR, stdout=web_log, stderr=subprocess.STDOUT, preexec_fn=os.setsid)
            stream_procs.append(p_web)
            
            # Allow services to start
            time.sleep(2)
        except Exception as e:
            print(f"  ❌ 스트리밍 서비스 시작 실패: {e}")
            args.stream = False
            
    print(f"  {BOLD}{GREEN}▶ 파이프라인 시작 ({args.num_channels}채널 동시 실행){RESET}")
    if args.stream:
        print(f"  stdout → FFmpeg → RTSP (MediaMTX)")
    else:
        print(f"  stdout → /dev/null (H264 스트림 폐기)")
    print(f"  {args.duration}초 후 SIGINT로 정상 종료 (로그 자동 저장)")
    print()

    # 시스템 모니터 시작
    monitor = SystemMonitor()
    monitor.start()

    start_time = time.time()
    stderr_lines = []

    try:
        for i in range(args.num_channels):
            output_prefix = os.path.join(run_dir, f"pipeline_ch{i}_{run_timestamp}")
            cmd = [
                PIPELINE_BIN,
                "--input", args.input,
                "--model", args.model,
                "--interval", str(args.interval),
                "--channel-id", str(i),
                "--decoder", args.decoder,
                "--encoder", args.encoder,
                "--output-prefix", output_prefix,
            ]
            
            if args.stream:
                rtsp_out = f"rtsp://localhost:8554/live{i}"
                ffmpeg_cmd = [
                    "ffmpeg", "-hide_banner", "-nostdin",
                    "-fflags", "nobuffer",
                    "-flags", "low_delay",
                    "-f", "h264",
                    "-i", "pipe:0",
                    "-c:v", "copy",
                    "-f", "rtsp",
                    "-rtsp_transport", "tcp",
                    rtsp_out
                ]
                
                # gst_pipeline stdout pipes into ffmpeg stdin
                p_pipeline = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE if i == 0 else subprocess.DEVNULL,
                    cwd=SCRIPT_DIR,
                    preexec_fn=os.setsid,
                )
                
                p_ffmpeg = subprocess.Popen(
                    ffmpeg_cmd,
                    stdin=p_pipeline.stdout,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    cwd=SCRIPT_DIR,
                    preexec_fn=os.setsid,
                )
                
                p_pipeline.stdout.close() # Allow p_pipeline to receive a SIGPIPE if p_ffmpeg exits
                procs.append(p_pipeline)
                stream_procs.append(p_ffmpeg)
            else:
                p = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,   # H264 출력 폐기
                    stderr=subprocess.PIPE if i == 0 else subprocess.DEVNULL,  # 채널 0만 출력 읽기
                    cwd=SCRIPT_DIR,
                    preexec_fn=os.setsid,
                )
                procs.append(p)

        # stderr 실시간 표시 (채널 0만)
        def _read_stderr():
            for raw in procs[0].stderr:
                line = raw.decode("utf-8", errors="replace").rstrip("\n")
                stderr_lines.append(line)
                if line.startswith("║") or line.startswith("╔") or line.startswith("╠") or line.startswith("╚"):
                    print(f"  {line}", flush=True)
                elif line.startswith("✅") or line.startswith("📦") or line.startswith("🔧") or line.startswith("🔗"):
                    print(f"  {line}", flush=True)
                elif line.startswith("🟢") or line.startswith("🔴") or line.startswith("🔵"):
                    print(f"  {line}", flush=True)
                elif "⚠️" in line:
                    print(f"  {line}", flush=True)

        reader = threading.Thread(target=_read_stderr, daemon=True)
        reader.start()

        # 지정 시간 대기
        try:
            for p in procs:
                p.wait(timeout=max(0, args.duration - (time.time() - start_time)))
            print(f"\n  ⚠️  모든 파이프라인이 {args.duration}초 전에 종료됨")
        except subprocess.TimeoutExpired:
            print(f"\n  ⏱️  {args.duration}초 경과 → SIGINT 전송 (정상 종료 대기)...")
            for p in procs:
                os.killpg(os.getpgid(p.pid), signal.SIGINT)
            try:
                for p in procs:
                    p.wait(timeout=10)
                print(f"  ✅ 모든 파이프라인 정상 종료")
            except subprocess.TimeoutExpired:
                print(f"  ⚠️  10초 내 종료되지 않아 강제 종료")
                for p in procs:
                    os.killpg(os.getpgid(p.pid), signal.SIGKILL)
                    p.wait()

        reader.join(timeout=2)

    except KeyboardInterrupt:
        print("\n  ✋ 사용자 중단 (Ctrl+C). 프로세스 종료 중...")
        monitor.stop()
    except Exception as e:
        print(f"  ❌ 파이프라인 실행 실패: {e}")
        monitor.stop()
    finally:
        # 정리: 모든 파이프라인 및 백그라운드 프로세스 종료 (MediaMTX, WebServer, FFmpeg 등)
        print(f"\n  🛑 모든 파이프라인 및 스트리밍 서비스 종료 중...")
        all_procs = procs + stream_procs
        
        for p in all_procs:
            if p.poll() is None:
                try:
                    os.killpg(os.getpgid(p.pid), signal.SIGINT)
                except:
                    pass
        
        time.sleep(1)
        for p in all_procs:
            if p.poll() is None:
                try:
                    os.killpg(os.getpgid(p.pid), signal.SIGKILL)
                except:
                    pass

    elapsed = time.time() - start_time
    # monitor is already stopped in finally or except block
    sys_stats = monitor.summary()

    print(f"\n  경과 시간: {elapsed:.1f}초")
    print()

    # ── .log 파일 찾기 및 파싱 (현재 실행된 서브디렉토리 내부) ──
    log_files = sorted(glob.glob(os.path.join(run_dir, f"pipeline_ch*_{run_timestamp}.log")))

    if not log_files:
        print(f"  ❌ 파이프라인 로그 파일이 생성되지 않았습니다.")
        print(f"     확인 경로: {run_dir}")
        if 'stderr_lines' in locals() and stderr_lines:
            print(f"\n  --- stderr 마지막 20줄 ---")
            for line in stderr_lines[-20:]:
                print(f"  {line}")
        return 1

    valid_parsed = []
    recent_logs = log_files[-args.num_channels:]
    
    for lf in recent_logs:
        p = parse_pipeline_log(lf)
        if p["stages"]:
            valid_parsed.append(p)

    if not valid_parsed:
        print(f"  ❌ 로그 파싱 실패 — 단계별 데이터가 없습니다.")
        return 1

    print(f"  📄 파싱된 로그 파일 수: {len(valid_parsed)}개")

    # Aggregate parsed results
    parsed = {
        "total_frames": sum(p["total_frames"] for p in valid_parsed),
        "inference_frames": sum(p["inference_frames"] for p in valid_parsed),
        "elapsed_sec": valid_parsed[0]["elapsed_sec"],
        "overall_fps": sum(p["overall_fps"] for p in valid_parsed),
        "ram_current_mb": max(p["ram_current_mb"] for p in valid_parsed),
        "ram_peak_mb": max(p["ram_peak_mb"] for p in valid_parsed),
        "decoder_type": valid_parsed[0].get("decoder_type", "Unknown"),
        "encoder_type": valid_parsed[0].get("encoder_type", "Unknown"),
        "stages": {}
    }

    for stage in STAGE_NAMES:
        counts = [p["stages"][stage]["count"] for p in valid_parsed if stage in p["stages"]]
        if not counts: continue
        parsed["stages"][stage] = {
            "count": sum(counts) // len(counts),
            "mean_ms": sum(p["stages"][stage]["mean_ms"] for p in valid_parsed if stage in p["stages"]) / len(counts),
            "sd_ms": sum(p["stages"][stage]["sd_ms"] for p in valid_parsed if stage in p["stages"]) / len(counts),
            "min_ms": min(p["stages"][stage]["min_ms"] for p in valid_parsed if stage in p["stages"]),
            "max_ms": max(p["stages"][stage]["max_ms"] for p in valid_parsed if stage in p["stages"]),
        }

    # ── 결과 출력 ──────────────────────────────────
    print_pipeline_results(parsed, sys_stats, board_model, model_name, args.interval,
                           args.decoder, args.encoder)

    # ── 결과 저장 ──────────────────────────────────
    print(f"  📊 결과 저장 중...")
    json_path, md_path = save_pipeline_results(
        parsed, sys_stats, board_model, model_name, args.interval,
        args.decoder, args.encoder, run_dir
    )

    # ── 권한 복구 (sudo 실행 시 root 소유자가 되는 현상 방지) ───
    if os.environ.get("SUDO_USER"):
        sudo_user = os.environ.get("SUDO_USER")
        os.system(f"chown -R {sudo_user}:{sudo_user} {args.output}")

    print(f"\n  ✅ GStreamer 파이프라인 벤치마크 완료!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
