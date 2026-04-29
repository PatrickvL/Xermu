# gen_shader_inc.cmake — wrap a GLSL file as a C++ raw string literal.
# Usage: cmake -DGLSL_FILE=path/to/shader.glsl -P gen_shader_inc.cmake
file(READ "${GLSL_FILE}" CONTENT)
file(WRITE "${GLSL_FILE}.inc" "R\"GLSL(\n${CONTENT})GLSL\"\n")
