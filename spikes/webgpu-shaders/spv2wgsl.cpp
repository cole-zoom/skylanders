// Spike S2, step 2: SPIR-V to WGSL with Tint, inside wasm -- what the
// backend would run on the GPU thread each time VideoCommon creates a
// shader. Built with Emscripten against Tint's libraries
// (build-spv2wgsl.sh) and run under node with direct file access.
//
// For every NNN.<stage>.spv in a directory: translate, time it, and compare
// the WGSL with the native Tint's NNN.<stage>.wgsl beside it, which Chrome
// has validated (validate.html).
//
//   node spv2wgsl.js <dir>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/tint/api/tint.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/wgsl/program/program.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

static std::string Slurp(const std::string& path)
{
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv)
{
  if (argc < 2)
    return 2;
  tint::Initialize();
  std::vector<std::string> names;
  if (DIR* d = opendir(argv[1]))
  {
    while (dirent* e = readdir(d))
    {
      const std::string n = e->d_name;
      if (n.size() > 4 && n.compare(n.size() - 4, 4, ".spv") == 0)
        names.push_back(n);
    }
    closedir(d);
  }
  std::sort(names.begin(), names.end());

  int ok = 0, same = 0;
  double total_ms = 0, worst_ms = 0, first_ms = -1;
  size_t spv_bytes = 0, wgsl_bytes = 0;
  for (const std::string& n : names)
  {
    const std::string bin = Slurp(std::string(argv[1]) + "/" + n);
    std::vector<uint32_t> spv(bin.size() / 4);
    std::memcpy(spv.data(), bin.data(), spv.size() * 4);
    spv_bytes += bin.size();

    const auto t0 = std::chrono::steady_clock::now();
    tint::spirv::reader::Options ropts;
    auto ir = tint::spirv::reader::ReadIR(spv, ropts);
    std::string wgsl;
    bool good = ir == tint::Success;
    if (good)
    {
      tint::wgsl::writer::Options wopts;
      wopts.allowed_features = tint::wgsl::AllowedFeatures::Everything();
      auto program = tint::wgsl::writer::ProgramFromIR(ir.Get(), wopts);
      good = program == tint::Success;
      if (good)
      {
        auto out = tint::wgsl::writer::Generate(program.Get(), wopts);
        good = out == tint::Success;
        if (good)
          wgsl = out->wgsl;
      }
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (first_ms < 0)
      first_ms = ms;
    total_ms += ms;
    worst_ms = ms > worst_ms ? ms : worst_ms;
    if (!good)
    {
      std::printf("FAIL %s\n", n.c_str());
      continue;
    }
    ++ok;
    wgsl_bytes += wgsl.size();
    // The CLI writes the WGSL with a leading newline; compare the text.
    std::string native = Slurp(std::string(argv[1]) + "/" + n.substr(0, n.size() - 4) + ".wgsl");
    while (!native.empty() && native.front() == '\n')
      native.erase(native.begin());
    std::string mine = wgsl;
    while (!mine.empty() && mine.front() == '\n')
      mine.erase(mine.begin());
    while (!native.empty() && native.back() == '\n')
      native.pop_back();
    while (!mine.empty() && mine.back() == '\n')
      mine.pop_back();
    if (native == mine)
      ++same;
  }
  std::printf("%d of %zu translated, %d identical to the native Tint's\n", ok, names.size(), same);
  std::printf("%.2f ms each on average, %.2f ms worst, %.2f ms the first (cold)\n",
              total_ms / names.size(), worst_ms, first_ms);
  std::printf("%zu KB of SPIR-V in, %zu KB of WGSL out\n", spv_bytes / 1024, wgsl_bytes / 1024);
  return 0;
}
