#!/usr/bin/env python3
"""
CPU 온도 + 사용량 모니터 — 경량 터미널 모니터링 도구
사용법: python3 cpu_temp_monitor.py [간격_초]
기본 간격: 2초
"""

import os
import sys
import time
import signal

# ── ANSI 색상 코드 (외부 라이브러리 의존성 없음) ───────────────────────────────
RESET  = "\033[0m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
HOME   = "\033[H"          # 커서를 왼쪽 상단으로 이동 (화면 전체를 지우지 않아 깜빡임 방지)
EOL    = "\033[K"          # 현재 줄의 끝까지 지움
ALT_ON  = "\033[?1049h"   # 대체 화면 버퍼 사용 시작 (htop/vim처럼 기존 화면 보존)
ALT_OFF = "\033[?1049l"   # 대체 화면 버퍼 종료 (원래 화면으로 복구)

FG_WHITE  = "\033[97m"
FG_GREEN  = "\033[92m"
FG_YELLOW = "\033[93m"
FG_RED    = "\033[91m"
FG_CYAN   = "\033[96m"
FG_GRAY   = "\033[37m"
FG_BLUE   = "\033[94m"

BG_DARK   = "\033[40m"

# ── 설정 (Config) ────────────────────────────────────────────────────────────
THERMAL_ZONES_PATH = "/sys/class/thermal"
DEFAULT_INTERVAL   = 2        # 초 단위
BAR_WIDTH          = 30       # 온도 진행 바 너비
CPU_BAR_WIDTH      = 24       # CPU 사용량 바 너비
MAX_TEMP           = 100.0    # °C (바 스케일링 기준)

# 임계값 (색상 변경 기준)
TEMP_WARN          = 70.0     # °C  → 노란색
TEMP_CRIT          = 85.0     # °C  → 빨간색

CPU_WARN           = 60.0     # %   → 노란색
CPU_CRIT           = 85.0     # %   → 빨간색

# 제외할 온도 구역 (타입 이름 기준)
EXCLUDED_ZONES     = {"gpu-thermal"}

# ── /proc/stat을 통한 CPU 사용량 계산 ──────────────────────────────────────────

# 이전 CPU 통계 스냅샷 저장용 (변화량 계산을 위해 필요)
_prev_cpu_stats: dict[str, tuple] = {}


def _read_cpu_stats() -> dict[str, tuple]:
    """
    /proc/stat 파일을 읽어 각 코어별 jiffie 카운터를 반환합니다.
    반환 값: {코어_이름: (user, nice, system, idle, iowait, irq, softirq, ...)}
    """
    stats = {}
    try:
        with open("/proc/stat") as f:
            for line in f:
                if not line.startswith("cpu"):
                    break  # cpu로 시작하는 줄이 항상 먼저 나옵니다.
                parts = line.split()
                label = parts[0]   # "cpu" (전체), "cpu0", "cpu1", ...
                values = tuple(int(x) for x in parts[1:])
                stats[label] = values
    except OSError:
        pass
    return stats


def read_cpu_usage() -> dict[str, float]:
    """
    이전 호출 이후의 각 코어별 CPU 사용률(%)을 계산합니다.
    첫 호출 시에는 기준 데이터가 없으므로 빈 딕셔너리를 반환합니다.
    """
    global _prev_cpu_stats
    curr = _read_cpu_stats()
    
    # 첫 실행 시 데이터만 저장하고 종료
    if not _prev_cpu_stats:
        _prev_cpu_stats = curr
        return {}

    usage = {}
    for label, curr_vals in curr.items():
        if label not in _prev_cpu_stats:
            continue
        prev_vals = _prev_cpu_stats[label]
        
        # 이전 값과 현재 값의 차이(delta) 계산
        length = min(len(curr_vals), len(prev_vals))
        diff = [curr_vals[i] - prev_vals[i] for i in range(length)]

        # idle: 3번 인덱스, iowait: 4번 인덱스
        idle_delta = diff[3] + (diff[4] if len(diff) > 4 else 0)
        total_delta = sum(diff)
        
        if total_delta == 0:
            usage[label] = 0.0
        else:
            # 사용률 = (1 - 유휴시간 변화량 / 전체시간 변화량) * 100
            usage[label] = max(0.0, min(100.0, (1.0 - idle_delta / total_delta) * 100.0))

    _prev_cpu_stats = curr
    return usage


def cpu_count() -> int:
    """/proc/stat을 통해 사용 가능한 CPU 코어 수를 셉니다."""
    stats = _read_cpu_stats()
    return sum(1 for k in stats if k.startswith("cpu") and k != "cpu")


# ── 도움 함수 (Helpers) ───────────────────────────────────────────────────────

def read_thermal_zones():
    """시스템의 온도 구역 이름과 현재 온도를 읽어 리스트로 반환합니다."""
    zones = []
    try:
        entries = sorted(os.listdir(THERMAL_ZONES_PATH))
    except OSError:
        return zones

    for entry in entries:
        if not entry.startswith("thermal_zone"):
            continue
        temp_file = os.path.join(THERMAL_ZONES_PATH, entry, "temp")
        type_file = os.path.join(THERMAL_ZONES_PATH, entry, "type")
        try:
            # 온도는 보통 1/1000도 단위로 저장되어 있으므로 1000으로 나눕니다.
            with open(temp_file) as f:
                temp_c = int(f.read().strip()) / 1000.0
            
            zone_type = entry  # 대체 이름
            if os.path.exists(type_file):
                with open(type_file) as f:
                    zone_type = f.read().strip()
            
            if zone_type in EXCLUDED_ZONES:
                continue
                
            zones.append((zone_type, temp_c))
        except (OSError, ValueError):
            continue
    return zones


def temp_color(temp):
    """온도에 따른 색상 코드를 반환합니다."""
    if temp >= TEMP_CRIT:
        return FG_RED
    elif temp >= TEMP_WARN:
        return FG_YELLOW
    return FG_GREEN


def cpu_color(pct):
    """CPU 사용률에 따른 색상 코드를 반환합니다."""
    if pct >= CPU_CRIT:
        return FG_RED
    elif pct >= CPU_WARN:
        return FG_YELLOW
    return FG_GREEN


def make_bar(value, width=BAR_WIDTH, max_val=MAX_TEMP, color_fn=None):
    """지정한 너비와 값에 비례하는 ASCII 프로그레스 바를 생성합니다."""
    ratio  = min(value / max_val, 1.0)
    filled = int(round(ratio * width))
    empty  = width - filled
    color  = color_fn(value) if color_fn else temp_color(value)
    body   = "█" * filled + "░" * empty
    return f"{color}{body}{RESET}"


def _read_single_freq_mhz(cpu_id: int):
    """지정한 cpu 번호의 현재 주파수(MHz)를 읽어옵니다."""
    path = f"/sys/devices/system/cpu/cpu{cpu_id}/cpufreq/scaling_cur_freq"
    try:
        with open(path) as f:
            return int(f.read().strip()) // 1000
    except OSError:
        return None


def cpu_freq_mhz():
    """big.LITTLE 구조를 고려하여 little 코어(cpu0)와 big 코어(cpu6→cpu4) 주파수를 반환합니다.
    반환 값: (little_mhz, big_mhz) — 읽기 실패 시 해당 항목은 None
    """
    little = _read_single_freq_mhz(0)  # A55 little 코어

    # Orange Pi 5 Plus: big 코어는 cpu6(A76) 위치; 없으면 cpu4 시도
    big = _read_single_freq_mhz(6)
    if big is None:
        big = _read_single_freq_mhz(4)

    return little, big


def read_ram_usage():
    """/proc/meminfo 에서 RAM 전체/사용/사용가능 을 MB 단위로 반환합니다."""
    info = {}
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    info[parts[0].rstrip(":")] = int(parts[1])  # kB 단위
    except OSError:
        return None, None, None

    total = info.get("MemTotal", 0) // 1024      # → MB
    free  = info.get("MemFree", 0) // 1024
    avail = info.get("MemAvailable", 0) // 1024
    used  = total - avail
    return total, used, avail


def uptime_str():
    """시스템 가동 시간을 hh:mm:ss 형식으로 반환합니다."""
    try:
        with open("/proc/uptime") as f:
            secs = int(float(f.read().split()[0]))
        h, rem = divmod(secs, 3600)
        m, s   = divmod(rem, 60)
        return f"{h:02d}:{m:02d}:{s:02d}"
    except OSError:
        return "N/A"


def hostname():
    """시스템의 호스트 이름을 가져옵니다."""
    try:
        with open("/proc/sys/kernel/hostname") as f:
            return f.read().strip()
    except OSError:
        return "unknown"


# ── 화면 렌더링 (Display) ─────────────────────────────────────────────────────

WIDTH = 56  # 전체 박스 너비


def hline(char="─", left="├", right="┤"):
    """가로 구분선을 생성합니다."""
    return f"{left}{char * (WIDTH - 2)}{right}"


def render(zones, cpu_usage, interval):
    """화면에 모든 정보를 렌더링합니다."""
    now  = time.strftime("%Y-%m-%d  %H:%M:%S")
    freq = cpu_freq_mhz()  # (little_mhz, big_mhz) 튜플
    up   = uptime_str()
    host = hostname()

    lines = []
    lines.append(HOME)  # 커서를 맨 위로 이동 (깜빡임 없이 덮어쓰기 위해)

    # ── 헤더 (Header) ─────────────────────────────────────────────────────────
    title = "  CPU Monitor  "
    pad   = (WIDTH - 2 - len(title)) // 2
    lines.append(f"{BOLD}{FG_CYAN}┌{'─' * (WIDTH - 2)}┐{RESET}")
    lines.append(f"{BOLD}{FG_CYAN}│{' ' * pad}{title}{' ' * (WIDTH - 2 - pad - len(title))}│{RESET}")
    lines.append(f"{BOLD}{FG_CYAN}└{'─' * (WIDTH - 2)}┘{RESET}")
    lines.append("")

    # ── 시스템 정보 (System info) ──────────────────────────────────────────────
    lines.append(f"  {DIM}Host   :{RESET} {FG_WHITE}{host}{RESET}")
    lines.append(f"  {DIM}Time   :{RESET} {FG_WHITE}{now}{RESET}")
    lines.append(f"  {DIM}Uptime :{RESET} {FG_WHITE}{up}{RESET}")
    # big.LITTLE 주파수 표시
    little_mhz, big_mhz = freq
    if little_mhz is not None:
        lines.append(f"  {DIM}Freq L :{RESET} {FG_WHITE}{little_mhz} MHz{RESET}  {DIM}(A55 little){RESET}")
    if big_mhz is not None:
        lines.append(f"  {DIM}Freq B :{RESET} {FG_WHITE}{big_mhz} MHz{RESET}  {DIM}(A76 big){RESET}")

    # RAM 사용량 표시
    ram_total, ram_used, ram_avail = read_ram_usage()
    if ram_total is not None:
        ram_pct = ram_used / ram_total * 100 if ram_total > 0 else 0
        ram_col = cpu_color(ram_pct)  # 사용률에 따른 색상
        ram_bar = make_bar(ram_pct, width=CPU_BAR_WIDTH, max_val=100.0, color_fn=cpu_color)
        lines.append(
            f"  {DIM}RAM    :{RESET} {ram_col}{BOLD}{ram_used:,} / {ram_total:,} MB{RESET}"
            f"  ({ram_col}{ram_pct:.1f}%{RESET})"
        )
        lines.append(f"  {DIM}         {RESET} {ram_bar}")
    lines.append("")

    # ── CPU 코어 사용량 (CPU Core Usage) ───────────────────────────────────────
    lines.append(f"  {BOLD}{FG_BLUE}CPU Core Usage{RESET}")
    lines.append(f"  {'─' * (WIDTH - 4)}")

    if not cpu_usage:
        lines.append(f"  {DIM}데이터 수집 중…{RESET}")
    else:
        # 전체(total) 사용량 표시
        total_pct = cpu_usage.get("cpu", 0.0)
        total_bar = make_bar(total_pct, width=CPU_BAR_WIDTH, max_val=100.0, color_fn=cpu_color)
        total_col = cpu_color(total_pct)
        lines.append(
            f"  {FG_WHITE}{'All':>5}{RESET}  "
            f"{total_col}{BOLD}{total_pct:5.1f}%{RESET}  {total_bar}"
        )
        lines.append("")

        # 각 코어별 사용량 (숫자 순서대로 정렬)
        core_keys = sorted(
            (k for k in cpu_usage if k != "cpu"),
            key=lambda x: int(x[3:]) if x[3:].isdigit() else 0
        )
        for key in core_keys:
            pct   = cpu_usage[key]
            bar   = make_bar(pct, width=CPU_BAR_WIDTH, max_val=100.0, color_fn=cpu_color)
            col   = cpu_color(pct)
            label = key.replace("cpu", "Core ")
            lines.append(
                f"  {FG_GRAY}{label:>6}{RESET}  "
                f"{col}{pct:5.1f}%{RESET}  {bar}"
            )
    lines.append("")

    # ── 온도 구역 (Thermal zones) ────────────────────────────────────────────
    lines.append(f"  {BOLD}{FG_CYAN}Thermal Zones{RESET}")
    lines.append(f"  {'─' * (WIDTH - 4)}")

    if not zones:
        lines.append(f"  {FG_YELLOW}온도 구역을 찾을 수 없습니다.{RESET}")
    else:
        for zone_type, temp in zones:
            color  = temp_color(temp)
            bar    = make_bar(temp, width=BAR_WIDTH, max_val=MAX_TEMP)
            label  = zone_type[:20].ljust(20)
            temp_s = f"{color}{BOLD}{temp:5.1f}°C{RESET}"
            lines.append(f"  {FG_GRAY}{label}{RESET} {temp_s}  {bar}")
            lines.append("")

    # ── 범례 (Legend) ─────────────────────────────────────────────────────────
    lines.append(f"  {'─' * (WIDTH - 4)}")
    lines.append(
        f"  {FG_GREEN}■ Cool{RESET}  "
        f"{FG_YELLOW}■ Warn{RESET}  "
        f"{FG_RED}■ Hot{RESET}"
    )
    lines.append("")
    lines.append(f"  {DIM}갱신 주기 {interval}초  │  Ctrl+C 종료{RESET}")

    # 각 줄 끝에 EOL(\033[K)을 붙여, 이전 화면의 잔상을 지웁니다.
    output = "\n".join(line + EOL for line in lines)
    
    # \033[J는 현재 위치부터 화면 끝까지 지웁니다 (프레임 크기 변화 대응)
    sys.stdout.write(output + "\033[J")
    sys.stdout.flush()


# ── 실행 진입점 (Entry point) ──────────────────────────────────────────────────

def main():
    interval = DEFAULT_INTERVAL
    if len(sys.argv) > 1:
        try:
            interval = float(sys.argv[1])
            if interval <= 0:
                raise ValueError
        except ValueError:
            print(f"사용법: {sys.argv[0]} [간격_초]", file=sys.stderr)
            sys.exit(1)

    # 대체 화면 버퍼 사용 + 커서 숨기기 (터미널 도구 공통 사항)
    sys.stdout.write(ALT_ON + "\033[?25l")
    sys.stdout.flush()

    def cleanup(sig=None, frame=None):
        """종료 시 터미널 상태를 원래대로 복구합니다."""
        sys.stdout.write("\033[?25h" + ALT_OFF)
        sys.stdout.flush()
        sys.exit(0)

    # 시스템 신호(Signal) 설정 (Ctrl+C 등)
    signal.signal(signal.SIGINT,  cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    # 첫 CPU 사용량 계산을 위해 호출 (첫 델타값은 항상 0)
    read_cpu_usage()

    try:
        while True:
            zones     = read_thermal_zones()
            cpu_usage = read_cpu_usage()
            render(zones, cpu_usage, interval)
            time.sleep(interval)
    finally:
        cleanup()


if __name__ == "__main__":
    main()
