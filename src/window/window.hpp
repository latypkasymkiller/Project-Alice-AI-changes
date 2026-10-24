#pragma once

#include <chrono>

#include "constants.hpp"
#include "system_state_forward.hpp"

namespace ui {
class element_base;
}


struct ITfThreadMgr;

namespace window {
struct text_services_object;

struct win32_text_services {
private:
	ITfThreadMgr* manager_ptr = nullptr;
	unsigned long client_id = 0;
	bool send_notifications = true;
public:
	win32_text_services();
	~win32_text_services();
	void start_text_services();
	void end_text_services();
	text_services_object* create_text_service_object(sys::state&, ui::element_base& ei);
	void on_text_change(text_services_object*, uint32_t old_start, uint32_t old_end, uint32_t new_end);
	void on_selection_change(text_services_object*);
	void set_focus(sys::state& win, text_services_object*);
	void suspend_keystroke_handling();
	void resume_keystroke_handling();
	bool send_mouse_event_to_tso(text_services_object* ts, int32_t x, int32_t y, uint32_t buttons);
	friend struct text_services_object;
};
}

#ifdef _WIN64

#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"

typedef struct HWND__* HWND;
typedef struct HDC__* HDC;

namespace window {
class window_data_impl {
public:
	win32_text_services text_services;
	HWND hwnd = nullptr;
	HDC opengl_window_dc = nullptr;
	HCURSOR cursors[9] = { HCURSOR(NULL) };
	std::chrono::time_point<std::chrono::steady_clock> last_dbl_click;

	int32_t creation_x_size = 600;
	int32_t creation_y_size = 400;

	bool in_fullscreen = false;
	bool left_mouse_down = false;
};
} // namespace window
#elif defined(__ANDROID__)
struct ANativeWindow;
struct android_app;
typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* EGLConfig;

namespace window {
class window_data_impl {
public:
	win32_text_services text_services;
	// ver window_android.cpp: o contexto EGL vive enquanto o app existir; a
	// superficie (ANativeWindow) e criada/destruida a cada APP_CMD_INIT_WINDOW /
	// APP_CMD_TERM_WINDOW (app em segundo plano, tela desligada etc.)
	android_app* app = nullptr;
	ANativeWindow* native_window = nullptr;
	EGLDisplay egl_display = nullptr;
	EGLConfig egl_config = nullptr;
	EGLSurface egl_surface = nullptr;
	EGLContext egl_context = nullptr;

	int32_t creation_x_size = 600;
	int32_t creation_y_size = 400;

	bool in_fullscreen = true; // sempre fullscreen em Android
	bool left_mouse_down = false;

	bool resumed = false;      // entre APP_CMD_RESUME e APP_CMD_PAUSE
	bool has_focus = false;    // entre APP_CMD_GAINED_FOCUS e APP_CMD_LOST_FOCUS
	bool game_started = false; // android_start_game ja rodou (on_create etc.)
	bool loading = false;      // cenario sendo carregado/montado (tela de espera)
	bool first_run_defaults = false; // sem user_settings.dat: escala da interface pela tela, MSAA desligado

	// Renderizacao em resolucao reduzida: o jogo desenha em render_width x
	// render_height e o hardware de video do aparelho amplia para a tela
	// (ANativeWindow_setBuffersGeometry), sem custo para a GPU. O shader do mapa
	// e pesado (dezenas de leituras de textura por pixel) e a tela de um celular
	// tem 2-3 milhoes de pixels; 0,75 desenha ~44% menos pixels.
	float render_scale = 0.75f;
	int32_t physical_width = 0, physical_height = 0; // pixels da tela (coordenadas do toque)
	int32_t render_width = 0, render_height = 0;     // pixels desenhados (coordenadas do jogo)
	std::chrono::steady_clock::time_point last_background_save{};
};

// Escala da interface para uma tela de width x height pixels com a densidade
// dada (dpi do Android, 160 = 1x). Fica entre os valores de sys::ui_scales.
float android_default_ui_scale(int32_t width, int32_t height, int32_t density_dpi);

// Ponto de entrada do loop principal no Android, chamado pelo android_main
// (entry_point_android.cpp). So retorna quando o app e destruido.
void run_android_main_loop(sys::state& game_state, android_app* app);

// Implementado em entry_point_android.cpp: chamado a cada volta do loop
// principal (com ou sem superficie) para avancar a extracao dos arquivos,
// a escolha da pasta do jogo e o carregamento do cenario.
void android_launcher_update(sys::state& game_state);

// Implementado em entry_point_android.cpp: abre (mensagem) ou fecha ("") a
// janela de carregamento da AliceActivity. Pode ser chamado de qualquer thread.
void android_set_loading(char const* message);
// contador de desempenho (toque de tres dedos); vazio esconde
void android_set_stats(char const* text);

// Com o cenario carregado e uma superficie EGL ativa: inicia OpenGL, som e o
// on_create (o final do create_window de window_nix.cpp).
void android_start_game(sys::state& game_state);
} // namespace window
#else
struct GLFWwindow;

namespace window {
class window_data_impl {
public:
	win32_text_services text_services;
	GLFWwindow* window = nullptr;

	int32_t creation_x_size = 600;
	int32_t creation_y_size = 400;

	bool in_fullscreen = false;
	bool left_mouse_down = false;
};
} // namespace window
#endif

namespace sys {
struct state;
}

namespace window {
enum class window_state : uint8_t { normal, maximized, minimized };
struct creation_parameters {
	int32_t size_x = 1024;
	int32_t size_y = 768;
	window_state initial_state = window_state::maximized;
	bool borderless_fullscreen = false;
};

void create_window(sys::state& game_state,
		creation_parameters const& params);		 // this function will not return until the window is closed or otherwise destroyed
void close_window(sys::state& game_state); // close the main window
void set_borderless_full_screen(sys::state& game_state, bool fullscreen);
bool is_in_fullscreen(sys::state const& game_state);
bool is_key_depressed(sys::state const& game_state, sys::virtual_key key); // why not cheer it up then?

enum class cursor_type : uint8_t {
	normal,
	busy,
	drag_select,
	hostile_move,
	friendly_move,
	no_move,
	text,
	normal_cancel_busy
};
void change_cursor(sys::state& state, cursor_type type);

void get_window_size(sys::state const& game_state, int& width, int& height);
int32_t cursor_blink_ms();
int32_t double_click_ms();
void release_text_services_object(text_services_object* ptr);

void emit_error_message(std::string const& content, bool fatal); // also terminates the program if fatal
} // namespace window
