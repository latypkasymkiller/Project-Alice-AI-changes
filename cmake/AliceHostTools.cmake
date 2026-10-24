# cmake/AliceHostTools.cmake
#
# ParserGenerator, DCONGENERATOR, DCONINTERFACEGEN, DataContainer-Lua e
# DataContainer_OOS_Reporter sao executados DURANTE o build (via
# add_custom_command) pra gerar .hpp que o resto do codigo depende. Isso
# significa que eles precisam ser binarios de HOST -- se o CMake os compilar
# com o toolchain do NDK (cross-compiling), o binario resultante e ARM e nao
# roda no PC/host pra gerar o arquivo, travando o build inteiro.
#
# Fluxo pra Android (ou qualquer cross-compile):
#   1) Build nativo normal primeiro, so pra essas 5 ferramentas:
#        cmake --preset x64-debug-linux-clang
#        cmake --build --preset x64-debug-linux-clang --target \
#            ParserGenerator DCONGENERATOR DCONINTERFACEGEN \
#            DataContainer-Lua DataContainer_OOS_Reporter
#   2) Copiar os 5 binarios gerados pra uma pasta separada e "achatada", ex:
#        mkdir -p native-tools
#        find out/build/x64-debug-linux-clang -type f \
#            \( -name ParserGenerator -o -name DCONGENERATOR -o -name DCONINTERFACEGEN \
#               -o -name DataContainer-Lua -o -name DataContainer_OOS_Reporter \) \
#            -exec cp {} native-tools/ \;
#   3) Configurar o preset Android apontando pra essa pasta:
#        cmake --preset android-arm64 -DALICE_NATIVE_TOOLS_DIR=$(pwd)/native-tools
#
# alice_import_host_tool(<nome-do-alvo>) cria um alvo IMPORTED com esse nome
# apontando pro binario ja compilado -- os add_custom_command/add_dependencies
# existentes que referenciam esse nome continuam funcionando sem mudar nada,
# porque o CMake resolve pelo nome do alvo, nao importa se ele e IMPORTED ou nao.

set(ALICE_NATIVE_TOOLS_DIR "" CACHE PATH
	"Pasta com os binarios de host pre-compilados de ParserGenerator/DCONGENERATOR/DCONINTERFACEGEN/DataContainer-Lua/DataContainer_OOS_Reporter (necessario ao cross-compilar, ex: Android)")

function(alice_import_host_tool TOOL_NAME)
	if(TARGET ${TOOL_NAME})
		return() # ja existe (build nativo normal cuidou disso) -- nada a fazer
	endif()

	if(NOT ALICE_NATIVE_TOOLS_DIR)
		message(FATAL_ERROR
			"Cross-compilando (CMAKE_CROSSCOMPILING=ON) mas ALICE_NATIVE_TOOLS_DIR nao foi definido.\n"
			"'${TOOL_NAME}' roda DURANTE o build pra gerar codigo e precisa ser um binario de "
			"HOST, nao ${CMAKE_SYSTEM_NAME}${CMAKE_ANDROID_ARCH_ABI}.\n"
			"Builde-o nativamente primeiro (preset desktop) e aponte -DALICE_NATIVE_TOOLS_DIR=<pasta>. "
			"Ver comentario no topo de cmake/AliceHostTools.cmake para o passo a passo.")
	endif()

	find_program(ALICE_${TOOL_NAME}_PATH
		NAMES ${TOOL_NAME}
		PATHS "${ALICE_NATIVE_TOOLS_DIR}"
		NO_DEFAULT_PATH)

	if(NOT ALICE_${TOOL_NAME}_PATH)
		message(FATAL_ERROR
			"Nao encontrei um binario de host chamado '${TOOL_NAME}' dentro de "
			"ALICE_NATIVE_TOOLS_DIR='${ALICE_NATIVE_TOOLS_DIR}'. Builde-o nativamente primeiro "
			"(ver comentario no topo de cmake/AliceHostTools.cmake).")
	endif()

	add_executable(${TOOL_NAME} IMPORTED GLOBAL)
	set_target_properties(${TOOL_NAME} PROPERTIES IMPORTED_LOCATION "${ALICE_${TOOL_NAME}_PATH}")
	message(STATUS "${TOOL_NAME}: usando binario de host pre-compilado em ${ALICE_${TOOL_NAME}_PATH}")
endfunction()
