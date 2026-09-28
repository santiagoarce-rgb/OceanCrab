#include "display_window.hpp"
#include "ge_gpu_backend.hpp"
#include "spongebob_render_config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

// Minimal sanity-check entrypoint (Fase 4.5).
//
// Boots the Vulkan GE backend and the SDL window WITHOUT loading a game or any
// HLE, so we can validate that the render/platform/audio code compiles, links,
// and that the Vulkan shaders (embedded by glslangValidator at build time) load.
//
// Fase 5 replaces this with the real bootstrap: load EBOOT.ELF, install the
// profile HLE, register generated functions, and call runtime.run(entry).

int main(int argc, char **argv) {
    std::filesystem::path executable_directory = std::filesystem::current_path();
    if (argc > 0) {
        std::error_code error;
        executable_directory = std::filesystem::absolute(argv[0], error).parent_path();
    }

    // 1. Load render configuration (the Vulkan backend reads resolution/backend).
    //    Missing config falls back to defaults, which still select the Vulkan GE
    //    on Linux (Backend=DirectX12 maps to Vulkan in this build).
    spongebob::initialize_spongebob_render_configuration(executable_directory);

    // 2. Open the SDL window.
    spongebob::display_window_init();

    // 3. Initialize the Vulkan GE backend.
    std::string backend_error;
    if (!spongebob::initialize_ge_gpu_backend(backend_error)) {
        std::cerr << "[SpongeBob] Backend Vulkan no disponible: " << backend_error << "\n";
        spongebob::display_window_shutdown();
        return 1;
    }
    std::cout << "[SpongeBob] Backend Vulkan inicializado\n" << std::flush;
    spongebob::display_window_attach_gpu_backend();

    // 4. Short frame loop (no game loaded; just pumps SDL events).
    for (int frame = 0; frame < 60; ++frame) {
        spongebob::display_window_pump();
        if (spongebob::display_window_closed()) {
            std::cout << "[SpongeBob] ventana cerrada en frame " << frame << "\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // 5. Clean shutdown.
    spongebob::display_window_shutdown();
    spongebob::shutdown_ge_gpu_backend();
    std::cout << "[SpongeBob] sanity check OK\n" << std::flush;
    return 0;
}
