// Spike S2, step 1 (docs/PHASE3-WEBGPU.md §5): the shaders a run created,
// from GLSL to SPIR-V the way the Metal backend does it.
//
// Input: a bootgame log made with --dump-shaders ("SHADER <stage> <base64>"
// lines: VideoCommon's source for a backend reporting WebGPU's
// capabilities, in the Metal dialect). For each: prepend the header, compile
// with glslang under Dolphin's options (VideoCommon/Spirv.cpp:
// CompileShaderToSPV), and write NNN.<stage>.glsl and NNN.<stage>.spv for
// Tint. Prints one line per shader that glslang rejects.
//
//   glsl2spv <log> <outdir> [--rewrite]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

// The Metal backend's SHADER_HEADER (VideoBackends/Metal/MTLUtil.mm) without
// its four 8- and 16-bit extensions, which WGSL has no types for.
static const char* kHeader = R"(
#version 450 core
#define ATTRIBUTE_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
#define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
#define SAMPLER_BINDING(x) layout(set = 1, binding = x)
#define TEXEL_BUFFER_BINDING(x) layout(set = 1, binding = (x + 8))
#define SSBO_BINDING(x) layout(std430, set = 2, binding = x)
#define INPUT_ATTACHMENT_BINDING(x, y, z) layout(set = x, binding = y, input_attachment_index = z)
#define VARYING_LOCATION(x) layout(location = x)
// WGSL cannot force early fragment tests. A WebGPU backend reports
// bSupportsEarlyZ = false, and VideoCommon takes its other path; empty here
// so the spike measures the rest.
#define FORCE_EARLY_Z const int dolphin_no_forced_early_z = 0
#define FB_FETCH_VALUE subpassLoad(in_ocol0)
#define API_METAL 1
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define frac fract
#define lerp mix
#define gl_VertexID gl_VertexIndex
#define gl_InstanceID gl_InstanceIndex
)";

static const char* kComputeHeader = R"(
#version 450 core
#define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
#define SAMPLER_BINDING(x) layout(set = 1, binding = x)
#define SSBO_BINDING(x) layout(std430, set = 2, binding = x)
#define IMAGE_BINDING(format, x) layout(format, set = 3, binding = x)
#define API_METAL 1
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define frac fract
#define lerp mix
)";

static std::string Unbase64(const std::string& in)
{
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  int bits = 0, acc = 0;
  for (char c : in)
  {
    const int v = val(c);
    if (v < 0)
      continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

// What VideoCommon's generators would have to emit for WebGPU, applied as
// text so the rest of the translation can be measured before they are
// changed (--rewrite). Each is a finding of this spike:
//
//   1. dolphin_isnan: Tint's SPIR-V reader has no OpIsNan. The bit test
//      WriteIsNanHeader already emits for D3D.
//   2. samp[8]: core WebGPU has no arrays of textures. Eight bindings, and
//      the one indexing site -- sampleTextureWrapper, always called with a
//      literal -- becomes a switch the optimiser folds.
static void Replace(std::string& s, const std::string& from, const std::string& to)
{
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
    s.replace(at, from.size(), to);
}

static std::string RewriteForWebGPU(std::string s)
{
  Replace(s, "#define dolphin_isnan(f) isnan(f)\n",
          "bool dolphin_isnan(float f) { return (floatBitsToInt(f) & 0x7FFFFFFF) > 0x7F800000; }\n");
  if (s.find("uniform sampler2DArray samp[8];") != std::string::npos)
  {
    std::string decls;
    for (int i = 0; i < 8; ++i)
      decls += "SAMPLER_BINDING(" + std::to_string(i) + ") uniform sampler2DArray samp" +
               std::to_string(i) + ";\n";
    Replace(s, "SAMPLER_BINDING(0) uniform sampler2DArray samp[8];\n", decls);
    std::string wrapper = "int4 sampleTextureWrapper(uint texmap, int2 uv, int layer) {\n"
                          "  switch (texmap) {\n";
    for (int i = 0; i < 8; ++i)
      wrapper += "  case " + std::to_string(i) + "u: return sampleTexture(texmap, samp" +
                 std::to_string(i) + ", uv, layer);\n";
    wrapper += "  }\n  return int4(0);\n}\n";
    Replace(s,
            "#define sampleTextureWrapper(texmap, uv, layer) sampleTexture(texmap, samp[texmap], "
            "uv, layer)\n",
            wrapper);
  }
  return s;
}

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: glsl2spv <log> <outdir>\n");
    return 2;
  }
  glslang::InitializeProcess();
  std::ifstream log(argv[1]);
  std::string line;
  int n = 0, ok = 0;
  while (std::getline(log, line))
  {
    if (line.rfind("SHADER ", 0) != 0)
      continue;
    const int stage = line[7] - '0';  // ShaderStage: 0 vertex, 1 geometry, 2 pixel, 3 compute
    std::string source = Unbase64(line.substr(9));
    if (argc > 3 && std::strcmp(argv[3], "--rewrite") == 0)
      source = RewriteForWebGPU(std::move(source));
    static const EShLanguage kLang[] = {EShLangVertex, EShLangGeometry, EShLangFragment,
                                        EShLangCompute};
    static const char* kExt[] = {"vert", "geom", "frag", "comp"};
    const std::string full = std::string(stage == 3 ? kComputeHeader : kHeader) + source;
    char base[512];
    std::snprintf(base, sizeof(base), "%s/%03d.%s", argv[2], n, kExt[stage]);
    std::ofstream(std::string(base) + ".glsl") << full;

    // As CompileShaderToSPV: core profile 450, SPIR-V and Vulkan rules.
    glslang::TShader shader(kLang[stage]);
    const char* text = full.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const EShMessages messages =
        static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader::ForbidIncluder includer;
    glslang::TProgram program;
    bool good = shader.parse(GetDefaultResources(), 450, ECoreProfile, false, true, messages,
                             includer);
    if (good)
    {
      program.addShader(&shader);
      good = program.link(messages);
    }
    if (!good)
    {
      std::printf("%03d.%s glslang: %s%s\n", n, kExt[stage], shader.getInfoLog(),
                  program.getInfoLog());
    }
    else
    {
      std::vector<unsigned int> spv;
      glslang::SpvOptions options;
      options.disableOptimizer = false;
      options.stripDebugInfo = true;
      glslang::GlslangToSpv(*program.getIntermediate(kLang[stage]), spv, &options);
      std::ofstream(std::string(base) + ".spv", std::ios::binary)
          .write(reinterpret_cast<const char*>(spv.data()), spv.size() * 4);
      ++ok;
    }
    ++n;
  }
  std::printf("%d shaders, %d to SPIR-V\n", n, ok);
  glslang::FinalizeProcess();
  return 0;
}
