if(ANDROID)
	# Nada e linkado: o app roda a partir do Android 11 (API 30) e o NDK so
	# publica a libicu.so para apps a partir do Android 12 (API 31). Os headers
	# <unicode/*.h> vem do sysroot do NDK; src/text/icu_android_compat.hpp
	# carrega a libicu.so com dlopen quando existe e, no Android 11, usa uma
	# implementacao propria da quebra de texto (a unica parte do ICU usada aqui).
	add_library(dependency_icu INTERFACE)
	target_link_libraries(dependency_icu INTERFACE dl)
else()
	message(STATUS "Fetching icu...")

	find_package(ICU 70.1 COMPONENTS uc i18n REQUIRED)

	add_library(dependency_icu INTERFACE)
	target_link_libraries(dependency_icu INTERFACE ICU::uc ICU::i18n)
endif()
