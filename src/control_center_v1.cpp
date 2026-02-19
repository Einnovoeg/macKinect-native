#include <libfreenect.h>
#include <libfreenect_audio.h>
#include <libfreenect_registration.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#if defined(__APPLE__)
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

namespace {

constexpr int kFrameWidth = 640;
constexpr int kFrameHeight = 480;
constexpr int kFramePixels = kFrameWidth * kFrameHeight;
constexpr int kFrameRgbBytes = kFramePixels * 3;

pthread_t g_freenect_thread;
volatile int g_die = 0;

int g_argc = 0;
char **g_argv = nullptr;
int g_window = 0;
int g_window_width = 1280;
int g_window_height = 640;

pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_frame_cond = PTHREAD_COND_INITIALIZER;

uint16_t *g_depth_mm_mid = nullptr;
uint16_t *g_depth_mm_front = nullptr;
uint8_t *g_depth_rgb_mid = nullptr;
uint8_t *g_depth_rgb_front = nullptr;

uint8_t *g_rgb_back = nullptr;
uint8_t *g_rgb_mid = nullptr;
uint8_t *g_rgb_front = nullptr;

int g_got_rgb = 0;
int g_got_depth = 0;
std::uint64_t g_rgb_frames = 0;
std::uint64_t g_depth_frames = 0;

GLuint g_gl_depth_tex = 0;
GLuint g_gl_rgb_tex = 0;

freenect_context *g_ctx = nullptr;
freenect_device *g_dev = nullptr;

int g_freenect_angle = 0;
freenect_led_options g_led_mode = LED_GREEN;

freenect_video_format g_requested_video_format = FREENECT_VIDEO_RGB;
freenect_video_format g_current_video_format = FREENECT_VIDEO_RGB;
freenect_depth_format g_requested_depth_format = FREENECT_DEPTH_REGISTERED;
freenect_depth_format g_current_depth_format = FREENECT_DEPTH_REGISTERED;

freenect_flag_value g_auto_exposure = FREENECT_ON;
freenect_flag_value g_auto_white_balance = FREENECT_ON;
freenect_flag_value g_mirror = FREENECT_ON;
freenect_flag_value g_near_mode = FREENECT_OFF;
int g_manual_exposure_us = 33333;
int g_ir_brightness = 20;
bool g_audio_stream_available = false;

std::mutex g_status_mutex;
std::string g_status_message = "Ready.";

std::mutex g_audio_mutex;
bool g_audio_recording = false;
double g_audio_level = 0.0;

struct WavSink {
  std::FILE *file = nullptr;
  std::string path;
  int bits_per_sample = 0;
  std::uint64_t sample_count = 0;
};

WavSink g_mic_wavs[4];
WavSink g_cancelled_wav;

std::string TimestampNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &t);
#else
  localtime_r(&t, &tm_snapshot);
#endif

  std::ostringstream out;
  out << std::put_time(&tm_snapshot, "%Y%m%d-%H%M%S");
  return out.str();
}

void SetStatus(const std::string &message) {
  std::lock_guard<std::mutex> lock(g_status_mutex);
  g_status_message = message;
  std::cout << "[status] " << message << "\n";
}

std::string GetStatus() {
  std::lock_guard<std::mutex> lock(g_status_mutex);
  return g_status_message;
}

const char *VideoFormatLabel(freenect_video_format format) {
  switch (format) {
    case FREENECT_VIDEO_RGB:
      return "RGB";
    case FREENECT_VIDEO_YUV_RGB:
      return "YUV->RGB";
    case FREENECT_VIDEO_IR_8BIT:
      return "IR8";
    default:
      return "Other";
  }
}

const char *DepthFormatLabel(freenect_depth_format format) {
  switch (format) {
    case FREENECT_DEPTH_REGISTERED:
      return "REGISTERED(mm)";
    case FREENECT_DEPTH_MM:
      return "MM";
    case FREENECT_DEPTH_11BIT:
      return "RAW11";
    default:
      return "Other";
  }
}

const char *OnOffLabel(freenect_flag_value value) {
  return value == FREENECT_ON ? "on" : "off";
}

void PrintHelp() {
  std::cout << "\nKinect Control Center (v1) controls\n"
            << "  ESC: quit\n"
            << "  w/x: tilt up/down 2 degrees\n"
            << "  s: center tilt\n"
            << "  0..6: LED mode\n"
            << "  v: cycle video mode (RGB/YUV)\n"
            << "  d: cycle depth mode (REGISTERED/MM)\n"
            << "  m: toggle mirror\n"
            << "  e: toggle auto exposure/flicker/white-balance\n"
            << "  b: toggle auto white-balance only\n"
            << "  n: toggle near mode\n"
            << "  [: decrease manual exposure by 1ms\n"
            << "  ]: increase manual exposure by 1ms\n"
            << "  -/=: decrease/increase IR brightness\n"
            << "  a: start/stop microphone recording (WAV)\n"
            << "  c: capture color+depth+point cloud\n"
            << "  h: print this help\n\n";
}

void PutLe16(std::uint8_t *dst, std::uint16_t value) {
  dst[0] = static_cast<std::uint8_t>(value & 0xff);
  dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

void PutLe32(std::uint8_t *dst, std::uint32_t value) {
  dst[0] = static_cast<std::uint8_t>(value & 0xff);
  dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
  dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
  dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

bool WriteWavHeader(std::FILE *file, int sample_rate, int bits_per_sample, std::uint32_t data_bytes) {
  if (file == nullptr) {
    return false;
  }

  std::uint8_t header[44];
  std::memset(header, 0, sizeof(header));

  const std::uint16_t channels = 1;
  const std::uint32_t byte_rate = static_cast<std::uint32_t>(sample_rate * channels * bits_per_sample / 8);
  const std::uint16_t block_align = static_cast<std::uint16_t>(channels * bits_per_sample / 8);
  const std::uint32_t chunk_size = 36 + data_bytes;

  std::memcpy(header + 0, "RIFF", 4);
  PutLe32(header + 4, chunk_size);
  std::memcpy(header + 8, "WAVE", 4);
  std::memcpy(header + 12, "fmt ", 4);
  PutLe32(header + 16, 16);
  PutLe16(header + 20, 1);
  PutLe16(header + 22, channels);
  PutLe32(header + 24, static_cast<std::uint32_t>(sample_rate));
  PutLe32(header + 28, byte_rate);
  PutLe16(header + 32, block_align);
  PutLe16(header + 34, static_cast<std::uint16_t>(bits_per_sample));
  std::memcpy(header + 36, "data", 4);
  PutLe32(header + 40, data_bytes);

  std::fseek(file, 0, SEEK_SET);
  return std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

void CloseWavSink(WavSink *sink) {
  if (sink == nullptr || sink->file == nullptr) {
    return;
  }

  const std::uint32_t bytes_per_sample = static_cast<std::uint32_t>(sink->bits_per_sample / 8);
  const std::uint64_t bytes_u64 = sink->sample_count * bytes_per_sample;
  const std::uint32_t data_bytes = bytes_u64 > std::numeric_limits<std::uint32_t>::max()
                                       ? std::numeric_limits<std::uint32_t>::max()
                                       : static_cast<std::uint32_t>(bytes_u64);

  WriteWavHeader(sink->file, 16000, sink->bits_per_sample, data_bytes);
  std::fclose(sink->file);
  sink->file = nullptr;
}

void StopAudioRecordingLocked() {
  for (WavSink &sink : g_mic_wavs) {
    CloseWavSink(&sink);
  }
  CloseWavSink(&g_cancelled_wav);
  g_audio_recording = false;
}

bool StartAudioRecordingLocked() {
  const std::string stamp = TimestampNow();
  const std::string base_dir = "captures/audio/" + stamp;
  
  // Create directories recursively
  mkdir("captures", 0755);
  mkdir("captures/audio", 0755);
  mkdir(base_dir.c_str(), 0755);

  auto open_sink = [&](WavSink *sink, const std::string &name, int bits_per_sample) -> bool {
    std::string filepath = base_dir + "/" + name;
    sink->path = filepath;
    sink->file = std::fopen(sink->path.c_str(), "wb+");
    sink->bits_per_sample = bits_per_sample;
    sink->sample_count = 0;
    if (sink->file == nullptr) {
      return false;
    }
    return WriteWavHeader(sink->file, 16000, bits_per_sample, 0);
  };

  const bool opened = open_sink(&g_mic_wavs[0], "mic1.wav", 32) && open_sink(&g_mic_wavs[1], "mic2.wav", 32) &&
                      open_sink(&g_mic_wavs[2], "mic3.wav", 32) && open_sink(&g_mic_wavs[3], "mic4.wav", 32) &&
                      open_sink(&g_cancelled_wav, "cancelled.wav", 16);
  if (!opened) {
    StopAudioRecordingLocked();
    SetStatus("Failed to open all WAV files for microphone recording.");
    return false;
  }

  g_audio_recording = true;
  SetStatus("Audio recording started: " + base_dir);
  return true;
}

void ToggleAudioRecording() {
  std::lock_guard<std::mutex> lock(g_audio_mutex);
  if (!g_audio_stream_available) {
    SetStatus("Audio stream is unavailable on this device/session.");
    return;
  }
  if (!g_audio_recording) {
    StartAudioRecordingLocked();
  } else {
    StopAudioRecordingLocked();
    SetStatus("Audio recording stopped.");
  }
}

void ApplyTilt() {
  if (g_dev == nullptr) {
    return;
  }
  g_freenect_angle = std::max(-30, std::min(30, g_freenect_angle));
  freenect_set_tilt_degs(g_dev, g_freenect_angle);
}

void ApplyMirror() {
  if (g_dev == nullptr) {
    return;
  }
  freenect_set_flag(g_dev, FREENECT_MIRROR_DEPTH, g_mirror);
  freenect_set_flag(g_dev, FREENECT_MIRROR_VIDEO, g_mirror);
}

void ApplyAutoExposureBundle() {
  if (g_dev == nullptr) {
    return;
  }
  freenect_set_flag(g_dev, FREENECT_AUTO_EXPOSURE, g_auto_exposure);
  freenect_set_flag(g_dev, FREENECT_AUTO_FLICKER, g_auto_exposure);
  freenect_set_flag(g_dev, FREENECT_AUTO_WHITE_BALANCE, g_auto_exposure);
}

void ApplyWhiteBalance() {
  if (g_dev == nullptr) {
    return;
  }
  freenect_set_flag(g_dev, FREENECT_AUTO_WHITE_BALANCE, g_auto_white_balance);
}

void ApplyNearMode() {
  if (g_dev == nullptr) {
    return;
  }
  freenect_set_flag(g_dev, FREENECT_NEAR_MODE, g_near_mode);
}

void ApplyManualExposure() {
  if (g_dev == nullptr) {
    return;
  }
  if (g_auto_exposure == FREENECT_OFF) {
    freenect_set_exposure(g_dev, g_manual_exposure_us);
  }
}

void ApplyIrBrightness() {
  if (g_dev == nullptr) {
    return;
  }
  g_ir_brightness = std::max(1, std::min(50, g_ir_brightness));
  freenect_set_ir_brightness(g_dev, static_cast<uint16_t>(g_ir_brightness));
}

void DepthToFalseColor(uint16_t mm, uint8_t *rgb) {
  if (mm == 0) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    return;
  }

  const float clamped = std::max(400.0f, std::min(6000.0f, static_cast<float>(mm)));
  const float t = (clamped - 400.0f) / (6000.0f - 400.0f);
  const float hue = (1.0f - t) * 240.0f;

  const float c = 1.0f;
  const float hprime = hue / 60.0f;
  const float x = c * (1.0f - std::fabs(std::fmod(hprime, 2.0f) - 1.0f));

  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;

  if (hprime >= 0.0f && hprime < 1.0f) {
    r = c;
    g = x;
  } else if (hprime < 2.0f) {
    r = x;
    g = c;
  } else if (hprime < 3.0f) {
    g = c;
    b = x;
  } else if (hprime < 4.0f) {
    g = x;
    b = c;
  } else if (hprime < 5.0f) {
    r = x;
    b = c;
  } else {
    r = c;
    b = x;
  }

  rgb[0] = static_cast<uint8_t>(r * 255.0f);
  rgb[1] = static_cast<uint8_t>(g * 255.0f);
  rgb[2] = static_cast<uint8_t>(b * 255.0f);
}

bool SaveColorPpm(const std::string &path, const std::vector<uint8_t> &rgb) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  out << "P6\n" << kFrameWidth << " " << kFrameHeight << "\n255\n";
  out.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
  return out.good();
}

bool SaveDepthPgm16(const std::string &path, const std::vector<uint16_t> &depth) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  out << "P5\n" << kFrameWidth << " " << kFrameHeight << "\n65535\n";
  for (uint16_t value : depth) {
    const char hi = static_cast<char>((value >> 8) & 0xff);
    const char lo = static_cast<char>(value & 0xff);
    out.put(hi);
    out.put(lo);
  }
  return out.good();
}

std::size_t SavePointCloudPly(
    const std::string &path,
    const std::vector<uint16_t> &depth,
    const std::vector<uint8_t> &rgb) {
  if (g_dev == nullptr) {
    return 0;
  }

  std::size_t valid_points = 0;
  for (std::size_t i = 0; i < depth.size(); ++i) {
    const uint16_t d = depth[i];
    if (d >= 350 && d <= 6000) {
      ++valid_points;
    }
  }

  std::ofstream out(path);
  if (!out.is_open()) {
    return 0;
  }

  out << "ply\n";
  out << "format ascii 1.0\n";
  out << "element vertex " << valid_points << "\n";
  out << "property float x\n";
  out << "property float y\n";
  out << "property float z\n";
  out << "property uchar red\n";
  out << "property uchar green\n";
  out << "property uchar blue\n";
  out << "end_header\n";

  for (int y = 0; y < kFrameHeight; ++y) {
    for (int x = 0; x < kFrameWidth; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * kFrameWidth + x;
      const uint16_t d = depth[index];
      if (d < 350 || d > 6000) {
        continue;
      }

      double wx = 0.0;
      double wy = 0.0;
      freenect_camera_to_world(g_dev, x, y, d, &wx, &wy);

      const uint8_t r = rgb[index * 3 + 0];
      const uint8_t g = rgb[index * 3 + 1];
      const uint8_t b = rgb[index * 3 + 2];
      out << wx << " " << wy << " " << d << " " << static_cast<int>(r) << " " << static_cast<int>(g) << " "
          << static_cast<int>(b) << "\n";
    }
  }

  return valid_points;
}

void CaptureFrameBundle() {
  std::vector<uint8_t> rgb(static_cast<std::size_t>(kFrameRgbBytes));
  std::vector<uint16_t> depth(static_cast<std::size_t>(kFramePixels));

  pthread_mutex_lock(&g_frame_mutex);
  if (g_rgb_front == nullptr || g_depth_mm_front == nullptr) {
    pthread_mutex_unlock(&g_frame_mutex);
    SetStatus("No frame buffers available to capture.");
    return;
  }
  std::memcpy(rgb.data(), g_rgb_front, kFrameRgbBytes);
  std::memcpy(depth.data(), g_depth_mm_front, static_cast<std::size_t>(kFramePixels) * sizeof(uint16_t));
  pthread_mutex_unlock(&g_frame_mutex);

    const std::string dir = "captures/" + TimestampNow();
  mkdir("captures", 0755);
  mkdir(dir.c_str(), 0755);

  const bool color_ok = SaveColorPpm(dir + "/color.ppm", rgb);
  const bool depth_ok = SaveDepthPgm16(dir + "/depth_mm.pgm", depth);
  const std::size_t points = SavePointCloudPly(dir + "/scan.ply", depth, rgb);

  std::ostringstream msg;
  msg << "Capture saved to " << dir << " (color=" << (color_ok ? "ok" : "fail")
      << ", depth=" << (depth_ok ? "ok" : "fail") << ", points=" << points << ")";
  SetStatus(msg.str());
}

void DrawText(float x, float y, const std::string &text) {
  glRasterPos2f(x, y);
  for (unsigned char c : text) {
    glutBitmapCharacter(GLUT_BITMAP_8_BY_13, c);
  }
}

void DrawTexturedQuad(GLuint texture, float x0, float y0, float x1, float y1) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glBegin(GL_QUADS);
  glTexCoord2f(0.0f, 1.0f);
  glVertex2f(x0, y0);
  glTexCoord2f(1.0f, 1.0f);
  glVertex2f(x1, y0);
  glTexCoord2f(1.0f, 0.0f);
  glVertex2f(x1, y1);
  glTexCoord2f(0.0f, 0.0f);
  glVertex2f(x0, y1);
  glEnd();
}

void DrawGLScene() {
  pthread_mutex_lock(&g_frame_mutex);
  while (!g_got_depth && !g_got_rgb) {
    pthread_cond_wait(&g_frame_cond, &g_frame_mutex);
  }

  if (g_got_depth) {
    std::swap(g_depth_mm_front, g_depth_mm_mid);
    std::swap(g_depth_rgb_front, g_depth_rgb_mid);
    g_got_depth = 0;
  }
  if (g_got_rgb) {
    std::swap(g_rgb_front, g_rgb_mid);
    g_got_rgb = 0;
  }
  pthread_mutex_unlock(&g_frame_mutex);

  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, g_window_width, 0, g_window_height, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, g_gl_rgb_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, kFrameWidth, kFrameHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, g_rgb_front);

  glBindTexture(GL_TEXTURE_2D, g_gl_depth_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, kFrameWidth, kFrameHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, g_depth_rgb_front);

  const int margin = 10;
  const int info_height = 150;
  const int available_height = std::max(200, g_window_height - info_height - 2 * margin);
  int panel_width = (g_window_width - 3 * margin) / 2;
  int panel_height = panel_width * 3 / 4;
  if (panel_height > available_height) {
    panel_height = available_height;
    panel_width = panel_height * 4 / 3;
  }

  const int left_x = (g_window_width / 2) - panel_width - (margin / 2);
  const int right_x = (g_window_width / 2) + (margin / 2);
  const int panel_y = info_height + (available_height - panel_height) / 2;

  DrawTexturedQuad(g_gl_rgb_tex, static_cast<float>(left_x), static_cast<float>(panel_y),
                   static_cast<float>(left_x + panel_width), static_cast<float>(panel_y + panel_height));
  DrawTexturedQuad(g_gl_depth_tex, static_cast<float>(right_x), static_cast<float>(panel_y),
                   static_cast<float>(right_x + panel_width), static_cast<float>(panel_y + panel_height));

  glDisable(GL_TEXTURE_2D);

  glColor3f(1.0f, 1.0f, 1.0f);
  DrawText(static_cast<float>(left_x), static_cast<float>(panel_y + panel_height + 12), "Color");
  DrawText(static_cast<float>(right_x), static_cast<float>(panel_y + panel_height + 12), "Depth");

  float y = static_cast<float>(g_window_height - 20);
  DrawText(10.0f, y, "Kinect Control Center (v1)   ESC: quit   h: help");
  y -= 16.0f;

  std::ostringstream line2;
  line2 << "tilt=" << g_freenect_angle << "deg  led=" << static_cast<int>(g_led_mode)
        << "  video=" << VideoFormatLabel(g_current_video_format) << "  depth=" << DepthFormatLabel(g_current_depth_format)
        << "  fps(rgb/depth)=" << g_rgb_frames << "/" << g_depth_frames;
  DrawText(10.0f, y, line2.str());
  y -= 16.0f;

  std::ostringstream line3;
  line3 << "auto_exp=" << OnOffLabel(g_auto_exposure) << "  auto_wb=" << OnOffLabel(g_auto_white_balance)
        << "  mirror=" << OnOffLabel(g_mirror)
        << "  exposure_us=" << g_manual_exposure_us << "  ir_brightness=" << g_ir_brightness;
  DrawText(10.0f, y, line3.str());
  y -= 16.0f;

  {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    std::ostringstream line4;
    line4 << "audio_stream=" << (g_audio_stream_available ? "available" : "unavailable")
          << "  recording=" << (g_audio_recording ? "on" : "off")
          << "  level=" << std::fixed << std::setprecision(3) << g_audio_level;
    DrawText(10.0f, y, line4.str());
  }
  y -= 16.0f;

  DrawText(10.0f, y, "keys: w/x/s tilt  v video  d depth  m mirror  e auto-exp  b wb  n near  [/ ] exposure");
  y -= 16.0f;
  DrawText(10.0f, y, "      -/= IR brightness  0..6 LED  a audio rec  c capture color+depth+ply");
  y -= 16.0f;
  DrawText(10.0f, y, "status: " + GetStatus());

  glutSwapBuffers();
}

void ReSizeGLScene(int width, int height) {
  g_window_width = std::max(320, width);
  g_window_height = std::max(240, height);
  glViewport(0, 0, g_window_width, g_window_height);
}

void InitGL() {
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glEnable(GL_TEXTURE_2D);
  glShadeModel(GL_FLAT);

  glGenTextures(1, &g_gl_depth_tex);
  glBindTexture(GL_TEXTURE_2D, g_gl_depth_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glGenTextures(1, &g_gl_rgb_tex);
  glBindTexture(GL_TEXTURE_2D, g_gl_rgb_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

void ShutdownAndExit(int exit_code) {
  g_die = 1;
  pthread_join(g_freenect_thread, nullptr);
  glutDestroyWindow(g_window);

  std::lock_guard<std::mutex> audio_lock(g_audio_mutex);
  StopAudioRecordingLocked();

  free(g_depth_mm_mid);
  free(g_depth_mm_front);
  free(g_depth_rgb_mid);
  free(g_depth_rgb_front);
  free(g_rgb_back);
  free(g_rgb_mid);
  free(g_rgb_front);

  std::exit(exit_code);
}

void keyPressed(unsigned char key, int, int) {
  if (key == 27) {
    ShutdownAndExit(0);
    return;
  }

  if (key == 'h' || key == 'H') {
    PrintHelp();
    return;
  }

  if (key == 'w' || key == 'W') {
    g_freenect_angle += 2;
    ApplyTilt();
    SetStatus("Tilt increased.");
    return;
  }
  if (key == 'x' || key == 'X') {
    g_freenect_angle -= 2;
    ApplyTilt();
    SetStatus("Tilt decreased.");
    return;
  }
  if (key == 's' || key == 'S') {
    g_freenect_angle = 0;
    ApplyTilt();
    SetStatus("Tilt centered.");
    return;
  }

  if (key == 'v' || key == 'V') {
    g_requested_video_format =
        (g_requested_video_format == FREENECT_VIDEO_RGB) ? FREENECT_VIDEO_YUV_RGB : FREENECT_VIDEO_RGB;
    SetStatus("Requested video mode: " + std::string(VideoFormatLabel(g_requested_video_format)));
    return;
  }
  if (key == 'd' || key == 'D') {
    g_requested_depth_format =
        (g_requested_depth_format == FREENECT_DEPTH_REGISTERED) ? FREENECT_DEPTH_MM : FREENECT_DEPTH_REGISTERED;
    SetStatus("Requested depth mode: " + std::string(DepthFormatLabel(g_requested_depth_format)));
    return;
  }

  if (key == 'm' || key == 'M') {
    g_mirror = (g_mirror == FREENECT_ON) ? FREENECT_OFF : FREENECT_ON;
    ApplyMirror();
    SetStatus("Mirror: " + std::string(OnOffLabel(g_mirror)));
    return;
  }
  if (key == 'e' || key == 'E') {
    g_auto_exposure = (g_auto_exposure == FREENECT_ON) ? FREENECT_OFF : FREENECT_ON;
    ApplyAutoExposureBundle();
    ApplyManualExposure();
    SetStatus("Auto exposure bundle: " + std::string(OnOffLabel(g_auto_exposure)));
    return;
  }
  if (key == 'b' || key == 'B') {
    g_auto_white_balance = (g_auto_white_balance == FREENECT_ON) ? FREENECT_OFF : FREENECT_ON;
    ApplyWhiteBalance();
    SetStatus("Auto white balance: " + std::string(OnOffLabel(g_auto_white_balance)));
    return;
  }
  if (key == 'n' || key == 'N') {
    SetStatus("Near mode not supported on this Kinect device");
    return;
  }

  if (key == '[') {
    g_manual_exposure_us = std::max(1000, g_manual_exposure_us - 1000);
    ApplyManualExposure();
    SetStatus("Manual exposure set to " + std::to_string(g_manual_exposure_us) + " us.");
    return;
  }
  if (key == ']') {
    g_manual_exposure_us = std::min(200000, g_manual_exposure_us + 1000);
    ApplyManualExposure();
    SetStatus("Manual exposure set to " + std::to_string(g_manual_exposure_us) + " us.");
    return;
  }
  if (key == '-') {
    g_ir_brightness -= 1;
    ApplyIrBrightness();
    SetStatus("IR brightness set to " + std::to_string(g_ir_brightness));
    return;
  }
  if (key == '=') {
    g_ir_brightness += 1;
    ApplyIrBrightness();
    SetStatus("IR brightness set to " + std::to_string(g_ir_brightness));
    return;
  }

  if (key == 'a' || key == 'A') {
    ToggleAudioRecording();
    return;
  }
  if (key == 'c' || key == 'C') {
    CaptureFrameBundle();
    return;
  }

  if (key == '0') {
    g_led_mode = LED_OFF;
    freenect_set_led(g_dev, g_led_mode);
    SetStatus("LED set to OFF.");
    return;
  }
  if (key == '1') {
    g_led_mode = LED_GREEN;
    freenect_set_led(g_dev, g_led_mode);
    SetStatus("LED set to GREEN.");
    return;
  }
  if (key == '2') {
    g_led_mode = LED_RED;
    freenect_set_led(g_dev, g_led_mode);
    SetStatus("LED set to RED.");
    return;
  }
  if (key == '3') {
    g_led_mode = LED_YELLOW;
    freenect_set_led(g_dev, g_led_mode);
    SetStatus("LED set to YELLOW.");
    return;
  }
  if (key == '4' || key == '5') {
    g_led_mode = LED_BLINK_GREEN;
    freenect_set_led(g_dev, g_led_mode);
    SetStatus("LED set to BLINK_GREEN.");
    return;
  }
  if (key == '6') {
    g_led_mode = LED_BLINK_RED_YELLOW;
    freenect_set_led(g_dev, g_led_mode);
    SetStatus("LED set to BLINK_RED_YELLOW.");
    return;
  }
}

void depth_cb(freenect_device *, void *v_depth, uint32_t) {
  const auto *depth = static_cast<uint16_t *>(v_depth);

  pthread_mutex_lock(&g_frame_mutex);
  std::memcpy(g_depth_mm_mid, depth, static_cast<std::size_t>(kFramePixels) * sizeof(uint16_t));
  for (int i = 0; i < kFramePixels; ++i) {
    DepthToFalseColor(g_depth_mm_mid[i], g_depth_rgb_mid + i * 3);
  }
  g_got_depth = 1;
  ++g_depth_frames;
  pthread_cond_signal(&g_frame_cond);
  pthread_mutex_unlock(&g_frame_mutex);
}

void rgb_cb(freenect_device *, void *rgb, uint32_t) {
  pthread_mutex_lock(&g_frame_mutex);

  if (g_current_video_format == FREENECT_VIDEO_RGB || g_current_video_format == FREENECT_VIDEO_YUV_RGB) {
    assert(g_rgb_back == rgb);
    g_rgb_back = g_rgb_mid;
    freenect_set_video_buffer(g_dev, g_rgb_back);
    g_rgb_mid = static_cast<uint8_t *>(rgb);
  } else {
    std::memcpy(g_rgb_mid, rgb, kFrameRgbBytes);
  }

  g_got_rgb = 1;
  ++g_rgb_frames;
  pthread_cond_signal(&g_frame_cond);
  pthread_mutex_unlock(&g_frame_mutex);
}

void audio_in_cb(
    freenect_device *,
    int num_samples,
    int32_t *mic1,
    int32_t *mic2,
    int32_t *mic3,
    int32_t *mic4,
    int16_t *cancelled,
    void *) {
  std::lock_guard<std::mutex> lock(g_audio_mutex);

  if (cancelled != nullptr && num_samples > 0) {
    double acc = 0.0;
    for (int i = 0; i < num_samples; ++i) {
      acc += std::abs(static_cast<int>(cancelled[i]));
    }
    g_audio_level = acc / (static_cast<double>(num_samples) * 32768.0);
  }

  if (!g_audio_recording) {
    return;
  }

  auto write_i32 = [&](WavSink *sink, int32_t *samples) {
    if (sink->file != nullptr && samples != nullptr) {
      std::fwrite(samples, sizeof(int32_t), static_cast<std::size_t>(num_samples), sink->file);
      sink->sample_count += static_cast<std::uint64_t>(num_samples);
    }
  };
  auto write_i16 = [&](WavSink *sink, int16_t *samples) {
    if (sink->file != nullptr && samples != nullptr) {
      std::fwrite(samples, sizeof(int16_t), static_cast<std::size_t>(num_samples), sink->file);
      sink->sample_count += static_cast<std::uint64_t>(num_samples);
    }
  };

  write_i32(&g_mic_wavs[0], mic1);
  write_i32(&g_mic_wavs[1], mic2);
  write_i32(&g_mic_wavs[2], mic3);
  write_i32(&g_mic_wavs[3], mic4);
  write_i16(&g_cancelled_wav, cancelled);
}

void *freenect_threadfunc(void *) {
  freenect_set_tilt_degs(g_dev, g_freenect_angle);
  freenect_set_led(g_dev, g_led_mode);

  ApplyMirror();
  ApplyAutoExposureBundle();
  ApplyWhiteBalance();
  ApplyNearMode();
  ApplyIrBrightness();
  ApplyManualExposure();

  freenect_set_depth_callback(g_dev, depth_cb);
  freenect_set_video_callback(g_dev, rgb_cb);
  freenect_set_audio_in_callback(g_dev, audio_in_cb);

  freenect_set_video_mode(g_dev, freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, g_current_video_format));
  freenect_set_depth_mode(g_dev, freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, g_current_depth_format));
  freenect_set_video_buffer(g_dev, g_rgb_back);

  if (freenect_start_depth(g_dev) < 0) {
    SetStatus("Could not start depth stream.");
    g_die = 1;
  }
  if (freenect_start_video(g_dev) < 0) {
    SetStatus("Could not start video stream.");
    g_die = 1;
  }

  if (freenect_start_audio(g_dev) < 0) {
    g_audio_stream_available = false;
    SetStatus("Video/depth started. Audio stream unavailable (firmware/permission/adapter).");
  } else {
    g_audio_stream_available = true;
    SetStatus("Video, depth, and audio streams started.");
  }

  PrintHelp();

  while (!g_die) {
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 30000;
    const int process_result = freenect_process_events_timeout(g_ctx, &timeout);
    if (process_result < 0) {
      SetStatus("USB event processing failed. Device may have disconnected.");
      break;
    }

    if (g_requested_video_format != g_current_video_format) {
      freenect_stop_video(g_dev);
      const freenect_frame_mode mode = freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, g_requested_video_format);
      if (mode.is_valid) {
        freenect_set_video_mode(g_dev, mode);
        g_current_video_format = g_requested_video_format;
        SetStatus("Video mode changed to " + std::string(VideoFormatLabel(g_current_video_format)));
      } else {
        SetStatus("Requested video mode is not valid.");
        g_requested_video_format = g_current_video_format;
      }
      freenect_set_video_buffer(g_dev, g_rgb_back);
      freenect_start_video(g_dev);
    }

    if (g_requested_depth_format != g_current_depth_format) {
      freenect_stop_depth(g_dev);
      const freenect_frame_mode mode = freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, g_requested_depth_format);
      if (mode.is_valid) {
        freenect_set_depth_mode(g_dev, mode);
        g_current_depth_format = g_requested_depth_format;
        SetStatus("Depth mode changed to " + std::string(DepthFormatLabel(g_current_depth_format)));
      } else {
        SetStatus("Requested depth mode is not valid.");
        g_requested_depth_format = g_current_depth_format;
      }
      freenect_start_depth(g_dev);
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    StopAudioRecordingLocked();
  }

  freenect_stop_depth(g_dev);
  freenect_stop_video(g_dev);
  if (g_audio_stream_available) {
    freenect_stop_audio(g_dev);
  }

  freenect_set_led(g_dev, LED_OFF);
  freenect_close_device(g_dev);
  freenect_shutdown(g_ctx);

  SetStatus("Shutdown complete.");
  return nullptr;
}

void *gl_threadfunc(void *) {
  glutInit(&g_argc, g_argv);
  glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_ALPHA | GLUT_DEPTH);
  glutInitWindowSize(g_window_width, g_window_height);
  glutInitWindowPosition(0, 0);

  g_window = glutCreateWindow("Kinect Control Center (v1)");
  glutDisplayFunc(&DrawGLScene);
  glutIdleFunc(&DrawGLScene);
  glutReshapeFunc(&ReSizeGLScene);
  glutKeyboardFunc(&keyPressed);

  InitGL();
  glutMainLoop();
  return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
  g_argc = argc;
  g_argv = argv;

  g_depth_mm_mid = static_cast<uint16_t *>(std::malloc(static_cast<std::size_t>(kFramePixels) * sizeof(uint16_t)));
  g_depth_mm_front = static_cast<uint16_t *>(std::malloc(static_cast<std::size_t>(kFramePixels) * sizeof(uint16_t)));
  g_depth_rgb_mid = static_cast<uint8_t *>(std::malloc(static_cast<std::size_t>(kFrameRgbBytes)));
  g_depth_rgb_front = static_cast<uint8_t *>(std::malloc(static_cast<std::size_t>(kFrameRgbBytes)));
  g_rgb_back = static_cast<uint8_t *>(std::malloc(static_cast<std::size_t>(kFrameRgbBytes)));
  g_rgb_mid = static_cast<uint8_t *>(std::malloc(static_cast<std::size_t>(kFrameRgbBytes)));
  g_rgb_front = static_cast<uint8_t *>(std::malloc(static_cast<std::size_t>(kFrameRgbBytes)));

  if (g_depth_mm_mid == nullptr || g_depth_mm_front == nullptr || g_depth_rgb_mid == nullptr || g_depth_rgb_front == nullptr ||
      g_rgb_back == nullptr || g_rgb_mid == nullptr || g_rgb_front == nullptr) {
    std::cerr << "Out of memory while allocating frame buffers.\n";
    return EXIT_FAILURE;
  }
  std::memset(g_depth_mm_mid, 0, static_cast<std::size_t>(kFramePixels) * sizeof(uint16_t));
  std::memset(g_depth_mm_front, 0, static_cast<std::size_t>(kFramePixels) * sizeof(uint16_t));
  std::memset(g_depth_rgb_mid, 0, static_cast<std::size_t>(kFrameRgbBytes));
  std::memset(g_depth_rgb_front, 0, static_cast<std::size_t>(kFrameRgbBytes));
  std::memset(g_rgb_back, 0, static_cast<std::size_t>(kFrameRgbBytes));
  std::memset(g_rgb_mid, 0, static_cast<std::size_t>(kFrameRgbBytes));
  std::memset(g_rgb_front, 0, static_cast<std::size_t>(kFrameRgbBytes));

  int device_index = 0;
  if (argc > 1) {
    device_index = std::max(0, std::atoi(argv[1]));
  }

  if (freenect_init(&g_ctx, nullptr) < 0) {
    std::cerr << "freenect_init() failed\n";
    return EXIT_FAILURE;
  }
  freenect_set_log_level(g_ctx, FREENECT_LOG_WARNING);
  freenect_select_subdevices(
      g_ctx,
      static_cast<freenect_device_flags>(FREENECT_DEVICE_MOTOR | FREENECT_DEVICE_CAMERA | FREENECT_DEVICE_AUDIO));

  const int device_count = freenect_num_devices(g_ctx);
  std::cout << "Number of Kinect v1 devices found: " << device_count << "\n";
  if (device_count < 1 || device_index >= device_count) {
    std::cerr << "No usable Kinect v1 device at index " << device_index << ".\n";
    freenect_shutdown(g_ctx);
    return EXIT_FAILURE;
  }

  if (freenect_open_device(g_ctx, &g_dev, device_index) < 0) {
    std::cerr << "Could not open Kinect v1 device index " << device_index << "\n";
    freenect_shutdown(g_ctx);
    return EXIT_FAILURE;
  }

  freenect_device_attributes *attributes = nullptr;
  if (freenect_list_device_attributes(g_ctx, &attributes) > 0 && attributes != nullptr) {
    const freenect_device_attributes *cursor = attributes;
    int index = 0;
    while (cursor != nullptr) {
      if (index == device_index && cursor->camera_serial != nullptr) {
        SetStatus(std::string("Connected serial: ") + cursor->camera_serial);
        break;
      }
      cursor = cursor->next;
      ++index;
    }
    freenect_free_device_attributes(attributes);
  }

  if (pthread_create(&g_freenect_thread, nullptr, freenect_threadfunc, nullptr) != 0) {
    std::cerr << "pthread_create for freenect thread failed.\n";
    freenect_close_device(g_dev);
    freenect_shutdown(g_ctx);
    return EXIT_FAILURE;
  }

  // On macOS, GLUT must run on the main thread.
  gl_threadfunc(nullptr);
  return EXIT_SUCCESS;
}
