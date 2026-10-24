message(STATUS "Fetching SIMDe...")

# SIMD Everywhere: implementa as intrinsecas x86 (SSE/AVX) usadas pelo
# DataContainer (ve_*.hpp) e pelo jogo em cima do NEON do ARM. So header.
FetchContent_Declare(
	simde
	GIT_REPOSITORY https://github.com/simd-everywhere/simde
	GIT_TAG v0.8.2
	GIT_SHALLOW 1
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(simde)

add_library(dependency_simde INTERFACE)
# a pasta x86_compat tem os headers <immintrin.h>, <x86intrin.h> etc. que
# redirecionam pro SIMDe; ela precisa vir ANTES dos headers do compilador
target_include_directories(dependency_simde INTERFACE
	"${PROJECT_SOURCE_DIR}/src/x86_compat"
	"${simde_SOURCE_DIR}")
target_compile_definitions(dependency_simde INTERFACE SIMDE_ENABLE_NATIVE_ALIASES)
