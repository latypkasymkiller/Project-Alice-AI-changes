// window_android.cpp
//
// Implementacao Android da interface declarada em window.hpp -- sem GLFW.
//
// Ciclo de vida (android_native_app_glue -> handle_cmd):
//   APP_CMD_INIT_WINDOW  -> cria display/contexto EGL (so na primeira vez) e a
//                           superficie para a ANativeWindow nova
//   APP_CMD_TERM_WINDOW  -> destroi so a superficie; o contexto (e com ele as
//                           texturas/shaders do jogo) continua vivo
//   APP_CMD_RESUME/PAUSE -> liga/desliga o loop de render
//   APP_CMD_GAINED/LOST_FOCUS -> pausa o som (se mute_on_focus_lost)
//   APP_CMD_WINDOW_RESIZED/CONFIG_CHANGED -> atualiza x_size/y_size
//   APP_CMD_DESTROY      -> app->destroyRequested, o loop sai
//
// O loop so desenha com superficie + app em primeiro plano; fora isso ele
// bloqueia no ALooper sem gastar CPU/bateria.
//
// Toque -> eventos de mouse do jogo (ver a secao "entrada" abaixo):
//   toque rapido                -> clique esquerdo
//   segurar parado (~0,5 s)     -> clique direito (ordens de movimento etc.)
//   arrastar sobre a interface  -> arrasto com o botao esquerdo (barras, janelas)
//   arrastar sobre o mapa       -> arrasto com o botao do meio (move o mapa)
//   dois dedos                  -> pinca = zoom (roda do mouse), mover = arrasta o mapa
//   tres dedos                  -> mostra/esconde o contador de desempenho
//   botao/gesto de voltar       -> ESC
// Mouse e teclado fisicos (USB/Bluetooth) tambem funcionam.

#include "window.hpp"
#include "system_state.hpp"

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>

#define ALICE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Alice", __VA_ARGS__)
#define ALICE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Alice", __VA_ARGS__)

namespace window {

int32_t cursor_blink_ms() {
	return 1000; // igual ao valor fixo usado em window_nix.cpp
}
int32_t double_click_ms() {
	return 500; // igual ao valor fixo usado em window_nix.cpp
}

bool is_key_depressed(sys::state const& game_state, sys::virtual_key key) {
	// TODO (entrada): teclado fisico/bluetooth via AInputEvent. Em touchscreen
	// nao ha teclas "seguradas", entao false e o comportamento correto por ora.
	return false;
}

void get_window_size(sys::state const& game_state, int& width, int& height) {
	if(game_state.win_ptr && game_state.win_ptr->native_window && game_state.win_ptr->render_width > 0) {
		width = game_state.win_ptr->render_width;
		height = game_state.win_ptr->render_height;
	} else {
		// sem janela (app em segundo plano): ultimo tamanho conhecido
		width = game_state.x_size;
		height = game_state.y_size;
	}
}

bool is_in_fullscreen(sys::state const& game_state) {
	return true; // no Android o jogo sempre ocupa a tela toda
}

void set_borderless_full_screen(sys::state& game_state, bool fullscreen) {
	// no-op: no Android nao existe "janela" pra alternar -- sempre fullscreen
}

void close_window(sys::state& game_state) {
	// equivalente ao glfwSetWindowShouldClose: pede pro sistema fechar a
	// Activity, que chega de volta como APP_CMD_DESTROY
	if(game_state.win_ptr && game_state.win_ptr->app)
		ANativeActivity_finish(game_state.win_ptr->app->activity);
}

void create_window(sys::state& game_state, creation_parameters const& params) {
	// No Android quem cria a janela e o sistema (APP_CMD_INIT_WINDOW), e o
	// loop principal e o run_android_main_loop chamado pelo android_main.
	emit_error_message("create_window nao e usado no Android -- ver run_android_main_loop", false);
}

namespace {

// pixels desenhados / pixels da tela: converte as coordenadas do toque e do
// mouse (sempre em pixels da tela) para as do jogo
float g_input_scale = 1.f;

// Define o tamanho dos buffers da janela pela render_scale. O hardware de
// composicao do Android amplia a imagem para a tela inteira.
void apply_render_scale(window_data_impl& win) {
	if(!win.native_window)
		return;
	// (0, 0) volta ao tamanho natural, para medir a tela de verdade
	ANativeWindow_setBuffersGeometry(win.native_window, 0, 0, WINDOW_FORMAT_RGBX_8888);
	win.physical_width = ANativeWindow_getWidth(win.native_window);
	win.physical_height = ANativeWindow_getHeight(win.native_window);
	if(win.physical_width <= 0 || win.physical_height <= 0)
		return;
	float const scale = std::clamp(win.render_scale, 0.25f, 1.f);
	win.render_width = std::max(1, int32_t(std::lround(float(win.physical_width) * scale)));
	win.render_height = std::max(1, int32_t(std::lround(float(win.physical_height) * scale)));
	if(win.render_width != win.physical_width || win.render_height != win.physical_height)
		ANativeWindow_setBuffersGeometry(win.native_window, win.render_width, win.render_height, WINDOW_FORMAT_RGBX_8888);
	g_input_scale = float(win.render_width) / float(win.physical_width);
}

bool init_egl_display(window_data_impl& win) {
	win.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if(win.egl_display == EGL_NO_DISPLAY || !eglInitialize(win.egl_display, nullptr, nullptr)) {
		ALICE_LOGE("eglInitialize falhou (0x%x)", eglGetError());
		return false;
	}

	// Superficie OPACA (sem canal alfa). Com alfa, os pixels semitransparentes
	// que o jogo deixa no framebuffer (bordas e texturas com transparencia dos
	// paineis) viram pixels translucidos da janela do app, e o compositor do
	// Android os mistura com o que esta atras -- os paineis piscavam na Adreno.
	// No desktop o alfa da janela e ignorado, entao la isso nunca apareceu.
	EGLint const config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0,
		EGL_DEPTH_SIZE, 24,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	// eglChooseConfig trata os tamanhos como minimos e ordena primeiro os com
	// mais bits de cor (RGBA8 antes de RGB8), entao procura um sem alfa
	EGLConfig configs[64];
	EGLint num_configs = 0;
	if(!eglChooseConfig(win.egl_display, config_attribs, configs, 64, &num_configs) || num_configs == 0) {
		ALICE_LOGE("nenhuma configuracao EGL com GLES 3 + RGB8 + depth24/stencil8 (0x%x)", eglGetError());
		return false;
	}
	win.egl_config = configs[0];
	for(EGLint i = 0; i < num_configs; ++i) {
		EGLint r = 0, g = 0, b = 0, a = 0;
		eglGetConfigAttrib(win.egl_display, configs[i], EGL_RED_SIZE, &r);
		eglGetConfigAttrib(win.egl_display, configs[i], EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib(win.egl_display, configs[i], EGL_BLUE_SIZE, &b);
		eglGetConfigAttrib(win.egl_display, configs[i], EGL_ALPHA_SIZE, &a);
		if(r == 8 && g == 8 && b == 8 && a == 0) {
			win.egl_config = configs[i];
			break;
		}
	}
	EGLint chosen_alpha = 0;
	eglGetConfigAttrib(win.egl_display, win.egl_config, EGL_ALPHA_SIZE, &chosen_alpha);
	ALICE_LOGI("configuracao EGL: %s", chosen_alpha == 0 ? "RGB8 opaca" : "RGBA8 (sem opcao opaca; buffer marcado como RGBX)");
	return true;
}

bool create_egl_context(window_data_impl& win) {
	// o jogo precisa de GLES 3.2 (ver uses-feature no AndroidManifest.xml)
	EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 2,
#ifndef NDEBUG
		EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE,
#endif
		EGL_NONE
	};
	win.egl_context = eglCreateContext(win.egl_display, win.egl_config, EGL_NO_CONTEXT, context_attribs);
#ifndef NDEBUG
	if(win.egl_context == EGL_NO_CONTEXT) {
		// alguns drivers recusam contexto de debug -- tenta sem
		context_attribs[4] = EGL_NONE;
		win.egl_context = eglCreateContext(win.egl_display, win.egl_config, EGL_NO_CONTEXT, context_attribs);
	}
#endif
	if(win.egl_context == EGL_NO_CONTEXT) {
		ALICE_LOGE("eglCreateContext (GLES 3.2) falhou (0x%x)", eglGetError());
		return false;
	}
	return true;
}

bool create_egl_surface(window_data_impl& win, ANativeWindow* native_window) {
	win.native_window = native_window;
	apply_render_scale(win);
	win.egl_surface = eglCreateWindowSurface(win.egl_display, win.egl_config, native_window, nullptr);
	if(win.egl_surface == EGL_NO_SURFACE) {
		ALICE_LOGE("eglCreateWindowSurface falhou (0x%x)", eglGetError());
		return false;
	}
	if(!eglMakeCurrent(win.egl_display, win.egl_surface, win.egl_surface, win.egl_context)) {
		ALICE_LOGE("eglMakeCurrent falhou (0x%x)", eglGetError());
		return false;
	}
	eglSwapInterval(win.egl_display, 1); // vsync, como o glfwSwapInterval(1) do desktop
	return true;
}

void destroy_egl_surface(window_data_impl& win) {
	if(win.egl_display != EGL_NO_DISPLAY) {
		eglMakeCurrent(win.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if(win.egl_surface != EGL_NO_SURFACE)
			eglDestroySurface(win.egl_display, win.egl_surface);
	}
	win.egl_surface = EGL_NO_SURFACE;
	win.native_window = nullptr;
}

void terminate_egl(window_data_impl& win) {
	destroy_egl_surface(win);
	if(win.egl_display != EGL_NO_DISPLAY) {
		if(win.egl_context != EGL_NO_CONTEXT)
			eglDestroyContext(win.egl_display, win.egl_context);
		eglTerminate(win.egl_display);
	}
	win.egl_context = EGL_NO_CONTEXT;
	win.egl_display = EGL_NO_DISPLAY;
}

void update_window_size(sys::state& state) {
	auto& win = *state.win_ptr;
	if(!win.native_window)
		return;
	apply_render_scale(win); // rotacao etc.: mede a tela de novo
	int32_t const width = win.render_width;
	int32_t const height = win.render_height;
	if(width <= 0 || height <= 0 || (width == state.x_size && height == state.y_size))
		return;
	ALICE_LOGI("tela %dx%d, desenhando em %dx%d (escala %.2f)", win.physical_width, win.physical_height, width, height, win.render_scale);
	if(win.game_started)
		state.on_resize(width, height, window_state::maximized);
	state.x_size = width;
	state.y_size = height;
}

void on_window_created(sys::state& state, ANativeWindow* native_window) {
	auto& win = *state.win_ptr;
	if(win.egl_display == EGL_NO_DISPLAY) {
		if(!init_egl_display(win) || !create_egl_context(win)) {
			emit_error_message("Nao foi possivel iniciar o OpenGL ES 3.2 (EGL). O aparelho e compativel?", true);
		}
	}
	if(!create_egl_surface(win, native_window)) {
		emit_error_message("Nao foi possivel criar a superficie de desenho (EGL).", true);
	}

	static bool logged_gl_info = false;
	if(!logged_gl_info) {
		logged_gl_info = true;
		ALICE_LOGI("GL_VERSION: %s", reinterpret_cast<char const*>(glGetString(GL_VERSION)));
		ALICE_LOGI("GL_RENDERER: %s", reinterpret_cast<char const*>(glGetString(GL_RENDERER)));
		ALICE_LOGI("GL_VENDOR: %s", reinterpret_cast<char const*>(glGetString(GL_VENDOR)));
	}

	update_window_size(state);
	// o jogo em si e iniciado pelo android_launcher_update quando o cenario
	// estiver carregado (ver android_start_game)
}

// Ao ir para segundo plano o Android pode matar o processo a qualquer momento
// para liberar memoria, sem aviso. Entao, numa partida single-player:
//  - pausa a simulacao como o botao de pausa faz (guarda a velocidade em
//    held_game_speed), para o tempo do jogo nao correr com o app fora da tela;
//  - pede um save pelo mesmo comando do menu "Salvar", que a thread do jogo
//    executa (nada de gravar o estado daqui, com a simulacao rodando).
// O arquivo e sempre o mesmo (sobrescrito), e no maximo um a cada 30 s.
constexpr char const* background_save_name = "android_autosave.bin";
constexpr auto background_save_interval = std::chrono::seconds(30);

void pause_and_save_on_background(sys::state& state) {
	auto& win = *state.win_ptr;
	if(!win.game_started || !state.current_scene.game_in_progress)
		return;
	if(state.network_mode != sys::network_mode_type::single_player)
		return; // no multiplayer o host decide pausa e saves
	if(!state.local_player_nation)
		return; // ex.: ainda escolhendo o pais

	if(auto const speed = state.actual_game_speed.load(); speed > 0) {
		state.ui_state.held_game_speed = speed;
		state.actual_game_speed = 0;
	}

	auto const now = std::chrono::steady_clock::now();
	if(now - win.last_background_save < background_save_interval)
		return;
	win.last_background_save = now;
	ALICE_LOGI("indo para segundo plano: pausando e salvando em %s", background_save_name);
	command::save_game(state, state.local_player_nation, false, background_save_name);
}

void handle_cmd(android_app* app, int32_t cmd) {
	auto& state = *static_cast<sys::state*>(app->userData);
	auto& win = *state.win_ptr;

	switch(cmd) {
	case APP_CMD_INIT_WINDOW:
		ALICE_LOGI("APP_CMD_INIT_WINDOW");
		if(app->window)
			on_window_created(state, app->window);
		break;
	case APP_CMD_TERM_WINDOW:
		ALICE_LOGI("APP_CMD_TERM_WINDOW");
		destroy_egl_surface(win);
		break;
	case APP_CMD_WINDOW_RESIZED:
	case APP_CMD_CONFIG_CHANGED:
	case APP_CMD_CONTENT_RECT_CHANGED:
		update_window_size(state);
		break;
	case APP_CMD_GAINED_FOCUS:
		win.has_focus = true;
		if(win.game_started && state.user_settings.mute_on_focus_lost)
			sound::resume_all(state);
		break;
	case APP_CMD_LOST_FOCUS:
		win.has_focus = false;
		if(win.game_started && state.user_settings.mute_on_focus_lost)
			sound::pause_all(state);
		break;
	case APP_CMD_RESUME:
		ALICE_LOGI("APP_CMD_RESUME");
		win.resumed = true;
		break;
	case APP_CMD_PAUSE:
		ALICE_LOGI("APP_CMD_PAUSE");
		win.resumed = false;
		pause_and_save_on_background(state);
		break;
	case APP_CMD_SAVE_STATE:
		// TODO: autosave -- o Android pode matar o app em segundo plano
		break;
	case APP_CMD_DESTROY:
		ALICE_LOGI("APP_CMD_DESTROY");
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// entrada: toque, mouse e teclado
// ---------------------------------------------------------------------------

constexpr auto long_press_time = std::chrono::milliseconds(500);
constexpr float touch_slop_dp = 10.f;	// quanto o dedo pode mexer e ainda ser toque/segurar
constexpr float wheel_clicks_per_doubling = 3.f; // pinca: dobrar a distancia = 3 "cliques" da roda
// rolagem de UI: quantos pixels de arrasto com dois dedos equivalem a "um clique"
// da roda do mouse (aproximadamente uma linha/lista por cliques)
constexpr float ui_scroll_px_per_click = 56.f;

struct touch_tracker {
	enum class mode : uint8_t {
		none,
		pending,         // dedo encostou; ainda nao se sabe se e toque, segurar ou arrasto
		left_drag,       // arrasto na interface (botao esquerdo)
		map_pan,         // arrasto no mapa (botao do meio)
		pinch,           // dois dedos no mapa
		ui_scroll,       // dois dedos sobre a interface (rola listas, sem roda de mouse no celular)
		ignore_until_up  // gesto ja consumido; espera todos os dedos sairem
	};
	mode current = mode::none;
	int32_t primary_id = -1;
	float start_x = 0.f, start_y = 0.f;
	float last_x = 0.f, last_y = 0.f;
	float pinch_last_distance = 0.f;
	float scroll_last_mid_y = 0.f; // coordenada Y do ponto medio em pixels do jogo
	float scroll_last_mid_x = 0.f; // coordenada X do ponto medio em pixels do jogo
	float scroll_accumulator = 0.f; // fracao de "cliques" da roda acumulados
	std::chrono::steady_clock::time_point down_time;

	// so para o feedback visual enquanto o jogo nao esta carregado
	enum class flash : uint8_t { none, tap, long_press };
	flash last_flash = flash::none;
	std::chrono::steady_clock::time_point flash_time;
	float placeholder_zoom = 0.f;
};
touch_tracker touch;

// posicao do ponteiro em pixels do jogo (ver g_input_scale)
float event_x(AInputEvent const* event, size_t pointer) {
	return AMotionEvent_getX(event, pointer) * g_input_scale;
}
float event_y(AInputEvent const* event, size_t pointer) {
	return AMotionEvent_getY(event, pointer) * g_input_scale;
}

// em pixels do jogo
float touch_slop_px(android_app* app) {
	int32_t const density = app->config ? AConfiguration_getDensity(app->config) : ACONFIGURATION_DENSITY_MEDIUM;
	return touch_slop_dp * float(density > 0 ? density : ACONFIGURATION_DENSITY_MEDIUM) / float(ACONFIGURATION_DENSITY_MEDIUM) * g_input_scale;
}

sys::key_modifiers modifiers_from_meta(int32_t meta) {
	uint32_t val = 0;
	if(meta & AMETA_CTRL_ON)
		val |= uint32_t(sys::key_modifiers::modifiers_ctrl);
	if(meta & AMETA_ALT_ON)
		val |= uint32_t(sys::key_modifiers::modifiers_alt);
	if(meta & AMETA_SHIFT_ON)
		val |= uint32_t(sys::key_modifiers::modifiers_shift);
	return sys::key_modifiers(val);
}

// Os eventos so chegam ao jogo depois do on_create; no desktop eles rodam
// dentro do mesmo lock do render (ver o loop de window_nix.cpp).
template<typename F>
void dispatch_to_game(sys::state& state, F&& f) {
	if(!state.win_ptr->game_started)
		return;
	std::shared_lock lock(state.game_state_resetting_lock);
	state.game_state_resetting_cv.wait(lock, [&] { return !state.yield_game_state_resetting_lock; });
	f();
}

void game_mouse_move(sys::state& state, int32_t x, int32_t y, sys::key_modifiers mod) {
	dispatch_to_game(state, [&] {
		state.on_mouse_move(x, y, mod);
		if(state.win_ptr->left_mouse_down)
			state.on_mouse_drag(x, y, mod);
	});
	state.mouse_x_position = x;
	state.mouse_y_position = y;
}
void game_lbutton(sys::state& state, int32_t x, int32_t y, bool down, sys::key_modifiers mod) {
	dispatch_to_game(state, [&] {
		if(down)
			state.on_lbutton_down(x, y, mod);
		else
			state.on_lbutton_up(x, y, mod);
	});
	state.win_ptr->left_mouse_down = down;
	if(!down)
		state.ui_state.selecting_edit_text = ui::edit_selection_mode::none;
	state.mouse_x_position = x;
	state.mouse_y_position = y;
}
void game_rbutton(sys::state& state, int32_t x, int32_t y, bool down, sys::key_modifiers mod) {
	dispatch_to_game(state, [&] {
		if(down)
			state.on_rbutton_down(x, y, mod);
		else
			state.on_rbutton_up(x, y, mod);
	});
	state.mouse_x_position = x;
	state.mouse_y_position = y;
}
void game_mbutton(sys::state& state, int32_t x, int32_t y, bool down, sys::key_modifiers mod) {
	dispatch_to_game(state, [&] {
		if(down)
			state.on_mbutton_down(x, y, mod);
		else
			state.on_mbutton_up(x, y, mod);
	});
	state.mouse_x_position = x;
	state.mouse_y_position = y;
}
void game_wheel(sys::state& state, int32_t x, int32_t y, float amount, sys::key_modifiers mod) {
	dispatch_to_game(state, [&] { sys::on_mouse_wheel(state, x, y, mod, amount); });
	if(!state.win_ptr->game_started)
		touch.placeholder_zoom = std::clamp(touch.placeholder_zoom + amount, -10.f, 10.f);
}
void game_key(sys::state& state, sys::virtual_key key, bool down, sys::key_modifiers mod) {
	dispatch_to_game(state, [&] {
		if(down)
			state.on_key_down(key, mod);
		else
			state.on_key_up(key, mod);
	});
}

// O dedo esta sobre a interface (e nao sobre o mapa)? O jogo calcula
// ui_state.under_mouse a cada frame a partir de mouse_x/y_position, que ja foi
// atualizado no ACTION_DOWN -- entao no primeiro movimento ele ja vale.
bool touch_is_over_ui(sys::state const& state) {
	if(!state.win_ptr->game_started)
		return false;
	return state.ui_state.under_mouse != nullptr || state.iui_state.over_ui;
}

int32_t find_pointer_index(AInputEvent const* event, int32_t id) {
	size_t const count = AMotionEvent_getPointerCount(event);
	for(size_t i = 0; i < count; ++i) {
		if(AMotionEvent_getPointerId(event, i) == id)
			return int32_t(i);
	}
	return -1;
}

void pinch_geometry(AInputEvent const* event, float& mid_x, float& mid_y, float& distance) {
	float const x0 = event_x(event, 0), y0 = event_y(event, 0);
	float const x1 = event_x(event, 1), y1 = event_y(event, 1);
	mid_x = (x0 + x1) * 0.5f;
	mid_y = (y0 + y1) * 0.5f;
	distance = std::max(std::hypot(x1 - x0, y1 - y0), 1.f);
}

// encerra o gesto de um dedo em andamento, sem gerar clique
void end_single_finger_gesture(sys::state& state) {
	auto const x = int32_t(touch.last_x), y = int32_t(touch.last_y);
	auto const mod = sys::key_modifiers::modifiers_none;
	if(touch.current == touch_tracker::mode::left_drag)
		game_lbutton(state, x, y, false, mod);
	else if(touch.current == touch_tracker::mode::map_pan || touch.current == touch_tracker::mode::pinch)
		game_mbutton(state, x, y, false, mod);
	touch.current = touch_tracker::mode::none;
}

void start_flash(touch_tracker::flash f) {
	touch.last_flash = f;
	touch.flash_time = std::chrono::steady_clock::now();
}

// Contador de desempenho: quadros por segundo, tempo medio e pior quadro, e
// quantos dias do jogo passaram no ultimo segundo. O contador de FPS do proprio
// jogo (comando "fps" do console) depende de um elemento de interface que nao
// existe nos arquivos, entao este e mostrado pela AliceActivity.
struct perf_counter {
	bool visible = false;
	std::chrono::steady_clock::time_point window_start;
	std::chrono::steady_clock::time_point last_frame;
	int32_t frames = 0;
	float worst_ms = 0.f;
	int32_t start_date = 0;
};
perf_counter perf;

void toggle_perf_counter() {
	perf.visible = !perf.visible;
	perf.frames = 0;
	perf.worst_ms = 0.f;
	perf.window_start = perf.last_frame = std::chrono::steady_clock::now();
	perf.start_date = -1;
	android_set_stats(perf.visible ? "medindo..." : "");
}

void update_perf_counter(sys::state& state) {
	if(!perf.visible)
		return;
	auto const now = std::chrono::steady_clock::now();
	float const frame_ms = std::chrono::duration<float, std::milli>(now - perf.last_frame).count();
	perf.last_frame = now;
	int32_t const date = state.current_date.to_raw_value();
	if(perf.start_date < 0 || frame_ms > 1000.f) {
		// primeira medida, ou o app ficou parado em segundo plano: recomeca
		perf.window_start = now;
		perf.frames = 0;
		perf.worst_ms = 0.f;
		perf.start_date = date;
		return;
	}
	perf.worst_ms = std::max(perf.worst_ms, frame_ms);
	++perf.frames;

	float const elapsed_ms = std::chrono::duration<float, std::milli>(now - perf.window_start).count();
	if(elapsed_ms < 1000.f || perf.frames == 0)
		return;
	int32_t const speed = state.actual_game_speed.load(std::memory_order::relaxed);
	char speed_text[24] = "pausado";
	if(speed > 0)
		snprintf(speed_text, sizeof(speed_text), "velocidade %d", int(speed));
	char text[128];
	snprintf(text, sizeof(text), "FPS %.0f  quadro %.0f ms (pior %.0f)\n%s  dias/s %.1f",
		float(perf.frames) * 1000.f / elapsed_ms, elapsed_ms / float(perf.frames), perf.worst_ms,
		speed_text, float(date - perf.start_date) * 1000.f / elapsed_ms);
	android_set_stats(text);
	perf.window_start = now;
	perf.frames = 0;
	perf.worst_ms = 0.f;
	perf.start_date = date;
}

// ---------------------------------------------------------------------------
// reposicionando janelas que ficariam fora da tela
// ---------------------------------------------------------------------------

// Ajeita a posicao de uma janela de topo para ela caber na viewport virtual do
// pai (a cena inteira). O upstream deixa um "TODO move windows" no fim do
// update_ui_scale: sem isso, subir a escala da interface empurra janelas
// ancoradas por posicao fixa fora dos bordos e deixa botoes inalcansaveis --
// em telas pequenas de celular isso mata a jogabilidade.
// Janela MAIOR que a tela: os deslocamentos (dx/dy) permitem pana-la com dois
// dedos no intervalo [tamanho_da_tela - tamanho_da_janela .. 0], assim todo o
// conteudo continua alcancavel mesmo sem caber de uma vez. Sem deslocamento a
// funcao "encaixa" a janela normal no canto mais proximo.
static void fit_window_position(ui::element_base& root, ui::element_base& win, int32_t dx = 0, int32_t dy = 0) {
	int32_t const pw = root.base_data.size.x;
	int32_t const ph = root.base_data.size.y;
	int32_t const w = win.base_data.size.x;
	int32_t const h = win.base_data.size.y;
	if(pw <= 0 || ph <= 0 || w <= 0 || h <= 0)
		return;
	// janela que cabe: [0, dimensao - janela]; janela maior: o invervalo de pan
	int32_t const min_x = std::min(0, pw - w);
	int32_t const max_x = std::max(0, pw - w);
	int32_t const min_y = std::min(0, ph - h);
	int32_t const max_y = std::max(0, ph - h);
	ui::orientation const o = win.base_data.get_orientation();
	auto const resolved_x = [&]() {
		switch(o) { // ver child_relative_non_mirror_location em gui_graphics.cpp
			case ui::orientation::upper_right:
			case ui::orientation::lower_right:
				return int32_t(pw) + int32_t(win.base_data.position.x);
			case ui::orientation::upper_center:
			case ui::orientation::lower_center:
			case ui::orientation::center:
				return int32_t(pw) / 2 + int32_t(win.base_data.position.x);
			default: // upper_left, lower_left
				return int32_t(win.base_data.position.x);
		}
	}();
	auto const resolved_y = [&]() {
		switch(o) { // ver child_relative_location_y_component em gui_graphics.cpp
			case ui::orientation::lower_left:
			case ui::orientation::lower_right:
				return int32_t(ph) + int32_t(win.base_data.position.y);
			case ui::orientation::lower_center:
			case ui::orientation::center:
				return int32_t(ph) / 2 + int32_t(win.base_data.position.y);
			default: // upper_left, upper_right, upper_center
				return int32_t(win.base_data.position.y);
		}
	}();
	int32_t const new_x = std::clamp(resolved_x + dx, min_x, max_x);
	int32_t const new_y = std::clamp(resolved_y + dy, min_y, max_y);
	switch(o) {
		case ui::orientation::upper_right:
		case ui::orientation::lower_right:
			win.base_data.position.x = int16_t(new_x - int32_t(pw));
			break;
		case ui::orientation::upper_center:
		case ui::orientation::lower_center:
		case ui::orientation::center:
			win.base_data.position.x = int16_t(new_x - int32_t(pw) / 2);
			break;
		default:
			win.base_data.position.x = int16_t(new_x);
			break;
	}
	switch(o) {
		case ui::orientation::lower_left:
		case ui::orientation::lower_right:
			win.base_data.position.y = int16_t(new_y - int32_t(ph));
			break;
		case ui::orientation::lower_center:
		case ui::orientation::center:
			win.base_data.position.y = int16_t(new_y - int32_t(ph) / 2);
			break;
		default:
			win.base_data.position.y = int16_t(new_y);
			break;
	}
}

// Dois dedos sobre uma janela MAIOR que a tela: pana a propria janela no
// espaco que sobra, para alcancar todo o conteudo (botao de mobilizar etc.).
// Retorna false se a janela sob os dedos cabe na tela -- o gesto entao rola o
// conteudo como a roda do mouse.
static bool pan_oversized_window(sys::state& state, float dx, float dy) {
	auto* root_elm = state.current_scene.get_root(state);
	if(!root_elm)
		return false;
	float const inv_scale = 1.f / std::max(0.01f, state.user_settings.ui_scale);
	auto probe = root_elm->impl_probe_mouse(state, int32_t(float(state.mouse_x_position) * inv_scale),
		int32_t(float(state.mouse_y_position) * inv_scale), ui::mouse_probe_type::click);
	auto* target = probe.under_mouse;
	if(!target)
		return false;
	// sobe ate a janela de topo (filha direta da cena)
	while(target->parent != nullptr && target->parent != root_elm)
		target = target->parent;
	if(target->parent != root_elm)
		return false;
	bool const wide = target->base_data.size.x > root_elm->base_data.size.x;
	bool const tall = target->base_data.size.y > root_elm->base_data.size.y;
	if(!wide && !tall)
		return false;
	fit_window_position(*root_elm, *target,
		wide ? int32_t(std::lround(dx)) : 0,
		tall ? int32_t(std::lround(dy)) : 0);
	return true;
}

// Percorre as janelas de topo das cenas de janelas comuns e recoloca as que
// sairiam da tela. Roda todo quadro, antes do render: assim cobre a abertura
// de novas janelas e as mudancas de escala da interface sem um espelhamento
// em cada lugar. As outras raizes (rgos_root, province_details_root,
// military_root, army_group_*) guardam icones ancorados a provinces no mapa
// -- mudar a posicao delas quebra o desenho dos icones.
static void clamp_windows_into_screen(sys::state& state) {
	ui::element_base* const window_scenes[] = {
		state.ui_state.root.get(),
		state.ui_state.nation_picker.get(),
		state.ui_state.end_screen.get(),
	};
	for(auto* scene : window_scenes) {
		if(!scene)
			continue;
		for(int32_t index = 0;; ++index) {
			auto* child = scene->get_child_by_index(state, index);
			if(!child)
				break;
			if(!child->is_visible())
				continue;
			// (0,0): sem pan de gesto, apenas "encaixa" a janela no lugar
			fit_window_position(*scene, *child);
		}
	}
}

int32_t handle_touch(android_app* app, sys::state& state, AInputEvent* event) {
	int32_t const action = AMotionEvent_getAction(event);
	int32_t const masked = action & AMOTION_EVENT_ACTION_MASK;
	auto const mod = sys::key_modifiers::modifiers_none;

	switch(masked) {
	case AMOTION_EVENT_ACTION_DOWN: {
		touch.current = touch_tracker::mode::pending;
		touch.primary_id = AMotionEvent_getPointerId(event, 0);
		touch.start_x = touch.last_x = event_x(event, 0);
		touch.start_y = touch.last_y = event_y(event, 0);
		touch.down_time = std::chrono::steady_clock::now();
		// sem botao apertado: so posiciona o "cursor" para o jogo achar o que esta sob o dedo
		game_mouse_move(state, int32_t(touch.last_x), int32_t(touch.last_y), mod);
		break;
	}
	case AMOTION_EVENT_ACTION_POINTER_DOWN: {
		if(AMotionEvent_getPointerCount(event) == 3 && touch.current != touch_tracker::mode::ignore_until_up) {
			// terceiro dedo: desfaz a pinca que comecou com os dois primeiros
			end_single_finger_gesture(state);
			toggle_perf_counter();
			touch.current = touch_tracker::mode::ignore_until_up;
		} else if(AMotionEvent_getPointerCount(event) == 2 && touch.current != touch_tracker::mode::ignore_until_up) {
			end_single_finger_gesture(state);
			float mid_x, mid_y;
			pinch_geometry(event, mid_x, mid_y, touch.pinch_last_distance);
			touch.last_x = mid_x;
			touch.last_y = mid_y;
			game_mouse_move(state, int32_t(mid_x), int32_t(mid_y), mod);
			if(touch_is_over_ui(state)) {
				// dois dedos sobre a interface = rolar a lista (equivale a roda do
				// mouse) ou panear a janela se ela nao couber na tela; sem isso,
				// listas compridas nao rolam em tela de toque
				touch.scroll_last_mid_x = mid_x;
				touch.scroll_last_mid_y = mid_y;
				touch.scroll_accumulator = 0.f;
				touch.current = touch_tracker::mode::ui_scroll;
			} else {
				game_mbutton(state, int32_t(mid_x), int32_t(mid_y), true, mod); // arrastar o mapa pelo ponto medio
				touch.current = touch_tracker::mode::pinch;
			}
		}
		break;
	}
	case AMOTION_EVENT_ACTION_MOVE: {
		if(touch.current == touch_tracker::mode::pinch) {
			if(AMotionEvent_getPointerCount(event) < 2)
				break;
			float mid_x, mid_y, distance;
			pinch_geometry(event, mid_x, mid_y, distance);
			touch.last_x = mid_x;
			touch.last_y = mid_y;
			game_mouse_move(state, int32_t(mid_x), int32_t(mid_y), mod);
			float const clicks = std::log2(distance / touch.pinch_last_distance) * wheel_clicks_per_doubling;
			if(std::abs(clicks) > 0.01f) {
				game_wheel(state, int32_t(mid_x), int32_t(mid_y), clicks, mod);
				touch.pinch_last_distance = distance;
			}
			break;
		}
		if(touch.current == touch_tracker::mode::ui_scroll) {
			if(AMotionEvent_getPointerCount(event) < 2)
				break;
			float mid_x, mid_y, distance;
			pinch_geometry(event, mid_x, mid_y, distance);
			touch.last_x = mid_x;
			touch.last_y = mid_y;
			game_mouse_move(state, int32_t(mid_x), int32_t(mid_y), mod);
			// deslocamento do ponto medio (dedo descendo = positivo), em pixels
			// virtuais do jogo
			float const inv_scale = 1.f / std::max(0.01f, state.user_settings.ui_scale);
			float const dx = (mid_x - touch.scroll_last_mid_x) * inv_scale;
			float const dy = (mid_y - touch.scroll_last_mid_y) * inv_scale;
			touch.scroll_last_mid_x = mid_x;
			touch.scroll_last_mid_y = mid_y;
			bool const panned = pan_oversized_window(state, dx, dy);
			// janela maior que a tela: o gesto moveu a propria janela; caso
			// contrario rola o conteudo como a roda do mouse
			if(!panned && dy != 0.f) {
				touch.scroll_accumulator += dy / ui_scroll_px_per_click;
				float const clicks = std::truncf(touch.scroll_accumulator);
				if(clicks != 0.f) {
					touch.scroll_accumulator -= clicks;
					game_wheel(state, int32_t(mid_x), int32_t(mid_y), clicks, mod);
				}
			}
			break;
		}

		int32_t const index = find_pointer_index(event, touch.primary_id);
		if(index < 0)
			break;
		float const x = event_x(event, index);
		float const y = event_y(event, index);

		if(touch.current == touch_tracker::mode::pending) {
			if(std::hypot(x - touch.start_x, y - touch.start_y) < touch_slop_px(app))
				break;
			// virou arrasto: comeca no ponto onde o dedo encostou
			auto const sx = int32_t(touch.start_x), sy = int32_t(touch.start_y);
			if(touch_is_over_ui(state)) {
				game_lbutton(state, sx, sy, true, mod);
				touch.current = touch_tracker::mode::left_drag;
			} else {
				game_mbutton(state, sx, sy, true, mod);
				touch.current = touch_tracker::mode::map_pan;
			}
		}
		if(touch.current == touch_tracker::mode::left_drag || touch.current == touch_tracker::mode::map_pan) {
			touch.last_x = x;
			touch.last_y = y;
			game_mouse_move(state, int32_t(x), int32_t(y), mod);
		}
		break;
	}
	case AMOTION_EVENT_ACTION_POINTER_UP: {
		if(touch.current == touch_tracker::mode::pinch || touch.current == touch_tracker::mode::ui_scroll) {
			// saiu um dos dedos: termina o gesto e ignora o dedo restante
			// (senao ele viraria um toque/arrasto acidental)
			end_single_finger_gesture(state);
			touch.current = touch_tracker::mode::ignore_until_up;
		}
		break;
	}
	case AMOTION_EVENT_ACTION_UP: {
		if(touch.current == touch_tracker::mode::pending) {
			// toque rapido = clique esquerdo onde o dedo encostou
			auto const x = int32_t(touch.start_x), y = int32_t(touch.start_y);
			game_lbutton(state, x, y, true, mod);
			game_lbutton(state, x, y, false, mod);
			start_flash(touch_tracker::flash::tap);
			touch.current = touch_tracker::mode::none;
		} else {
			end_single_finger_gesture(state);
		}
		break;
	}
	case AMOTION_EVENT_ACTION_CANCEL: {
		end_single_finger_gesture(state);
		break;
	}
	default:
		break;
	}
	return 1;
}

// Segurar parado nao gera evento, entao o clique direito e verificado a cada frame.
void update_touch(sys::state& state) {
	if(touch.current != touch_tracker::mode::pending)
		return;
	if(std::chrono::steady_clock::now() - touch.down_time < long_press_time)
		return;
	auto const x = int32_t(touch.start_x), y = int32_t(touch.start_y);
	game_rbutton(state, x, y, true, sys::key_modifiers::modifiers_none);
	game_rbutton(state, x, y, false, sys::key_modifiers::modifiers_none);
	start_flash(touch_tracker::flash::long_press);
	touch.current = touch_tracker::mode::ignore_until_up;
}

// mouse USB/Bluetooth: repassa direto como no desktop
int32_t handle_mouse(sys::state& state, AInputEvent* event) {
	int32_t const masked = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
	auto const x = int32_t(event_x(event, 0));
	auto const y = int32_t(event_y(event, 0));
	auto const mod = modifiers_from_meta(AMotionEvent_getMetaState(event));

	switch(masked) {
	case AMOTION_EVENT_ACTION_HOVER_MOVE:
	case AMOTION_EVENT_ACTION_MOVE:
		game_mouse_move(state, x, y, mod);
		return 1;
	case AMOTION_EVENT_ACTION_BUTTON_PRESS:
	case AMOTION_EVENT_ACTION_BUTTON_RELEASE: {
		// AMotionEvent_getActionButton so existe na API 33+; compara com o estado anterior
		static int32_t previous_buttons = 0;
		int32_t const buttons = AMotionEvent_getButtonState(event);
		int32_t const changed = buttons ^ previous_buttons;
		previous_buttons = buttons;
		if(changed & AMOTION_EVENT_BUTTON_PRIMARY)
			game_lbutton(state, x, y, (buttons & AMOTION_EVENT_BUTTON_PRIMARY) != 0, mod);
		if(changed & AMOTION_EVENT_BUTTON_SECONDARY)
			game_rbutton(state, x, y, (buttons & AMOTION_EVENT_BUTTON_SECONDARY) != 0, mod);
		if(changed & AMOTION_EVENT_BUTTON_TERTIARY)
			game_mbutton(state, x, y, (buttons & AMOTION_EVENT_BUTTON_TERTIARY) != 0, mod);
		return 1;
	}
	case AMOTION_EVENT_ACTION_SCROLL:
		game_wheel(state, x, y, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0), mod);
		return 1;
	case AMOTION_EVENT_ACTION_DOWN:
	case AMOTION_EVENT_ACTION_UP:
		// ja tratados como BUTTON_PRESS/RELEASE
		return 1;
	default:
		return 0;
	}
}

sys::virtual_key android_key_to_virtual_key(int32_t code) {
	if(code >= AKEYCODE_A && code <= AKEYCODE_Z)
		return sys::virtual_key(uint8_t(sys::virtual_key::A) + (code - AKEYCODE_A));
	if(code >= AKEYCODE_0 && code <= AKEYCODE_9)
		return sys::virtual_key(uint8_t(sys::virtual_key::NUM_0) + (code - AKEYCODE_0));
	if(code >= AKEYCODE_F1 && code <= AKEYCODE_F12)
		return sys::virtual_key(uint8_t(sys::virtual_key::F1) + (code - AKEYCODE_F1));
	if(code >= AKEYCODE_NUMPAD_0 && code <= AKEYCODE_NUMPAD_9)
		return sys::virtual_key(uint8_t(sys::virtual_key::NUMPAD0) + (code - AKEYCODE_NUMPAD_0));
	switch(code) {
	case AKEYCODE_BACK: // botao/gesto de voltar
	case AKEYCODE_ESCAPE: return sys::virtual_key::ESCAPE;
	case AKEYCODE_ENTER:
	case AKEYCODE_NUMPAD_ENTER: return sys::virtual_key::RETURN;
	case AKEYCODE_TAB: return sys::virtual_key::TAB;
	case AKEYCODE_DEL: return sys::virtual_key::BACK; // backspace
	case AKEYCODE_FORWARD_DEL: return sys::virtual_key::DELETE_KEY;
	case AKEYCODE_SPACE: return sys::virtual_key::SPACE;
	case AKEYCODE_DPAD_LEFT: return sys::virtual_key::LEFT;
	case AKEYCODE_DPAD_RIGHT: return sys::virtual_key::RIGHT;
	case AKEYCODE_DPAD_UP: return sys::virtual_key::UP;
	case AKEYCODE_DPAD_DOWN: return sys::virtual_key::DOWN;
	case AKEYCODE_PAGE_UP: return sys::virtual_key::PRIOR;
	case AKEYCODE_PAGE_DOWN: return sys::virtual_key::NEXT;
	case AKEYCODE_MOVE_HOME: return sys::virtual_key::HOME;
	case AKEYCODE_MOVE_END: return sys::virtual_key::END;
	case AKEYCODE_INSERT: return sys::virtual_key::INSERT;
	case AKEYCODE_SHIFT_LEFT: return sys::virtual_key::LSHIFT;
	case AKEYCODE_SHIFT_RIGHT: return sys::virtual_key::RSHIFT;
	case AKEYCODE_CTRL_LEFT: return sys::virtual_key::LCONTROL;
	case AKEYCODE_CTRL_RIGHT: return sys::virtual_key::RCONTROL;
	case AKEYCODE_ALT_LEFT: return sys::virtual_key::LMENU;
	case AKEYCODE_ALT_RIGHT: return sys::virtual_key::RMENU;
	case AKEYCODE_COMMA: return sys::virtual_key::COMMA;
	case AKEYCODE_PERIOD: return sys::virtual_key::PERIOD;
	case AKEYCODE_MINUS: return sys::virtual_key::MINUS;
	case AKEYCODE_EQUALS:
	case AKEYCODE_PLUS: return sys::virtual_key::PLUS;
	case AKEYCODE_NUMPAD_ADD: return sys::virtual_key::ADD;
	case AKEYCODE_NUMPAD_SUBTRACT: return sys::virtual_key::SUBTRACT;
	case AKEYCODE_SEMICOLON: return sys::virtual_key::SEMICOLON;
	case AKEYCODE_SLASH: return sys::virtual_key::FORWARD_SLASH;
	case AKEYCODE_BACKSLASH: return sys::virtual_key::BACK_SLASH;
	case AKEYCODE_LEFT_BRACKET: return sys::virtual_key::OPEN_BRACKET;
	case AKEYCODE_RIGHT_BRACKET: return sys::virtual_key::CLOSED_BRACKET;
	case AKEYCODE_APOSTROPHE: return sys::virtual_key::QUOTE;
	case AKEYCODE_GRAVE: return sys::virtual_key::TILDA;
	default: return sys::virtual_key::NONE;
	}
}

int32_t handle_key(sys::state& state, AInputEvent* event) {
	int32_t const code = AKeyEvent_getKeyCode(event);
	auto const key = android_key_to_virtual_key(code);
	if(key == sys::virtual_key::NONE)
		return 0; // volume etc.: deixa o sistema tratar

	auto const mod = modifiers_from_meta(AKeyEvent_getMetaState(event));
	switch(AKeyEvent_getAction(event)) {
	case AKEY_EVENT_ACTION_DOWN:
		// repeticao so para as teclas de edicao, como no window_nix.cpp
		if(AKeyEvent_getRepeatCount(event) > 0) {
			switch(key) {
			case sys::virtual_key::RETURN: case sys::virtual_key::BACK: case sys::virtual_key::DELETE_KEY:
			case sys::virtual_key::LEFT: case sys::virtual_key::RIGHT: case sys::virtual_key::UP: case sys::virtual_key::DOWN:
				break;
			default:
				return 1;
			}
		}
		game_key(state, key, true, mod);
		return 1;
	case AKEY_EVENT_ACTION_UP:
		game_key(state, key, false, mod);
		return 1;
	default:
		return 1;
	}
	// TODO (entrada): caracteres digitados (on_text) precisam do teclado virtual
	// e de KeyEvent.getUnicodeChar via JNI -- a API nativa nao entrega o caractere.
}

int32_t handle_input(android_app* app, AInputEvent* event) {
	auto& state = *static_cast<sys::state*>(app->userData);
	switch(AInputEvent_getType(event)) {
	case AINPUT_EVENT_TYPE_MOTION:
		if(AInputEvent_getSource(event) == AINPUT_SOURCE_MOUSE)
			return handle_mouse(state, event);
		if(AInputEvent_getSource(event) & AINPUT_SOURCE_CLASS_POINTER)
			return handle_touch(app, state, event);
		return 0;
	case AINPUT_EVENT_TYPE_KEY:
		return handle_key(state, event);
	default:
		return 0;
	}
}

// Enquanto o jogo nao esta carregado: mostra os gestos reconhecidos, para
// testar o toque no aparelho. Fundo clareia/escurece com a pinca; quadrado
// branco = dedo arrastando, amarelo = toque (clique), vermelho = segurar (clique direito).
void render_touch_placeholder(sys::state& state) {
	float z = touch.placeholder_zoom / 10.f; // -1..1
	if(state.win_ptr->loading) {
		// carregando: o fundo "respira" para mostrar que o app nao travou
		auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		z += 0.5f * std::sin(float(ms % 2000) / 2000.f * 6.2831853f);
	}
	glViewport(0, 0, state.x_size, state.y_size);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.11f + 0.08f * z, 0.20f + 0.10f * z, 0.33f + 0.12f * z, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	auto square = [&](float x, float y, float r, float g, float b) {
		int32_t const half = int32_t(touch_slop_px(state.win_ptr->app) * 4.f);
		glEnable(GL_SCISSOR_TEST);
		glScissor(int32_t(x) - half, state.y_size - int32_t(y) - half, half * 2, half * 2); // GL tem y para cima
		glClearColor(r, g, b, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
	};

	using m = touch_tracker::mode;
	if(touch.current == m::left_drag || touch.current == m::map_pan || touch.current == m::pinch || touch.current == m::pending)
		square(touch.last_x, touch.last_y, 0.95f, 0.95f, 0.95f);

	if(std::chrono::steady_clock::now() - touch.flash_time < std::chrono::milliseconds(300)) {
		if(touch.last_flash == touch_tracker::flash::tap)
			square(touch.start_x, touch.start_y, 0.98f, 0.85f, 0.20f);
		else if(touch.last_flash == touch_tracker::flash::long_press)
			square(touch.start_x, touch.start_y, 0.90f, 0.25f, 0.20f);
	}
}

bool can_render(window_data_impl const& win) {
	return win.resumed && win.egl_surface != EGL_NO_SURFACE;
}

void render_frame(sys::state& state) {
	auto& win = *state.win_ptr;

	if(win.game_started) {
		// Carregar um save trava a tela ate a thread do jogo terminar (e o
		// pedido de cursor "ocupado" dela fica na fila desta mesma tela). A
		// janela de carregamento abre antes de esperar, e recebe os toques.
		bool const must_wait = state.yield_game_state_resetting_lock;
		if(must_wait)
			android_set_loading("Carregando...");
		std::shared_lock lock(state.game_state_resetting_lock);
		state.game_state_resetting_cv.wait(lock, [&] { return !state.yield_game_state_resetting_lock; });
		if(must_wait)
			android_set_loading("");
		state.render();
	} else {
		// sem jogo carregado ainda: prova que EGL/superficie/vsync e o toque funcionam
		render_touch_placeholder(state);
	}

	// GPU de celular desenha em blocos no proprio chip; avisar que depth e
	// stencil do quadro nao serao mais usados evita grava-los na memoria.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	GLenum const discard[] = { GL_DEPTH, GL_STENCIL };
	glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, discard);

	if(!eglSwapBuffers(win.egl_display, win.egl_surface)) {
		EGLint const err = eglGetError();
		ALICE_LOGE("eglSwapBuffers falhou (0x%x)", err);
		if(err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) {
			// a janela sumiu entre o poll e o swap -- recria na mesma janela se ainda existir
			ANativeWindow* native_window = win.native_window;
			destroy_egl_surface(win);
			if(native_window && win.app->window == native_window)
				create_egl_surface(win, native_window);
		} else if(err == EGL_CONTEXT_LOST) {
			// raro (driver reiniciado): todos os recursos GL se perderam
			emit_error_message("O contexto OpenGL ES foi perdido pelo sistema.", true);
		}
	}

	if(win.game_started)
		sound::update_music_track(state);
}

} // namespace

float android_default_ui_scale(int32_t width, int32_t height, int32_t density_dpi) {
	// A interface do Victoria 2 foi feita para pelo menos ~1024x720 pixels:
	// com escala s, a tela "logica" e (width/s) x (height/s). Entao:
	//  - pela densidade: s = dpi/160 (1 dp do Android = 1 pixel da interface),
	//    o que deixa botoes e textos com tamanho fisico confortavel no dedo;
	//  - mas sem passar da maior escala que ainda cabe 1024x720 (num celular em
	//    paisagem e isso que limita: 1080 de altura -> no maximo 1,5).
	int32_t const long_side = std::max(width, height);
	int32_t const short_side = std::min(width, height);
	float const by_density = float(density_dpi > 0 ? density_dpi : 160) / 160.f;
	float const fits = std::min(float(long_side) / 1024.f, float(short_side) / 720.f);
	float const target = std::min(by_density, fits);

	// o maior valor da lista de opcoes que nao passa do alvo (minimo 0,75)
	float chosen = 0.75f;
	for(float v : sys::ui_scales) {
		if(v <= target + 0.001f && v >= chosen)
			chosen = v;
	}
	return chosen;
}

void android_start_game(sys::state& state) {
	auto& win = *state.win_ptr;
	assert(win.egl_surface != EGL_NO_SURFACE && !win.game_started);
	ALICE_LOGI("iniciando o jogo (OpenGL, som, on_create)");

	ogl::initialize_opengl(state);

	sound::initialize_sound_system(state);
	sound::start_music(state, state.user_settings.master_volume * state.user_settings.music_volume);

	// equivalente ao on_window_change inicial do window_nix.cpp
	int32_t const width = win.render_width;
	int32_t const height = win.render_height;

	if(win.first_run_defaults) {
		// primeira execucao (sem user_settings.dat); fica salvo e pode ser
		// mudado depois nas opcoes do jogo:
		//  - escala da interface pela tela. A densidade conta em pixels
		//    desenhados: com render_scale 0,75 cada um ocupa mais espaco na tela.
		int32_t const density = win.app->config ? AConfiguration_getDensity(win.app->config) : ACONFIGURATION_DENSITY_MEDIUM;
		int32_t const render_density = int32_t(float(density) * float(width) / float(std::max(1, win.physical_width)));
		state.user_settings.ui_scale = android_default_ui_scale(width, height, render_density);
		//  - sem MSAA: o jogo desenha numa textura multisample separada e resolve
		//    com um shader proprio; numa GPU de celular (tile-based) isso tira as
		//    4 amostras de cada pixel do chip para a memoria a cada quadro.
		state.user_settings.antialias_level = 0;
		state.save_user_settings();
		win.first_run_defaults = false;
		ALICE_LOGI("primeira execucao: escala da interface %.2f (desenho %dx%d, %d dpi efetivos), MSAA desligado", state.user_settings.ui_scale, width, height, render_density);
	}

	state.on_resize(width, height, window_state::maximized);
	state.x_size = width;
	state.y_size = height;

	state.on_create();
	win.loading = false;
	win.game_started = true;

	// Esta thread le o toque e desenha. Com o tempo correndo, a simulacao ocupa
	// todos os nucleos (thread do jogo + workers do TBB) e, com a mesma
	// prioridade, o toque espera a vez. Prioridade de "display urgente", a mesma
	// que o Android usa na thread de desenho dos apps. Fica para o fim: threads
	// herdam a prioridade de quem as cria, e as do jogo ja foram criadas.
	if(setpriority(PRIO_PROCESS, gettid(), -8) != 0)
		ALICE_LOGE("nao foi possivel subir a prioridade da thread de desenho (errno %d)", errno);
}

void run_android_main_loop(sys::state& game_state, android_app* app) {
	game_state.win_ptr = std::make_unique<window_data_impl>();
	auto& win = *game_state.win_ptr;
	win.app = app;

	app->userData = &game_state;
	app->onAppCmd = handle_cmd;
	app->onInputEvent = handle_input;

	while(!app->destroyRequested) {
		// renderizando: so processa o que ja chegou (timeout 0); parado: espera evento
		int const timeout = can_render(win) ? 0 : -1;
		android_poll_source* source = nullptr;
		int result = ALooper_pollOnce(timeout, nullptr, nullptr, reinterpret_cast<void**>(&source));
		while(result >= 0 || result == ALOOPER_POLL_CALLBACK) {
			if(source)
				source->process(app, source);
			if(app->destroyRequested)
				break;
			source = nullptr;
			result = ALooper_pollOnce(0, nullptr, nullptr, reinterpret_cast<void**>(&source));
		}
		if(app->destroyRequested)
			break;

		android_launcher_update(game_state);

		if(can_render(win)) {
			update_touch(game_state);
			clamp_windows_into_screen(game_state);
			render_frame(game_state);
			update_perf_counter(game_state);
		}
	}

	terminate_egl(win);
	ALICE_LOGI("loop principal encerrado");
}

void change_cursor(sys::state& state, cursor_type type) {
	// Nao ha cursor em touchscreen. Mas o jogo marca com o cursor "ocupado" os
	// trabalhos demorados que travam a thread (criar a interface ao entrar na
	// partida, carregar save...): no Android isso vira a janela de carregamento.
	static std::atomic<bool> busy{ false };
	bool const now_busy = type == cursor_type::busy;
	if(busy.exchange(now_busy) != now_busy)
		android_set_loading(now_busy ? "Carregando..." : "");
}

void emit_error_message(std::string const& content, bool fatal) {
	__android_log_print(fatal ? ANDROID_LOG_FATAL : ANDROID_LOG_ERROR, "Alice", "%s", content.c_str());
	if(fatal) {
		std::abort(); // std::exit não é seguro de dentro do processo do app Android
	}
}

// text services (IME) -- mesmos no-ops usados em window_nix.cpp; entrada de texto
// no Android sera tratada via android_app / soft keyboard, nao por essa interface COM do Windows.
win32_text_services::win32_text_services() { }
win32_text_services::~win32_text_services() { }
void win32_text_services::start_text_services() { }
void win32_text_services::end_text_services() { }
void win32_text_services::on_text_change(text_services_object* ts, uint32_t old_start, uint32_t old_end, uint32_t new_end) { }
void win32_text_services::on_selection_change(text_services_object* ts) { }
bool win32_text_services::send_mouse_event_to_tso(text_services_object* ts, int32_t x, int32_t y, uint32_t buttons) {
	return false;
}
void win32_text_services::set_focus(sys::state& win, text_services_object* o) { }
void win32_text_services::suspend_keystroke_handling() { }
void win32_text_services::resume_keystroke_handling() { }
text_services_object* win32_text_services::create_text_service_object(sys::state& win, ui::element_base& ei) {
	return nullptr;
}
void release_text_services_object(text_services_object* ptr) { }

} // namespace window
