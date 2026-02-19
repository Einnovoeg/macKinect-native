#include "gui/gui_app.h"
#include "backends/backend.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

enum class BackendChoice {
  kAuto,
  kV1,
  kV2,
};

struct Options {
  bool run_preview = false;
  bool list_devices = false;
  int preview_seconds = 5;
  BackendChoice backend = BackendChoice::kAuto;
};

void PrintUsage(const char *program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "\n"
            << "Options:\n"
            << "  --gui               Run the graphical interface (default if no args)\n"
            << "  --list              List connected devices\n"
            << "  --preview [sec]     Run a CLI preview for N seconds\n"
            << "  --backend [v1|v2]   Force a specific backend\n"
            << "  --help, -h          Show this help\n";
}

bool ParsePositiveInt(const std::string &text, int *value) {
  try {
    const int parsed = std::stoi(text);
    if (parsed <= 0) {
      return false;
    }
    *value = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseBackendChoice(const std::string &raw, BackendChoice *choice) {
  if (raw == "auto") {
    *choice = BackendChoice::kAuto;
    return true;
  }
  if (raw == "v1") {
    *choice = BackendChoice::kV1;
    return true;
  }
  if (raw == "v2") {
    *choice = BackendChoice::kV2;
    return true;
  }
  return false;
}

bool MatchesChoice(const KinectBackend &backend, BackendChoice choice) {
  if (choice == BackendChoice::kAuto) {
    return true;
  }
  if (choice == BackendChoice::kV1) {
    return backend.generation() == KinectGeneration::kV1;
  }
  if (choice == BackendChoice::kV2) {
    return backend.generation() == KinectGeneration::kV2;
  }
  return false;
}

}  // namespace

int main(int argc, char **argv) {
  Options options;
  bool gui_mode = false;
  bool cli_mode = false;

  if (argc == 1) {
      gui_mode = true;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return EXIT_SUCCESS;
    }
    
    if (arg == "--gui") {
        gui_mode = true;
        continue;
    }

    if (arg == "--list") {
      options.list_devices = true;
      cli_mode = true;
      continue;
    }

    if (arg == "--preview") {
      options.run_preview = true;
      cli_mode = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        ++i;
        if (!ParsePositiveInt(argv[i], &options.preview_seconds)) {
          std::cerr << "Invalid preview seconds: " << argv[i] << "\n";
          return EXIT_FAILURE;
        }
      }
      continue;
    }

    if (arg.rfind("--preview=", 0) == 0) {
      options.run_preview = true;
      cli_mode = true;
      if (!ParsePositiveInt(arg.substr(std::string("--preview=").size()), &options.preview_seconds)) {
        std::cerr << "Invalid preview seconds: " << arg << "\n";
        return EXIT_FAILURE;
      }
      continue;
    }

    if (arg == "--backend") {
      if (i + 1 >= argc) {
        std::cerr << "--backend expects one value: auto, v1, or v2\n";
        return EXIT_FAILURE;
      }
      ++i;
      if (!ParseBackendChoice(argv[i], &options.backend)) {
        std::cerr << "Invalid backend value: " << argv[i] << "\n";
        return EXIT_FAILURE;
      }
      continue;
    }
    
    // Other args handled or fail
  }

  if (gui_mode && !cli_mode) {
      return RunGuiApp(argc, argv);
  }

  // CLI Logic
  std::vector<std::unique_ptr<KinectBackend>> backends;
  backends.push_back(CreateKinectV1Backend());
  backends.push_back(CreateKinectV2Backend());

  int selected_backends = 0;
  const auto preview_duration = std::chrono::seconds(options.preview_seconds);

  for (const auto &backend_ptr : backends) {
    KinectBackend &backend = *backend_ptr;
    if (!MatchesChoice(backend, options.backend)) {
      continue;
    }
    ++selected_backends;

    const ProbeResult probe = backend.probe();
    std::cout << "[" << backend.name() << "] " << (probe.available ? "available" : "unavailable") << "\n";
    if (!probe.detail.empty()) {
      std::cout << "  " << probe.detail << "\n";
    }

    if (!probe.available) {
      continue;
    }

    const std::vector<DeviceInfo> devices = backend.listDevices();
    if (devices.empty()) {
      std::cout << "  Devices: none\n";
      continue;
    }

    std::cout << "  Devices:\n";
    for (const DeviceInfo &device : devices) {
      std::cout << "    - " << KinectGenerationLabel(device.generation) << " serial: " << device.serial << "\n";
    }

    if (options.run_preview) {
      const PreviewResult preview = backend.preview(preview_duration);
      std::cout << "  Preview: " << (preview.success ? "success" : "failed") << "\n";
      std::cout << "    " << preview.detail << "\n";
      std::cout << "    color frames: " << preview.color_frames << "\n";
      std::cout << "    depth frames: " << preview.depth_frames << "\n";
    }
  }

  if (selected_backends == 0) {
    std::cerr << "No backend matched the requested backend filter.\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
