#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include "display_window.hpp"
#include "ge_gpu_backend.hpp"
#include "spongebob_audio_output.hpp"
#include "spongebob_profile.hpp"
#include "spongebob_render_config.hpp"
#include "spongebob_runtime_log.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

struct BootstrapPaths {
    std::filesystem::path eboot;
    std::filesystem::path game_root;
};

// Same helper VCS uses, simplified for SpongeBob's Linux/Vulkan target.
std::filesystem::path native_executable_directory(const char *argv0) {
#ifdef _WIN32
    return std::filesystem::current_path();
#else
    return std::filesystem::absolute(argv0 != nullptr ? argv0 : "SpongeBobNative").parent_path();
#endif
}

// Optional args: --eboot <path>, --game-dir <path>, --config <path>.
BootstrapPaths resolve_bootstrap_paths(int argc, char **argv,
                                       const std::filesystem::path &exe_dir) {
    BootstrapPaths paths;
    paths.eboot = exe_dir / "original" / "ULUS10478_EBOOT.ELF";
    paths.game_root = exe_dir / "original" / "PSP_GAME" / "USRDIR";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string{};
        };
        if (arg == "--eboot") {
            paths.eboot = next();
        } else if (arg == "--game-dir") {
            paths.game_root = next();
        } else if (arg == "--config") {
            // initialize_spongebob_render_configuration already honors PSPRECOMP_CONFIG.
            const std::string value = next();
#if defined(_WIN32)
            _putenv_s("PSPRECOMP_CONFIG", value.c_str());
#else
            (void)setenv("PSPRECOMP_CONFIG", value.c_str(), 1);
#endif
        }
    }
    return paths;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const std::filesystem::path executable_directory =
            native_executable_directory(argc > 0 ? argv[0] : nullptr);
        const BootstrapPaths paths = resolve_bootstrap_paths(argc, argv, executable_directory);

        // 1. Configuration (SpongeBobNative.ini; the .toml is codegen-only).
        spongebob::initialize_spongebob_render_configuration(executable_directory);
        const spongebob::SpongebobConfiguration &configuration =
            spongebob::spongebob_render_configuration();
        spongebob::runtime_log_initialize(configuration);
        spongebob::runtime_log_line("bootstrap eboot=" + paths.eboot.string());
        spongebob::runtime_log_line("bootstrap root=" + paths.game_root.string());

        // 2. Load EBOOT + relocate.
        psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(paths.eboot);
        psprecomp::Runtime runtime(32u * 1024u * 1024u);   // 32 MB
        runtime.set_game_root(paths.game_root);
        const psprecomp::RelocationStats relocations =
            elf.load_and_relocate(runtime.memory(), psprecomp::kDefaultPspUserLoadBase);

        // 3. Install profile: generated functions first, then the HLE.
#ifndef SPONGEBOB_NO_GENERATED
        psprecomp::register_generated_functions(runtime);
#endif
        spongebob::install_spongebob_profile(runtime);

        if (runtime.function_count() == 0u) {
            std::cerr << "No generated SpongeBob functions are linked yet. "
                         "Run psp_recomp first (AOT corpus).\n";
            spongebob::runtime_log_shutdown();
            return 3;
        }

        // 4. Host initialization (order proven by the Fase 4.5 sanity check).
        spongebob::display_window_init();
        std::string backend_error;
        if (!spongebob::initialize_ge_gpu_backend(backend_error)) {
            std::cerr << "[SpongeBob] Backend GPU no disponible: " << backend_error << "\n";
            spongebob::display_window_shutdown();
            spongebob::runtime_log_shutdown();
            return 1;
        }
        spongebob::display_window_attach_gpu_backend();
        // There is no audio_output_init(): the device opens lazily on the first
        // submit from the sceSasCore HLE.

        // 5. CPU setup: GP from the module info (critical for boot).
        if (const auto module =
                elf.find_module_info(runtime.memory(), psprecomp::kDefaultPspUserLoadBase)) {
            runtime.cpu().set_gpr(28, module->gp);
        } else {
            throw psprecomp::Error("PSP module info not found after relocation");
        }
        runtime.cpu().set_gpr(31, 0u);   // ra
        runtime.cpu().set_gpr(4, 0u);    // a0
        runtime.cpu().set_gpr(5, 0u);    // a1
        // Establish the loader/module stack at the top of user RAM (mirrors VCS
        // install_profile(); the SpongeBob HLE stubs do not create a thread yet).
        runtime.cpu().set_gpr(26, 0x09FFFF00u);   // k0: kernel context
        runtime.cpu().set_gpr(29, 0x09FFFF00u);   // sp: loader stack

        std::cout << "SpongeBobNative PSP bootstrap\n"
                  << "Executable: " << paths.eboot.string() << "\n"
                  << "Entry:      " << psprecomp::hex32(elf.runtime_entry()) << "\n"
                  << "Relocs:     " << relocations.total
                  << " (invalid " << relocations.invalid
                  << ", unsupported " << relocations.unsupported << ")\n"
                  << "Functions:  " << runtime.function_count() << "\n"
                  << "Config:     " << configuration.source_path.string()
                  << (configuration.loaded_from_file ? " (loaded)" : " (defaults)") << "\n";

        // 6. Boot. elf.runtime_entry() == 0x08804124 for ULUS-10478.
        runtime.run(elf.runtime_entry());

        std::cout << "Runtime stopped: " << runtime.stop_reason() << "\n";

        // 7. Cleanup.
        spongebob::audio_output_shutdown();
        spongebob::display_window_shutdown();
        spongebob::shutdown_ge_gpu_backend();
        spongebob::runtime_log_shutdown();
        return runtime.stop_reason().empty() ? 0 : 4;
    } catch (const std::exception &e) {
        std::cerr << "SpongeBobNative error: " << e.what() << "\n";
        return 1;
    }
}
