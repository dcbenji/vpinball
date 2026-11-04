// license:GPLv3+

// Development only, see docs/FastIteration.md. The watcher thread polls src/shaders/bgfx and runs shaderc; nothing here touches bgfx
// or the renderer outside of Update (logic thread) and Apply (render thread at the end of a frame, with the logic thread blocked).

#include "core/stdafx.h"
#include "ShaderHotReload.h"

#ifdef VPX_DEV_TOOLS

#include "Shader.h"
#include "RenderDevice.h"
#include "ui/live/LiveUI.h"

#include <bgfx/embedded_shader.h>

#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>

namespace
{
uint64_t NowNs() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

uint64_t MTimeNs(const std::filesystem::path& path)
{
   struct stat st;
   if (stat(path.c_str(), &st) != 0)
      return 0;
   #ifdef __APPLE__
   return static_cast<uint64_t>(st.st_mtimespec.tv_sec) * 1000000000ull + st.st_mtimespec.tv_nsec;
   #else
   return static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull + st.st_mtim.tv_nsec;
   #endif
}

string ShellQuote(const string& s)
{
   string out = "'";
   for (const char c : s)
      out += c == '\'' ? "'\\''"s : string(1, c);
   return out + '\'';
}

string ReadFile(const std::filesystem::path& path)
{
   std::ifstream in(path, std::ios::binary);
   return string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

double Ms(const uint64_t from, const uint64_t to) { return to > from ? static_cast<double>(to - from) / 1000000.0 : 0.0; }

// Same arguments as shaders.cmake, for the backend of the running renderer
bool BackendArgs(string& args, string& define)
{
   switch (bgfx::getRendererType())
   {
   case bgfx::RendererType::Vulkan: args = "--platform linux -p spirv"; define = "TARGET_spv"; return true;
   case bgfx::RendererType::Metal: args = "--platform osx -p metal21-11"; define = "TARGET_mtl"; return true;
   case bgfx::RendererType::OpenGL: args = "--platform linux -p 440"; define = "TARGET_glsl"; return true;
   case bgfx::RendererType::OpenGLES: args = "--platform android -p 320_es"; define = "TARGET_essl"; return true;
   default: return false;
   }
}
}

ShaderHotReload* ShaderHotReload::Get()
{
   static const std::unique_ptr<ShaderHotReload> instance = []() -> std::unique_ptr<ShaderHotReload>
   {
      const char* const dir = std::getenv("VPX_SHADER_HOTRELOAD");
      if (dir == nullptr || dir[0] == '\0')
         return nullptr;
      if (!std::filesystem::is_directory(dir))
      {
         PLOGE << "VPX_SHADER_HOTRELOAD: not a folder: " << dir;
         return nullptr;
      }
      string shaderc;
      if (const char* const env = std::getenv("VPX_SHADERC"); env != nullptr && env[0] != '\0')
         shaderc = env;
      else
      {
         #ifdef __APPLE__
         const char* const tool = "third-party/tools/macos-arm64/shaderc";
         #else
         const char* const tool = "third-party/tools/linux-x64/shaderc";
         #endif
         const std::filesystem::path inTree = std::filesystem::path(dir) / "../../.." / tool; // <repo>/src/shaders/bgfx
         if (!std::filesystem::exists(inTree))
         {
            PLOGE << "VPX_SHADER_HOTRELOAD: shaderc not found at " << inTree.string() << ", set VPX_SHADERC";
            return nullptr;
         }
         shaderc = std::filesystem::absolute(inTree).lexically_normal().string();
      }
      return std::unique_ptr<ShaderHotReload>(new ShaderHotReload(std::filesystem::absolute(dir).lexically_normal().string(), shaderc));
   }();
   return instance.get();
}

ShaderHotReload::ShaderHotReload(const string& dir, const string& shaderc)
   : m_dir(dir)
   , m_shaderc(shaderc)
{
   string workDir = (std::filesystem::temp_directory_path() / "vpx-hotreload-XXXXXX").string();
   if (mkdtemp(workDir.data()) == nullptr)
      PLOGE << "Shader hot reload: can not create a temporary folder";
   m_workDir = workDir;
   PLOGI << "Shader hot reload active: watching " << m_dir << " with " << m_shaderc;
   m_thread = std::thread(&ShaderHotReload::WatchLoop, this);
}

ShaderHotReload::~ShaderHotReload()
{
   m_stop = true;
   if (m_thread.joinable())
      m_thread.join();
   std::error_code ec;
   std::filesystem::remove_all(m_workDir, ec);
}

void ShaderHotReload::AddManifest(const char* const* lines)
{
   std::lock_guard lock(m_mutex);
   for (; *lines != nullptr; ++lines)
   {
      const string line = *lines;
      const size_t a = line.find('|'), b = line.find('|', a + 1), c = line.find('|', b + 1);
      if (c == string::npos)
         continue;
      m_variants.push_back({ line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1, c - b - 1), line.substr(c + 1) });
   }
}

bgfx::ShaderHandle ShaderHotReload::MakeShader(const string& name, const Blobs* extra, const bgfx::EmbeddedShader* embedded) const
{
   for (const Blobs* const blobs : { extra, &m_blobs })
      if (blobs != nullptr)
         if (const auto it = blobs->find(name); it != blobs->end())
            return bgfx::createShader(bgfx::copy(it->second->data(), static_cast<uint32_t>(it->second->size())));
   return bgfx::createEmbeddedShader(embedded, bgfx::getRendererType(), name.c_str());
}

bgfx::ShaderHandle ShaderHotReload::CreateShader(const bgfx::EmbeddedShader* embedded, const char* name)
{
   std::lock_guard lock(m_mutex);
   return MakeShader(name, nullptr, embedded);
}

void ShaderHotReload::RegisterProgram(Shader* shader, ShaderTechnique technique, bool isClipVariant, const char* vsName, const char* fsName, const bgfx::EmbeddedShader* embedded)
{
   std::lock_guard lock(m_mutex);
   m_programs.push_back({ shader, technique, isClipVariant, vsName, fsName, embedded });
}

void ShaderHotReload::UnregisterShader(Shader* shader)
{
   std::lock_guard lock(m_mutex);
   std::erase_if(m_programs, [shader](const Program& p) { return p.shader == shader; });
}

// Sources depending on the changed files, following includes. Other files (an editor's swap file) concern nothing
std::set<string> ShaderHotReload::SourcesUsing(const std::set<string>& changedFiles) const
{
   std::map<string, std::set<string>> includes;
   std::set<string> included;
   for (const auto& entry : std::filesystem::directory_iterator(m_dir))
   {
      if (!entry.is_regular_file())
         continue;
      const string text = ReadFile(entry.path());
      auto& inc = includes[entry.path().filename().string()];
      for (size_t pos = text.find("#include"); pos != string::npos; pos = text.find("#include", pos + 8))
      {
         const size_t open = text.find_first_of("\"<\n", pos);
         if (open == string::npos || text[open] == '\n')
            continue;
         const size_t close = text.find_first_of(">\"\n", open + 1);
         if (close == string::npos || text[close] == '\n')
            continue;
         inc.insert(text.substr(open + 1, close - open - 1));
         included.insert(text.substr(open + 1, close - open - 1));
      }
   }
   std::set<string> dirty = changedFiles;
   const bool all = changedFiles.contains("varying.def.sc"); // Nothing includes it, it concerns every source
   for (bool grew = true; grew;)
   {
      grew = false;
      for (const auto& [file, inc] : includes)
         if (!dirty.contains(file) && std::ranges::any_of(inc, [&dirty](const string& i) { return dirty.contains(i); }))
         {
            dirty.insert(file);
            grew = true;
         }
   }
   std::set<string> sources;
   for (const auto& [file, inc] : includes)
      if (file.ends_with(".sc") && file != "varying.def.sc" && (all || dirty.contains(file)))
         sources.insert(file);
   return sources;
}

void ShaderHotReload::SetError(const string& error)
{
   PLOGE << "Shader hot reload failed, keeping the previous shaders:\n" << error;
   std::lock_guard lock(m_mutex);
   m_error = error;
   m_errorPending = true;
   m_state = "idle";
   m_attempts++;
}

void ShaderHotReload::Compile(const vector<string>& changedFiles, const uint64_t editNs, const uint64_t detectNs)
{
   string args, targetDefine;
   if (!BackendArgs(args, targetDefine))
   {
      SetError("Unsupported renderer for hot reload");
      return;
   }
   const std::set<string> sources = SourcesUsing(std::set<string>(changedFiles.begin(), changedFiles.end()));

   vector<Variant> todo;
   {
      std::lock_guard lock(m_mutex);
      std::set<string> used;
      for (const Program& p : m_programs)
      {
         used.insert(p.vs);
         used.insert(p.fs);
      }
      if (!todo.empty())
         m_error.clear(); // The error of the latest attempt, if any
      for (const Variant& v : m_variants)
         if (used.contains(v.name) && sources.contains(v.source))
            todo.push_back(v);
      m_state = "compiling";
   }
   auto batch = std::make_shared<Batch>();
   for (const string& file : changedFiles)
      batch->files += (batch->files.empty() ? "" : " ") + file;
   batch->editNs = editNs;
   batch->detectNs = detectNs;
   if (todo.empty())
   {
      PLOGI << "Shader hot reload: no loaded shader uses " << batch->files;
      std::lock_guard lock(m_mutex);
      m_state = "idle";
      m_attempts++;
      return;
   }

   std::atomic<size_t> next { 0 };
   std::mutex resultMutex;
   string errors;
   const auto worker = [&]()
   {
      for (size_t i = next++; i < todo.size(); i = next++)
      {
         const Variant& v = todo[i];
         const std::filesystem::path out = m_workDir / (v.name + ".bin"), log = m_workDir / (v.name + ".log");
         const string define = targetDefine + (v.defines.empty() ? ""s : ';' + v.defines);
         // WORKING_DIRECTORY of shaders.cmake: varying.def.sc and the includes are found from there
         const string cmd = "cd " + ShellQuote(m_dir) + " && " + ShellQuote(m_shaderc) + " -f " + ShellQuote(v.source) + ' ' + args + " --type " + v.type + " --define " + ShellQuote(define) + " -o "
            + ShellQuote(out.string()) + " > " + ShellQuote(log.string()) + " 2>&1";
         const int rc = std::system(cmd.c_str());
         std::lock_guard lock(resultMutex);
         if (rc != 0)
         {
            if (errors.empty()) // Variants of one source fail the same way: the first one tells it all
               errors = v.source + " (" + v.name + "):\n" + ReadFile(log);
            continue;
         }
         const string data = ReadFile(out);
         batch->blobs[v.name] = std::make_shared<vector<uint8_t>>(data.begin(), data.end());
      }
   };
   vector<std::thread> pool;
   const unsigned int nWorkers = std::clamp(std::thread::hardware_concurrency(), 2u, 16u) - 1;
   for (unsigned int i = 0; i < nWorkers; i++)
      pool.emplace_back(worker);
   for (std::thread& t : pool)
      t.join();

   if (!errors.empty())
   {
      SetError(errors);
      return;
   }
   batch->compiledNs = NowNs();
   PLOGI << "Shader hot reload: " << batch->files << ": " << batch->blobs.size() << " variants compiled in " << Ms(detectNs, batch->compiledNs) << " ms";
   std::lock_guard lock(m_mutex);
   if (m_ready != nullptr) // Not swapped yet (no frame was rendered since): nothing of it may get lost
      batch->blobs.insert(m_ready->blobs.begin(), m_ready->blobs.end());
   m_ready = batch;
   m_state = "swapping";
}

void ShaderHotReload::WatchLoop()
{
   std::map<string, uint64_t> known;
   const auto scan = [this]()
   {
      std::map<string, uint64_t> files;
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec))
         if (entry.is_regular_file(ec))
            files[entry.path().filename().string()] = MTimeNs(entry.path());
      return files;
   };
   known = scan();
   while (!m_stop)
   {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const auto files = scan();
      vector<string> forced;
      {
         std::lock_guard lock(m_mutex);
         forced.swap(m_forced);
      }
      const auto compile = [this](const vector<string>& list, const uint64_t editNs)
      {
         try
         {
            Compile(list, editNs, NowNs());
         }
         catch (const std::exception& e)
         {
            SetError(e.what());
         }
      };
      if (files == known)
      {
         if (!forced.empty())
            compile(forced, NowNs());
         continue;
      }
      // A write may still be running: go on once the times stand still for one more poll
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const auto settled = scan();
      if (settled != files)
         continue;
      vector<string> changed = forced;
      uint64_t editNs = 0;
      for (const auto& [file, time] : settled)
         if (const auto it = known.find(file); it == known.end() || it->second != time)
         {
            changed.push_back(file);
            editNs = max(editNs, time);
         }
      known = settled;
      if (!changed.empty())
         compile(changed, editNs); // Edits made meanwhile are found by the next scan
   }
}

void ShaderHotReload::Update(Player* player)
{
   Renderer* const renderer = player->m_renderer.get();
   RenderDevice* const rd = renderer->m_renderDevice;
   std::shared_ptr<Batch> ready, swapped;
   string error;
   {
      std::lock_guard lock(m_mutex);
      ready = std::exchange(m_ready, nullptr);
      swapped = std::exchange(m_swapped, nullptr);
      if (std::exchange(m_errorPending, false))
         error = m_error;
      if (m_measuring && rd->GetFrameIndex() > m_measuring->swapFrame + 1)
      {
         const auto batch = std::exchange(m_measuring, nullptr);
         batch->pixelsNs = NowNs();
         m_last = {
            { "files", batch->files }, { "variants", batch->blobs.size() }, { "programs", batch->programs },
            { "detect_ms", Ms(batch->editNs, batch->detectNs) }, { "compile_ms", Ms(batch->detectNs, batch->compiledNs) },
            { "swap_ms", Ms(batch->compiledNs, batch->swappedNs) }, { "present_ms", Ms(batch->swappedNs, batch->pixelsNs) }, { "edit_to_pixels_ms", Ms(batch->editNs, batch->pixelsNs) } };
         m_generation++;
         m_attempts++;
         PLOGI << "Shader hot reload done: " << m_last.dump();
         m_state = "idle";
      }
   }
   if (!error.empty() && player->m_liveUI != nullptr)
   {
      // On screen, only the lines saying what is wrong (the whole output is in the log and in the status)
      string shown;
      std::istringstream lines(error);
      for (string line; std::getline(lines, line);)
         if (line.starts_with("ERROR") || line.find(" error ") != string::npos)
            shown += line + '\n';
      player->m_liveUI->PushNotification("Shader error, keeping the previous shader:\n" + (shown.empty() ? error : shown).substr(0, 500), 10000);
   }
   if (ready != nullptr)
      rd->AddEndOfFrameCmd([this, ready]() { Apply(ready); });
   if (swapped != nullptr)
   {
      // Baked with the old shaders: the static prepass. Shadow pages only hold depth, from the vertex and depth shaders
      renderer->MarkStaticPrepassDirty();
      if (std::ranges::any_of(swapped->blobs, [](const auto& blob) { return blob.first.starts_with("vs_") || blob.first.find("depth") != string::npos || blob.first.find("_sb") != string::npos; }))
      {
         renderer->InvalidateShadowContent();
         m_shadowRestartFrame = rd->GetFrameIndex();
      }
   }
}

void ShaderHotReload::Apply(const std::shared_ptr<Batch>& batch)
{
   struct Swap { const Program* program; bgfx::ProgramHandle handle; bgfx::ShaderHandle vsh, fsh; };
   vector<Swap> swaps;
   string error;
   std::lock_guard lock(m_mutex);
   for (const Program& p : m_programs)
   {
      if (!batch->blobs.contains(p.vs) && !batch->blobs.contains(p.fs))
         continue;
      const bgfx::ShaderHandle vsh = MakeShader(p.vs, &batch->blobs, p.embedded), fsh = MakeShader(p.fs, &batch->blobs, p.embedded);
      const bgfx::ProgramHandle handle = bgfx::isValid(vsh) && bgfx::isValid(fsh) ? bgfx::createProgram(vsh, fsh, true) : bgfx::ProgramHandle(BGFX_INVALID_HANDLE);
      if (!bgfx::isValid(handle))
      {
         error = "Could not build the program from " + p.vs + " / " + p.fs + " (varyings of the two stages differ?)";
         break;
      }
      swaps.push_back({ &p, handle, vsh, fsh });
      if (const string bad = p.shader->HotReloadCheckUniforms(vsh, fsh); !bad.empty())
      {
         error = bad + " is new to this shader: add it in C++ or restart (" + p.fs + ')';
         break;
      }
   }
   if (!error.empty())
   {
      for (const Swap& s : swaps)
         bgfx::destroy(s.handle); // Also releases the shaders
      PLOGE << "Shader hot reload failed, keeping the previous shaders:\n" << error;
      m_error = error;
      m_errorPending = true;
      m_state = "idle";
      m_attempts++;
      return;
   }
   for (const Swap& s : swaps)
      s.program->shader->HotReloadSwapProgram(s.program->technique, s.program->isClipVariant, s.handle, s.vsh, s.fsh, s.program->vs.c_str(), s.program->fs.c_str());
   for (const auto& [name, blob] : batch->blobs)
      m_blobs[name] = blob;
   batch->programs = static_cast<int>(swaps.size());
   batch->swappedNs = NowNs();
   batch->swapFrame = swaps.empty() ? 0 : swaps[0].program->shader->GetRenderDevice()->GetFrameIndex();
   m_error.clear();
   m_swapped = batch;
   m_measuring = batch;
}

void ShaderHotReload::RequestReload(const string& file)
{
   std::lock_guard lock(m_mutex);
   m_forced.push_back(file);
}

nlohmann::json ShaderHotReload::Status() const
{
   std::lock_guard lock(m_mutex);
   return { { "generation", m_generation }, { "attempts", m_attempts }, { "state", m_state }, { "error", m_error }, { "last", m_last }, { "shaderc", m_shaderc }, { "dir", m_dir } };
}

#endif
