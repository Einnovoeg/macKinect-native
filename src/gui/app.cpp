#include "gui_app.h"
#include "backends/backend.h"

#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include <vector>
#include <iostream>
#include <memory>
#include <mutex>

// Global state for GLUT (since it's C-style callback based)
struct AppState {
    std::unique_ptr<KinectBackend> backend;
    std::unique_ptr<KinectDevice> device;
    
    std::vector<uint8_t> rgb_buffer;
    std::vector<uint8_t> depth_buffer_visual; // Grayscale for display
    
    int width = 640;
    int height = 480;
    
    GLuint rgb_tex = 0;
    GLuint depth_tex = 0;
    
    bool running = true;
};

static AppState g_app;

void UpdateTexture(GLuint tex, int w, int h, const void* data, GLenum format) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, format, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Display() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Draw RGB (Left)
    if (g_app.rgb_tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_app.rgb_tex);
        
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1.0, 1.0);
        glTexCoord2f(1, 0); glVertex2f(0.0, 1.0);
        glTexCoord2f(1, 1); glVertex2f(0.0, -1.0);
        glTexCoord2f(0, 1); glVertex2f(-1.0, -1.0);
        glEnd();
        
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }
    
    // Draw Depth (Right)
    if (g_app.depth_tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_app.depth_tex);
        
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0.0, 1.0);
        glTexCoord2f(1, 0); glVertex2f(1.0, 1.0);
        glTexCoord2f(1, 1); glVertex2f(1.0, -1.0);
        glTexCoord2f(0, 1); glVertex2f(0.0, -1.0);
        glEnd();
        
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }
    
    glutSwapBuffers();
}

void Idle() {
    if (!g_app.device) return;
    
    FrameData frame;
    if (g_app.device->getFrame(frame)) {
        // Update RGB Texture
        if (!frame.rgb.empty()) {
            if (g_app.rgb_tex == 0) {
                glGenTextures(1, &g_app.rgb_tex);
                glBindTexture(GL_TEXTURE_2D, g_app.rgb_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            // Assuming RGB format
            UpdateTexture(g_app.rgb_tex, frame.width, frame.height, frame.rgb.data(), GL_RGB);
        }
        
        // Update Depth Texture (Convert uint16 to grayscale RGB)
        if (!frame.depth.empty()) {
            if (g_app.depth_tex == 0) {
                glGenTextures(1, &g_app.depth_tex);
                glBindTexture(GL_TEXTURE_2D, g_app.depth_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            
            size_t pixel_count = frame.width * frame.height;
            if (g_app.depth_buffer_visual.size() != pixel_count * 3) {
                g_app.depth_buffer_visual.resize(pixel_count * 3);
            }
            
            for (size_t i = 0; i < pixel_count; ++i) {
                uint16_t d = frame.depth[i];
                // Simple visualization: scale 0-4000mm to 0-255
                uint8_t val = static_cast<uint8_t>(d / 16); 
                g_app.depth_buffer_visual[i*3 + 0] = val;
                g_app.depth_buffer_visual[i*3 + 1] = val;
                g_app.depth_buffer_visual[i*3 + 2] = val;
            }
            
            UpdateTexture(g_app.depth_tex, frame.width, frame.height, g_app.depth_buffer_visual.data(), GL_RGB);
        }
        
        glutPostRedisplay();
    }
    
    // Also call update() if the backend requires polling
    g_app.device->update();
}

void Keyboard(unsigned char key, int x, int y) {
    if (key == 27) { // ESC
        exit(0);
    }
}

int RunGuiApp(int argc, char** argv) {
    std::cout << "Starting GUI...\n";
    
    // Initialize Backend (Try V2 then V1)
    g_app.backend = CreateKinectV2Backend();
    auto probe = g_app.backend->probe();
    if (!probe.available) {
        std::cout << "V2 not available: " << probe.detail << "\nTrying V1...\n";
        g_app.backend = CreateKinectV1Backend();
        probe = g_app.backend->probe();
    }
    
    if (!probe.available) {
        std::cerr << "No Kinect devices found or backends unavailable.\n";
        // We can still run to show the window
    } else {
        auto devices = g_app.backend->listDevices();
        if (!devices.empty()) {
            std::cout << "Opening device: " << devices[0].serial << "\n";
            g_app.device = g_app.backend->openDevice(devices[0].serial);
            if (g_app.device) {
                g_app.device->start();
            } else {
                std::cerr << "Failed to open device.\n";
            }
        }
    }
    
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(1280, 480);
    glutCreateWindow("macKinect Legacy Preview");
    
    glutDisplayFunc(Display);
    glutIdleFunc(Idle);
    glutKeyboardFunc(Keyboard);
    
    std::cout << "Entering GLUT main loop...\n";
    glutMainLoop();
    
    return 0;
}
