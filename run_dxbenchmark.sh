#!/bin/bash
# dxbenchmark 실행 스크립트

BENCHMARK_DIR="/home/prische/benchmark"
RESULT_DIR="$BENCHMARK_DIR/result"
mkdir -p "$RESULT_DIR"
cd "$BENCHMARK_DIR" || exit 1

DXBENCHMARK_PATH="/home/prische/dx-all-suite/dx-runtime/dx_rt/bin/dxbenchmark"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="$RESULT_DIR/benchmark_${TIMESTAMP}.log"

# ============================================
# 기본값 설정
# ============================================

DEFAULT_MODEL_DIR="/home/prische/benchmark/model_single"  # YOLOV5X_2.dxnn (172MB)
DEFAULT_TIME=60
MODEL_GFLOPS=180   # YOLOV5X 기준
TARGET_TOPS=25     # NPU 이론 최대치

# ============================================
# 옵션 파싱 (--parallel 플래그 분리)
# ============================================

HAS_DIR=0; HAS_TIME=0; PARALLEL_MODE=0
PASS_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --parallel) PARALLEL_MODE=1 ;;
        --dir)      HAS_DIR=1;  PASS_ARGS+=("$arg") ;;
        -l|--loops) HAS_TIME=1; PASS_ARGS+=("$arg") ;;
        -t|--time)  HAS_TIME=1; PASS_ARGS+=("$arg") ;;
        *)          PASS_ARGS+=("$arg") ;;
    esac
done

# --verbose 는 항상 포함 (NPU Processing Time / Latency 상세 출력)
EXTRA_ARGS="--verbose"
[ "$HAS_DIR"  -eq 0 ] && EXTRA_ARGS="$EXTRA_ARGS --dir $DEFAULT_MODEL_DIR"
[ "$HAS_TIME" -eq 0 ] && EXTRA_ARGS="$EXTRA_ARGS -t $DEFAULT_TIME"

# ============================================
# 시스템 정보 수집
# ============================================

CPU_MODEL=$(lscpu 2>/dev/null | grep "Model name" | awk -F: '{print $2}' | xargs || echo "Unknown")
CPU_CORES=$(nproc 2>/dev/null || echo "?")
CPU_MAX_MHZ=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null || echo "?")
CPU_MAX_GHZ=$(python3 -c "print(f'{int(\"$CPU_MAX_MHZ\")/1000000:.2f} GHz')" 2>/dev/null || echo "?")
BOARD_MODEL=$(cat /proc/device-tree/model 2>/dev/null || echo "Unknown")
TOTAL_RAM=$(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo "?")
AVAIL_RAM=$(free -h 2>/dev/null | awk '/^Mem:/{print $7}' || echo "?")

# ============================================
# 사전 정보 출력
# ============================================

MODEL_SIZE=$(du -sh "$DEFAULT_MODEL_DIR" 2>/dev/null | cut -f1 || echo "N/A")

echo ""
echo "=========================================="
if [ "$PARALLEL_MODE" -eq 1 ]; then
    echo "  dxbenchmark 실행 【병렬 모드 - NPU 3코어】"
else
    echo "  dxbenchmark 실행"
fi
echo "=========================================="
echo ""
echo "  【 하드웨어 정보 】"
echo "  보드     : $BOARD_MODEL"
echo "  CPU      : $CPU_MODEL ($CPU_CORES코어, max $CPU_MAX_GHZ)"
echo "  RAM      : $TOTAL_RAM 전체 / $AVAIL_RAM 사용 가능"
echo ""
echo "  【 벤치마크 설정 】"
echo "  모델 경로 : $DEFAULT_MODEL_DIR"
echo "  모델 크기 : $MODEL_SIZE"
echo "  연산량   : ${MODEL_GFLOPS} GFLOPS"
echo "  실행 시간 : ${DEFAULT_TIME}초"
echo "  결과 저장 : $RESULT_DIR"
echo ""

# ============================================
# 결과 분석 함수
# ============================================

print_json_result() {
    local JSON_FILE="$1"
    local LABEL="$2"

    if [ -z "$JSON_FILE" ] || [ ! -f "$JSON_FILE" ]; then
        echo "  [$LABEL] ⚠️  JSON 결과 없음"
        return
    fi

    FPS=$(python3 -c "
import json
try:
    d = json.load(open('$JSON_FILE'))
    print(d['results'][0].get('FPS', 0))
except: print(0)
" 2>/dev/null || echo "0")
    # FPS가 빈 문자열이면 0으로 대체 (Python 인라인 연산 오류 방어)
    FPS=${FPS:-0}
    [[ "$FPS" =~ ^[0-9]+(\.?[0-9]*)$ ]] || FPS=0

    NPU_TIME=$(python3 -c "
import json
try:
    d = json.load(open('$JSON_FILE'))
    t = d['results'][0].get('NPU Inference Time', {})
    print(t.get('mean', 0))
except: print(0)
" 2>/dev/null || echo "0")
    NPU_TIME=${NPU_TIME:-0}

    TOPS=$(python3 -c "print(f'{float(\"$FPS\") * $MODEL_GFLOPS / 1000:.2f}')" 2>/dev/null || echo "0")
    UTILIZATION=$(python3 -c "print(f'{float(\"$TOPS\") / $TARGET_TOPS * 100:.1f}')" 2>/dev/null || echo "0")
    BAR_LEN=$(python3 -c "print(min(int(float('$UTILIZATION') / 2), 50))" 2>/dev/null || echo "0")
    BAR=$(python3 -c "print('█' * $BAR_LEN + '░' * (50 - $BAR_LEN))" 2>/dev/null || echo "")

    echo "  [$LABEL]"
    echo "    ├─ FPS           : $FPS"
    echo "    ├─ NPU 추론 시간 : ${NPU_TIME} ms"
    echo "    ├─ 실제 TOPS     : $TOPS / $TARGET_TOPS"
    echo "    └─ 활용률        : [$BAR] ${UTILIZATION}%"
}

# ============================================
# 단일 모드 실행 (example.sh 방식 — 파이프 없이 직접 tee)
# ============================================

run_single() {
    local BIN="$1"; shift

    echo "=========================================="
    echo "▶ 시작: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "=========================================="
    echo ""

    local START_TIME; START_TIME=$(date +%s)

    # example.sh 방식: 파이프 없이 직접 tee → CPU 부하 최소화
    # dxbenchmark가 --result-path로 지정된 폴더에 JSON/CSV/HTML을 알아서 생성
    "$BIN" --result-path "$RESULT_DIR" $EXTRA_ARGS "${PASS_ARGS[@]}" 2>&1 | tee "$LOG_FILE"
    local EXIT_CODE="${PIPESTATUS[0]}"

    local ELAPSED=$(( $(date +%s) - START_TIME ))
    echo ""; echo "  소요 시간: ${ELAPSED}초"

    if [ "$EXIT_CODE" -ne 0 ]; then
        echo "❌ 벤치마크 실패 (exit $EXIT_CODE)"
        rm -f "$LOG_FILE"
        return "$EXIT_CODE"
    fi

    echo ""; echo "=========================================="; echo "  📊 결과 분석"; echo "=========================================="; echo ""
    JSON_FILE=$(find "$RESULT_DIR" -maxdepth 1 -name "DXBENCHMARK_*.json" -type f -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)
    print_json_result "$JSON_FILE" "단일 실행"
    echo ""

    # NPU Output Format Handler 분리 분석
    if [ -f "$LOG_FILE" ]; then
        FH_AVG=$(grep -o "NPU Output Format Handler.*|" "$LOG_FILE" | grep -o "| *[0-9]*\." | tail -1 | tr -d '| ' || echo "")
        if [ -n "$FH_AVG" ]; then
            echo "  ⚠️  CPU 후처리 시간 (NPU Output Format Handler): ${FH_AVG} us 평균"
            echo "     → 이 값이 클수록 CPU 성능이 병목입니다"
        fi
    fi

    echo ""
    echo "  로그 : $LOG_FILE"
    [ -n "$JSON_FILE" ] && echo "  결과 : $JSON_FILE"
    echo ""; echo "✅ 완료: $(date '+%Y-%m-%d %H:%M:%S')"; echo ""
}

# ============================================
# 병렬 모드 실행
# ============================================

run_parallel() {
    local BIN="$1"; shift

    echo "=========================================="
    echo "▶ 병렬 시작: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "  ⚠️  주의: CPU 후처리 경합으로 단일 실행보다 FPS가 낮을 수 있습니다"
    echo "=========================================="
    echo ""

    local START_TIME; START_TIME=$(date +%s)

    local RD0="$RESULT_DIR/npu0_${TIMESTAMP}"
    local RD1="$RESULT_DIR/npu1_${TIMESTAMP}"
    local RD2="$RESULT_DIR/npu2_${TIMESTAMP}"
    mkdir -p "$RD0" "$RD1" "$RD2"

    echo "  [NPU_0] 시작..."; "$BIN" --result-path "$RD0" $EXTRA_ARGS -n 1 "${PASS_ARGS[@]}" > "$RD0/stdout.log" 2>&1 & PID0=$!
    echo "  [NPU_1] 시작..."; "$BIN" --result-path "$RD1" $EXTRA_ARGS -n 2 "${PASS_ARGS[@]}" > "$RD1/stdout.log" 2>&1 & PID1=$!
    echo "  [NPU_2] 시작..."; "$BIN" --result-path "$RD2" $EXTRA_ARGS -n 3 "${PASS_ARGS[@]}" > "$RD2/stdout.log" 2>&1 & PID2=$!

    echo ""; echo "  ⏳ 3개 코어 실행 중... (dxtop 으로 실시간 확인 가능)"; echo ""

    local DOT=0
    while kill -0 "$PID0" 2>/dev/null || kill -0 "$PID1" 2>/dev/null || kill -0 "$PID2" 2>/dev/null; do
        sleep 5; DOT=$((DOT + 5)); printf "  경과: %ds\r" "$DOT"
    done; echo ""

    wait "$PID0"; local E0=$?
    wait "$PID1"; local E1=$?
    wait "$PID2"; local E2=$?

    local ELAPSED=$(( $(date +%s) - START_TIME ))
    echo ""; echo "  소요 시간: ${ELAPSED}초"

    { echo "=== NPU_0 ===" ; cat "$RD0/stdout.log"; echo "=== NPU_1 ===" ; cat "$RD1/stdout.log"; echo "=== NPU_2 ===" ; cat "$RD2/stdout.log"; } > "$LOG_FILE"

    echo ""; echo "=========================================="; echo "  📊 결과 분석 (코어별)"; echo "=========================================="; echo ""

    local J0 J1 J2
    J0=$(find "$RD0" -name "*.json" -type f -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)
    J1=$(find "$RD1" -name "*.json" -type f -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)
    J2=$(find "$RD2" -name "*.json" -type f -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)

    print_json_result "$J0" "NPU_0 (exit $E0)"
    echo ""
    print_json_result "$J1" "NPU_1 (exit $E1)"
    echo ""
    print_json_result "$J2" "NPU_2 (exit $E2)"
    echo ""

    TOTAL_TOPS=$(python3 -c "
import json, glob
total = 0
for d in ['$RD0','$RD1','$RD2']:
    files = glob.glob(d+'/*.json')
    if not files: continue
    try:
        r = json.load(open(sorted(files)[-1]))['results'][0]
        total += $MODEL_GFLOPS * float(r.get('FPS',0)) / 1000
    except: pass
print(f'{total:.2f}')
" 2>/dev/null || echo "?")

    TOTAL_UTIL=$(python3 -c "print(f'{float(\"$TOTAL_TOPS\") / $TARGET_TOPS * 100:.1f}')" 2>/dev/null || echo "?")
    BAR_LEN=$(python3 -c "print(min(int(float('$TOTAL_UTIL') / 2), 50))" 2>/dev/null || echo "0")
    BAR=$(python3 -c "print('█' * $BAR_LEN + '░' * (50 - $BAR_LEN))" 2>/dev/null || echo "")

    echo "  ────────────────────────────────────────"
    echo "  합산 TOPS : $TOTAL_TOPS / $TARGET_TOPS"
    echo "  전체 활용률: [$BAR] ${TOTAL_UTIL}%"
    echo ""
    echo "  로그 : $LOG_FILE"
    echo ""; echo "✅ 완료: $(date '+%Y-%m-%d %H:%M:%S')"; echo ""
}

# ============================================
# 실행
# ============================================

BIN="$DXBENCHMARK_PATH"
if [ ! -f "$BIN" ]; then
    ALT_PATH="/home/prische/dx-all-suite/dx-runtime/dx_rt/build_aarch64/bin/dxbenchmark"
    [ -f "$ALT_PATH" ] && BIN="$ALT_PATH" || { echo "Error: dxbenchmark not found."; exit 1; }
fi

if [ "$PARALLEL_MODE" -eq 1 ]; then
    run_parallel "$BIN"
else
    run_single "$BIN"
fi
