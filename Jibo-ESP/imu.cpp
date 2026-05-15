#include "imu.h"
#include "pin_config.h"
#include "storage.h"
#include "log.h"
#include <Wire.h>
#include <math.h>
#include <lvgl.h>
#include "SensorQMI8658.hpp"

#define QMI_ADDR  0x6B

static SensorQMI8658 qmi;
static bool ready = false;

// Raw sensor readings
static float gx, gy, gz;     // gyro deg/s
static float ax, ay, az;     // accel m/s²

// Accelerometer bias calibration
static float calBiasX = 0, calBiasY = 0, calBiasZ = 0;
static bool  calLoaded = false;

// Calibration accumulator
static bool     calRunning = false;
static float    calSumX, calSumY, calSumZ;
static uint32_t calCount;
#define CAL_SAMPLES 500

// Fused orientation (complementary filter)
static float pitch = 0.0f;   // degrees, nose up positive
static float roll  = 0.0f;   // degrees, right wing down positive
static float yaw   = 0.0f;   // degrees, cumulative from start

static uint32_t lastPollUs = 0;

#define COMP_ALPHA  0.98f


// ─── Sensor API ─────────────────────────────────────────────────────────────

bool imu_init() {
    if (ready) return true;

    if (!qmi.begin(Wire, QMI_ADDR, IIC_SDA, IIC_SCL)) {
        LOG1("[imu] QMI8658 not found at 0x%02X\n", QMI_ADDR);
        return false;
    }

    qmi.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_125Hz,
        SensorQMI8658::LPF_MODE_0
    );
    qmi.configGyroscope(
        SensorQMI8658::GYR_RANGE_256DPS,
        SensorQMI8658::GYR_ODR_112_1Hz,
        SensorQMI8658::LPF_MODE_0
    );
    qmi.enableAccelerometer();
    qmi.enableGyroscope();

    pitch = roll = yaw = 0.0f;
    gx = gy = gz = 0.0f;
    ax = ay = az = 0.0f;
    lastPollUs = micros();

    if (!calLoaded) {
        calLoaded = storage_get_imu_cal(calBiasX, calBiasY, calBiasZ);
        if (calLoaded)
            LOG1("[imu] loaded cal bias: %.3f %.3f %.3f\n", calBiasX, calBiasY, calBiasZ);
    }

    ready = true;
    LOG1("[imu] QMI8658 ready (accel 4G + gyro 256 DPS)\n");
    return true;
}

void imu_deinit() {
    if (!ready) return;
    qmi.disableGyroscope();
    qmi.disableAccelerometer();
    ready = false;
    gx = gy = gz = 0.0f;
    LOG1("[imu] sensors disabled\n");
}

void imu_poll() {
    if (!ready) return;
    if (!qmi.getDataReady()) return;

    qmi.getAccelerometer(ax, ay, az);
    if (qmi.getGyroscope(gx, gy, gz) == 0) return;

    // Accumulate raw samples during calibration (before bias correction)
    if (calRunning) {
        calSumX += ax;
        calSumY += ay;
        calSumZ += az;
        calCount++;
    }

    ax -= calBiasX;
    ay -= calBiasY;
    az -= calBiasZ;

    uint32_t nowUs = micros();
    float dt = (nowUs - lastPollUs) / 1000000.0f;
    if (dt <= 0.0f || dt > 0.2f) { lastPollUs = nowUs; return; }
    lastPollUs = nowUs;

    float accelPitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * (180.0f / M_PI);
    float accelRoll  = atan2f(ay, az) * (180.0f / M_PI);

    pitch = COMP_ALPHA * (pitch + gy * dt) + (1.0f - COMP_ALPHA) * accelPitch;
    roll  = COMP_ALPHA * (roll  + gx * dt) + (1.0f - COMP_ALPHA) * accelRoll;

    yaw += gz * dt;
    yaw = fmodf(yaw, 360.0f);
    if (yaw < 0.0f) yaw += 360.0f;
}

float imu_get_yaw_rate() {
    return gz;
}

bool imu_is_ready() {
    return ready;
}

void imu_get_orientation(float &p, float &r, float &y) {
    p = pitch; r = roll; y = yaw;
}

void imu_get_accel_raw(float &ox, float &oy, float &oz) {
    ox = ax; oy = ay; oz = az;
}

void imu_get_gyro_raw(float &ox, float &oy, float &oz) {
    ox = gx; oy = gy; oz = gz;
}

bool imu_is_calibrated() {
    return calLoaded;
}

void imu_cal_start() {
    calSumX = calSumY = calSumZ = 0;
    calCount = 0;
    calRunning = true;
    LOG1("[imu] calibration started — collecting %d samples\n", CAL_SAMPLES);
}

bool imu_cal_tick() {
    if (!calRunning) return true;
    if (calCount < CAL_SAMPLES) return false;

    calRunning = false;
    float n = (float)calCount;
    calBiasX = calSumX / n;
    calBiasY = calSumY / n;
    calBiasZ = calSumZ / n - 9.81f;
    calLoaded = true;

    storage_set_imu_cal(calBiasX, calBiasY, calBiasZ);
    LOG1("[imu] calibration done: bias %.4f %.4f %.4f (%u samples)\n",
         calBiasX, calBiasY, calBiasZ, calCount);
    return true;
}

void imu_cal_abort() {
    calRunning = false;
}

// ─── On-screen 3D debug visualization ───────────────────────────────────────

static const int16_t SCR_W = LCD_WIDTH;
static const int16_t SCR_H = LCD_HEIGHT;

#define DBG_CANVAS_SZ  400

static lv_obj_t   *dbgContainer = NULL;
static lv_obj_t   *dbgCanvas    = NULL;
static lv_color_t *dbgBuf       = NULL;
static lv_obj_t   *dbgPitchLbl  = NULL;
static lv_obj_t   *dbgRollLbl   = NULL;
static lv_obj_t   *dbgYawLbl    = NULL;
static lv_obj_t   *dbgAccelLbl  = NULL;
static lv_obj_t   *dbgTitleLbl  = NULL;

static void project_pt(float x, float y, float z,
                       float cp, float sp, float cr, float sr,
                       float cy, float sy,
                       int &sx, int &sy_out) {
    // Yaw rotation (around vertical axis)
    float x1 =  x * cy + z * sy;
    float z1 = -x * sy + z * cy;
    float y1 =  y;
    // Pitch rotation
    float y2 =  y1 * cp - z1 * sp;
    float z2 =  y1 * sp + z1 * cp;
    float x2 =  x1;
    // Roll rotation
    float x3 =  x2 * cr - y2 * sr;
    float y3 =  x2 * sr + y2 * cr;
    (void)z2;
    // Orthographic projection centered on canvas
    sx = DBG_CANVAS_SZ / 2 + (int)(x3 * 100.0f);
    sy_out = DBG_CANVAS_SZ / 2 + (int)(y3 * 100.0f);
}

static void draw_line(lv_obj_t *canvas, int x0, int y0, int x1, int y1,
                      lv_color_t color, lv_coord_t width) {
    lv_point_t pts[2] = {{(lv_coord_t)x0, (lv_coord_t)y0},
                         {(lv_coord_t)x1, (lv_coord_t)y1}};
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa   = LV_OPA_COVER;
    lv_canvas_draw_line(canvas, pts, 2, &dsc);
}

void imu_debug_create_ui() {
    if (!dbgBuf) {
        dbgBuf = (lv_color_t *)ps_malloc(DBG_CANVAS_SZ * DBG_CANVAS_SZ * sizeof(lv_color_t));
        if (!dbgBuf) { LOG1("[imu-dbg] canvas alloc failed\n"); return; }
    }

    dbgContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(dbgContainer);
    lv_obj_set_size(dbgContainer, SCR_W, SCR_H);
    lv_obj_set_pos(dbgContainer, 0, 0);
    lv_obj_clear_flag(dbgContainer, LV_OBJ_FLAG_SCROLLABLE);

    dbgCanvas = lv_canvas_create(dbgContainer);
    lv_canvas_set_buffer(dbgCanvas, dbgBuf, DBG_CANVAS_SZ, DBG_CANVAS_SZ, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(dbgCanvas, LV_ALIGN_CENTER, 0, -25);
    lv_canvas_fill_bg(dbgCanvas, lv_color_black(), LV_OPA_COVER);

    dbgTitleLbl = lv_label_create(dbgContainer);
    lv_label_set_text(dbgTitleLbl, "IMU Debug");
    lv_obj_set_style_text_color(dbgTitleLbl, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(dbgTitleLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(dbgTitleLbl, LV_ALIGN_TOP_MID, 0, 30);

    // Data labels at bottom
    auto mkLbl = [&](lv_obj_t *&lbl, lv_coord_t yOff) {
        lbl = lv_label_create(dbgContainer);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, yOff);
    };
    mkLbl(dbgAccelLbl,  -20);
    mkLbl(dbgYawLbl,    -40);
    mkLbl(dbgRollLbl,   -60);
    mkLbl(dbgPitchLbl,  -80);

    LOG1("[imu-dbg] UI created\n");
}

void imu_debug_render() {
    if (!dbgCanvas || !dbgBuf) return;

    imu_poll();

    lv_canvas_fill_bg(dbgCanvas, lv_color_black(), LV_OPA_COVER);

    float p = pitch * (M_PI / 180.0f);
    float r = roll  * (M_PI / 180.0f);
    float y = yaw * (M_PI / 180.0f);
    float cp = cosf(p), sp = sinf(p);
    float cr = cosf(r), sr = sinf(r);
    float cy = cosf(y), sy = sinf(y);

    // Device wireframe: flat rectangle (wider than tall, thin)
    static const float verts[][3] = {
        {-1.2f, -0.8f, -0.15f}, { 1.2f, -0.8f, -0.15f},
        { 1.2f,  0.8f, -0.15f}, {-1.2f,  0.8f, -0.15f},
        {-1.2f, -0.8f,  0.15f}, { 1.2f, -0.8f,  0.15f},
        { 1.2f,  0.8f,  0.15f}, {-1.2f,  0.8f,  0.15f},
    };
    static const int edges[][2] = {
        {0,1},{1,2},{2,3},{3,0},  // back face
        {4,5},{5,6},{6,7},{7,4},  // front face
        {0,4},{1,5},{2,6},{3,7},  // depth edges
    };

    int px[8], py[8];
    for (int i = 0; i < 8; i++)
        project_pt(verts[i][0], verts[i][1], verts[i][2],
                   cp, sp, cr, sr, cy, sy, px[i], py[i]);

    lv_color_t cBack  = lv_color_make(80, 80, 80);
    lv_color_t cFront = lv_color_white();
    lv_color_t cDepth = lv_color_make(140, 140, 140);

    // Back face
    for (int i = 0; i < 4; i++)
        draw_line(dbgCanvas, px[edges[i][0]], py[edges[i][0]],
                  px[edges[i][1]], py[edges[i][1]], cBack, 2);
    // Front face
    for (int i = 4; i < 8; i++)
        draw_line(dbgCanvas, px[edges[i][0]], py[edges[i][0]],
                  px[edges[i][1]], py[edges[i][1]], cFront, 2);
    // Depth edges
    for (int i = 8; i < 12; i++)
        draw_line(dbgCanvas, px[edges[i][0]], py[edges[i][0]],
                  px[edges[i][1]], py[edges[i][1]], cDepth, 2);

    // Axis indicators from center (world-fixed reference)
    lv_color_t cRed   = lv_color_make(255, 60, 60);
    lv_color_t cGreen = lv_color_make(60, 255, 60);
    lv_color_t cBlue  = lv_color_make(60, 120, 255);

    int cx = DBG_CANVAS_SZ / 2, ccy = DBG_CANVAS_SZ / 2;
    int ex, ey;

    project_pt(1.8f, 0, 0, cp, sp, cr, sr, cy, sy, ex, ey);
    draw_line(dbgCanvas, cx, ccy, ex, ey, cRed, 3);

    project_pt(0, 1.8f, 0, cp, sp, cr, sr, cy, sy, ex, ey);
    draw_line(dbgCanvas, cx, ccy, ex, ey, cGreen, 3);

    project_pt(0, 0, 1.8f, cp, sp, cr, sr, cy, sy, ex, ey);
    draw_line(dbgCanvas, cx, ccy, ex, ey, cBlue, 3);

    // Axis labels
    auto putLabel = [&](float lx, float ly, float lz, const char *txt, lv_color_t c) {
        int lsx, lsy;
        project_pt(lx, ly, lz, cp, sp, cr, sr, cy, sy, lsx, lsy);
        lv_point_t lpt = {(lv_coord_t)(lsx - 4), (lv_coord_t)(lsy - 6)};
        lv_draw_label_dsc_t ldsc;
        lv_draw_label_dsc_init(&ldsc);
        ldsc.color = c;
        ldsc.font  = &lv_font_montserrat_14;
        lv_canvas_draw_text(dbgCanvas, lpt.x, lpt.y, 30, &ldsc, txt);
    };
    putLabel(2.1f, 0, 0, "X", cRed);
    putLabel(0, 2.1f, 0, "Y", cGreen);
    putLabel(0, 0, 2.1f, "Z", cBlue);

    lv_obj_invalidate(dbgCanvas);

    // Update data labels
    char buf[64];
    snprintf(buf, sizeof(buf), "Pitch: %+.1f\xC2\xB0", pitch);
    lv_label_set_text(dbgPitchLbl, buf);
    snprintf(buf, sizeof(buf), "Roll:  %+.1f\xC2\xB0", roll);
    lv_label_set_text(dbgRollLbl, buf);
    snprintf(buf, sizeof(buf), "Yaw:   %+.1f\xC2\xB0", yaw);
    lv_label_set_text(dbgYawLbl, buf);
    snprintf(buf, sizeof(buf), "Accel: %.1f %.1f %.1f", ax, ay, az);
    lv_label_set_text(dbgAccelLbl, buf);
}

void imu_debug_destroy_ui() {
    if (dbgContainer) { lv_obj_del(dbgContainer); dbgContainer = NULL; }
    dbgCanvas = NULL;
    dbgPitchLbl = dbgRollLbl = dbgYawLbl = dbgAccelLbl = dbgTitleLbl = NULL;
    LOG1("[imu-dbg] UI destroyed\n");
}
