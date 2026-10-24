if(CMAKE_CROSSCOMPILING)
	# DataContainer-Lua roda durante o build (gera codigo) -- o proprio
	# CMakeLists.txt desse repositorio de terceiros define o add_executable,
	# entao nem chegamos a fazer o FetchContent aqui ao cross-compilar (senao
	# o CMake tentaria compila-lo com o toolchain do NDK). Ver cmake/AliceHostTools.cmake.
	alice_import_host_tool(DataContainer-Lua)
else()
	message(STATUS "Fetching DataContainerLua...")

	FetchContent_Declare(
		DataContainerLua
		GIT_REPOSITORY https://github.com/ineveraskedforthis/DataContainer-Lua
		GIT_TAG allow-creation
		GIT_SHALLOW 1
		GIT_PROGRESS TRUE
	)

	FetchContent_MakeAvailable(DataContainerLua)
endif()

add_library(dependency_DataContainerLua INTERFACE)
