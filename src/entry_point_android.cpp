// entry_point_android.cpp
//
// Equivalente Android do entry_point_nix.cpp. No lugar de main(), a
// AliceActivity (NativeActivity, via android_native_app_glue) chama
// android_main numa thread propria; o ciclo de vida (janela, pausa, foco) e o
// loop de render ficam em window::run_android_main_loop (window_android.cpp),
// que chama window::android_launcher_update a cada volta.
//
// Ate o jogo comecar, o launcher (abaixo) faz numa thread separada -- para o
// Android nao achar que o app travou -- o que o entry_point_nix.cpp faz antes de
// abrir a janela:
//   1. extrai os arquivos do Alice (pasta assets/ do repositorio, dentro do APK)
//      para o armazenamento interno, se ainda nao extraiu esta versao;
//   2. acha a pasta do Victoria 2 (salva de uma execucao anterior, ou pede ao
//      usuario pelo seletor de pastas da AliceActivity);
//   3. acha um cenario ja montado para essa pasta ou monta um novo (minutos);
//   4. carrega o cenario e as configuracoes.
// Depois, na thread principal e com uma superficie EGL, inicia as threads do
// jogo e window::android_start_game (OpenGL, som, on_create).
//
// Raizes do sistema de arquivos do jogo: [pasta do Victoria 2, pasta interna
// com assets/]. No desktop a pasta assets/ fica dentro da pasta do jogo; aqui
// ela fica no armazenamento do app, sem precisar escrever na pasta do usuario.
//
// Ainda nao ha selecao de mods (so o jogo base).

#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <jni.h>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "serialization.hpp"
#include "system_state.hpp"
#include "game_scene.hpp"
#include "parsers_declarations.hpp"

static sys::state game_state;

namespace alice_android {

#define LAUNCHER_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Alice", __VA_ARGS__)
#define LAUNCHER_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Alice", __VA_ARGS__)

android_app* g_app = nullptr;
std::string alice_data_root; // contem assets/ (extraido do APK)

enum class load_status : uint8_t { idle, working, need_folder, ready, failed };
std::atomic<load_status> status{ load_status::idle };
std::thread loader_thread;
bool folder_dialog_open = false;
bool game_threads_started = false;

std::mutex shared_mutex; // protege os dois campos abaixo
std::optional<std::string> picked_folder; // vindo do seletor (thread da interface Java)
std::string need_folder_reason;           // por que a pasta atual nao serve

// --- Java (AliceActivity) ------------------------------------------------------

// chama um metodo void(String) da AliceActivity, de qualquer thread nativa
void call_activity(char const* method, std::string const& argument) {
	if(!g_app)
		return;
	JavaVM* vm = g_app->activity->vm;
	JNIEnv* env = nullptr;
	bool attached = false;
	if(vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
		if(vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
			return;
		attached = true;
	}
	jobject activity = g_app->activity->clazz;
	jclass cls = env->GetObjectClass(activity);
	jmethodID id = env->GetMethodID(cls, method, "(Ljava/lang/String;)V");
	if(id) {
		jstring jarg = env->NewStringUTF(argument.c_str());
		env->CallVoidMethod(activity, id, jarg);
		env->DeleteLocalRef(jarg);
	}
	if(env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
	}
	env->DeleteLocalRef(cls);
	if(attached)
		vm->DetachCurrentThread();
}

// Etapa da abertura na janela de carregamento, que fica aberta ate o jogo
// aparecer (um aviso comum some em segundos e a montagem do cenario leva
// minutos). Vazio fecha.
void show_progress(std::string const& message) {
	if(!message.empty())
		LAUNCHER_LOGI("%s", message.c_str());
	call_activity("setLoading", message);
}

void wake_main_loop() {
	if(g_app)
		ALooper_wake(g_app->looper);
}

// --- arquivos -----------------------------------------------------------------

bool make_directories(std::string const& path) {
	for(size_t pos = 1; pos <= path.size(); ++pos) {
		if(pos == path.size() || path[pos] == '/') {
			auto const partial = path.substr(0, pos);
			if(mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST)
				return false;
		}
	}
	return true;
}

std::optional<std::string> read_text_file(std::string const& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if(!f)
		return std::nullopt;
	std::string content;
	char buffer[4096];
	size_t n = 0;
	while((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
		content.append(buffer, n);
	std::fclose(f);
	return content;
}

bool write_text_file(std::string const& path, std::string const& content) {
	FILE* f = std::fopen(path.c_str(), "wb");
	if(!f)
		return false;
	bool const ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
	return std::fclose(f) == 0 && ok;
}

std::optional<std::string> read_apk_asset(char const* name) {
	AAsset* asset = AAssetManager_open(g_app->activity->assetManager, name, AASSET_MODE_BUFFER);
	if(!asset)
		return std::nullopt;
	std::string content(static_cast<char const*>(AAsset_getBuffer(asset)), size_t(AAsset_getLength(asset)));
	AAsset_close(asset);
	return content;
}

// Extrai os arquivos listados em alice_assets_manifest.txt (gerado pelo Gradle,
// ver android/app/build.gradle.kts). Repete so quando a lista muda.
bool extract_alice_assets() {
	auto manifest = read_apk_asset("alice_assets_manifest.txt");
	if(!manifest) {
		LAUNCHER_LOGE("alice_assets_manifest.txt nao esta no APK");
		return false;
	}
	std::string const extracted_manifest_path = alice_data_root + "/alice_assets_manifest.txt";
	if(auto existing = read_text_file(extracted_manifest_path); existing && *existing == *manifest) {
		LAUNCHER_LOGI("arquivos do Alice ja extraidos");
		return true;
	}

	show_progress("Preparando os arquivos do Project Alice...");
	// a lista antiga sai primeiro: se a extracao for interrompida, recomeca na proxima vez
	unlink(extracted_manifest_path.c_str());

	size_t count = 0;
	size_t line_start = 0;
	std::vector<char> buffer(1 << 16);
	while(line_start < manifest->size()) {
		size_t line_end = manifest->find('\n', line_start);
		if(line_end == std::string::npos)
			line_end = manifest->size();
		auto const line = manifest->substr(line_start, line_end - line_start);
		line_start = line_end + 1;
		auto const relative = line.substr(0, line.find('\t'));
		if(relative.empty())
			continue;

		auto const destination = alice_data_root + "/" + relative;
		make_directories(destination.substr(0, destination.rfind('/')));
		AAsset* asset = AAssetManager_open(g_app->activity->assetManager, relative.c_str(), AASSET_MODE_STREAMING);
		if(!asset) {
			LAUNCHER_LOGE("arquivo faltando no APK: %s", relative.c_str());
			return false;
		}
		FILE* out = std::fopen(destination.c_str(), "wb");
		if(!out) {
			AAsset_close(asset);
			LAUNCHER_LOGE("nao foi possivel criar %s", destination.c_str());
			return false;
		}
		int read = 0;
		bool ok = true;
		while((read = AAsset_read(asset, buffer.data(), buffer.size())) > 0) {
			if(std::fwrite(buffer.data(), 1, size_t(read), out) != size_t(read)) {
				ok = false;
				break;
			}
		}
		ok = (std::fclose(out) == 0) && ok && read >= 0;
		AAsset_close(asset);
		if(!ok) {
			LAUNCHER_LOGE("erro ao gravar %s (sem espaco?)", destination.c_str());
			return false;
		}
		++count;
	}
	LAUNCHER_LOGI("%zu arquivos do Alice extraidos para %s", count, alice_data_root.c_str());
	return write_text_file(extracted_manifest_path, *manifest);
}

std::string game_folder_config_path() {
	return std::string(getenv("HOME")) + "/game_folder.txt";
}

bool has_subdirectory_caseless(std::string const& base, char const* name) {
	DIR* d = opendir(base.c_str());
	if(!d)
		return false;
	bool found = false;
	while(auto* entry = readdir(d)) {
		if(strcasecmp(entry->d_name, name) != 0)
			continue;
		struct stat st;
		if(stat((base + "/" + entry->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

// "" se a pasta serve; senao, o motivo (mostrado ao usuario)
std::string check_game_folder(std::string const& path) {
	DIR* d = opendir(path.c_str());
	if(!d)
		return "Nao foi possivel abrir a pasta " + path + " (falta a permissao de acesso a arquivos?).";
	closedir(d);
	if(!has_subdirectory_caseless(path, "common") || !has_subdirectory_caseless(path, "map") || !has_subdirectory_caseless(path, "gfx"))
		return "A pasta " + path + " nao parece ser a do Victoria 2 (faltam as pastas common, map ou gfx).";
	return "";
}

// --- cenario (mesma logica do entry_point_nix.cpp, so que sem mods) -----------

struct scenario_file {
	native_string file_name;
	sys::mod_identifier ident;
};
std::vector<scenario_file> scenario_files;
uint32_t max_scenario_count = 0;
native_string selected_scenario_file;
std::string game_folder;

void add_roots(simple_fs::file_system& fs) {
	simple_fs::add_root(fs, game_folder);
	simple_fs::add_root(fs, alice_data_root);
}

native_string produce_mod_path() {
	simple_fs::file_system dummy;
	add_roots(dummy);
	return simple_fs::extract_state(dummy);
}

void check_scenario_folder() {
	scenario_files.clear();
	max_scenario_count = 0;
	auto sdir = simple_fs::get_or_create_scenario_directory();
	for(auto& f : simple_fs::list_files(sdir, NATIVE(".bin"))) {
		if(auto of = simple_fs::open_file(f); of) {
			auto content = view_contents(*of);
			auto desc = sys::extract_mod_information(reinterpret_cast<uint8_t const*>(content.data), content.file_size);
			if(desc.count != 0) {
				max_scenario_count = std::max(desc.count, max_scenario_count);
				scenario_files.push_back(scenario_file{ simple_fs::get_file_name(f), desc });
			}
		}
	}
	std::sort(scenario_files.begin(), scenario_files.end(), [](scenario_file const& a, scenario_file const& b) {
		return a.ident.count > b.ident.count;
	});
}

void find_scenario_file() {
	selected_scenario_file = NATIVE("");
	auto const mod_path = produce_mod_path();
	for(auto& f : scenario_files) {
		if(f.ident.mod_path == mod_path) {
			selected_scenario_file = f.file_name;
			break;
		}
	}
}

native_string to_hex(uint64_t v) {
	native_string ret;
	constexpr native_char digits[] = NATIVE("0123456789ABCDEF");
	do {
		ret += digits[v & 0x0F];
		v = v >> 4;
	} while(v != 0);
	return ret;
}

// igual ao build_scenario_file do entry_point_nix.cpp
bool build_scenario_file() {
	auto const path = produce_mod_path();
	simple_fs::file_system fs_root;
	simple_fs::restore_state(fs_root, path);
	parsers::error_handler err("");
	auto root = get_root(fs_root);
	auto common = open_directory(root, NATIVE("common"));
	parsers::bookmark_context bookmark_context;
	if(auto f = open_file(common, NATIVE("bookmarks.txt")); f) {
		auto bookmark_content = simple_fs::view_contents(*f);
		err.file_name = "bookmarks.txt";
		parsers::token_generator gen(bookmark_content.data, bookmark_content.data + bookmark_content.file_size);
		parsers::parse_bookmark_file(gen, err, bookmark_context);
	} else {
		err.accumulated_errors += "File common/bookmarks.txt could not be opened\n";
	}
	if(bookmark_context.bookmark_dates.empty()) {
		LAUNCHER_LOGE("common/bookmarks.txt sem datas: %s", err.accumulated_errors.c_str());
		return false;
	}

	sys::checksum_key scenario_key;
	for(uint32_t date_index = 0; date_index < uint32_t(bookmark_context.bookmark_dates.size()); date_index++) {
		err.accumulated_errors.clear();
		err.accumulated_warnings.clear();
		auto scenario_state = std::make_unique<sys::state>();
		simple_fs::restore_state(scenario_state->common_fs, path);
		scenario_state->load_scenario_data(err, bookmark_context.bookmark_dates[date_index].date_);
		if(err.fatal)
			break;
		if(date_index == 0) {
			auto sdir = simple_fs::get_or_create_scenario_directory();
			int32_t append = 0;
			auto const base_name = to_hex(uint64_t(std::time(0)));
			while(simple_fs::peek_file(sdir, base_name + NATIVE("-") + std::to_string(append) + NATIVE(".bin"))) {
				++append;
			}
			++max_scenario_count;
			selected_scenario_file = base_name + NATIVE("-") + std::to_string(append) + NATIVE(".bin");
			sys::write_scenario_file(*scenario_state, selected_scenario_file, max_scenario_count);
			scenario_key = scenario_state->scenario_checksum;
		} else {
			scenario_state->scenario_checksum = scenario_key;
			sys::write_save_file(*scenario_state, sys::save_type::bookmark, bookmark_context.bookmark_dates[date_index].name_);
		}
	}

	if(!err.accumulated_errors.empty() || !err.accumulated_warnings.empty()) {
		auto const assembled_msg = std::string("The following problems were encountered while creating the scenario:\r\n\r\nErrors:\r\n") + err.accumulated_errors + "\r\n\r\nWarnings:\r\n" + err.accumulated_warnings;
		auto pdir = simple_fs::get_or_create_scenario_directory();
		simple_fs::write_file(pdir, NATIVE("scenario_errors.log"), assembled_msg.data(), uint32_t(assembled_msg.length()));
		LAUNCHER_LOGE("problemas ao montar o cenario; ver %s/scenario_errors.log", simple_fs::get_full_name(pdir).c_str());
	}
	return !err.fatal && !selected_scenario_file.empty();
}

// --- thread de carregamento -------------------------------------------------------

void fail(std::string const& message) {
	LAUNCHER_LOGE("%s", message.c_str());
	show_progress("");
	call_activity("showMessage", message);
	status.store(load_status::failed);
	wake_main_loop();
}

void need_folder(std::string const& reason) {
	show_progress(""); // o seletor de pasta precisa dos toques
	{
		std::lock_guard lock(shared_mutex);
		need_folder_reason = reason;
	}
	status.store(load_status::need_folder);
	wake_main_loop();
}

void run_loader() {
	show_progress("Abrindo o Project Alice...");
	if(!extract_alice_assets()) {
		fail("Nao foi possivel preparar os arquivos do Project Alice (sem espaco livre?).");
		return;
	}

	// pasta do jogo: a recem-escolhida ou a salva de uma execucao anterior
	std::string folder;
	bool newly_picked = false;
	{
		std::lock_guard lock(shared_mutex);
		if(picked_folder) {
			folder = *picked_folder;
			picked_folder.reset();
			newly_picked = true;
		}
	}
	if(folder.empty()) {
		if(auto saved = read_text_file(game_folder_config_path()); saved)
			folder = *saved;
		while(!folder.empty() && (folder.back() == '\n' || folder.back() == '\r'))
			folder.pop_back();
	}
	if(folder.empty()) {
		need_folder("");
		return;
	}
	if(auto problem = check_game_folder(folder); !problem.empty()) {
		need_folder(problem);
		return;
	}
	if(newly_picked)
		write_text_file(game_folder_config_path(), folder);
	game_folder = folder;
	LAUNCHER_LOGI("pasta do Victoria 2: %s", game_folder.c_str());

	add_roots(game_state.common_fs);
	check_scenario_folder();
	find_scenario_file();
	if(selected_scenario_file.empty()) {
		show_progress("Montando o cenário do Victoria 2.\nIsso só acontece na primeira vez e pode levar vários minutos.");
		if(!build_scenario_file()) {
			fail("Nao foi possivel montar o cenario. Veja scenario_errors.log na pasta do app.");
			return;
		}
	}

	show_progress("Carregando o cenário...");
	if(!sys::try_read_scenario_and_save_file(game_state, selected_scenario_file)) {
		fail("O arquivo de cenario nao pode ser lido.");
		return;
	}
	LAUNCHER_LOGI("cenario carregado: %s", selected_scenario_file.c_str());
	game_state.loaded_scenario_file = selected_scenario_file;
	game_state.fill_unsaved_data();

	network::init(game_state);
	// sem configuracoes salvas (primeira execucao) a escala da interface e o
	// antialiasing sao escolhidos para o aparelho em window::android_start_game
	bool const has_saved_settings = bool(simple_fs::peek_file(simple_fs::get_or_create_settings_directory(), NATIVE("user_settings.dat")));
	game_state.load_user_settings();
	game_state.win_ptr->first_run_defaults = !has_saved_settings;
	ui::populate_definitions_map(game_state);

	show_progress("Iniciando o jogo...");
	status.store(load_status::ready);
	wake_main_loop();
}

void start_loader() {
	if(loader_thread.joinable())
		loader_thread.join();
	status.store(load_status::working);
	loader_thread = std::thread(run_loader);
}

// as mesmas threads que o entry_point_nix.cpp inicia antes de abrir a janela
void start_game_threads(sys::state& state) {
	std::thread([&state]() { state.game_loop(); }).detach();
	std::thread([&state]() { state.ui_cached_data.process_update(state); }).detach();
	std::thread([&state]() { state.map_state.update_cache(state); }).detach();
	std::thread([&state]() { state.map_state.update_map_labels(state); }).detach();
	game_threads_started = true;
}

} // namespace alice_android

namespace window {

void android_set_loading(char const* message) {
	alice_android::call_activity("setLoading", message ? message : "");
}

void android_set_stats(char const* text) {
	alice_android::call_activity("setStats", text ? text : "");
}

void android_launcher_update(sys::state& state) {
	using namespace alice_android;
	auto& win = *state.win_ptr;

	switch(status.load()) {
	case load_status::idle:
		win.loading = true;
		start_loader();
		break;
	case load_status::need_folder: {
		win.loading = false;
		bool picked = false;
		std::string reason;
		{
			std::lock_guard lock(shared_mutex);
			picked = picked_folder.has_value();
			reason = need_folder_reason;
		}
		if(picked) {
			folder_dialog_open = false;
			win.loading = true;
			start_loader();
		} else if(!folder_dialog_open) {
			folder_dialog_open = true;
			call_activity("requestGameFolder", reason);
		}
		break;
	}
	case load_status::ready:
		// o jogo precisa do contexto EGL com uma superficie; sem ela espera
		if(!win.game_started && win.egl_surface != EGL_NO_SURFACE) {
			if(loader_thread.joinable())
				loader_thread.join();
			if(!game_threads_started)
				start_game_threads(state);
			android_start_game(state);
			show_progress(""); // a janela de carregamento da abertura fecha com o jogo na tela
		}
		break;
	case load_status::working:
	case load_status::failed:
		win.loading = status.load() == load_status::working;
		break;
	}
}

} // namespace window

// chamado pela AliceActivity (thread da interface) com a pasta escolhida
extern "C" JNIEXPORT void JNICALL Java_org_projectalice_mobile_AliceActivity_nativeOnGameFolderPicked(JNIEnv* env, jclass, jstring path) {
	char const* chars = env->GetStringUTFChars(path, nullptr);
	{
		std::lock_guard lock(alice_android::shared_mutex);
		alice_android::picked_folder = std::string(chars);
	}
	env->ReleaseStringUTFChars(path, chars);
	alice_android::wake_main_loop();
}

void android_main(struct android_app* app) {
	__android_log_print(ANDROID_LOG_INFO, "Alice", "android_main iniciado");
	alice_android::g_app = app;

	// simple_fs_nix.cpp guarda configuracoes, cenarios e saves em
	// $HOME/.local/share/Alice; no Android $HOME nao existe. A pasta externa
	// do app (Android/data/org.projectalice.mobile/files) da para acessar pelo
	// cabo USB, para copiar saves.
	char const* home = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
	alice_android::make_directories(home);
	setenv("HOME", home, 1);
	alice_android::alice_data_root = std::string(app->activity->internalDataPath) + "/alice";
	alice_android::make_directories(alice_android::alice_data_root);
	__android_log_print(ANDROID_LOG_INFO, "Alice", "HOME=%s, arquivos do Alice em %s", home, alice_android::alice_data_root.c_str());

	window::run_android_main_loop(game_state, app);

	// O app foi fechado de verdade (configChanges evita recriar a Activity em
	// rotacao etc.). As threads do jogo nao tem como ser encerradas com seguranca
	// a partir daqui e o estado estatico nao pode ser reaproveitado por uma nova
	// Activity no mesmo processo, entao o processo termina.
	game_state.quit_signaled.store(true, std::memory_order_release);
	__android_log_print(ANDROID_LOG_INFO, "Alice", "encerrando o processo");
	_exit(0);
}
