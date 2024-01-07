# Function for building single example
function(buildExample EXAMPLE_NAME)
	SET(EXAMPLE_FOLDER ${CMAKE_CURRENT_SOURCE_DIR}/${EXAMPLE_NAME})
	message(STATUS "Generating project file for example in ${EXAMPLE_FOLDER}")
	# Main
	file(GLOB SOURCE *.cpp ${BASE_HEADERS} ${EXAMPLE_FOLDER}/*.cpp)
	SET(MAIN_CPP ${EXAMPLE_FOLDER}/${EXAMPLE_NAME}.cpp)
	if(EXISTS ${EXAMPLE_FOLDER}/main.cpp)
		SET(MAIN_CPP ${EXAMPLE_FOLDER}/main.cpp)
	ENDIF()
	if(EXISTS ${EXAMPLE_FOLDER}/${EXAMPLE_NAME}.h)
		SET(MAIN_HEADER ${EXAMPLE_FOLDER}/${EXAMPLE_NAME}.h)
	ENDIF()
	# imgui example requires additional source files
	IF(${EXAMPLE_NAME} STREQUAL "imgui")
		file(GLOB ADD_SOURCE "../external/imgui/*.cpp")
		SET(SOURCE ${SOURCE} ${ADD_SOURCE})
	ENDIF()
#	execute_process(
#		COMMAND "python" "-c" "../shaders/glsl/compileshaders.py"
#		RESULT_VARIABLE _status 
#		OUTPUT_VARIABLE _output
#	)
#	message(STATUS "Compiling shaders")
#	message(STATUS ${_output})
	# Add shaders
	set(SHADER_DIR_GLSL "../shaders/glsl/${EXAMPLE_NAME}")
	file(GLOB SHADERS_GLSL "${SHADER_DIR_GLSL}/*.vert" "${SHADER_DIR_GLSL}/*.frag" "${SHADER_DIR_GLSL}/*.comp" "${SHADER_DIR_GLSL}/*.geom" "${SHADER_DIR_GLSL}/*.tesc" "${SHADER_DIR_GLSL}/*.tese" "${SHADER_DIR_GLSL}/*.mesh" "${SHADER_DIR_GLSL}/*.task" "${SHADER_DIR_GLSL}/*.rgen" "${SHADER_DIR_GLSL}/*.rchit" "${SHADER_DIR_GLSL}/*.rmiss" "${SHADER_DIR_GLSL}/*.rcall" "${SHADER_DIR_GLSL}/*.rahit" "${SHADER_DIR_GLSL}/*.rint" "${SHADER_DIR_GLSL}/*.glsl")
	source_group("Shaders\\GLSL" FILES ${SHADERS_GLSL})
	# Add optional readme / tutorial
	file(GLOB README_FILES "${EXAMPLE_FOLDER}/*.md")
	if(WIN32)
		add_executable(${EXAMPLE_NAME} WIN32 ${MAIN_CPP} ${SOURCE} ${MAIN_HEADER} ${SHADERS_GLSL} ${SHADERS_HLSL} ${README_FILES})
		target_link_libraries(${EXAMPLE_NAME} base ${Vulkan_LIBRARY} ${WINLIBS})
	else(WIN32)
		add_executable(${EXAMPLE_NAME} ${MAIN_CPP} ${SOURCE} ${MAIN_HEADER} ${SHADERS_GLSL} ${SHADERS_HLSL} ${README_FILES})
		target_link_libraries(${EXAMPLE_NAME} base )
	endif(WIN32)

	file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
	set_target_properties(${EXAMPLE_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

	if(RESOURCE_INSTALL_DIR)
		install(TARGETS ${EXAMPLE_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
	endif()
endfunction(buildExample)

# Build all examples
function(buildExamples)
	foreach(EXAMPLE ${EXAMPLES})
		buildExample(${EXAMPLE})
	endforeach(EXAMPLE)
endfunction(buildExamples)

set(EXAMPLES
	pathtracing
	ssprobe
)

buildExamples()
