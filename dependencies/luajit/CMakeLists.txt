cmake_minimum_required(VERSION 3.15)

if(WIN32)
	message(STATUS "A certain luajit version is distributed with Alice")
	add_library(lib_luajit INTERFACE)
	target_link_libraries(lib_luajit INTERFACE ${PROJECT_SOURCE_DIR}/libs/luajit.lib)
else()
	message(STATUS "Fetching luajit...")
	include(ExternalProject)

	# Ativa isso (ou passe -DALICE_ANDROID_LUAJIT_DISABLE_JIT=ON) se o JIT travar
	# em runtime em algum aparelho por causa de W^X/SELinux mais restritivo --
	# aparelhos "de fabrica" costumam aceitar numa boa, porque o LuaJIT nunca
	# pede memoria RWX de uma vez so (sempre alterna write/exec via mprotect
	# sequencial), mas alguns fabricantes endurecem isso alem do AOSP padrao.
	option(ALICE_ANDROID_LUAJIT_DISABLE_JIT "Compilar o LuaJIT em modo interpretado (sem JIT) no Android" OFF)

	if(ANDROID)
		# LuaJIT cross-compila via Makefile proprio, nao entende o toolchain file
		# do CMake sozinho -- precisa apontar os binarios do NDK na mao.
		# Ao contrario do alvo armeabi-v7a (32-bit), arm64-v8a nao precisa de
		# HOST_CC especial: host (x86_64) e alvo (aarch64) tem o mesmo tamanho de
		# ponteiro (64 bits), que e a unica exigencia do Makefile do LuaJIT pra
		# rodar minilua/buildvm com o compilador padrao do host.
		# Ver https://luajit.org/install.html (secao Android/ARM64).
		if(NOT DEFINED ENV{ANDROID_NDK})
			message(FATAL_ERROR "ANDROID_NDK nao esta definida no ambiente -- necessaria pra cross-compilar o LuaJIT (ver toolchainFile em CMakePresets.json).")
		endif()

		if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
			set(ALICE_NDK_HOST_TAG "linux-x86_64")
		elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
			set(ALICE_NDK_HOST_TAG "darwin-x86_64")
		else()
			set(ALICE_NDK_HOST_TAG "windows-x86_64")
		endif()

		string(REPLACE "android-" "" ALICE_ANDROID_API "${ANDROID_PLATFORM}")
		set(ALICE_NDK_BIN "$ENV{ANDROID_NDK}/toolchains/llvm/prebuilt/${ALICE_NDK_HOST_TAG}/bin")
		set(ALICE_NDK_CLANG "${ALICE_NDK_BIN}/aarch64-linux-android${ALICE_ANDROID_API}-clang")

		set(ALICE_LUAJIT_BUILD_COMMAND make
			"CROSS=${ALICE_NDK_BIN}/aarch64-linux-android-"
			"STATIC_CC=${ALICE_NDK_CLANG} -fPIC" # a libluajit.a e linkada dentro da libAlice.so
			"DYNAMIC_CC=${ALICE_NDK_CLANG} -fPIC"
			"TARGET_LD=${ALICE_NDK_CLANG}"
			"TARGET_AR=${ALICE_NDK_BIN}/llvm-ar rcus"
			"TARGET_STRIP=${ALICE_NDK_BIN}/llvm-strip"
			"TARGET_SYS=Linux")
	else()
		set(ALICE_LUAJIT_BUILD_COMMAND make)
	endif()

	if(ALICE_ANDROID_LUAJIT_DISABLE_JIT)
		list(APPEND ALICE_LUAJIT_BUILD_COMMAND "XCFLAGS=-DLUAJIT_DISABLE_JIT")
	endif()

	ExternalProject_Add(
		luajit

		SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/luajit/source

		GIT_REPOSITORY https://luajit.org/git/luajit.git
		GIT_TAG origin/v2.1
		GIT_SHALLOW FALSE
		GIT_PROGRESS TRUE

		CONFIGURE_COMMAND ""
		BUILD_COMMAND ${ALICE_LUAJIT_BUILD_COMMAND}

		BUILD_IN_SOURCE TRUE
		BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/luajit/source/luajit.h
		BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/luajit/source/src/libluajit.so
		BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/luajit/source/src/libluajit.a
		INSTALL_COMMAND make install DESTDIR=${DESTDIR} PREFIX=<INSTALL_DIR>
	)
	add_library(lib_luajit INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/luajit/source/luajit.h)
	add_dependencies(lib_luajit luajit)
	if(ANDROID)
		# estatico no Android: a .so do LuaJIT tem soname versionado
		# (libluajit-5.1.so.2), que o APK nao consegue empacotar/carregar
		target_link_libraries(lib_luajit INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/luajit/source/src/libluajit.a)
	else()
		target_link_libraries(lib_luajit INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/luajit/source/src/libluajit.so)
	endif()
endif()

