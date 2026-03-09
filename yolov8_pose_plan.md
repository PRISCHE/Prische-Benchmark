# YOLOv8 Pose 통합 계획서 (Tensor 파싱 및 NV12 OSD 렌더링)

본 문서는 현재 Gstbenchmark 파이프라인에서 `YoloV5N.dxnn` (객체 탐지) 모델을 `yolov8l_pose.dxnn` (자세 추정) 모델로 교체할 때 필요한 **C++ 코드 (`main.cpp`) 변경 플랜**입니다. GStreamer 파이프라인 아키텍처 자체는 수정할 필요 없이, 아래 두 가지 구성 요소만 교체/추가하면 됩니다.

---

## 1. Tensor 파싱 로직 변경 (YOLOv5 -> YOLOv8 Pose)

### 1.1. Tensor 출력 형태(Shape) 비교
*   **YOLOv5 (현재)**: `[1, 25200, 85]` 
    *   `85` = 4 (Box) + 1 (Objectness) + 80 (Class Probabilities)
*   **YOLOv8 Pose (대상)**: 보통 `[1, 56, 8400]` 또는 `[1, 8400, 56]` 형태로 출력됩니다.
    *   `56` = 4 (Box cx, cy, w, h) + 1 (Person Class Score) + 51 (17 Keypoints * [x, y, confidence])
    *   주의: YOLOv8은 Objectness 점수가 따로 없고, 곧바로 Class Score가 나옵니다.

### 1.2. 데이터 구조체 확장
기존 `YoloBox` 구조체에 17개의 관절(Keypoint) 데이터를 담을 수 있도록 확장해야 합니다.

```cpp
// 17개의 관절(Keypoint)을 담기 위한 구조체
struct KeyPoint {
    float x, y, conf;
};

struct YoloBox {
    float x1, y1, x2, y2, score;
    int cls;
    std::string class_name;
    // --- 추가되는 부분 ---
    KeyPoint kpts[17]; 
};
```

### 1.3. 파싱 함수 설계 (`post_process_yolov8_pose`)
배열에서 56개의 값을 순회하며 Box와 17개 Keypoint 좌표를 추출합니다.

```cpp
static std::vector<YoloBox> post_process_yolov8_pose(
    const std::vector<std::shared_ptr<dxrt::Tensor>>& outputs,
    int orig_w, int orig_h)
{
    std::vector<YoloBox> boxes;
    const float* data = (const float*)outputs[0]->data();
    
    float scale_x = (float)INPUT_W / orig_w;
    float scale_y = (float)INPUT_H / orig_h;

    // 예시: [1, 8400, 56] 형태라고 가정 (Transpose 된 상태)
    int num_anchors = 8400;
    
    for (int i = 0; i < num_anchors; ++i) {
        const float* row = data + i * 56;
        float class_score = row[4]; // Person 클래스 점수
        
        if (class_score < CONF_THRESH) continue;

        YoloBox b;
        b.cls = 0; // Person
        b.score = class_score;
        
        // 1. Box 파싱 (cx, cy, w, h)
        float cx = row[0], cy = row[1], w = row[2], h = row[3];
        b.x1 = (cx - w/2) / scale_x;
        b.y1 = (cy - h/2) / scale_y;
        b.x2 = (cx + w/2) / scale_x;
        b.y2 = (cy + h/2) / scale_y;

        // 2. 17개 Keypoints 파싱 (x, y, conf)
        for (int k = 0; k < 17; ++k) {
            b.kpts[k].x    = row[5 + k*3 + 0] / scale_x;
            b.kpts[k].y    = row[5 + k*3 + 1] / scale_y;
            b.kpts[k].conf = row[5 + k*3 + 2];
        }
        boxes.push_back(b);
    }
    
    // 이 후 NMS(Non-Maximum Suppression)를 수행하여 중복 뼈대 제거
    return apply_nms(boxes);
}
```

---

## 2. NV12 OSD 렌더링 함수 추가

현재 파이프라인의 핵심인 **NV12 패스스루**를 유지하려면, 관절(Point)과 뼈대(Line) 역시 NV12 버퍼상에서 Y, U, V 플레인에 직접 수학적으로 계산하여 그려 넣어야 합니다.

### 2.1. YUV 픽셀 쓰기 헬퍼 (초기화)
```cpp
// 점 하나를 NV12 포맷(Y/UV)에 찍는 람다 (기존 draw_rect_nv12의 로직을 함수화)
auto draw_point_nv12 = [&](uint8_t* nv12_data, int w, int h, int stride, 
                           int x, int y, uint8_t yv, uint8_t uv, uint8_t vv, int radius=3) {
    // x,y 중심으로 반경 radius만큼 순회하며 Y/UV 플레인에 점 찍기
    // (범위 클램핑 및 최적화된 루프 작성 필요)
};
```

### 2.2. 브레즌햄(Bresenham) 알고리즘 기반 선 긋기
관절과 관절 사이를 잇는 뼈대를 그리기 위해, CPU에서 두 점 사이의 픽셀 좌표를 계산하는 선 긋기 알고리즘이 필요합니다.

```cpp
static void draw_line_nv12(uint8_t* nv12_data, int img_w, int img_h, int stride,
                           int x0, int y0, int x1, int y1,
                           uint8_t r, uint8_t g, uint8_t b, int thickness = 2)
{
    // RGB -> YUV 변환 (미리 계산)
    uint8_t yv = (uint8_t)(0.299*r + 0.587*g + 0.114*b);
    uint8_t uv = (uint8_t)(-0.169*r - 0.331*g + 0.500*b + 128);
    uint8_t vv = (uint8_t)(0.500*r - 0.419*g - 0.081*b + 128);

    // 알고리즘: Bresenham's line algorithm
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
    int err = dx + dy, e2;

    while (true) {
        // 현재 점(x0, y0)에 두께(thickness)만큼 점 찍기
        draw_point_nv12(nv12_data, img_w, img_h, stride, x0, y0, yv, uv, vv, thickness);
        
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
```

### 2.3. 인체 스켈레톤(Skeleton) 연결 렌더링
`display_thread_func` 내에서 NMS가 완료된 Box 리스트를 받아 루프를 돌며, 뼈대 연결망 배열(예: COCO 17 KPT Topology)을 참조하여 선을 긋습니다.

```cpp
// COCO 17 Keypoints 관절 연결 쌍 설정
const int SKELETON[][2] = {
    {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, 
    {5, 11}, {6, 12}, {5, 6}, {5, 7}, {6, 8}, {7, 9}, 
    {8, 10}, {1, 2}, {0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 6}
};

// ...루프 내부...
for (const auto& b : local_boxes) {
    // 1. 사람 바운딩 박스 그리기
    draw_rect_nv12(nv12_data, w, h, stride, b.x1, b.y1, b.w, b.h, 255, 0, 0, 2);

    // 2. 뼈대(Line) 그리기
    for (auto& bone : SKELETON) {
        const auto& p1 = b.kpts[bone[0]];
        const auto& p2 = b.kpts[bone[1]];
        // 둘 다 신뢰도가 일정 이상일 때만 선을 긋는다
        if (p1.conf > 0.5f && p2.conf > 0.5f) {
            draw_line_nv12(nv12_data, w, h, stride, p1.x, p1.y, p2.x, p2.y, 0, 255, 0, 2);
        }
    }
    
    // 3. 관절(Point) 그리기
    for (int k = 0; k < 17; ++k) {
        if (b.kpts[k].conf > 0.5f) {
            draw_point_nv12(nv12_data, w, h, stride, b.kpts[k].x, b.kpts[k].y, 255, 0, 0, 3);
        }
    }
}
```

---

## 요약
YOLOv8 Pose를 도입하려면 위에서 명시된 데이터 구조와 파서, 그리고 선/점 그리기 수학 알고리즘을 추가해야 합니다. NV12 색상 최적화를 그대로 유지한 상태에서 뼈대가 예쁘게 출력될 것입니다.
