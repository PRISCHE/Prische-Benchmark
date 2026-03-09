/**
 * GStreamer 기반 크로스플랫폼 AI 파이프라인 벤치마크
 *
 * GStreamer를 통해 Decode / Preprocess / Encode를 처리하고,
 * appsink/appsrc를 통해 C++ 측에서 NPU 추론(DX-RT)과 후처리를 수행한다.
 *
 * 다양한 보드(RK3588, NXP, Jetson, x86)에서 동일 소스코드로 동작하며,
 * --decoder hw|sw / --encoder hw|sw 옵션으로 HW/SW 비교 벤치마크가 가능하다.
 *
 * 파이프라인 구조:
 *   [GStreamer: RTSP→Decode→Convert→Scale] → appsink
 *        → [C++: NPU 추론 → 후처리 → OSD]
 *        → appsrc → [GStreamer: Encode] → stdout
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <csignal>
#include <getopt.h>
#include <thread>
#include <mutex>

// GStreamer
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

// DeepX DX-RT
#include "dxrt/dxrt_api.h"

// ==================== 설정 상수 ====================

// YOLOv5 모델 기본 설정
constexpr int INPUT_W = 640;
constexpr int INPUT_H = 640;
constexpr int NUM_CLASSES = 80;
constexpr int NUM_ANCHORS = 25200;  // YOLOv5: 25200
constexpr float CONF_THRESH = 0.25f;
constexpr float NMS_THRESH = 0.45f;

// ==================== CLI 설정 (전역) ====================
// 아래 하드코딩된 변수들은 C++ 바이너리를 단독 실행할 때의 기본(Fallback) 값입니다.
// 실제 운영 시 run_pipeline_benchmark.py 파이썬 스크립트가 `rtsp_address.txt` 위치로부터
// 입력 소스를 파싱하여 인자(--input 및 --model)로 해당 주소들을 덮어씌우게 됩니다.
static std::string g_rtsp_input = "rtsp://admin:password@192.168.0.200:554/Streaming/Channels/101";
static std::string g_model_path = "model/YoloV5N.dxnn";
static std::string g_output_prefix = "result/pipeline_ch0"; // 기본 출력 접두사 (벤치마크 스크립트가 --output-prefix로 덮어씀)
static std::string g_task_type = "det"; // "det" 또는 "pose"

// ==================== 전역 변수 ====================
bool g_running = true;
int g_infer_interval = 3;  // n 프레임마다 추론
int g_channel_id = 0;      // 채널 식별자 (로그/콘솔 출력용)
bool g_use_hw_decoder = true;
bool g_use_hw_encoder = true;

// ==================== 비동기 스레드 공유 상태 ====================
// DX-RT RunAsync + Wait 패턴 기반 (display_async_thread 참고)
// 스레드 A(Display)와 스레드 B(Inference) 간 바운딩 박스 공유
std::mutex g_box_mutex;
// g_latest_boxes는 YoloBox 정의 이후에 선언 (아래 참조)

// ==================== Pad Probe (Frame Dropper) ====================
// videoscale 플러그인이 모든 프레임을 CPU로 연산하는 낭비를 막기 위해,
// 큐에 들어가는 프레임을 가로채어 추론 주기가 아닌 프레임은 버림(DROP) 처리합니다.
long g_probe_cnt = 0;
static GstPadProbeReturn drop_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    g_probe_cnt++;
    if (g_probe_cnt % g_infer_interval != 0) {
        return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
}

// ==================== COCO 클래스 이름 ====================
static const char* YOLO_CLASSES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus",
    "train", "truck", "boat", "traffic light", "fire hydrant",
    "stop sign", "parking meter", "bench", "bird", "cat", "dog",
    "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe",
    "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat",
    "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl",
    "banana", "apple", "sandwich", "orange", "broccoli", "carrot",
    "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop",
    "mouse", "remote", "keyboard", "cell phone", "microwave", "oven",
    "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush"
};

// ==================== 파이프라인 계측 (Instrumentation) ====================
// 기존 0_prischebenchmark와 동일한 구조

struct StageStat {
    std::vector<double> samples_us;   // 각 프레임 소요 시간 (µs)
    double sum = 0;

    void record(double us) {
        samples_us.push_back(us);
        sum += us;
    }

    size_t count() const { return samples_us.size(); }
    double mean() const { return count() > 0 ? sum / count() : 0; }

    double sd() const {
        if (count() < 2) return 0;
        double m = mean();
        double acc = 0;
        for (auto v : samples_us) acc += (v - m) * (v - m);
        return std::sqrt(acc / (count() - 1));
    }

    double cv() const {
        double m = mean();
        return m > 0 ? sd() / m : 0;
    }

    double min_val() const {
        if (count() == 0) return 0;
        return *std::min_element(samples_us.begin(), samples_us.end());
    }

    double max_val() const {
        if (count() == 0) return 0;
        return *std::max_element(samples_us.begin(), samples_us.end());
    }
};

// /proc/self/status에서 VmRSS(물리 메모리 사용량) 읽기
static double get_rss_mb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            std::istringstream iss(line.substr(6));
            double kb = 0;
            iss >> kb;
            return kb / 1024.0;
        }
    }
    return 0;
}

struct PipelineStats {
    StageStat gst_decode;    // GStreamer 디코딩+변환+스케일 (HW 또는 SW)
    StageStat npu_infer;     // NPU 추론 (DX-RT)
    StageStat postprocess;   // 후처리 (NMS 등)
    StageStat osd;           // OSD (BBox 그리기)
    StageStat gst_encode;    // GStreamer 인코딩 (HW 또는 SW)
    StageStat frame_total;   // 프레임 전체 처리 시간

    int total_frames = 0;
    int inference_frames = 0;
    double peak_rss_mb = 0;
    std::chrono::steady_clock::time_point start_time;

    void begin() { start_time = std::chrono::steady_clock::now(); }

    double elapsed_sec() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_time).count();
    }

    double overall_fps() const {
        double e = elapsed_sec();
        return e > 0 ? total_frames / e : 0;
    }

    // 주기적 콘솔 출력 (메모리 사용량 포함)
    void print_periodic() {
        double e = elapsed_sec();
        double cur_rss = get_rss_mb();
        if (cur_rss > peak_rss_mb) peak_rss_mb = cur_rss;

        fprintf(stderr, "\n"
            "╔══════════════════════════════════════════════════════════════╗\n"
            "║          GStreamer Pipeline Performance (%.1f초 경과)        ║\n"
            "╠══════════════════════════════════════════════════════════════╣\n"
            "║ 총 프레임: %d  |  추론 프레임: %d  |  FPS: %.2f             ║\n"
            "║ RAM: %.1f MB (현재)  |  %.1f MB (최대)                      ║\n"
            "║ Decoder: %-8s  |  Encoder: %-8s                      ║\n"
            "╠══════════════════════════════════════════════════════════════╣\n"
            "║ 단계            │ Avg(ms)   │ SD(ms)  │ Min(ms) │ Max(ms)  ║\n"
            "╠─────────────────┼───────────┼─────────┼─────────┼──────────╣\n",
            e, total_frames, inference_frames, overall_fps(),
            cur_rss, peak_rss_mb,
            g_use_hw_decoder ? "HW(VPU)" : "SW(CPU)",
            g_use_hw_encoder ? "HW(VPU)" : "SW(CPU)");

        auto print_row = [](const char* name, const StageStat& s) {
            if (s.count() == 0) return;
            fprintf(stderr, "║ %-15s │ %9.2f │ %7.2f │ %7.2f │ %8.2f ║\n",
                name, s.mean()/1000.0, s.sd()/1000.0, s.min_val()/1000.0, s.max_val()/1000.0);
        };

        print_row("Decode&Preproc",    gst_decode);
        print_row("NPU Infer",    npu_infer);
        print_row("PostProcess",  postprocess);
        print_row("OSD",          osd);
        print_row("GstEncode",    gst_encode);
        print_row("Frame Total",  frame_total);

        fprintf(stderr,
            "╚══════════════════════════════════════════════════════════════╝\n");
        fflush(stderr);
    }

    // 결과를 로그 파일로 저장 (기존 1_prischebenchmark와 호환 형식)
    void save_summary(const std::string& path) {
        std::ofstream f(path);
        if (!f.is_open()) { fprintf(stderr, "[Stats] 로그 파일 저장 실패: %s\n", path.c_str()); return; }

        auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        f << "=== GStreamer Pipeline Benchmark Summary ===" << std::endl;
        f << "Date: " << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S") << std::endl;
        f << "Decoder: " << (g_use_hw_decoder ? "HW(VPU)" : "SW(CPU)") << std::endl;
        f << "Encoder: " << (g_use_hw_encoder ? "HW(VPU)" : "SW(CPU)") << std::endl;
        f << "Total Frames: " << total_frames << std::endl;
        f << "Inference Frames: " << inference_frames << std::endl;
        f << std::fixed << std::setprecision(2);
        f << "Elapsed Time: " << elapsed_sec() << " sec" << std::endl;
        f << "Overall FPS: " << overall_fps() << std::endl;
        double final_rss = get_rss_mb();
        if (final_rss > peak_rss_mb) peak_rss_mb = final_rss;
        f << "RAM Current(RSS): " << final_rss << " MB" << std::endl;
        f << "RAM Peak(RSS): " << peak_rss_mb << " MB" << std::endl;
        f << std::endl;

        auto write_stat = [&](const char* name, const StageStat& s) {
            if (s.count() == 0) return;
            f << "[" << name << "]" << std::endl;
            f << "  Count:    " << s.count() << std::endl;
            f << "  Mean:     " << s.mean()/1000.0 << " ms (" << (int)s.mean() << " us)" << std::endl;
            f << "  SD:       " << s.sd()/1000.0 << " ms" << std::endl;
            f << "  CV:       " << s.cv() << std::endl;
            f << "  Min:      " << s.min_val()/1000.0 << " ms" << std::endl;
            f << "  Max:      " << s.max_val()/1000.0 << " ms" << std::endl;
            f << std::endl;
        };

        write_stat("Decode&Preproc",    gst_decode);
        write_stat("NPU Infer",    npu_infer);
        write_stat("PostProcess",  postprocess);
        write_stat("OSD",          osd);
        write_stat("GstEncode",    gst_encode);
        write_stat("Frame Total",  frame_total);

        fprintf(stderr, "[Stats] 요약 저장 완료: %s\n", path.c_str());
    }

    // 프레임별 상세 CSV 저장
    void save_csv(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return;

        f << "frame,gst_decode_us,npu_infer_us,postprocess_us,osd_us,gst_encode_us,total_us" << std::endl;
        size_t n = frame_total.count();
        for (size_t i = 0; i < n; ++i) {
            f << i;
            f << "," << (i < gst_decode.count()   ? gst_decode.samples_us[i] : 0);
            f << "," << (i < npu_infer.count()     ? npu_infer.samples_us[i] : 0);
            f << "," << (i < postprocess.count()   ? postprocess.samples_us[i] : 0);
            f << "," << (i < osd.count()           ? osd.samples_us[i] : 0);
            f << "," << (i < gst_encode.count()    ? gst_encode.samples_us[i] : 0);
            f << "," << frame_total.samples_us[i];
            f << std::endl;
        }
        fprintf(stderr, "[Stats] CSV 저장 완료: %s\n", path.c_str());
    }
};

// 시간 측정 헬퍼 매크로
#define TIMER_START(name) auto _timer_##name = std::chrono::steady_clock::now()
#define TIMER_END_US(name) (std::chrono::duration_cast<std::chrono::microseconds>( \
    std::chrono::steady_clock::now() - _timer_##name).count())

// ==================== YOLOv5 & YOLOv8 Pose 후처리 ====================

struct Keypoint {
    float x, y, conf;
};

struct YoloBox {
    float x1, y1, x2, y2, score;
    int cls;
    std::string class_name;
    std::vector<Keypoint> keypoints; // Pose 모델의 17개 키포인트 (있는 경우)
};

// 비동기 스레드 간 공유 바운딩 박스 (완전한 YoloBox 타입 필요)
std::vector<YoloBox> g_latest_boxes;

static float iou(const YoloBox& a, const YoloBox& b) {
    float ix1 = std::max(a.x1, b.x1);
    float iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2);
    float iy2 = std::min(a.y2, b.y2);
    float area_i = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    return area_i / (area_a + area_b - area_i + 1e-6f);
}

/**
 * YOLOv5 후처리
 * 텐서 레이아웃: [1, 25200, 85]
 * 각 Anchor: [cx, cy, w, h, objectness, class_scores[80]]
 *
 * orig_w/orig_h: 원본 프레임 크기 (Letterbox 역변환용)
 * GStreamer에서는 videoscale이 letterbox 없이 단순 리사이즈하므로,
 * lb_x=0, lb_y=0, scale은 width/height 비율로 계산
 */
static std::vector<YoloBox> post_process_yolov5(
    const std::vector<std::shared_ptr<dxrt::Tensor>>& outputs,
    int orig_w, int orig_h)
{
    std::vector<YoloBox> boxes;
    if (outputs.empty()) return boxes;

    auto tensor = outputs[0];
    const float* data = (const float*)tensor->data();

    // GStreamer videoscale은 단순 리사이즈 (Letterbox 아님)
    float scale_x = (float)INPUT_W / orig_w;
    float scale_y = (float)INPUT_H / orig_h;

    for (int i = 0; i < NUM_ANCHORS; ++i) {
        const float* anchor = data + i * (5 + NUM_CLASSES);
        float objectness = anchor[4];
        if (objectness < CONF_THRESH) continue;

        float max_class_score = 0;
        int max_cls = -1;
        for (int c = 0; c < NUM_CLASSES; ++c) {
            float s = anchor[5 + c];
            if (s > max_class_score) {
                max_class_score = s;
                max_cls = c;
            }
        }

        float score = objectness * max_class_score;
        if (score < CONF_THRESH) continue;

        float cx = anchor[0], cy = anchor[1], w = anchor[2], h = anchor[3];

        // 단순 리사이즈 역변환 (Letterbox가 아닌 stretch 방식)
        float x1 = (cx - w/2) / scale_x;
        float y1 = (cy - h/2) / scale_y;
        float x2 = (cx + w/2) / scale_x;
        float y2 = (cy + h/2) / scale_y;

        // 클램핑
        x1 = std::max(0.0f, std::min((float)orig_w, x1));
        y1 = std::max(0.0f, std::min((float)orig_h, y1));
        x2 = std::max(0.0f, std::min((float)orig_w, x2));
        y2 = std::max(0.0f, std::min((float)orig_h, y2));

        boxes.push_back({x1, y1, x2, y2, score, max_cls, YOLO_CLASSES[max_cls]});
    }

    // NMS (Non-Maximum Suppression)
    std::sort(boxes.begin(), boxes.end(), [](const YoloBox& a, const YoloBox& b){ return a.score > b.score; });
    std::vector<YoloBox> final_boxes;
    for (const auto& b : boxes) {
        bool keep = true;
        for (const auto& fb : final_boxes) {
            if (iou(b, fb) > NMS_THRESH) { keep = false; break; }
        }
        if (keep) final_boxes.push_back(b);
    }
    return final_boxes;
}

/**
 * YOLOv8 Pose 후처리
 * 텐서 레이아웃: [1, 56, 8400]
 *   8400: 후보 box/anchor 개수
 *   56:   [cx, cy, w, h] (4) + [person_score] (1) + 17 * [x, y, conf] (51)
 *
 * orig_w/orig_h: Letterbox 역변환용 (단순 stretch 가정)
 */
static std::vector<YoloBox> post_process_yolov8_pose(
    const std::vector<std::shared_ptr<dxrt::Tensor>>& outputs,
    int orig_w, int orig_h)
{
    std::vector<YoloBox> boxes;
    if (outputs.empty()) return boxes;

    auto tensor = outputs[0];
    const float* data = (const float*)tensor->data();
    
    // GStreamer videoscale stretch 비율 역변환
    float scale_x = (float)INPUT_W / orig_w;
    float scale_y = (float)INPUT_H / orig_h;

    const int num_anchors = 8400; // YoloV8 resolution dependent
    const int box_dim = 56;

    for (int i = 0; i < num_anchors; ++i) {
        // [1, 56, 8400] transposed iteration logic -> Memory layout is often [batch, dim, anchors] (e.g. 1x56x8400) internally from DxRT,
        // Wait, dx_rt outputs are usually flattened arrays. Let's assume standard YoloV8 layout [1, 56, 8400].
        // Data format: data[d * 8400 + i] where d is dimension
        
        float cx = data[0 * num_anchors + i];
        float cy = data[1 * num_anchors + i];
        float w  = data[2 * num_anchors + i];
        float h  = data[3 * num_anchors + i];
        
        float score = data[4 * num_anchors + i]; // Person score

        if (score < CONF_THRESH) continue;

        float x1 = (cx - w/2) / scale_x;
        float y1 = (cy - h/2) / scale_y;
        float x2 = (cx + w/2) / scale_x;
        float y2 = (cy + h/2) / scale_y;

        x1 = std::max(0.0f, std::min((float)orig_w, x1));
        y1 = std::max(0.0f, std::min((float)orig_h, y1));
        x2 = std::max(0.0f, std::min((float)orig_w, x2));
        y2 = std::max(0.0f, std::min((float)orig_h, y2));

        YoloBox box = {x1, y1, x2, y2, score, 0, "person"}; // YOLOv8 Pose is single class (person=0)
        
        // Extract 17 keypoints
        for (int k = 0; k < 17; ++k) {
            float kx = data[(5 + k*3 + 0) * num_anchors + i] / scale_x;
            float ky = data[(5 + k*3 + 1) * num_anchors + i] / scale_y;
            float kv = data[(5 + k*3 + 2) * num_anchors + i]; // visibility / confidence
            
            box.keypoints.push_back({kx, ky, kv});
        }
        boxes.push_back(box);
    }

    // NMS (Non-Maximum Suppression)
    std::sort(boxes.begin(), boxes.end(), [](const YoloBox& a, const YoloBox& b){ return a.score > b.score; });
    std::vector<YoloBox> final_boxes;
    for (const auto& b : boxes) {
        bool keep = true;
        for (const auto& fb : final_boxes) {
            if (iou(b, fb) > NMS_THRESH) { keep = false; break; }
        }
        if (keep) final_boxes.push_back(b);
    }
    return final_boxes;
}


// ==================== OSD: NV12 버퍼에 사각형/선/점 등 그리기 ====================
// NV12 패스스루 아키텍처: 디코더 출력(NV12)을 변환 없이 인코더에 전달.
// NV12 레이아웃: Y 플레인(w*h) + UV 인터리브드 플레인(w*h/2)
// RGB → YUV 색상 변환 공식 (BT.601):
//   Y  =  0.299*R + 0.587*G + 0.114*B
//   U  = -0.169*R - 0.331*G + 0.500*B + 128
//   V  =  0.500*R - 0.419*G - 0.081*B + 128

static void draw_rect_nv12(uint8_t* nv12_data, int img_w, int img_h, int y_stride,
                            int x, int y, int w, int h,
                            uint8_t r, uint8_t g, uint8_t b, int thickness = 3)
{
    // RGB → YUV 변환 (BT.601)
    uint8_t yv = (uint8_t)std::clamp((int)(0.299f*r + 0.587f*g + 0.114f*b), 0, 255);
    uint8_t uv = (uint8_t)std::clamp((int)(-0.169f*r - 0.331f*g + 0.500f*b + 128), 0, 255);
    uint8_t vv = (uint8_t)std::clamp((int)(0.500f*r - 0.419f*g - 0.081f*b + 128), 0, 255);

    // 범위 클램핑
    x = std::max(0, std::min(x, img_w - 1));
    y = std::max(0, std::min(y, img_h - 1));
    w = std::max(1, std::min(w, img_w - x));
    h = std::max(1, std::min(h, img_h - y));

    // UV 플레인 시작 오프셋 (NV12: Y 플레인 다음에 UV 인터리브드)
    uint8_t* y_plane = nv12_data;
    uint8_t* uv_plane = nv12_data + y_stride * img_h;

    auto set_pixel_nv12 = [&](int px, int py) {
        if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
            // Y 플레인 (매 픽셀)
            y_plane[py * y_stride + px] = yv;
            // UV 플레인 (2x2 블록당 1개, 인터리브드)
            int uv_off = (py / 2) * y_stride + (px / 2) * 2;
            uv_plane[uv_off + 0] = uv;  // U
            uv_plane[uv_off + 1] = vv;  // V
        }
    };

    // 4변 그리기
    for (int t = 0; t < thickness; ++t) {
        for (int dx = 0; dx < w; ++dx) {
            set_pixel_nv12(x + dx, y + t);
            set_pixel_nv12(x + dx, y + h - 1 - t);
        }
        for (int dy = 0; dy < h; ++dy) {
            set_pixel_nv12(x + t, y + dy);
            set_pixel_nv12(x + w - 1 - t, y + dy);
        }
    }
}

// 점 그리기 (키포인트)
static void draw_point_nv12(uint8_t* nv12_data, int img_w, int img_h, int y_stride,
                            int cx, int cy, int radius,
                            uint8_t r, uint8_t g, uint8_t b)
{
    int size = radius * 2;
    // 사각형으로 근사하여 점 그리기
    draw_rect_nv12(nv12_data, img_w, img_h, y_stride, cx - radius, cy - radius, size, size, r, g, b, radius);
}

// 브레즌햄 선 긋기 (뼈대)
static void draw_line_nv12(uint8_t* nv12_data, int img_w, int img_h, int y_stride,
                           int x1, int y1, int x2, int y2,
                           uint8_t r, uint8_t g, uint8_t b, int thickness = 2)
{
    int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;

    while (true) {
        // 두께 적용 (단순 사각형)
        draw_rect_nv12(nv12_data, img_w, img_h, y_stride, x1 - thickness/2, y1 - thickness/2, thickness, thickness, r, g, b, thickness);

        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

// (resize_rgb_bilinear 함수는 SIMD 기반의 GStreamer videoscale 분기로 성능상 대체됨)

// ==================== GStreamer 플러그인 자동 탐지 ====================

struct PluginInfo {
    std::string hw_decoder;     // HW 디코더 element 이름
    std::string hw_decoder_desc;
    std::string sw_decoder;     // SW 디코더 element 이름
    std::string sw_decoder_desc;
    std::string hw_encoder;
    std::string hw_encoder_desc;
    std::string sw_encoder;
    std::string sw_encoder_desc;
};

static PluginInfo detect_plugins() {
    PluginInfo info;

    // SW 디코더/인코더는 항상 사용 가능 (GStreamer 기본)
    info.sw_decoder = "avdec_h264";
    info.sw_decoder_desc = "FFmpeg libavcodec (CPU)";
    info.sw_encoder = "x264enc speed-preset=ultrafast tune=zerolatency";
    info.sw_encoder_desc = "x264 (CPU)";

    // HW 디코더 탐지 — 우선순위 순
    struct { const char* name; const char* desc; } hw_decoders[] = {
        {"mppvideodec",    "Rockchip MPP (VPU)"},       // RK3588 등
        {"v4l2slh264dec",  "V4L2 Stateless H264"},      // 범용 V4L2
        {"v4l2h264dec",    "V4L2 H264"},                 // 범용 V4L2
        {"nvv4l2decoder",  "NVIDIA V4L2 (Jetson)"},     // Jetson
        {"vaapih264dec",   "VAAPI H264 (Intel/AMD)"},   // x86
        {"omxh264dec",     "OpenMAX H264"},              // 구형 SoC
    };

    for (auto& d : hw_decoders) {
        GstElementFactory* factory = gst_element_factory_find(d.name);
        if (factory) {
            info.hw_decoder = d.name;
            info.hw_decoder_desc = d.desc;
            gst_object_unref(factory);
            break;
        }
    }

    // HW 인코더 탐지 — 우선순위 순
    struct { const char* name; const char* desc; } hw_encoders[] = {
        {"mpph264enc",     "Rockchip MPP H264 (VPU)"},
        {"v4l2h264enc",    "V4L2 H264 Encoder"},
        {"nvv4l2h264enc",  "NVIDIA V4L2 H264 (Jetson)"},
        {"vaapih264enc",   "VAAPI H264 (Intel/AMD)"},
        {"omxh264enc",     "OpenMAX H264 Encoder"},
    };

    for (auto& e : hw_encoders) {
        GstElementFactory* factory = gst_element_factory_find(e.name);
        if (factory) {
            info.hw_encoder = e.name;
            info.hw_encoder_desc = e.desc;
            gst_object_unref(factory);
            break;
        }
    }

    return info;
}

// 탐지된 플러그인 정보를 stderr에 출력
static void print_plugin_info(const PluginInfo& info) {
    fprintf(stderr, "\n"
        "╔══════════════════════════════════════════════════════════════╗\n"
        "║                 GStreamer 플러그인 탐지 결과                 ║\n"
        "╠══════════════════════════════════════════════════════════════╣\n");

    if (!info.hw_decoder.empty()) {
        fprintf(stderr, "║ 🟢 HW 디코더: %-20s (%s)\n", info.hw_decoder.c_str(), info.hw_decoder_desc.c_str());
    } else {
        fprintf(stderr, "║ 🔴 HW 디코더: 없음 (SW 폴백 필수)\n");
    }
    fprintf(stderr, "║ 🔵 SW 디코더: %-20s (%s)\n", info.sw_decoder.c_str(), info.sw_decoder_desc.c_str());

    if (!info.hw_encoder.empty()) {
        fprintf(stderr, "║ 🟢 HW 인코더: %-20s (%s)\n", info.hw_encoder.c_str(), info.hw_encoder_desc.c_str());
    } else {
        fprintf(stderr, "║ 🔴 HW 인코더: 없음 (SW 폴백 필수)\n");
    }
    fprintf(stderr, "║ 🔵 SW 인코더: x264enc             (%s)\n", info.sw_encoder_desc.c_str());

    fprintf(stderr,
        "╚══════════════════════════════════════════════════════════════╝\n\n");
}

// ==================== 전역 상태 ====================
static PipelineStats g_stats;

static void signal_handler(int sig) {
    fprintf(stderr, "\n🛑 [CH%d] 시그널 %d 수신, 파이프라인 정상 종료 중...\n", g_channel_id, sig);
    g_running = false;
}

// ==================== Thread A: 디스플레이 & 인코딩 (30 FPS) ====================
// GStreamer sink_full에서 FHD 프레임을 받아 최신 바운딩 박스를 그린 뒤 인코더로 전달.
// 추론 속도와 무관하게 카메라 입력 프레임 속도 그대로 영상을 출력한다.
static void display_thread_func(
    GstElement* app_sink_full,
    GstElement*& appsrc_elem,
    GstElement*& encode_pipeline,
    bool& encode_inited,
    int& stream_w, int& stream_h,
    const std::string& encoder_element)
{
    int display_frame_count = 0;

    while (g_running) {
        TIMER_START(frame);
        TIMER_START(gst_dec);

        // sink_full에서 FHD 프레임 가져오기 (100ms 타임아웃, 빠른 폴링)
        GstSample* sample_full = gst_app_sink_try_pull_sample(
            GST_APP_SINK(app_sink_full), 100 * GST_MSECOND);
        if (!sample_full) {
            if (gst_app_sink_is_eos(GST_APP_SINK(app_sink_full))) {
                fprintf(stderr, "📛 [CH%d] EOS 수신 (sink_full)\n", g_channel_id);
                g_running = false;
                break;
            }
            continue;
        }
        double gst_dec_us = TIMER_END_US(gst_dec);
        g_stats.gst_decode.record(gst_dec_us);

        // 프레임 데이터 추출
        GstCaps* caps_full = gst_sample_get_caps(sample_full);
        GstBuffer* buffer_full = gst_sample_get_buffer(sample_full);

        GstVideoInfo vinfo_full;
        if (!gst_video_info_from_caps(&vinfo_full, caps_full)) {
            fprintf(stderr, "⚠️ 비디오 정보 파싱 실패\n");
            gst_sample_unref(sample_full);
            continue;
        }

        int w = GST_VIDEO_INFO_WIDTH(&vinfo_full);
        int h = GST_VIDEO_INFO_HEIGHT(&vinfo_full);
        int stride = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo_full, 0);

        // 인코딩 파이프라인 지연 초기화 (첫 프레임에서 해상도 결정)
        if (!encode_inited) {
            stream_w = w;
            stream_h = h;

            std::ostringstream enc_pipe;
            // NV12 패스스루: 디코더에서 받은 NV12를 변환 없이 인코더에 직접 전달 (CPU 색변환 0%)
            enc_pipe << "appsrc name=src format=time is-live=true "
                     << "caps=video/x-raw,format=NV12,width=" << stream_w << ",height=" << stream_h
                     << ",framerate=30/1 "
                     << "! " << encoder_element << " "
                     << "! h264parse "
                     << "! video/x-h264,stream-format=byte-stream "
                     << "! fdsink fd=1";

            fprintf(stderr, "🔗 [CH%d] 인코딩 파이프라인:\n   %s\n\n", g_channel_id, enc_pipe.str().c_str());

            GError* enc_err = nullptr;
            encode_pipeline = gst_parse_launch(enc_pipe.str().c_str(), &enc_err);
            if (enc_err) {
                fprintf(stderr, "❌ 인코딩 파이프라인 생성 실패: %s\n", enc_err->message);
                g_error_free(enc_err);
                gst_sample_unref(sample_full);
                g_running = false;
                break;
            }

            appsrc_elem = gst_bin_get_by_name(GST_BIN(encode_pipeline), "src");

            // 인코딩 파이프라인 프리롤 (READY → PAUSED → PLAYING)
            gst_element_set_state(encode_pipeline, GST_STATE_READY);
            gst_element_set_state(encode_pipeline, GST_STATE_PAUSED);
            gst_element_set_state(encode_pipeline, GST_STATE_PLAYING);
            
            encode_inited = true;
            fprintf(stderr, "✅ [CH%d] 인코딩 파이프라인 시작 (%dx%d 원본유지)\n", g_channel_id, stream_w, stream_h);
        }

        // 프레임 버퍼 매핑 (읽기 전용)
        GstMapInfo map_full;
        if (!gst_buffer_map(buffer_full, &map_full, GST_MAP_READ)) {
            fprintf(stderr, "⚠️ 버퍼 매핑 실패\n");
            gst_sample_unref(sample_full);
            continue;
        }

        // 인코더용 쓰기 가능한 버퍼 할당 & 복사
        GstBuffer* enc_buf = gst_buffer_new_allocate(nullptr, map_full.size, nullptr);
        GstMapInfo enc_map;
        gst_buffer_map(enc_buf, &enc_map, GST_MAP_WRITE);
        memcpy(enc_map.data, map_full.data, map_full.size);
        uint8_t* nv12_data = enc_map.data;
        gst_buffer_unmap(buffer_full, &map_full);
        gst_sample_unref(sample_full);

        // ── OSD: NV12 버퍼에 최신 바운딩 박스/포즈 그리기 ──
        TIMER_START(osd_timer);
        std::vector<YoloBox> local_boxes;
        {
            std::lock_guard<std::mutex> lock(g_box_mutex);
            local_boxes = g_latest_boxes;
        }
        for (const auto& b : local_boxes) {
            // person=초록, 그 외=빨강 (RGB 값을 draw_rect_nv12 내부에서 YUV로 변환)
            uint8_t r = (b.cls == 0) ? 0 : 255;
            uint8_t g = (b.cls == 0) ? 255 : 0;
            uint8_t bv = 0;
            draw_rect_nv12(nv12_data, w, h, stride,
                           (int)b.x1, (int)b.y1,
                           (int)(b.x2 - b.x1), (int)(b.y2 - b.y1),
                           r, g, bv, 3);
            
            // 포즈 타스크일 경우 스켈레톤(뼈대) 그리기
            if (g_task_type == "pose" && b.keypoints.size() == 17) {
                // 선분을 연결할 관절 인덱스 (COCO Keypoint Format)
                int skeleton[][2] = {
                    {15,13}, {13,11}, {16,14}, {14,12}, {11,12}, {5,11}, {6,12},
                    {5,6}, {5,7}, {6,8}, {7,9}, {8,10}, {1,2}, {0,1}, {0,2},
                    {1,3}, {2,4}, {3,5}, {4,6}
                };
                
                // 뼈대 선 그리기
                for (int sf = 0; sf < 19; ++sf) {
                    const auto& kp1 = b.keypoints[skeleton[sf][0]];
                    const auto& kp2 = b.keypoints[skeleton[sf][1]];
                    // 신뢰도 0.5 이상인 관절만 연결
                    if (kp1.conf > 0.5f && kp2.conf > 0.5f) {
                        draw_line_nv12(nv12_data, w, h, stride,
                                       (int)kp1.x, (int)kp1.y,
                                       (int)kp2.x, (int)kp2.y,
                                       255, 255, 0, 2); // 노란색 선
                    }
                }
                
                // 관절 꼭짓점 점 그리기
                for (size_t k = 0; k < 17; ++k) {
                    const auto& kp = b.keypoints[k];
                    if (kp.conf > 0.5f) {
                        draw_point_nv12(nv12_data, w, h, stride,
                                        (int)kp.x, (int)kp.y, 3,
                                        0, 255, 255); // 시안색 점
                    }
                }
            }
        }
        g_stats.osd.record(TIMER_END_US(osd_timer));

        // ── 인코딩: GStreamer appsrc에 프레임 전달 ──
        TIMER_START(gst_enc);
        gst_buffer_unmap(enc_buf, &enc_map);

        if (encode_inited && appsrc_elem) {
            GST_BUFFER_PTS(enc_buf) = display_frame_count * GST_SECOND / 30;
            GST_BUFFER_DURATION(enc_buf) = GST_SECOND / 30;
            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_elem), enc_buf);
            if (ret != GST_FLOW_OK) {
                fprintf(stderr, "⚠️ appsrc push 실패: %d\n", ret);
            }
        } else {
            gst_buffer_unref(enc_buf);
        }
        g_stats.gst_encode.record(TIMER_END_US(gst_enc));

        // 프레임 전체 시간 기록
        g_stats.frame_total.record(TIMER_END_US(frame));
        g_stats.total_frames++;
        display_frame_count++;

        // 30프레임마다 통계 출력
        if (display_frame_count % 30 == 0) {
            g_stats.print_periodic();
        }
    }
}

// ==================== Thread B: NPU 추론 (Pad Probe 속도) ====================
// GStreamer sink_infer에서 640x640 프레임을 받아 DX-RT RunAsync → Wait 후
// 후처리 결과(바운딩 박스)를 전역 g_latest_boxes에 원자적으로 갱신한다.
static void infer_thread_func(
    GstElement* app_sink_infer,
    dxrt::InferenceEngine* engine,
    int orig_w, int orig_h)
{
    fprintf(stderr, "🧠 [CH%d] NPU 추론 스레드 시작 (RunAsync + Wait 패턴)\n", g_channel_id);

    while (g_running) {
        // Pad Probe에 의해 throttle된 프레임만 도착 (예: interval=5 → ~6 FPS)
        GstSample* sample_infer = gst_app_sink_try_pull_sample(
            GST_APP_SINK(app_sink_infer), 500 * GST_MSECOND);
        if (!sample_infer) {
            if (gst_app_sink_is_eos(GST_APP_SINK(app_sink_infer))) {
                fprintf(stderr, "📛 [CH%d] EOS 수신 (sink_infer)\n", g_channel_id);
                g_running = false;
                break;
            }
            continue;
        }

        // 640x640 추론용 프레임 매핑
        GstBuffer* buffer_infer = gst_sample_get_buffer(sample_infer);
        GstMapInfo map_infer;
        if (!gst_buffer_map(buffer_infer, &map_infer, GST_MAP_READ)) {
            fprintf(stderr, "⚠️ [CH%d] 추론 버퍼 매핑 실패\n", g_channel_id);
            gst_sample_unref(sample_infer);
            continue;
        }

        // DX-RT 공식 비동기 패턴: RunAsync() + Wait()
        TIMER_START(npu);
        int jobId = engine->RunAsync(map_infer.data);
        auto outputs = engine->Wait(jobId);
        g_stats.npu_infer.record(TIMER_END_US(npu));

        gst_buffer_unmap(buffer_infer, &map_infer);
        gst_sample_unref(sample_infer);

        // 후처리 (NMS 분기 처리)
        TIMER_START(postproc);
        std::vector<YoloBox> new_boxes;
        if (g_task_type == "pose") {
            new_boxes = post_process_yolov8_pose(outputs, orig_w, orig_h);
        } else {
            new_boxes = post_process_yolov5(outputs, orig_w, orig_h);
        }
        g_stats.postprocess.record(TIMER_END_US(postproc));

        // 전역 바운딩 박스 원자적 갱신 (display 스레드가 최신 결과를 즉시 사용)
        {
            std::lock_guard<std::mutex> lock(g_box_mutex);
            g_latest_boxes = std::move(new_boxes);
        }
        g_stats.inference_frames++;
    }
    fprintf(stderr, "🧠 [CH%d] NPU 추론 스레드 종료\n", g_channel_id);
}

// ==================== 메인 함수 ====================

static void print_usage(const char* prog) {
    fprintf(stderr,
        "사용법: %s [옵션]\n\n"
        "옵션:\n"
        "  --input URL       RTSP 입력 주소\n"
        "  --model PATH      DX-RT 모델 파일 경로 (.dxnn)\n"
        "  --interval N      N프레임마다 1회 추론 (기본: 3)\n"
        "  --channel-id N    채널 번호 (로그용, 기본: 0)\n"
        "  --task det|pose   실행할 Task 유형: det(객체인식, 기본) 또는 pose(자세 추정)\n"
        "  --decoder hw|sw   디코더 선택 (기본: hw)\n"
        "  --encoder hw|sw   인코더 선택 (기본: hw)\n"
        "  --output-prefix   로그 파일 접두사 (기본: result/pipeline_ch0)\n"
        "  --help            이 도움말 출력\n\n"
        "예시:\n"
        "  # HW 디코더 + HW 인코더 (기본, VPU 사용)\n"
        "  %s --input rtsp://... --model model/YoloV5N.dxnn\n\n"
        "  # SW 디코더 비교 벤치마크 (CPU 디코딩)\n"
        "  %s --decoder sw --input rtsp://... --model model/YoloV5N.dxnn\n",
        prog, prog, prog);
}

int main(int argc, char* argv[]) {
    // ── CLI 파싱 ──────────────────────────────────
    static struct option long_opts[] = {
        {"input",      required_argument, nullptr, 'u'},
        {"model",      required_argument, nullptr, 'm'},
        {"interval",   required_argument, nullptr, 'i'},
        {"channel-id", required_argument, nullptr, 'c'},
        {"task",       required_argument, nullptr, 't'},
        {"decoder",    required_argument, nullptr, 'd'},
        {"encoder",    required_argument, nullptr, 'e'},
        {"output-prefix", required_argument, nullptr, 'o'},
        {"help",       no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:m:i:c:t:d:e:o:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'u': g_rtsp_input = optarg; break;
            case 'm': g_model_path = optarg; break;
            case 'i': g_infer_interval = std::max(1, atoi(optarg)); break;
            case 'c': g_channel_id = atoi(optarg); break;
            case 't': 
                if (std::string(optarg) == "pose") g_task_type = "pose";
                else g_task_type = "det";
                break;
            case 'd':
                if (std::string(optarg) == "sw") g_use_hw_decoder = false;
                else if (std::string(optarg) == "hw") g_use_hw_decoder = true;
                else { fprintf(stderr, "❌ --decoder 옵션: hw 또는 sw만 가능\n"); return 1; }
                break;
            case 'e':
                if (std::string(optarg) == "sw") g_use_hw_encoder = false;
                else if (std::string(optarg) == "hw") g_use_hw_encoder = true;
                else { fprintf(stderr, "❌ --encoder 옵션: hw 또는 sw만 가능\n"); return 1; }
                break;
            case 'o': g_output_prefix = optarg; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    setbuf(stdout, NULL);

    // ── GStreamer 초기화 ──────────────────────────
    gst_init(&argc, &argv);

    // ── 플러그인 탐지 ──────────────────────────────
    PluginInfo plugins = detect_plugins();
    print_plugin_info(plugins);

    // HW 디코더/인코더가 없는데 hw 모드 요청한 경우 → 에러
    if (g_use_hw_decoder && plugins.hw_decoder.empty()) {
        fprintf(stderr, "❌ HW 디코더 플러그인을 찾을 수 없습니다. --decoder sw 를 사용하세요.\n");
        return 1;
    }
    if (g_use_hw_encoder && plugins.hw_encoder.empty()) {
        fprintf(stderr, "❌ HW 인코더 플러그인을 찾을 수 없습니다. --encoder sw 를 사용하세요.\n");
        return 1;
    }

    // 실제 사용할 디코더/인코더 결정
    std::string decoder_element = g_use_hw_decoder ? plugins.hw_decoder : plugins.sw_decoder;
    std::string encoder_element = g_use_hw_encoder ? plugins.hw_encoder : plugins.sw_encoder;
    std::string decoder_desc = g_use_hw_decoder ? plugins.hw_decoder_desc : plugins.sw_decoder_desc;
    std::string encoder_desc = g_use_hw_encoder ? plugins.hw_encoder_desc : plugins.sw_encoder_desc;

    fprintf(stderr, "🔧 [CH%d] 설정:\n"
        "   입력:    %s\n"
        "   모델:    %s\n"
        "   Task:    %s\n"
        "   간격:    %d프레임/추론\n"
        "   디코더:  %s (%s) %s\n"
        "   인코더:  %s (%s) %s\n\n",
        g_channel_id, g_rtsp_input.c_str(), g_model_path.c_str(), g_task_type.c_str(), g_infer_interval,
        decoder_element.c_str(), decoder_desc.c_str(),
        g_use_hw_decoder ? "✅" : "⚠️ CPU 기반 비교 벤치마크",
        encoder_element.c_str(), encoder_desc.c_str(),
        g_use_hw_encoder ? "✅" : "⚠️ CPU 기반 비교 벤치마크");

    // SW 디코더 사용 시 경고 메시지
    if (!g_use_hw_decoder) {
        fprintf(stderr,
            "╔══════════════════════════════════════════════════════════════╗\n"
            "║ ⚠️  SW 디코더 모드: CPU가 디코딩을 수행합니다.               ║\n"
            "║     이 모드는 HW 디코더와의 성능 비교 벤치마크 용도입니다.    ║\n"
            "║     프로덕션 환경에서는 반드시 HW 디코더를 사용하세요.        ║\n"
            "╚══════════════════════════════════════════════════════════════╝\n\n");
    }

    // ── NPU 모델 로딩 ─────────────────────────────
    fprintf(stderr, "📦 [CH%d] 모델 로딩: %s\n", g_channel_id, g_model_path.c_str());
    auto engine = std::make_unique<dxrt::InferenceEngine>(g_model_path.c_str());
    if (!engine) {
        fprintf(stderr, "❌ [CH%d] 모델 로딩 실패\n", g_channel_id);
        return -1;
    }
    fprintf(stderr, "✅ [CH%d] 모델 로딩 완료\n", g_channel_id);

    // ── 디코딩 파이프라인 구성 (NV12 패스스루 + Tee 분기) ──────
    // 디코더가 NV12를 네이티브로 출력 (H264 기본 포맷, 색변환 0회!)
    // 분기 1 (sink_full): NV12 그대로 OSD 후 인코더에 전달 (CPU 변환 없음)
    // 분기 2 (sink_infer): videoconvert로 RGB 변환 + videoscale로 640x640 (NPU 전용)
    std::string decoder_only = "! " + decoder_element + " ";

    std::ostringstream decode_pipe;
    decode_pipe << "rtspsrc location=\"" << g_rtsp_input << "\" latency=0 protocols=tcp "
                << "! rtph264depay ! h264parse "
                << decoder_only
                << "! tee name=t "
                << "t. ! queue max-size-buffers=2 leaky=2 ! video/x-raw,format=NV12 ! appsink name=sink_full emit-signals=false drop=true max-buffers=2 sync=false "
                << "t. ! queue name=infer_queue max-size-buffers=1 leaky=1 ! videoconvert ! videoscale ! video/x-raw,width=" << INPUT_W << ",height=" << INPUT_H << ",format=RGB ! appsink name=sink_infer emit-signals=false drop=true max-buffers=1 sync=false";

    fprintf(stderr, "🔗 [CH%d] 디코딩 파이프라인 (Tee 아키텍처):\n   %s\n\n", g_channel_id, decode_pipe.str().c_str());

    GError* err = nullptr;
    GstElement* decode_pipeline = gst_parse_launch(decode_pipe.str().c_str(), &err);
    if (err) {
        fprintf(stderr, "❌ 디코딩 파이프라인 생성 실패: %s\n", err->message);
        g_error_free(err);
        return -1;
    }

    // --- Pad Probe 연결 (videoscale 과부하 방지) ---
    GstElement* infer_queue = gst_bin_get_by_name(GST_BIN(decode_pipeline), "infer_queue");
    if (infer_queue) {
        GstPad* pad = gst_element_get_static_pad(infer_queue, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, drop_probe_cb, NULL, NULL);
        gst_object_unref(pad);
        gst_object_unref(infer_queue);
    } else {
        fprintf(stderr, "⚠️ [CH%d] infer_queue를 찾을 수 없습니다. Pad Probe 기능이 작동하지 않습니다.\n", g_channel_id);
    }

    GstElement* app_sink_full = gst_bin_get_by_name(GST_BIN(decode_pipeline), "sink_full");
    GstElement* app_sink_infer = gst_bin_get_by_name(GST_BIN(decode_pipeline), "sink_infer");
    if (!app_sink_full || !app_sink_infer) {
        fprintf(stderr, "❌ appsink(sink_full 또는 sink_infer)를 찾을 수 없습니다\n");
        gst_object_unref(decode_pipeline);
        return -1;
    }

    // ── 인코딩 파이프라인 구성 ────────────────────
    // appsrc → Convert → Encode → H264 Parse → fdsink(stdout)
    // 인코더는 원본 해상도(RTSP 스트림)로 인코딩해야 하므로,
    // 원본 해상도를 알기 전에는 생성할 수 없음.
    // → 첫 프레임 수신 후 지연 생성 (lazy init)
    GstElement* encode_pipeline = nullptr;
    GstElement* appsrc_elem = nullptr;
    bool encode_inited = false;
    int stream_w = 0, stream_h = 0;  // 원본 해상도 (첫 프레임에서 결정할 수 없으므로 NPU 입력 크기 사용)

    size_t last_slash = g_output_prefix.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string dir = g_output_prefix.substr(0, last_slash);
        std::string cmd = "mkdir -p " + dir;
        (void)!system(cmd.c_str());
    }

    // ── 파이프라인 프리롤 (READY → PAUSED → PLAYING) ───────
    // RTSP 연결, 디코더 초기화, caps 협상을 미리 완료하여
    // 타이밍 측정에서 초기화 오버헤드를 제거합니다.
    fprintf(stderr, "⏳ [CH%d] 파이프라인 프리롤 중 (READY → PAUSED)...\n", g_channel_id);
    gst_element_set_state(decode_pipeline, GST_STATE_READY);

    GstStateChangeReturn ret_state = gst_element_set_state(decode_pipeline, GST_STATE_PAUSED);
    if (ret_state == GST_STATE_CHANGE_ASYNC) {
        // RTSP 소스는 비동기로 PAUSED 전환 — 최대 10초 대기
        GstState state;
        ret_state = gst_element_get_state(decode_pipeline, &state, nullptr, 10 * GST_SECOND);
        if (ret_state == GST_STATE_CHANGE_SUCCESS || ret_state == GST_STATE_CHANGE_NO_PREROLL) {
            fprintf(stderr, "✅ [CH%d] 프리롤 완료 (PAUSED 도달)\n", g_channel_id);
        } else {
            fprintf(stderr, "⚠️ [CH%d] 프리롤 타임아웃 — PLAYING으로 직접 전환\n", g_channel_id);
        }
    }

    gst_element_set_state(decode_pipeline, GST_STATE_PLAYING);
    g_stats.begin();  // 프리롤 이후부터 타이밍 측정 시작
    fprintf(stderr, "🚀 [CH%d] 파이프라인 시작 (interval=%d, NV12 패스스루 + 비동기 멀티스레드)\n", g_channel_id, g_infer_interval);
    fprintf(stderr, "📊 [CH%d] Thread A: 디스플레이/인코딩 (30fps) | Thread B: NPU 추론 (RunAsync+Wait)\n\n", g_channel_id);

    // ── 비동기 멀티스레드 아키텍처 ─────────────────
    // 첫 프레임에서 원본 해상도를 알아내야 infer 스레드에 전달 가능.
    // 잠시 동기적으로 첫 프레임을 가져와서 해상도 감지.
    int orig_w = 0, orig_h = 0;
    {
        GstSample* first_sample = nullptr;
        while (g_running && !first_sample) {
            first_sample = gst_app_sink_try_pull_sample(
                GST_APP_SINK(app_sink_full), 1000 * GST_MSECOND);
        }
        if (first_sample) {
            GstCaps* fcaps = gst_sample_get_caps(first_sample);
            GstVideoInfo fvinfo;
            if (gst_video_info_from_caps(&fvinfo, fcaps)) {
                orig_w = GST_VIDEO_INFO_WIDTH(&fvinfo);
                orig_h = GST_VIDEO_INFO_HEIGHT(&fvinfo);
                fprintf(stderr, "📐 [CH%d] 원본 해상도 감지: %dx%d\n", g_channel_id, orig_w, orig_h);
            }
            gst_sample_unref(first_sample);
        }
        if (orig_w == 0 || orig_h == 0) {
            fprintf(stderr, "❌ [CH%d] 첫 프레임 해상도 감지 실패\n", g_channel_id);
            gst_element_set_state(decode_pipeline, GST_STATE_NULL);
            gst_object_unref(decode_pipeline);
            gst_object_unref(app_sink_full);
            gst_object_unref(app_sink_infer);
            return -1;
        }
    }

    // Thread A: 디스플레이 & 인코딩 (30 FPS)
    std::thread t_display(display_thread_func,
        app_sink_full,
        std::ref(appsrc_elem),
        std::ref(encode_pipeline),
        std::ref(encode_inited),
        std::ref(stream_w), std::ref(stream_h),
        encoder_element);

    // Thread B: NPU 추론 (Pad Probe에 의해 throttle된 속도)
    std::thread t_infer(infer_thread_func,
        app_sink_infer,
        engine.get(),
        orig_w, orig_h);

    // ── Main Thread: 감독 (SIGINT 대기) ──────────
    fprintf(stderr, "🎯 [CH%d] 메인 스레드: 감독 모드 (Ctrl+C로 종료)\n", g_channel_id);
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 스레드 종료 대기
    fprintf(stderr, "⏳ [CH%d] 스레드 종료 대기...\n", g_channel_id);
    t_display.join();
    t_infer.join();
    fprintf(stderr, "✅ [CH%d] 모든 스레드 종료 완료\n", g_channel_id);

    // ── 최종 통계 출력 및 저장 ──────────────────
    g_stats.print_periodic();

    auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now_c));
    std::string summary_path = g_output_prefix + ".log";
    std::string csv_path     = g_output_prefix + ".csv";
    g_stats.save_summary(summary_path);
    g_stats.save_csv(csv_path);

    // ── 리소스 해제 ───────────────────────────────
    fprintf(stderr, "✅ [CH%d] 종료 완료\n", g_channel_id);

    gst_element_set_state(decode_pipeline, GST_STATE_NULL);
    if (encode_pipeline) {
        gst_element_set_state(encode_pipeline, GST_STATE_NULL);
    }

    gst_object_unref(decode_pipeline);
    gst_object_unref(app_sink_full);
    gst_object_unref(app_sink_infer);
    if (appsrc_elem) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_elem));
        // EOS가 전파될 때까지 잠시 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        gst_object_unref(appsrc_elem);
    }
    if (encode_pipeline) { // This check is redundant if encode_pipeline is unref'd above
        gst_object_unref(encode_pipeline);
    }

    gst_deinit();

    fprintf(stderr, "✅ [CH%d] 종료 완료\n", g_channel_id);
    return 0;
}
