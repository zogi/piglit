/*
 * Copyright © 2010 Marek Olšák <maraeo@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

/** @file tex-miplevel-selection.c
 *
 * This tests interactions between GL_TEXTURE_BASE/MAX_LEVEL GL_TEXTURE_MIN/
 * MAX_LOD, TEXTURE_LOD_BIAS, mipmap filtering on/off, and scaling of texture
 * coordinates as a means to "bias" the LOD.
 *
 * On top of that, test as many texture GLSL functions, sampler types, and
 * texture targets which allow mipmapping as possible, e.g. with an explicit
 * LOD, bias, and derivatives.
 *
 * Each mipmap level is set to a different color/depth value, so that we can
 * check that the correct level is read.
 *
 * Texture targets with multiple layers/slices/faces have only one layer/etc
 * set to the expected value. The other layers are black, so that we can check
 * that the correct layer is read.
 *
 * Shadow samplers are tricky because we can't use the GL_EQUAL compare mode
 * because of precision issues. Therefore, we bind the same texture twice:
 * the first unit uses GL_LESS and a small number (tolerance) is subtracted
 * from Z, and the second unit uses GL_GREATER and a small number (tolerance)
 * is added to Z. If both shadow samplers return 1, which means the texel
 * value lies in between, the test passes.
 */

#include "piglit-util-gl-common.h"

PIGLIT_GL_TEST_CONFIG_BEGIN

	config.supports_gl_compat_version = 10;

	config.window_width = 900;
	config.window_height = 600;
	config.window_visual = PIGLIT_GL_VISUAL_RGB | PIGLIT_GL_VISUAL_DOUBLE;

PIGLIT_GL_TEST_CONFIG_END

#define TEX_SIZE 32
#define TEST_LAYER 9 // the layer index used for testing
#define LAST_LEVEL 5

static const float clear_colors[][3] = {
	{1.0, 0.0, 0.0},
	{0.0, 1.0, 0.0},
	{0.0, 0.0, 1.0},
	{1.0, 1.0, 0.0},
	{0.0, 1.0, 1.0},
	{1.0, 0.0, 1.0},
};

static const float shadow_colors[][3] = {
	{1.0, 1.0, 1.0},
	{1.0, 1.0, 1.0},
	{1.0, 1.0, 1.0},
	{1.0, 1.0, 1.0},
	{1.0, 1.0, 1.0},
	{1.0, 1.0, 1.0},
};

static const float clear_depths[] = {
	0.1,
	0.2,
	0.3,
	0.4,
	0.5,
	0.6
};

enum target_type {
	TEX_1D,		  /* proj. coords = vec2(x,w) */
	TEX_1D_PROJ_VEC4, /* proj. coords = vec4(x,0,0,w) */
	TEX_2D,		  /* proj. coords = vec3(x,y,w) */
	TEX_2D_PROJ_VEC4, /* proj. coords = vec4(x,y,0,w) */
	TEX_3D,
	TEX_CUBE,
	TEX_1D_ARRAY,
	TEX_2D_ARRAY,
	TEX_CUBE_ARRAY,
	TEX_1D_SHADOW,
	TEX_2D_SHADOW,
	TEX_CUBE_SHADOW,
	TEX_1D_ARRAY_SHADOW,
	TEX_2D_ARRAY_SHADOW,
	TEX_CUBE_ARRAY_SHADOW,
};

#define IS_SHADOW(t) ((t) >= TEX_1D_SHADOW)

enum shader_type {
	FIXED_FUNCTION,
	ARB_SHADER_TEXTURE_LOD,
	GL3_TEXTURE_LOD,
	GL3_TEXTURE_BIAS,
	GL3_TEXTURE,
	GL3_TEXTURE_OFFSET,
	GL3_TEXTURE_OFFSET_BIAS,
	GL3_TEXTURE_PROJ,
};

#define NEED_GL3(t) ((t) >= GL3_TEXTURE_LOD)

static enum shader_type test = FIXED_FUNCTION;
static enum target_type target = TEX_2D;
static GLenum gltarget;
static GLboolean has_offset;
static GLboolean in_place_probing, no_bias, no_lod_clamp;
static GLuint loc_lod, loc_bias, loc_z;
static GLuint samp[2];

static int offset[] = {3, -1, 2};

#define GL3_FS_PREAMBLE \
	"#version %s \n" \
	"#extension GL_ARB_texture_cube_map_array : enable \n" \
	"uniform sampler%s tex; \n" \
	"#define TYPE %s \n" \
	"#define OFFSET %s(ivec3(3, -1, 2)) \n"

#define GL3_FS_SHADOW_PREAMBLE \
	"#version %s \n" \
	"#extension GL_ARB_texture_cube_map_array : enable \n" \
	"uniform sampler%s tex, tex2; \n" \
	"uniform float z; \n" \
	"#define TYPE %s \n" \
	"#define MASK %s \n" \
	"#define OFFSET %s(ivec3(3, -1, 2)) \n"

static const char *fscode_arb_lod =
	"#extension GL_ARB_shader_texture_lod : require\n"
	"uniform sampler2D tex;\n"
	"uniform float lod;\n"
	"void main() {\n"
	"  gl_FragColor = texture2DLod(tex, gl_TexCoord[0].xy, lod); \n"
	"}\n";

static const char *fscode_gl3_lod =
	GL3_FS_PREAMBLE
	"uniform float lod; \n"
	"void main() { \n"
	"  gl_FragColor = textureLod(tex, TYPE(gl_TexCoord[0]), lod); \n"
	"} \n";

static const char *fscode_gl3_lod_shadow =
	GL3_FS_SHADOW_PREAMBLE
	"uniform float lod; \n"
	"void main() { \n"
	"  gl_FragColor = vec4(textureLod(tex, TYPE(gl_TexCoord[0]) - 0.05 * MASK, lod) * \n"
	"                      textureLod(tex2, TYPE(gl_TexCoord[0]) + 0.05 * MASK, lod)); \n"
	"} \n";

static const char *fscode_gl3_bias =
	GL3_FS_PREAMBLE
	"uniform float bias; \n"
	"void main() { \n"
	"  gl_FragColor = texture(tex, TYPE(gl_TexCoord[0]), bias); \n"
	"} \n";

static const char *fscode_gl3_bias_shadow =
	GL3_FS_SHADOW_PREAMBLE
	"uniform float bias; \n"
	"void main() { \n"
	"  gl_FragColor = vec4(texture(tex, TYPE(gl_TexCoord[0]) - 0.05 * MASK, bias) * \n"
	"                      texture(tex2, TYPE(gl_TexCoord[0]) + 0.05 * MASK, bias)); \n"
	"} \n";

static const char *fscode_gl3_simple =
	GL3_FS_PREAMBLE
	"void main() { \n"
	"  gl_FragColor = texture(tex, TYPE(gl_TexCoord[0])); \n"
	"} \n";

static const char *fscode_gl3_simple_shadow =
	GL3_FS_SHADOW_PREAMBLE
	"void main() { \n"
	"  gl_FragColor = vec4(texture(tex, TYPE(gl_TexCoord[0]) - 0.05 * MASK) * \n"
	"                      texture(tex2, TYPE(gl_TexCoord[0]) + 0.05 * MASK)); \n"
	"} \n";

static const char *fscode_gl3_simple_shadow_cubearray =
	GL3_FS_SHADOW_PREAMBLE
	"uniform float z; \n"
	"void main() { \n"
	"  gl_FragColor = vec4(texture(tex, gl_TexCoord[0], z - 0.05) * \n"
	"                      texture(tex2, gl_TexCoord[0], z + 0.05)); \n"
	"} \n";

static const char *fscode_gl3_offset =
	GL3_FS_PREAMBLE
	"void main() { \n"
	"  gl_FragColor = textureOffset(tex, TYPE(gl_TexCoord[0]), OFFSET); \n"
	"} \n";

static const char *fscode_gl3_offset_shadow =
	GL3_FS_SHADOW_PREAMBLE
	"void main() { \n"
	"  gl_FragColor = vec4(textureOffset(tex, TYPE(gl_TexCoord[0]) - 0.05 * MASK, \n"
	"                                    OFFSET) * \n"
	"                      textureOffset(tex2, TYPE(gl_TexCoord[0]) + 0.05 * MASK, \n"
	"                                    OFFSET)); \n"
	"} \n";

static const char *fscode_gl3_offset_bias =
	GL3_FS_PREAMBLE
	"uniform float bias; \n"
	"void main() { \n"
	"  gl_FragColor = textureOffset(tex, TYPE(gl_TexCoord[0]), OFFSET, bias); \n"
	"} \n";

static const char *fscode_gl3_offset_bias_shadow =
	GL3_FS_SHADOW_PREAMBLE
	"uniform float bias; \n"
	"void main() { \n"
	"  gl_FragColor = vec4(textureOffset(tex, TYPE(gl_TexCoord[0]) - 0.05 * MASK, \n"
	"                                    OFFSET, bias) * \n"
	"                      textureOffset(tex2, TYPE(gl_TexCoord[0]) + 0.05 * MASK, \n"
	"                                    OFFSET, bias)); \n"
	"} \n";

static const char *fscode_gl3_proj =
	GL3_FS_PREAMBLE
	"void main() { \n"
	"  gl_FragColor = textureProj(tex, TYPE(gl_TexCoord[0])); \n"
	"} \n";

static const char *fscode_gl3_proj_shadow =
	GL3_FS_SHADOW_PREAMBLE
	"void main() { \n"
	"  gl_FragColor = vec4(textureProj(tex, TYPE(gl_TexCoord[0]) - 0.05 * MASK) * \n"
	"                      textureProj(tex2, TYPE(gl_TexCoord[0]) + 0.05 * MASK)); \n"
	"} \n";

static void set_sampler_parameter(GLenum pname, GLint value)
{
	glSamplerParameteri(samp[0], pname, value);
	glSamplerParameteri(samp[1], pname, value);
}

void
piglit_init(int argc, char **argv)
{
	GLuint tex, fb, prog;
	GLenum status;
	int i, level, layer, dim, num_layers;
	const char *target_str, *type_str, *compare_value_mask = "", *offset_type_str = "";
	const char *version = "130";
	GLenum format, attachment, clearbits;
	char fscode[2048];

        for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-inplace") == 0)
			in_place_probing = GL_TRUE;
		else if (strcmp(argv[i], "-nobias") == 0)
			no_bias = GL_TRUE;
		else if (strcmp(argv[i], "-nolod") == 0)
			no_lod_clamp = GL_TRUE;
		else if (strcmp(argv[i], "-GL_ARB_shader_texture_lod") == 0)
			test = ARB_SHADER_TEXTURE_LOD;
		else if (strcmp(argv[i], "textureLod") == 0)
			test = GL3_TEXTURE_LOD;
		else if (strcmp(argv[i], "texture(bias)") == 0)
			test = GL3_TEXTURE_BIAS;
		else if (strcmp(argv[i], "texture()") == 0)
			test = GL3_TEXTURE;
		else if (strcmp(argv[i], "textureOffset") == 0)
			test = GL3_TEXTURE_OFFSET;
		else if (strcmp(argv[i], "textureOffset(bias)") == 0)
			test = GL3_TEXTURE_OFFSET_BIAS;
		else if (strcmp(argv[i], "textureProj") == 0)
			test = GL3_TEXTURE_PROJ;
		else if (strcmp(argv[i], "1D") == 0)
			target = TEX_1D;
		else if (strcmp(argv[i], "1D_ProjVec4") == 0)
			target = TEX_1D_PROJ_VEC4;
		else if (strcmp(argv[i], "2D") == 0)
			target = TEX_2D;
		else if (strcmp(argv[i], "2D_ProjVec4") == 0)
			target = TEX_2D_PROJ_VEC4;
		else if (strcmp(argv[i], "3D") == 0)
			target = TEX_3D;
		else if (strcmp(argv[i], "Cube") == 0)
			target = TEX_CUBE;
		else if (strcmp(argv[i], "1DArray") == 0)
			target = TEX_1D_ARRAY;
		else if (strcmp(argv[i], "2DArray") == 0)
			target = TEX_2D_ARRAY;
		else if (strcmp(argv[i], "CubeArray") == 0)
			target = TEX_CUBE_ARRAY;
		else if (strcmp(argv[i], "1DShadow") == 0)
			target = TEX_1D_SHADOW;
		else if (strcmp(argv[i], "2DShadow") == 0)
			target = TEX_2D_SHADOW;
		else if (strcmp(argv[i], "CubeShadow") == 0)
			target = TEX_CUBE_SHADOW;
		else if (strcmp(argv[i], "1DArrayShadow") == 0)
			target = TEX_1D_ARRAY_SHADOW;
		else if (strcmp(argv[i], "2DArrayShadow") == 0)
			target = TEX_2D_ARRAY_SHADOW;
		else if (strcmp(argv[i], "CubeArrayShadow") == 0)
			target = TEX_CUBE_ARRAY_SHADOW;
		else {
			printf("Unknown parameter: %s\n", argv[i]);
			piglit_report_result(PIGLIT_FAIL);
		}
        }

	piglit_require_extension("GL_ARB_framebuffer_object");
	piglit_require_extension("GL_ARB_sampler_objects");
	piglit_require_extension("GL_ARB_texture_storage");
	piglit_require_gl_version(NEED_GL3(test) ? 30 : 14);

	if (target == TEX_2D_ARRAY_SHADOW &&
	    test == GL3_TEXTURE_OFFSET) {
		piglit_require_GLSL_version(430);
		version = "430";
	}

	switch (target) {
	case TEX_1D:
		gltarget = GL_TEXTURE_1D;
		target_str = "1D";
		type_str = "float";
		offset_type_str = "int";
		break;
	case TEX_1D_PROJ_VEC4:
		gltarget = GL_TEXTURE_1D;
		target_str = "1D";
		type_str = "vec4";
		offset_type_str = "int";
		break;
	case TEX_2D:
		gltarget = GL_TEXTURE_2D;
		target_str = "2D";
		type_str = "vec2";
		offset_type_str = "ivec2";
		break;
	case TEX_2D_PROJ_VEC4:
		gltarget = GL_TEXTURE_2D;
		target_str = "2D";
		type_str = "vec4";
		offset_type_str = "ivec2";
		break;
	case TEX_3D:
		gltarget = GL_TEXTURE_3D;
		target_str = "3D";
		type_str = "vec3";
		offset_type_str = "ivec3";
		break;
	case TEX_CUBE:
		gltarget = GL_TEXTURE_CUBE_MAP;
		target_str = "Cube";
		type_str = "vec3";
		break;
	case TEX_1D_ARRAY:
		piglit_require_gl_version(30);
		gltarget = GL_TEXTURE_1D_ARRAY;
		target_str = "1DArray";
		type_str = "vec2";
		offset_type_str = "int";
		break;
	case TEX_2D_ARRAY:
		piglit_require_gl_version(30);
		gltarget = GL_TEXTURE_2D_ARRAY;
		target_str = "2DArray";
		type_str = "vec3";
		offset_type_str = "ivec2";
		break;
	case TEX_CUBE_ARRAY:
		piglit_require_gl_version(30);
		piglit_require_extension("GL_ARB_texture_cube_map_array");
		gltarget = GL_TEXTURE_CUBE_MAP_ARRAY;
		target_str = "CubeArray";
		type_str = "vec4";
		break;
	case TEX_1D_SHADOW:
		gltarget = GL_TEXTURE_1D;
		target_str = "1DShadow";
		type_str = "vec3";
		offset_type_str = "int";
		compare_value_mask = "vec3(0.0, 0.0, 1.0)";
		break;
	case TEX_2D_SHADOW:
		gltarget = GL_TEXTURE_2D;
		target_str = "2DShadow";
		type_str = "vec3";
		offset_type_str = "ivec2";
		compare_value_mask = "vec3(0.0, 0.0, 1.0)";
		break;
	case TEX_CUBE_SHADOW:
		piglit_require_gl_version(30);
		gltarget = GL_TEXTURE_CUBE_MAP;
		target_str = "CubeShadow";
		type_str = "vec4";
		compare_value_mask = "vec4(0.0, 0.0, 0.0, 1.0)";
		break;
	case TEX_1D_ARRAY_SHADOW:
		piglit_require_gl_version(30);
		gltarget = GL_TEXTURE_1D_ARRAY;
		target_str = "1DArrayShadow";
		type_str = "vec3";
		offset_type_str = "int";
		compare_value_mask = "vec3(0.0, 0.0, 1.0)";
		break;
	case TEX_2D_ARRAY_SHADOW:
		piglit_require_gl_version(30);
		gltarget = GL_TEXTURE_2D_ARRAY;
		target_str = "2DArrayShadow";
		type_str = "vec4";
		offset_type_str = "ivec2";
		compare_value_mask = "vec4(0.0, 0.0, 0.0, 1.0)";
		break;
	case TEX_CUBE_ARRAY_SHADOW:
		piglit_require_gl_version(30);
		piglit_require_extension("GL_ARB_texture_cube_map_array");
		gltarget = GL_TEXTURE_CUBE_MAP_ARRAY;
		target_str = "CubeArrayShadow";
		type_str = "vec4";
		break;
	}

	if (test == GL3_TEXTURE_PROJ) {
		if (!strcmp(type_str, "float"))
			type_str = "vec2";
		else if (!strcmp(type_str, "vec2"))
			type_str = "vec3";
		else if (!strcmp(type_str, "vec3"))
			type_str = "vec4";

		if (!strcmp(compare_value_mask, "vec3(0.0, 0.0, 1.0)"))
			compare_value_mask = "vec4(0.0, 0.0, 1.0, 0.0)";
	}

	switch (test) {
	case FIXED_FUNCTION:
		break;
	case ARB_SHADER_TEXTURE_LOD:
		piglit_require_GLSL();
		piglit_require_extension("GL_ARB_shader_texture_lod");

		prog = piglit_build_simple_program(NULL, fscode_arb_lod);
		loc_lod = glGetUniformLocation(prog, "lod");
		break;
	case GL3_TEXTURE_LOD:
		if (IS_SHADOW(target))
			sprintf(fscode, fscode_gl3_lod_shadow, version, target_str,
				type_str, compare_value_mask, offset_type_str);
		else
			sprintf(fscode, fscode_gl3_lod, version, target_str, type_str,
				offset_type_str);

		prog = piglit_build_simple_program(NULL, fscode);
		loc_lod = glGetUniformLocation(prog, "lod");
		break;
	case GL3_TEXTURE_BIAS:
		if (IS_SHADOW(target))
			sprintf(fscode, fscode_gl3_bias_shadow, version, target_str,
				type_str, compare_value_mask, offset_type_str);
		else
			sprintf(fscode, fscode_gl3_bias, version, target_str, type_str,
				offset_type_str);

		prog = piglit_build_simple_program(NULL, fscode);
		loc_bias = glGetUniformLocation(prog, "bias");
		break;
	case GL3_TEXTURE:
		if (target == TEX_CUBE_ARRAY_SHADOW)
			sprintf(fscode,
				fscode_gl3_simple_shadow_cubearray,
				version, target_str, type_str, compare_value_mask,
				offset_type_str);
		else if (IS_SHADOW(target))
			sprintf(fscode, fscode_gl3_simple_shadow,
				version, target_str, type_str, compare_value_mask,
				offset_type_str);
		else
			sprintf(fscode, fscode_gl3_simple, version, target_str,
				type_str, offset_type_str);

		prog = piglit_build_simple_program(NULL, fscode);
		if (target == TEX_CUBE_ARRAY_SHADOW)
			loc_z = glGetUniformLocation(prog, "z");
		break;
	case GL3_TEXTURE_OFFSET:
		if (IS_SHADOW(target))
			sprintf(fscode, fscode_gl3_offset_shadow,
				version, target_str, type_str, compare_value_mask,
				offset_type_str);
		else
			sprintf(fscode, fscode_gl3_offset, version, target_str,
				type_str, offset_type_str);

		prog = piglit_build_simple_program(NULL, fscode);

		has_offset = GL_TRUE;
		no_lod_clamp = GL_TRUE;
		break;
	case GL3_TEXTURE_OFFSET_BIAS:
		if (IS_SHADOW(target))
			sprintf(fscode, fscode_gl3_offset_bias_shadow,
				version, target_str, type_str, compare_value_mask,
				offset_type_str);
		else
			sprintf(fscode, fscode_gl3_offset_bias, version, target_str,
				type_str, offset_type_str);

		prog = piglit_build_simple_program(NULL, fscode);
		loc_bias = glGetUniformLocation(prog, "bias");

		has_offset = GL_TRUE;
		no_lod_clamp = GL_TRUE;
		break;
	case GL3_TEXTURE_PROJ:
		if (IS_SHADOW(target))
			sprintf(fscode, fscode_gl3_proj_shadow, version, target_str,
				type_str, compare_value_mask, offset_type_str);
		else
			sprintf(fscode, fscode_gl3_proj, version, target_str, type_str,
				offset_type_str);

		prog = piglit_build_simple_program(NULL, fscode);
		break;
	default:
		assert(0);
	}

	if (test != FIXED_FUNCTION) {
		GLuint loc_tex, loc_tex2;

		glUseProgram(prog);
		loc_tex = glGetUniformLocation(prog, "tex");
		glUniform1i(loc_tex, 0);

		if (IS_SHADOW(target)) {
			loc_tex2 = glGetUniformLocation(prog, "tex2");
			glUniform1i(loc_tex2, 1);
		}
	}

	glGenTextures(1, &tex);
	glBindTexture(gltarget, tex);

	if (IS_SHADOW(target)) {
		format = GL_DEPTH_COMPONENT24;
		attachment = GL_DEPTH_ATTACHMENT;
		clearbits = GL_DEPTH_BUFFER_BIT;
	}
	else {
		format = GL_RGBA8;
		attachment = GL_COLOR_ATTACHMENT0;
		clearbits = GL_COLOR_BUFFER_BIT;
	}

	switch (gltarget) {
	case GL_TEXTURE_1D:
		num_layers = 1;
		glTexStorage1D(gltarget, 6, format, TEX_SIZE);
		break;
	case GL_TEXTURE_2D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_1D_ARRAY:
		num_layers = gltarget == GL_TEXTURE_CUBE_MAP ? 6 :
			     gltarget == GL_TEXTURE_1D_ARRAY ? TEX_SIZE : 1;
		glTexStorage2D(gltarget, 6, format, TEX_SIZE, TEX_SIZE);
		break;
	case GL_TEXTURE_3D:
	case GL_TEXTURE_2D_ARRAY:
	case GL_TEXTURE_CUBE_MAP_ARRAY:
		num_layers = gltarget == GL_TEXTURE_CUBE_MAP_ARRAY ? 36 : TEX_SIZE;
		glTexStorage3D(gltarget, 6, format, TEX_SIZE, TEX_SIZE, num_layers);
		break;
	default:
		assert(0);
	}
	assert(glGetError() == 0);

	if (test == FIXED_FUNCTION)
		glDisable(gltarget);

	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);

	/* set one layer/face to the expected color and the other layers/faces to black */
	for (level = 0, dim = TEX_SIZE; dim > 0; level++, dim /= 2) {
		if (gltarget == GL_TEXTURE_3D)
			num_layers = dim;

		for (layer = 0; layer < num_layers; layer++) {
			switch (gltarget) {
			case GL_TEXTURE_1D:
				glFramebufferTexture1D(GL_FRAMEBUFFER, attachment,
						       gltarget, tex, level);
				break;
			case GL_TEXTURE_2D:
				glFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
						       gltarget, tex, level);
				break;
			case GL_TEXTURE_CUBE_MAP:
				glFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
						       GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer,
						       tex, level);
				break;
			case GL_TEXTURE_3D:
				glFramebufferTexture3D(GL_FRAMEBUFFER, attachment,
						       gltarget, tex, level, layer);
				break;
			case GL_TEXTURE_1D_ARRAY:
			case GL_TEXTURE_2D_ARRAY:
			case GL_TEXTURE_CUBE_MAP_ARRAY:
				glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment,
							  tex, level, layer);
				break;
			}
			assert(glGetError() == 0);

			status = glCheckFramebufferStatusEXT (GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE) {
				fprintf(stderr, "FBO incomplete status 0x%X for level %i, layer %i\n",
					status, level, layer);
				piglit_report_result(PIGLIT_SKIP);
			}

			/* For array and cube textures, only TEST_LAYER is
			 * cleared to the expected value.
			 * For 3D textures, the middle slice is cleared. */
			if (num_layers == 1 ||
			    (gltarget == GL_TEXTURE_3D && layer == num_layers/2 + (has_offset ? offset[2] : 0)) ||
			    (gltarget != GL_TEXTURE_3D && layer == TEST_LAYER % num_layers)) {
				if (has_offset) {
					/* For testing the shader-provided texture offset,
					 * only clear the texel which is expected
					 * to be fetched. The other texels are black. */
					glClearColor(0, 0, 0, 0);
					glClearDepth(0);
					glClear(clearbits);

					glClearColor(clear_colors[level][0],
							clear_colors[level][1],
							clear_colors[level][2],
							0.0);
					glClearDepth(clear_depths[level]);
					glEnable(GL_SCISSOR_TEST);
					/* Add +1, because the probed texel is at (1,1). */
					glScissor(offset[0]+1,
						  gltarget == GL_TEXTURE_1D ||
						  gltarget == GL_TEXTURE_1D_ARRAY ? 0 : offset[1]+1,
						  1, 1);
					glClear(clearbits);
					glDisable(GL_SCISSOR_TEST);
				}
				else {
					glClearColor(clear_colors[level][0],
							clear_colors[level][1],
							clear_colors[level][2],
							0.0);
					glClearDepth(clear_depths[level]);
					glClear(clearbits);
				}
			}
			else {
				glClearColor(0, 0, 0, 0);
				glClearDepth(0);
				glClear(clearbits);
			}

			assert(glGetError() == 0);
		}
	}

	glDeleteFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, piglit_winsys_fbo);
	piglit_ortho_projection(piglit_width, piglit_height, GL_FALSE);

	if (test == FIXED_FUNCTION)
		glEnable(gltarget);

	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	glGenSamplers(2, samp);
	glBindSampler(0, samp[0]);

	set_sampler_parameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	set_sampler_parameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	if (IS_SHADOW(target)) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(gltarget, tex);
		glActiveTexture(GL_TEXTURE0);
		glBindSampler(1, samp[1]);

		set_sampler_parameter(GL_TEXTURE_COMPARE_MODE,
				      GL_COMPARE_REF_TO_TEXTURE);
		glSamplerParameteri(samp[0], GL_TEXTURE_COMPARE_FUNC, GL_LESS);
		glSamplerParameteri(samp[1], GL_TEXTURE_COMPARE_FUNC, GL_GREATER);
	}

	assert(glGetError() == 0);
}

#define SET_VEC(c, x, y, z, w) do { c[0] = x; c[1] = y; c[2] = z; c[3] = w; } while (0)

static void
draw_quad(int x, int y, int w, int h, int expected_level, int fetch_level,
	  int baselevel, int maxlevel, int bias, int mipfilter)
{
	/* 2D coordinates */
	float s0 = 0;
	float t0 = 0;
	float s1 = (float)w / TEX_SIZE;
	float t1 = (float)h / TEX_SIZE;
	/* Cube coordinates */
	float x0 = 2*s0 - 1;
	float z0 = 2*t0 - 1;
	float x1 = 2*s1 - 1;
	float z1 = 2*t1 - 1;
	/* Final coordinates */
	float c0[4], c1[4], c2[4], c3[4];
	/* shadow compare value */
	float z = clear_depths[expected_level];
	/* multiplier for textureProj */
	float p = 1;

	switch (test) {
	case ARB_SHADER_TEXTURE_LOD:
	case GL3_TEXTURE_LOD:
		/* set an explicit LOD */
		glUniform1f(loc_lod, fetch_level - baselevel);
		break;

	case GL3_TEXTURE_BIAS:
		/* set a bias */
		glUniform1f(loc_bias, bias);
		/* fall through to scale the coordinates */
	case GL3_TEXTURE:
	case GL3_TEXTURE_PROJ:
	case FIXED_FUNCTION:
		/* scale the coordinates (decrease the texel size),
		 * so that the texture fetch selects this level
		 */
		s1 *= 1 << fetch_level;
		t1 *= 1 << fetch_level;
		break;

	case GL3_TEXTURE_OFFSET_BIAS:
		/* set a bias */
		glUniform1f(loc_bias, bias);
		/* fall through */
	case GL3_TEXTURE_OFFSET: {
		/* Things get quite complicated with offsets.
		 *
		 * The single pixel which is not black has the same integer
		 * coordinates in every mipmap level, but not the same normalized
		 * coordinates. Therefore we have to fix the normalized ones, so
		 * that GLSL always reads from the same integer coordinates.
		 */
		int maxlevel_clamped = mipfilter ? maxlevel : baselevel;
		int bias_clamped =
			CLAMP(fetch_level + bias, baselevel, maxlevel_clamped) - fetch_level;

		/* scale the coordinates */
		s1 *= 1 << fetch_level;
		t1 *= 1 << fetch_level;

		if (bias_clamped > 0) {
			float pixsize_before_bias = 1.0 / (TEX_SIZE >> fetch_level);
			float offset = pixsize_before_bias * ((1 << (bias_clamped-1))*3 - 1.5);

			s0 += offset;
			t0 += offset;
			s1 += offset;
			t1 += offset;
		}
		else if (bias_clamped < 0) {
			float pixsize_after_bias = 1.0 / (TEX_SIZE >> (fetch_level + bias_clamped));
			float offset = -pixsize_after_bias * ((1 << (-bias_clamped-1))*3 - 1.5);

			s0 += offset;
			t0 += offset;
			s1 += offset;
			t1 += offset;
		}
		break;
	}
	default:
		assert(0);
	}

	if (test == GL3_TEXTURE_PROJ)
		p = 7;

	switch (target) {
	case TEX_1D:
		SET_VEC(c0, s0*p, p, 0, 1);
		SET_VEC(c1, s1*p, p, 0, 1);
		SET_VEC(c2, s1*p, p, 0, 1);
		SET_VEC(c3, s0*p, p, 0, 1);
		break;
	case TEX_2D:
		SET_VEC(c0, s0*p, t0*p, p, 1);
		SET_VEC(c1, s1*p, t0*p, p, 1);
		SET_VEC(c2, s1*p, t1*p, p, 1);
		SET_VEC(c3, s0*p, t1*p, p, 1);
		break;
	case TEX_1D_PROJ_VEC4:
	case TEX_2D_PROJ_VEC4:
		SET_VEC(c0, s0*p, t0*p, 0, p);
		SET_VEC(c1, s1*p, t0*p, 0, p);
		SET_VEC(c2, s1*p, t1*p, 0, p);
		SET_VEC(c3, s0*p, t1*p, 0, p);
		break;
	case TEX_2D_ARRAY:
		SET_VEC(c0, s0, t0, TEST_LAYER, 1);
		SET_VEC(c1, s1, t0, TEST_LAYER, 1);
		SET_VEC(c2, s1, t1, TEST_LAYER, 1);
		SET_VEC(c3, s0, t1, TEST_LAYER, 1);
		break;
	case TEX_1D_SHADOW:
	case TEX_2D_SHADOW:
		SET_VEC(c0, s0*p, t0*p, z*p, p);
		SET_VEC(c1, s1*p, t0*p, z*p, p);
		SET_VEC(c2, s1*p, t1*p, z*p, p);
		SET_VEC(c3, s0*p, t1*p, z*p, p);
		break;
	case TEX_1D_ARRAY_SHADOW:
		SET_VEC(c0, s0, TEST_LAYER, z, 1);
		SET_VEC(c1, s1, TEST_LAYER, z, 1);
		SET_VEC(c2, s1, TEST_LAYER, z, 1);
		SET_VEC(c3, s0, TEST_LAYER, z, 1);
		break;
	case TEX_2D_ARRAY_SHADOW:
		SET_VEC(c0, s0, t0, TEST_LAYER, z);
		SET_VEC(c1, s1, t0, TEST_LAYER, z);
		SET_VEC(c2, s1, t1, TEST_LAYER, z);
		SET_VEC(c3, s0, t1, TEST_LAYER, z);
		break;
	case TEX_3D:
		SET_VEC(c0, s0*p, t0*p, 0.5*p, p);
		SET_VEC(c1, s1*p, t0*p, 0.5*p, p);
		SET_VEC(c2, s1*p, t1*p, 0.5*p, p);
		SET_VEC(c3, s0*p, t1*p, 0.5*p, p);
		break;
	case TEX_1D_ARRAY:
		SET_VEC(c0, s0, TEST_LAYER, 0, 1);
		SET_VEC(c1, s1, TEST_LAYER, 0, 1);
		SET_VEC(c2, s1, TEST_LAYER, 0, 1);
		SET_VEC(c3, s0, TEST_LAYER, 0, 1);
		break;
	case TEX_CUBE_ARRAY_SHADOW:
		/* Set the compare value through a uniform, because all
		 * components of TexCoord0 are taken. */
		glUniform1f(loc_z, z);
		/* fall through */
	case TEX_CUBE:
	case TEX_CUBE_ARRAY:
		assert(TEST_LAYER % 6 == 3); /* negative Y */
		SET_VEC(c0, x0, -1,  z0, TEST_LAYER / 6);
		SET_VEC(c1, x1, -1,  z0, TEST_LAYER / 6);
		SET_VEC(c2, x1, -1, -z1, TEST_LAYER / 6);
		SET_VEC(c3, x0, -1, -z1, TEST_LAYER / 6);
		break;
	case TEX_CUBE_SHADOW:
		assert(TEST_LAYER % 6 == 3); /* negative Y */
		SET_VEC(c0, x0, -1,  z0, z);
		SET_VEC(c1, x1, -1,  z0, z);
		SET_VEC(c2, x1, -1, -z1, z);
		SET_VEC(c3, x0, -1, -z1, z);
		break;
	default:
		assert(0);
	}

	glBegin(GL_QUADS);

	glTexCoord4fv(c0);
	glVertex2f(x, y);
	glTexCoord4fv(c1);
	glVertex2f(x + w, y);
	glTexCoord4fv(c2);
	glVertex2f(x + w, y + h);
	glTexCoord4fv(c3);
	glVertex2f(x, y + h);

	glEnd();
}

static bool
colors_equal(const float *c1, const unsigned char *c2)
{
	int i;

	for (i = 0; i < 3; i++)
		if (fabs(c1[i] - (c2[i]/255.0)) > 0.01)
			return false;
	return true;
}

static bool
check_result(const unsigned char *probed, int expected_level,
	     int fetch_level, int baselevel, int maxlevel, int minlod, int maxlod,
	     int bias, int mipfilter)
{
	const float (*colors)[3] = IS_SHADOW(target) ? shadow_colors : clear_colors;

	if (!colors_equal(colors[expected_level], probed)) {
		int i;
		static const float black[] = {0, 0, 0};

		printf("Failure:\n");
#if 0 /* disabled, not needed unless you are debugging the test */
		printf("  Expected: %f %f %f\n", colors[expected_level][0],
				colors[expected_level][1], colors[expected_level][2]);
		printf("  Observed: %f %f %f\n", probed[0]/255.0,
				probed[1]/255.0, probed[2]/255.0);
#endif
		printf("  Expected level: %i\n", expected_level);

		if (IS_SHADOW(target)) {
			if (colors_equal(black, probed))
				puts("  Observed: shadow comparison failed");
			else
				puts("  Observed: unknown value (broken driver?)");
		}
		else {
			for (i = 0; i < LAST_LEVEL; i++) {
				if (colors_equal(colors[i], probed)) {
					printf("  Observed level: %i\n", i);
					break;
				}
			}
			if (i == LAST_LEVEL) {
				if (colors_equal(black, probed))
					puts("  Observed: wrong layer/face/slice or wrong level or wrong offset");
				else
					puts("  Observed: unknown value (broken driver?)");
			}
		}

		printf("  Fetch level: %i, baselevel: %i, maxlevel: %i, "
		       "minlod: %i, maxlod: %i, bias: %i, mipfilter: %s\n",
		       fetch_level, baselevel, maxlevel, minlod,
		       no_lod_clamp ? LAST_LEVEL : maxlod, bias, mipfilter ? "yes" : "no");
		return false;
	}
	return true;
}

static int
calc_expected_level(int fetch_level, int baselevel, int maxlevel, int minlod,
		    int maxlod, int bias, int mipfilter)
{
	int expected_level;

	if (mipfilter) {
		if (no_lod_clamp) {
			expected_level = CLAMP(fetch_level + bias,
					       baselevel,
					       maxlevel);
		} else {
			expected_level = CLAMP(fetch_level + bias,
					       MIN2(baselevel + minlod, maxlevel),
					       MIN2(baselevel + maxlod, maxlevel));
		}
	} else {
		expected_level = baselevel;
	}
	assert(expected_level >= 0 && expected_level <= LAST_LEVEL);
	return expected_level;
}

enum piglit_result
piglit_display(void)
{
	int fetch_level, baselevel, maxlevel, minlod, maxlod, bias, mipfilter;
	int expected_level, x, y, total, failed;
	int start_bias, end_bias;
	int end_min_lod, end_max_lod;

	if (no_bias) {
		start_bias = 0;
		end_bias = 0;
	} else {
		start_bias = -LAST_LEVEL;
		end_bias = LAST_LEVEL;
	}

	if (no_lod_clamp) {
		end_min_lod = 0;
		end_max_lod = 0;
	} else {
		end_min_lod = LAST_LEVEL;
		end_max_lod = LAST_LEVEL;
	}

	glClearColor(0.5, 0.5, 0.5, 0.5);
	glClear(GL_COLOR_BUFFER_BIT);

	total = 0;
	failed = 0;
	for (fetch_level = 0; fetch_level <= LAST_LEVEL; fetch_level++)
		for (baselevel = 0; baselevel <= LAST_LEVEL; baselevel++)
			for (maxlevel = baselevel; maxlevel <= LAST_LEVEL; maxlevel++)
				for (minlod = 0; minlod <= end_min_lod; minlod++)
					for (maxlod = minlod; maxlod <= end_max_lod; maxlod++)
						for (bias = start_bias; bias <= end_bias; bias++)
							for (mipfilter = 0; mipfilter < 2; mipfilter++) {
								expected_level = calc_expected_level(fetch_level, baselevel,
											maxlevel, minlod, maxlod, bias,
											mipfilter);

								/* Skip this if the offset pixel lies outside of the texture. */
								if (has_offset &&
								    (TEX_SIZE >> expected_level) <= 1+MAX2(offset[0], offset[1]))
									continue;

								glTexParameteri(gltarget, GL_TEXTURE_BASE_LEVEL, baselevel);
								glTexParameteri(gltarget, GL_TEXTURE_MAX_LEVEL, maxlevel);
								if (!no_lod_clamp) {
									set_sampler_parameter(GL_TEXTURE_MIN_LOD, minlod);
									set_sampler_parameter(GL_TEXTURE_MAX_LOD, maxlod);
								}
								if (!no_bias &&
								    test != GL3_TEXTURE_BIAS &&
								    test != GL3_TEXTURE_OFFSET_BIAS)
									set_sampler_parameter(GL_TEXTURE_LOD_BIAS, bias);
								set_sampler_parameter(GL_TEXTURE_MIN_FILTER,
										mipfilter ? GL_NEAREST_MIPMAP_NEAREST
											  : GL_NEAREST);

								x = (total % (piglit_width/3)) * 3;
								y = (total / (piglit_width/3)) * 3;

								draw_quad(x, y, 3, 3, expected_level, fetch_level,
									  baselevel, maxlevel, bias, mipfilter);

								if (in_place_probing) {
									unsigned char probe[3];

									glReadPixels(x+1, y+1, 1, 1, GL_RGB,
										     GL_UNSIGNED_BYTE, probe);

									if (!check_result(probe, expected_level,
											  fetch_level, baselevel,
											  maxlevel, minlod, maxlod,
											  bias, mipfilter)) {
										failed++;
									}
								}
								total++;
							}

	if (!in_place_probing) {
		unsigned char *pix, *p;

		pix = malloc(piglit_width * piglit_height * 4);
		glReadPixels(0, 0, piglit_width, piglit_height, GL_RGBA, GL_UNSIGNED_BYTE, pix);

		total = 0;
		for (fetch_level = 0; fetch_level <= LAST_LEVEL; fetch_level++)
			for (baselevel = 0; baselevel <= LAST_LEVEL; baselevel++)
				for (maxlevel = baselevel; maxlevel <= LAST_LEVEL; maxlevel++)
					for (minlod = 0; minlod <= end_min_lod; minlod++)
						for (maxlod = minlod; maxlod <= end_max_lod; maxlod++)
							for (bias = start_bias; bias <= end_bias; bias++)
								for (mipfilter = 0; mipfilter < 2; mipfilter++) {
									expected_level = calc_expected_level(fetch_level,
												baselevel, maxlevel, minlod,
												maxlod, bias, mipfilter);

									/* Skip this if the offset pixel lies outside of the texture. */
									if (has_offset &&
									    (TEX_SIZE >> expected_level) <= 1+MAX2(offset[0], offset[1]))
										continue;

									x = (total % (piglit_width/3)) * 3 + 1;
									y = (total / (piglit_width/3)) * 3 + 1;
									p = pix + (y*piglit_width + x)*4;

									if (!check_result(p, expected_level, fetch_level,
											  baselevel, maxlevel, minlod,
											  maxlod, bias, mipfilter)) {
										failed++;
									}
									total++;
								}
		free(pix);
	}

	assert(glGetError() == 0);
	printf("Summary: %i/%i passed\n", total-failed, total);

	piglit_present_results();

	return !failed ? PIGLIT_PASS : PIGLIT_FAIL;
}
