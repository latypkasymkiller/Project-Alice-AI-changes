// glew.h (Android)
//
// Substituto do GLEW para o build Android. O codigo do jogo inclui "glew.h" em
// varios lugares; no desktop isso vem do GLEW (carregador de extensoes do
// OpenGL). No Android as funcoes GLES vem direto do sistema (libGLESv3), entao
// este header inclui os headers GLES 3.2 do NDK e implementa, em cima do GLES,
// as poucas funcoes do OpenGL de desktop que o jogo usa e o GLES nao tem.
//
// Os shaders sao GLSL de desktop (#version 460 core) adaptados para tambem
// compilar como GLSL ES 3.20; o cabecalho ES e colocado no lugar do de desktop
// por ogl::set_shader_prefix (opengl_wrapper.cpp).

#pragma once

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- constantes que so existem no OpenGL de desktop ---------------------------

#ifndef GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER
#define GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER 0x8CDB
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER
#define GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER 0x8CDC
#endif
// GL_EXT_disjoint_timer_query (so usado pelo contador de FPS). Sem a extensao o
// glBeginQuery gera GL_INVALID_ENUM e o contador fica sem o tempo de GPU.
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_FIRST_VERTEX_CONVENTION
#define GL_FIRST_VERTEX_CONVENTION 0x8E4D
#endif
// GLES 3 so tem primitive restart com indice FIXO (o maximo do tipo: 0xFFFF para
// GL_UNSIGNED_SHORT), ligado por GL_PRIMITIVE_RESTART_FIXED_INDEX. E exatamente o
// indice que o mapa usa (glPrimitiveRestartIndex(numeric_limits<uint16_t>::max())).
#ifndef GL_PRIMITIVE_RESTART
#define GL_PRIMITIVE_RESTART GL_PRIMITIVE_RESTART_FIXED_INDEX
#endif
// GLES nao tem esses "caps": MSAA depende so do framebuffer ser multisample, e
// nao existe suavizacao de linha. glEnable/glDisable deles viram no-op (abaixo).
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_LINE_SMOOTH
#define GL_LINE_SMOOTH 0x0B20
#endif

// --- funcoes do OpenGL de desktop implementadas com GLES -----------------------

inline void glClearDepth(GLdouble depth) {
	glClearDepthf(GLfloat(depth));
}

inline void glDepthRange(GLdouble n, GLdouble f) {
	glDepthRangef(GLfloat(n), GLfloat(f));
}

inline void glPrimitiveRestartIndex(GLuint) {
	// ver GL_PRIMITIVE_RESTART acima -- o indice no GLES e sempre o maximo do tipo
}

inline void glProvokingVertex(GLenum) {
	// GLES usa sempre o ULTIMO vertice como provocante e o jogo pede o primeiro.
	// So importa para varyings "flat", e os unicos (map_font: indice/opacidade do
	// glifo; textured_line_b: provincia da borda) sao iguais em todos os vertices
	// de cada primitiva -- entao o resultado e o mesmo.
}

// O jogo so usa glBufferStorage com flags == 0 (buffer imutavel, sem map),
// que e equivalente a um glBufferData estatico.
inline void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield) {
	glBufferData(target, size, data, GL_STATIC_DRAW);
}

inline void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
	for(GLsizei i = 0; i < drawcount; ++i) {
		if(count[i] > 0)
			glDrawArrays(mode, first[i], count[i]);
	}
}

// indirect = deslocamento dentro do GL_DRAW_INDIRECT_BUFFER ligado (GLES 3.1+)
inline void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
	GLsizei const step = stride != 0 ? stride : GLsizei(4 * sizeof(GLuint));
	for(GLsizei i = 0; i < drawcount; ++i) {
		glDrawArraysIndirect(mode, static_cast<const char*>(indirect) + intptr_t(i) * step);
	}
}

// GLES nao aceita formato nao-dimensionado (GL_RGBA) em texturas multisample
inline void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations) {
	if(internalformat == GL_RGBA)
		internalformat = GL_RGBA8;
	else if(internalformat == GL_RGB)
		internalformat = GL_RGB8;
	glTexStorage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
}

namespace android_gl_detail {
inline GLsizei bytes_per_pixel(GLenum format, GLenum type) {
	GLsizei channels = 4;
	switch(format) {
	case GL_RED: case GL_RED_INTEGER: channels = 1; break;
	case GL_RG: case GL_RG_INTEGER: channels = 2; break;
	case GL_RGB: case GL_RGB_INTEGER: channels = 3; break;
	default: channels = 4; break;
	}
	switch(type) {
	case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: return channels * 4;
	case GL_HALF_FLOAT: case GL_SHORT: case GL_UNSIGNED_SHORT: return channels * 2;
	default: return channels;
	}
}

// liga a textura (GL_TEXTURE_2D) num framebuffer temporario, para ler/limpar
struct scoped_texture_fbo {
	GLint previous_read = 0;
	GLint previous_draw = 0;
	GLuint fbo = 0;
	scoped_texture_fbo(GLuint texture, GLint level) {
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);
	}
	~scoped_texture_fbo() {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(previous_read));
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(previous_draw));
		glDeleteFramebuffers(1, &fbo);
	}
};

inline void texture_size(GLuint texture, GLint level, GLint& w, GLint& h) {
	GLint previous = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glBindTexture(GL_TEXTURE_2D, texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
	glBindTexture(GL_TEXTURE_2D, GLuint(previous));
}
} // namespace android_gl_detail

// So para texturas GL_TEXTURE_2D (o unico uso no jogo).
inline void glClearTexImage(GLuint texture, GLint level, GLenum format, GLenum type, const void* data) {
	GLint w = 0, h = 0;
	android_gl_detail::texture_size(texture, level, w, h);
	if(w <= 0 || h <= 0)
		return;
	GLsizei const bpp = android_gl_detail::bytes_per_pixel(format, type);
	uint8_t* pixels = static_cast<uint8_t*>(calloc(size_t(w) * size_t(h), size_t(bpp)));
	if(!pixels)
		return;
	if(data) {
		for(size_t i = 0; i < size_t(w) * size_t(h); ++i)
			memcpy(pixels + i * bpp, data, size_t(bpp));
	}
	GLint previous = 0, previous_alignment = 4;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, format, type, pixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, previous_alignment);
	glBindTexture(GL_TEXTURE_2D, GLuint(previous));
	free(pixels);
}

// GLES nao le texturas direto; le pelo framebuffer com glReadPixels.
// So para texturas GL_TEXTURE_2D com formato legivel como cor (RGBA8 etc.).
inline void glGetTextureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
	GLint w = 0, h = 0;
	android_gl_detail::texture_size(texture, level, w, h);
	if(w <= 0 || h <= 0 || GLsizei(w * h * android_gl_detail::bytes_per_pixel(format, type)) > bufSize)
		return;
	GLint previous_alignment = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &previous_alignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	{
		android_gl_detail::scoped_texture_fbo fbo{ texture, level };
		glReadPixels(0, 0, w, h, format, type, pixels);
	}
	glPixelStorei(GL_PACK_ALIGNMENT, previous_alignment);
}

inline void glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
	GLuint value = 0;
	glGetQueryObjectuiv(id, pname, &value);
	*params = GLint(value);
}

// --- glEnable/glDisable sem os caps que nao existem no GLES --------------------

inline void android_gl_enable(GLenum cap) {
	if(cap == GL_MULTISAMPLE || cap == GL_LINE_SMOOTH)
		return;
	glEnable(cap);
}
inline void android_gl_disable(GLenum cap) {
	if(cap == GL_MULTISAMPLE || cap == GL_LINE_SMOOTH)
		return;
	glDisable(cap);
}
#define glEnable android_gl_enable
#define glDisable android_gl_disable

// --- glTexImage2D: combinacoes de formato que o GLES recusa --------------------

// O framebuffer de indices de provincia e criado com interno GL_RGB e dados
// GL_RGBA (sem dados, so aloca). O GLES exige que o formato bata com o interno.
inline void android_gl_tex_image_2d(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels) {
	if(internalformat == GL_RGB && format == GL_RGBA && pixels == nullptr)
		format = GL_RGB;
	glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
#define glTexImage2D android_gl_tex_image_2d

// --- texturas S3TC (DXT1/3/5) ---------------------------------------------------
//
// As .dds do Victoria 2 sao S3TC. GPUs de celular normalmente nao tem
// GL_EXT_texture_compression_s3tc (Mali, PowerVR; parte das Adreno tem). Sem a
// extensao, a textura e descomprimida para RGBA8 na hora de enviar -- o
// resultado na tela e o mesmo, so ocupa mais memoria de video.

namespace android_gl_detail {

constexpr GLenum s3tc_rgb_dxt1 = 0x83F0;
constexpr GLenum s3tc_rgba_dxt1 = 0x83F1;
constexpr GLenum s3tc_rgba_dxt3 = 0x83F2;
constexpr GLenum s3tc_rgba_dxt5 = 0x83F3;

inline bool is_s3tc(GLenum format) {
	return format >= s3tc_rgb_dxt1 && format <= s3tc_rgba_dxt5;
}

inline bool has_s3tc_extension() {
	static int cached = -1;
	if(cached < 0) {
		cached = 0;
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		for(GLint i = 0; i < count; ++i) {
			auto const* name = reinterpret_cast<char const*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
			if(name && strcmp(name, "GL_EXT_texture_compression_s3tc") == 0) {
				cached = 1;
				break;
			}
		}
	}
	return cached == 1;
}

// 5/6 bits -> 8 bits por replicacao de bits, como as GPUs fazem
inline void rgb565(uint16_t c, uint8_t* out) {
	uint8_t const r = uint8_t((c >> 11) & 31), g = uint8_t((c >> 5) & 63), b = uint8_t(c & 31);
	out[0] = uint8_t((r << 3) | (r >> 2));
	out[1] = uint8_t((g << 2) | (g >> 4));
	out[2] = uint8_t((b << 3) | (b >> 2));
}

// bloco de cor (8 bytes) -> 16 pixels RGBA. four_color forca o modo de 4 cores
// (sempre usado nos blocos de cor do DXT3/DXT5).
inline void decode_color_block(uint8_t const* block, uint8_t* out_rgba, bool four_color, bool transparent_black) {
	uint16_t const c0 = uint16_t(block[0] | (block[1] << 8));
	uint16_t const c1 = uint16_t(block[2] | (block[3] << 8));
	uint8_t colors[4][4];
	rgb565(c0, colors[0]);
	rgb565(c1, colors[1]);
	colors[0][3] = colors[1][3] = 255;
	if(four_color || c0 > c1) {
		for(int k = 0; k < 3; ++k) {
			colors[2][k] = uint8_t((2 * colors[0][k] + colors[1][k]) / 3);
			colors[3][k] = uint8_t((colors[0][k] + 2 * colors[1][k]) / 3);
		}
		colors[2][3] = colors[3][3] = 255;
	} else {
		for(int k = 0; k < 3; ++k) {
			colors[2][k] = uint8_t((colors[0][k] + colors[1][k]) / 2);
			colors[3][k] = 0;
		}
		colors[2][3] = 255;
		colors[3][3] = transparent_black ? 0 : 255;
	}
	uint32_t const indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
	for(int i = 0; i < 16; ++i)
		memcpy(out_rgba + i * 4, colors[(indices >> (2 * i)) & 3], 4);
}

inline void decode_dxt3_alpha(uint8_t const* block, uint8_t* out_rgba) {
	for(int i = 0; i < 16; ++i) {
		uint8_t const nibble = uint8_t((block[i / 2] >> ((i & 1) * 4)) & 0xF);
		out_rgba[i * 4 + 3] = uint8_t(nibble * 17);
	}
}

inline void decode_dxt5_alpha(uint8_t const* block, uint8_t* out_rgba) {
	uint8_t alpha[8];
	alpha[0] = block[0];
	alpha[1] = block[1];
	if(alpha[0] > alpha[1]) {
		for(int i = 1; i < 7; ++i)
			alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
	} else {
		for(int i = 1; i < 5; ++i)
			alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
		alpha[6] = 0;
		alpha[7] = 255;
	}
	uint64_t bits = 0;
	for(int i = 0; i < 6; ++i)
		bits |= uint64_t(block[2 + i]) << (8 * i);
	for(int i = 0; i < 16; ++i)
		out_rgba[i * 4 + 3] = alpha[(bits >> (3 * i)) & 7];
}

inline size_t s3tc_block_size(GLenum format) {
	return (format == s3tc_rgb_dxt1 || format == s3tc_rgba_dxt1) ? 8 : 16;
}

// Descomprime uma imagem (ou uma camada) width x height. Devolve RGBA8 em malloc.
inline uint8_t* decode_s3tc_image(GLenum format, GLsizei width, GLsizei height, uint8_t const* data) {
	if(width <= 0 || height <= 0 || !data)
		return nullptr;
	uint8_t* out = static_cast<uint8_t*>(malloc(size_t(width) * size_t(height) * 4));
	if(!out)
		return nullptr;
	size_t const block_bytes = s3tc_block_size(format);
	GLsizei const blocks_x = (width + 3) / 4;
	GLsizei const blocks_y = (height + 3) / 4;
	uint8_t pixels[16 * 4];
	for(GLsizei by = 0; by < blocks_y; ++by) {
		for(GLsizei bx = 0; bx < blocks_x; ++bx) {
			uint8_t const* block = data + (size_t(by) * size_t(blocks_x) + size_t(bx)) * block_bytes;
			switch(format) {
			case s3tc_rgb_dxt1:
				decode_color_block(block, pixels, false, false);
				break;
			case s3tc_rgba_dxt1:
				decode_color_block(block, pixels, false, true);
				break;
			case s3tc_rgba_dxt3:
				decode_color_block(block + 8, pixels, true, false);
				decode_dxt3_alpha(block, pixels);
				break;
			default: // dxt5
				decode_color_block(block + 8, pixels, true, false);
				decode_dxt5_alpha(block, pixels);
				break;
			}
			for(int py = 0; py < 4; ++py) {
				GLsizei const y = by * 4 + py;
				if(y >= height)
					break;
				for(int px = 0; px < 4; ++px) {
					GLsizei const x = bx * 4 + px;
					if(x >= width)
						break;
					memcpy(out + (size_t(y) * size_t(width) + size_t(x)) * 4, pixels + (py * 4 + px) * 4, 4);
				}
			}
		}
	}
	return out;
}

inline size_t s3tc_image_size(GLenum format, GLsizei width, GLsizei height) {
	return size_t((width + 3) / 4) * size_t((height + 3) / 4) * s3tc_block_size(format);
}

// o jogo pode deixar GL_UNPACK_ROW_LENGTH etc. ligados (so valem para dados
// nao comprimidos); o RGBA descomprimido e sempre compacto
struct scoped_tight_unpack {
	GLint row_length = 0, skip_rows = 0, skip_pixels = 0, alignment = 4, image_height = 0, skip_images = 0;
	scoped_tight_unpack() {
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
		glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image_height);
		glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
		glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
	}
	~scoped_tight_unpack() {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
		glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
		glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, image_height);
		glPixelStorei(GL_UNPACK_SKIP_IMAGES, skip_images);
	}
};

} // namespace android_gl_detail

inline void android_gl_compressed_tex_image_2d(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data) {
	using namespace android_gl_detail;
	if(!is_s3tc(internalformat) || has_s3tc_extension()) {
		glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
		return;
	}
	uint8_t* rgba = decode_s3tc_image(internalformat, width, height, static_cast<uint8_t const*>(data));
	if(!rgba)
		return;
	{
		scoped_tight_unpack unpack;
		glTexImage2D(target, level, GL_RGBA8, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	}
	free(rgba);
}

// camadas uma depois da outra, como o GL le os dados de glCompressedTexImage3D
inline void android_gl_compressed_tex_image_3d(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
	using namespace android_gl_detail;
	if(!is_s3tc(internalformat) || has_s3tc_extension()) {
		glCompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, data);
		return;
	}
	scoped_tight_unpack unpack;
	glTexImage3D(target, level, GL_RGBA8, width, height, depth, border, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	if(!data)
		return;
	size_t const layer_bytes = s3tc_image_size(internalformat, width, height);
	for(GLsizei layer = 0; layer < depth; ++layer) {
		uint8_t* rgba = decode_s3tc_image(internalformat, width, height, static_cast<uint8_t const*>(data) + layer_bytes * size_t(layer));
		if(!rgba)
			return;
		glTexSubImage3D(target, level, 0, 0, layer, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		free(rgba);
	}
}

#define glCompressedTexImage2D android_gl_compressed_tex_image_2d
#define glCompressedTexImage3D android_gl_compressed_tex_image_3d
