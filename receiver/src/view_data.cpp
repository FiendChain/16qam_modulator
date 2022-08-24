#include <stdio.h>

// graphics code
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

// implot library
#include "implot.h"

// our imgui extras
#include "imgui_config.h"
#include "font_awesome_definitions.h"

#include <thread>
#include <stdlib.h>
#include <stdint.h>

#include "carrier_dsp.h"
#include "frame_synchroniser.h"
#include "constellation.h"
#include "filter_designer.h"

#include <io.h>
#include <fcntl.h>

#define PRINT_LOG 0
#if PRINT_LOG 
  #define LOG_MESSAGE(...) fprintf(stderr, ##__VA_ARGS__)
#else
  #define LOG_MESSAGE(...) (void)0
#endif


// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
// To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
// Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

int is_main_window_focused = true;

static void glfw_error_callback(int error, const char* description)
{
    LOG_MESSAGE("Glfw Error %d: %s\n", error, description);
}

// this occurs when we minimise or change focus to another window
static void glfw_window_focus_callback(GLFWwindow* window, int focused)
{
    is_main_window_focused = focused;
}

class AudioData {
public:
    const int audio_buffer_size;
    uint8_t* audio_buffer = NULL;
    int16_t* pcm_buffer = NULL;
    int total_packets = 0;
    int incorrect_packets = 0;
    int correct_packets = 0;
    int corrupted_packets = 0;
    int repaired_packets = 0;
public:
    AudioData(const int _N)
    : audio_buffer_size(_N) 
    {
        audio_buffer = new uint8_t[_N];
        pcm_buffer = new int16_t[_N];
    }

    ~AudioData() {
        delete [] audio_buffer;
        delete [] pcm_buffer;
    }
};

void demodulator_thread(
    FILE* fp, 
    const int ds_factor, const int block_size, 
    CarrierToSymbolDemodulatorBuffers* demod_buffer, CarrierToSymbolDemodulatorBuffers* snapshot_buffer, 
    CarrierDemodulatorSpecification* spec, ConstellationSpecification* constellation,
    bool* snapshot_trigger,
    AudioData* aud_data, int16_t* audio_gain) 
{
    const int ds_block_size = block_size*ds_factor;

    // quick map into buffers
    auto IQ_mod_buffer = new std::complex<uint8_t>[ds_block_size];
    auto IQ_demod_buffer = new std::complex<float>[block_size];

    const int audio_buffer_size = aud_data->audio_buffer_size;

    constexpr uint32_t PREAMBLE_CODE = 0b11111001101011111100110101101101;
    constexpr uint16_t SCRAMBLER_CODE = 0b1000010101011001;
    // constexpr uint32_t CRC32_POLY = 0x04C11DB7;
    constexpr uint8_t CRC8_POLY = 0xD5;

    auto frame_decoder = new FrameDecoder(block_size);
    auto scrambler = new AdditiveScrambler(SCRAMBLER_CODE);
    auto crc8_calc = new CRC8_Calculator(CRC8_POLY);
    auto vitdec = new ViterbiDecoder<encoder_decoder_type>(25);

    frame_decoder->descrambler = scrambler;
    frame_decoder->crc8_calc= crc8_calc;
    frame_decoder->vitdec = vitdec;

    auto frame_sync = new FrameSynchroniser();
    auto preamble_detector = new PreambleDetector(PREAMBLE_CODE, 4);
    frame_sync->constellation = constellation;
    frame_sync->frame_decoder = frame_decoder;
    frame_sync->preamble_detector = preamble_detector;

    auto demod = new CarrierToSymbolDemodulator(*spec, constellation);
    demod->buffers = demod_buffer;

    auto x_in_buffer = new std::complex<float>[ds_block_size];

    int curr_audio_buffer_index = 0;

    const int audio_payload_length = 100;
    const int message_metadata_length = 13;

    float ds_k = (spec->f_sample/2.0f)/(spec->f_sample*ds_factor/2.0f);
    if (ds_factor == 1) {
        ds_k = 0.9f;
    }
    auto ds_filter_spec = create_fir_lpf(ds_k, 50);
    FIR_Filter<std::complex<float>> ds_filter(ds_filter_spec->b, ds_filter_spec->N);

    constexpr float AC_FILTER_B[] = {1.0f, -1.0f};
    constexpr float AC_FILTER_A[] = {1.0f, -0.999999f};
    IIR_Filter<int16_t> audio_ac_filter(AC_FILTER_B, AC_FILTER_A, 2);
    int16_t* ac_audio_buffer = new int16_t[audio_payload_length];

    auto payload_handler = [
        &aud_data, &curr_audio_buffer_index, 
        &audio_gain, audio_payload_length,
        &audio_ac_filter, &ac_audio_buffer]
        (uint8_t* x, const uint16_t N) 
    {
        // if not an audio block
        if (N != audio_payload_length) {
            LOG_MESSAGE("message=%.*s\n", N, x);
            return;
        }

        for (int i = 0; i < N; i++) {
            uint16_t v = x[i];
            v = v - 128;
            v = v * 64;
            ac_audio_buffer[i] = v;
        }

        audio_ac_filter.process(ac_audio_buffer, ac_audio_buffer, N);

        const int16_t audio_gain_val = *audio_gain;

        for (int i = 0; i < N; i++) {
            const uint8_t v = x[i];
            aud_data->audio_buffer[curr_audio_buffer_index] = v;

            // amplify the signal
            const int16_t v0 = ac_audio_buffer[i] * audio_gain_val;
            aud_data->pcm_buffer[curr_audio_buffer_index] = v0;

            if (curr_audio_buffer_index == (aud_data->audio_buffer_size-1)) {
                fwrite(aud_data->pcm_buffer, sizeof(int16_t), aud_data->audio_buffer_size, stdout);
            }
            curr_audio_buffer_index = (curr_audio_buffer_index+1) % aud_data->audio_buffer_size;
        }
    };

    int rd_total_blocks = 0;
    while (true) {
        size_t rd_block_size = fread(IQ_mod_buffer, sizeof(std::complex<uint8_t>), ds_block_size, fp);
        if (rd_block_size != ds_block_size) {
            LOG_MESSAGE("Got mismatched block size after %d blocks\n", rd_total_blocks);
            // if we are reading from a file, repeat
            if (fp != stdin) {
                fseek(fp, 0, 0);
            }
            continue;
        }
        rd_total_blocks++; 

        for (int i = 0; i < ds_block_size; i++) {
            const uint8_t I = IQ_mod_buffer[i].real();
            const uint8_t Q = IQ_mod_buffer[i].imag();
            x_in_buffer[i].real((float)I - 127.5f);
            x_in_buffer[i].imag((float)Q - 127.5f);
        }

        if (ds_factor != 1) {
            ds_filter.process(x_in_buffer, x_in_buffer, ds_block_size);
            for (int i = 0; i < block_size; i++) {
                x_in_buffer[i] = x_in_buffer[i*ds_factor];
            }
        }

        const int total_symbols = demod->ProcessBlock(x_in_buffer, IQ_demod_buffer);

        for (int i = 0; i < total_symbols; i++) {
            const auto IQ = IQ_demod_buffer[i];
            const auto res = frame_sync->process(IQ);
            auto p = frame_decoder->GetPayload();

            using Res = FrameSynchroniser::Result;
            switch (res) {
            case Res::PAYLOAD_OK:
                {
                    {
                        aud_data->total_packets++;
                        aud_data->correct_packets++;

                        if (p.decoded_error > 0) {
                            aud_data->repaired_packets++;
                        }
                    } 
                    payload_handler(p.buf, p.length);
                }
                break;
            case Res::PAYLOAD_ERR:
                {
                    {
                        aud_data->total_packets++;
                        aud_data->incorrect_packets++;
                    }
                }
                break;
            case Res::BLOCK_SIZE_OK:
                // LOG_MESSAGE("block_size=%d\n", p.length);
                break;
            case Res::PREAMBLE_FOUND:
                {
                    if (preamble_detector->GetDesyncBitcount() > 0) {
                        // LOG_MESSAGE("preamble desync: %d bits\n", s.desync_bitcount);
                    }
                    if (preamble_detector->IsPhaseConflict()) {
                        // LOG_MESSAGE("phase conflict\n");
                    }
                }
                break;
            }
        }

        if (*snapshot_trigger) {
            snapshot_buffer->CopyFrom(demod->buffers);
            *snapshot_trigger = false;
        }
    }
    
    delete [] IQ_mod_buffer;
    delete [] IQ_demod_buffer;
    delete [] x_in_buffer;
    delete frame_decoder;
    delete scrambler;
    delete crc8_calc;
    delete vitdec;
    delete frame_sync;
    delete preamble_detector;
    delete demod;
}

int main(int argc, char** argv)
{
    // app startup
    FILE* fp_in = stdin;

    // NOTE: Windows does extra translation stuff that messes up the file if this isn't done
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/setmode?view=msvc-170
    freopen(NULL, "wb", stdout);
    _setmode(fileno(stdout), _O_BINARY);

    if (argc > 1) {
        FILE* tmp = NULL;
        fopen_s(&tmp, argv[1], "r");
        if (tmp == NULL) {
            LOG_MESSAGE("Failed to open file for reading\n");
            return 1;
        } 
        fp_in = tmp;
    }

    // NOTE: Windows does extra translation stuff that messes up the file if this isn't done
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/setmode?view=msvc-170
    freopen(NULL, "rb", fp_in);
    _setmode(fileno(fp_in), _O_BINARY);

    // const int block_size = 4096;
    const int ds_factor = 1;
    const int block_size = 8192 / ds_factor;
    const float Fsample = 1e6 / (float)(ds_factor);
    const float Fsymbol = 87e3;
    const float Faudio = Fsymbol/5.0f;

    CarrierDemodulatorSpecification spec;
    {
        const float PI = 3.1415f;

        spec.f_sample = Fsample; 
        spec.f_symbol = Fsymbol;
        spec.baseband_filter.cutoff = Fsymbol;
        spec.baseband_filter.M = 10;
        spec.ac_filter.k = 0.99999f;
        spec.agc.beta = 0.1f;
        spec.agc.initial_gain = 0.1f;
        spec.carrier_pll.f_center = 0e3;
        spec.carrier_pll.f_gain = 2.5e3;
        spec.carrier_pll.phase_error_gain = 8.0f/PI;
        spec.carrier_pll_filter.butterworth_cutoff = 5e3;
        spec.carrier_pll_filter.integrator_gain = 1000.0f;
        spec.ted_pll.f_gain = 5e3;
        spec.ted_pll.f_offset = 0e3;
        spec.ted_pll.phase_error_gain = 1.0f;
        spec.ted_pll_filter.butterworth_cutoff = 10e3;
        spec.ted_pll_filter.integrator_gain = 250.0f;
    }

    auto constellation = new SquareConstellation(4);
    auto demod_buffer = new CarrierToSymbolDemodulatorBuffers(block_size);
    auto snapshot_buffer = new CarrierToSymbolDemodulatorBuffers(block_size);
    // swap between live and snapshot buffer
    auto render_buffer = demod_buffer;
    bool snapshot_trigger = false;

    // save demodulated audio
    const int audio_buffer_size = (int)(std::ceilf(Faudio));

    const int16_t audio_gain_min = 0;
    const int16_t audio_gain_max = 32;
    int16_t audio_gain = 8;
    auto aud_data = AudioData(audio_buffer_size);
    
    auto demod_thread = std::thread(
        demodulator_thread, 
        fp_in, ds_factor,
        block_size, demod_buffer, snapshot_buffer, 
        &spec, constellation,
        &snapshot_trigger,
        &aud_data, &audio_gain);

    // Generate x-axis timescale for implot
    auto time_scale = new float[block_size];
    const float Fs = 1e6;
    const float Ts = 1.0f/Fs;
    for (int i = 0; i < block_size; i++) {
        time_scale[i] = (float)i * Ts;
    }

    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

        // Decide GL+GLSL versions
    #if defined(IMGUI_IMPL_OPENGL_ES2)
        // GL ES 2.0 + GLSL 100
        const char* glsl_version = "#version 100";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    #elif defined(__APPLE__)
        // GL 3.2 + GLSL 150
        const char* glsl_version = "#version 150";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
    #else
        // GL 3.0 + GLSL 130
        const char* glsl_version = "#version 130";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
        //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
    #endif

    // Create window with graphics context
    glfwWindowHint(GLFW_MAXIMIZED, 1);
    GLFWwindow* window = glfwCreateWindow(
        1280, 720, 
        "QPSK Demodulator Telemetry", 
        NULL, NULL);

    if (window == NULL) {
        return 1;
    }

    glfwSetWindowFocusCallback(window, glfw_window_focus_callback);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
    //io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;

    // Setup Dear ImGui style
    // ImGui::StyleColorsDark();
    ImGui::StyleColorsLight();

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    ImGuiSetupCustomConfig();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/ProggyTiny.ttf", 10.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != NULL);
    io.Fonts->AddFontFromFileTTF("res/Roboto-Regular.ttf", 15.0f);
    {
        static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
        ImFontConfig icons_config; 
        icons_config.MergeMode = true; 
        icons_config.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF("res/font_awesome.ttf", 16.0f, &icons_config, icons_ranges);
    }

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    double x_min = 0.0f;
    double x_max = (double)block_size;
    double iq_stream_y_min = -1.25f;
    double iq_stream_y_max =  1.25f;

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();

        if (!is_main_window_focused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();

        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());

        ImGui::Begin("Telemetry");
        {
        auto dockspace_id = ImGui::GetID("Telemetry dockspace");
        ImGui::DockSpace(dockspace_id);
        }
        ImGui::End();

        ImGui::Begin("Audio Buffer");
        if (ImPlot::BeginPlot("##Audio buffer")) {
            ImPlot::SetupAxisLimits(ImAxis_Y1, 9, 256, ImPlotCond_Once);
            ImPlot::PlotLine("Audio", aud_data.audio_buffer, aud_data.audio_buffer_size);
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Begin("PCM 16Bit Buffer");
        if (ImPlot::BeginPlot("##Audio buffer")) {
            static double y0 = static_cast<double>(INT16_MAX);
            static double y1 = static_cast<double>(INT16_MIN);
            ImPlot::SetupAxisLimits(ImAxis_Y1, y1, y0, ImPlotCond_Once);
            ImPlot::PlotLine("Audio", aud_data.pcm_buffer, aud_data.audio_buffer_size);
            ImPlot::DragLineY(0, &y0, ImVec4(1,0,0,1), 1);
            ImPlot::DragLineY(1, &y1, ImVec4(1,0,0,1), 1);
            ImPlot::EndPlot();
        }
        ImGui::End();


        if (ImGui::Begin("Controls")) {
            const bool is_rendering_snapshot = (render_buffer == snapshot_buffer);
            if (!is_rendering_snapshot) {
                if (ImGui::Button("Snapshot")) {
                    snapshot_trigger = true;
                    render_buffer = snapshot_buffer;
                }
            } else {
                if (ImGui::Button("Resume")) {
                    render_buffer = demod_buffer;
                }
            }

            // ImGui::SliderFloat("Symbol frequency scaler", &demod.ted_clock.fcenter_factor, 0.0f, 2.0f);
            // ImGui::SliderFloat("Carrier frequency offset", &demod.pll_mixer.fcenter, -10000.0f, 10000.0f);
            int audio_gain_val = audio_gain;
            if (ImGui::SliderInt("Audio gain", &audio_gain_val, audio_gain_min, audio_gain_max)) {
                audio_gain = audio_gain_val;
            }
            ImGui::End();
        }

        if (ImGui::Begin("Statistics")) {
            ImGui::Text("Received=%d\n", aud_data.total_packets);
            ImGui::Text("Correct=%d\n", aud_data.correct_packets);
            ImGui::Text("Incorrect=%d\n", aud_data.incorrect_packets);
            ImGui::Text("Corrupted=%d\n", aud_data.corrupted_packets);
            ImGui::Text("Repaired=%d\n", aud_data.repaired_packets);
            const float PER = (float)aud_data.incorrect_packets / (float)aud_data.total_packets;
            const float RER = (float)aud_data.repaired_packets / (float)aud_data.correct_packets;
            ImGui::Text("Packet error rate=%.2f%%\n", PER*100.0f);
            ImGui::Text("Packet repair rate=%.2f%%\n", RER*100.0f);

            if (ImGui::Button("Reset")) {
                aud_data.total_packets = 0;
                aud_data.incorrect_packets = 0;
                aud_data.correct_packets = 0;
                aud_data.corrupted_packets = 0;
                aud_data.repaired_packets = 0;
            }
            ImGui::End();
        }


        ImGui::Begin("Constellation");
        if (ImPlot::BeginPlot("##Constellation", ImVec2(-1,0), ImPlotFlags_Equal)) {
            ImPlot::SetupAxisLimits(ImAxis_X1, -2, 2, ImPlotCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -2, 2, ImPlotCond_Once);
            const float marker_size = 3.0f;
            {
                auto buffer = reinterpret_cast<float*>(render_buffer->y_sym_out);
                ImPlot::SetNextMarkerStyle(0, marker_size);
                ImPlot::PlotScatter("IQ demod", &buffer[0], &buffer[1], block_size, 0, 0, 2*sizeof(float));
            }
            {
                auto buffer = reinterpret_cast<float*>(render_buffer->x_pll_out);
                ImPlot::HideNextItem(true, ImPlotCond_Once);
                ImPlot::SetNextMarkerStyle(0, marker_size);
                ImPlot::PlotScatter("IQ raw", &buffer[0], &buffer[1], block_size, 0, 0, 2*sizeof(float));
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Begin("IQ signals");
        if (ImPlot::BeginPlot("Symbol out")) {
            auto buffer = reinterpret_cast<float*>(render_buffer->y_sym_out);
            ImPlot::SetupAxisLinks(ImAxis_X1, &x_min, &x_max);
            ImPlot::SetupAxisLinks(ImAxis_Y1, &iq_stream_y_min, &iq_stream_y_max);
            ImPlot::PlotLine("I", &buffer[0], block_size, 1.0, 0.0, 0, 0, 2*sizeof(float));
            ImPlot::PlotLine("Q", &buffer[1], block_size, 1.0, 0.0, 0, 0, 2*sizeof(float));
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("PLL out")) {
            auto buffer = reinterpret_cast<float*>(render_buffer->x_pll_out);
            ImPlot::SetupAxisLinks(ImAxis_X1, &x_min, &x_max);
            ImPlot::SetupAxisLinks(ImAxis_Y1, &iq_stream_y_min, &iq_stream_y_max);
            ImPlot::PlotLine("I", &buffer[0], block_size, 1.0, 0.0, 0, 0, 2*sizeof(float));
            ImPlot::PlotLine("Q", &buffer[1], block_size, 1.0, 0.0, 0, 0, 2*sizeof(float));
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Begin("Errors");
        if (ImPlot::BeginPlot("##Errors")) {
            ImPlot::SetupAxisLinks(ImAxis_X1, &x_min, &x_max);
            ImPlot::PlotLine("PLL error", render_buffer->error_pll, block_size);
            ImPlot::PlotLine("TED error", render_buffer->error_ted, block_size);
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Begin("Triggers");
        if (ImPlot::BeginPlot("##Triggers")) {
            ImPlot::SetupAxisLinks(ImAxis_X1, &x_min, &x_max);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -0.2, 1.5, ImPlotCond_Once);
            ImPlot::PlotStems("Zero crossing", (uint8_t*)render_buffer->trig_zero_crossing, block_size);
            ImPlot::PlotStems("Ramp oscillator", (uint8_t*)render_buffer->trig_ted_clock, block_size);
            ImPlot::PlotStems("Integrate+dump", (uint8_t*)render_buffer->trig_integrator_dump, block_size);
            ImPlot::EndPlot();
        }
        ImGui::End();

        // Rendering
        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    	
        // Update and Render additional Platform Windows
        // (Platform functions may change the current OpenGL context, so we save/restore it to make it easier to paste this code elsewhere.
        //  For this specific demo app we could also call glfwMakeContextCurrent(window) directly)
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        glfwSwapBuffers(window);
    }


    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();

    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    // closing down
    demod_thread.join();
    fclose(fp_in);

    delete constellation;
    delete demod_buffer;
    delete snapshot_buffer;

    return 0;
}
