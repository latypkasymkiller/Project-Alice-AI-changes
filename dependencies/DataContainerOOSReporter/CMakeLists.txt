if(CMAKE_CROSSCOMPILING)
	# Mesmo caso do DataContainer-Lua: DataContainer_OOS_Reporter roda durante
	# o build e o executavel e definido no CMakeLists.txt de terceiros -- ver
	# cmake/AliceHostTools.cmake.
	alice_import_host_tool(DataContainer_OOS_Reporter)
else()
	message(STATUS "Fetching DataContainerOOSReporter...")

	FetchContent_Declare(
		DataContainerOOSReporter
		GIT_REPOSITORY https://github.com/ZombieFreak115/DataContainer_OOS_Reporter
		GIT_TAG master
		GIT_SHALLOW 1
		GIT_PROGRESS TRUE
	)

	FetchContent_MakeAvailable(DataContainerOOSReporter)
endif()

add_library(dependency_DataContainerOOSReporter INTERFACE)
