// license:GPLv3+

// Development only, see docs/FastIteration.md. Connection threads only parse and wait: every tool runs on the logic thread in Update, between
// frames, where the editor's own edits run. Tool names and shapes follow the VPX Editor's MCP server where they overlap (vpx_guide, vpx_table,
// vpx_part, vpx_material, vpx_history, vpx_view). What only a running engine can do is apart, as vpx_engine_*.
// Property names are the JSON keys of the pack files; values go through the pack (de)serializers and the code undo uses.

#include "core/stdafx.h"
#include "EngineMcp.h"

#ifdef VPX_DEV_TOOLS

#include "core/pinundo.h"
#include "parts/primitive.h"
#include "parts/light.h"
#include "parts/flasher.h"
#include "physics/PhysicsEngine.h"
#include "renderer/RenderDevice.h"
#include "renderer/Texture.h"
#include "ui/live/LiveUI.h"
#include "ui/live/editor/PropertyPane.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>
#include <random>
#include <regex>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // macOS: SO_NOSIGPIPE on the socket
#endif

extern std::atomic<uint64_t> g_shadowWork; // Shadow pages and cached shadows rendered so far (Renderer)
extern std::atomic<int> g_shadowBakesPending;

namespace
{
constexpr const char* kProtocolVersion = "2025-06-18";

// Name, title-less description and input schema of each tool, as the client sees them
constexpr const char* kTools = R"JSON([
 {"name":"vpx_guide","annotations":{"readOnlyHint":true,"openWorldHint":false},
  "description":"START HERE. What this server is: the running VPX engine. Returns the state of the session and how to use the tools.",
  "inputSchema":{"type":"object","properties":{}}},
 {"name":"vpx_table","annotations":{"readOnlyHint":true},
  "description":"Read-only inspection of the running table. Dispatch by `action`: \"overview\" (path/name/dimensions/counts), \"parts\" (filtered part list), \"part\" (full JSON for one part by `name`). Field names are the pack file keys (parts/*.json).",
  "inputSchema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["overview","parts","part"]},
   "name":{"type":"string","description":"For action=part: the exact part name (case-insensitive)."},
   "type":{"type":"string","description":"For action=parts: filter by item type (e.g. flipper, primitive)."},
   "nameRegex":{"type":"string","description":"For action=parts: case-insensitive regex matched against names."},
   "limit":{"type":"integer","minimum":1,"maximum":2000,"default":200,"description":"For action=parts: max items."}}}},
 {"name":"vpx_part","annotations":{"destructiveHint":true},
  "description":"Change a part of the live table, through the same load path undo uses. `action`:\"modify\", `part` with `partName` plus the fields to change (top level of `part`, or in `more`). Applies immediately unless preview:true. Field names are the pack file keys (see vpx_table action=part). Undo with vpx_history.",
  "inputSchema":{"type":"object","required":["action","part"],"properties":{"action":{"type":"string","enum":["modify"]},
   "part":{"type":"object","required":["partName"],"properties":{"partName":{"type":"string"},"type":{"type":"string"},"more":{"type":"object"}},"additionalProperties":true},
   "preview":{"type":"boolean","description":"Return the resolved change without applying it."}}}},
 {"name":"vpx_material","annotations":{"destructiveHint":true},
  "description":"Read or edit materials of the live table. \"list\": all materials. \"modify\": `name` + `patch` with the fields to update; a preview until confirm:true. Field names are the pack file keys (materials/*.json). Undo with vpx_history.",
  "inputSchema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["list","modify"]},
   "name":{"type":"string"},"patch":{"type":"object"},"confirm":{"type":"boolean","default":false,"description":"Set true to actually apply the edit. Without it, returns a preview."}}}},
 {"name":"vpx_history","annotations":{"destructiveHint":true},
  "description":"Undo or redo the most recent edit, sharing one history with the live editor. Needs an edit session (-LiveEdit).",
  "inputSchema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["undo","redo"]}}}},
 {"name":"vpx_view","title":"View the table","annotations":{"readOnlyHint":true},
  "description":"LOOK at the running table: a PNG of the 3D view (in an edit session, without the editor panels unless area=window), from a frame after this call, so an edit made before is in it. With `path` it is saved there at full size and not returned inline.",
  "inputSchema":{"type":"object","properties":{"view":{"type":"string","enum":["3d"],"default":"3d"},"area":{"type":"string","enum":["view","window"],"default":"view"},
   "max_width":{"type":"integer","minimum":256,"maximum":4096,"default":1024,"description":"Downscale the inline PNG to at most this many pixels wide."},
   "path":{"type":"string","description":"Absolute .png or .jpg path to save the full size capture to."},
   "settle":{"type":"boolean","description":"Wait (up to 5 s) for the shadows rebuilt after an edit before capturing, so the light in the picture is right. Default: true in an edit session, false in play."}}}},
 {"name":"vpx_engine_status","annotations":{"readOnlyHint":true},
  "description":"Running engine: session mode (edit, inspect, play, capture), frame number, playing or paused, shader reload state.",
  "inputSchema":{"type":"object","properties":{}}},
 {"name":"vpx_engine_camera","annotations":{"readOnlyHint":false},
  "description":"Set the view camera: `eye` and `target` as [x,y,z] in table units (Z up), vertical `fov` in degrees. release:true goes back to the table's own camera.",
  "inputSchema":{"type":"object","properties":{"eye":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"target":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
   "fov":{"type":"number","default":39.6},"shading":{"type":"string","enum":["pbr","solid","wireframe"],"default":"pbr","description":"Edit session only."},"release":{"type":"boolean"}}}},
 {"name":"vpx_engine_lights","annotations":{"readOnlyHint":false},
  "description":"Hold a lamp or flasher at a `level` (1 = its full intensity), over the script and ROM, until released: leave `level` out or null to release. Not saved.",
  "inputSchema":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"level":{"type":["number","null"]}}}},
 {"name":"vpx_engine_run","annotations":{"readOnlyHint":false},
  "description":"Game time: \"pause\", \"resume\", \"step\" (run `ms` of game time and pause again, or one physics step without `ms`), \"wait_frames\" (return after `n` more presented frames).",
  "inputSchema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["pause","resume","step","wait_frames"]},"ms":{"type":"integer","minimum":1},"n":{"type":"integer","minimum":1,"default":1}}}},
 {"name":"vpx_engine_timing","annotations":{"readOnlyHint":true},
  "description":"Summary of the last presented frames (up to 1024): fps, frame and GPU ms (avg, p50, p95, p99, max), frames over twice the median.",
  "inputSchema":{"type":"object","properties":{"last":{"type":"integer","minimum":1},"reset":{"type":"boolean"}}}},
 {"name":"vpx_engine_shaders","annotations":{"readOnlyHint":false},
  "description":"Shader hot reload (VPX_SHADER_HOTRELOAD). \"status\": last timings and last compile error. \"reload\": recompile `file` now, as if edited.",
  "inputSchema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["status","reload"]},"file":{"type":"string","description":"For reload: a file name in the shader folder, e.g. fs_pbr.sc"}}}},
 {"name":"vpx_engine_reload","annotations":{"destructiveHint":true},
  "description":"Reload one `part` or `material` of the running table from the pack folder it was loaded from, as one undo step. mesh:true also reloads a primitive's mesh, which is not undoable.",
  "inputSchema":{"type":"object","properties":{"part":{"type":"string"},"material":{"type":"string"},"mesh":{"type":"boolean"}}}},
 {"name":"vpx_engine_quit","annotations":{"destructiveHint":true},
  "description":"Close the player. Unsaved edits are lost, without a prompt.",
  "inputSchema":{"type":"object","properties":{}}}
])JSON";

string Base64(const string& data)
{
   static const char* const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   string out;
   out.reserve((data.size() + 2) / 3 * 4);
   for (size_t i = 0; i < data.size(); i += 3)
   {
      const uint32_t b0 = static_cast<uint8_t>(data[i]), b1 = i + 1 < data.size() ? static_cast<uint8_t>(data[i + 1]) : 0, b2 = i + 2 < data.size() ? static_cast<uint8_t>(data[i + 2]) : 0;
      const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
      out += alphabet[(v >> 18) & 63];
      out += alphabet[(v >> 12) & 63];
      out += i + 1 < data.size() ? alphabet[(v >> 6) & 63] : '=';
      out += i + 2 < data.size() ? alphabet[v & 63] : '=';
   }
   return out;
}

string ReadFile(const std::filesystem::path& path)
{
   std::ifstream in(path, std::ios::binary);
   return string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool SendAll(const int fd, const string& data)
{
   for (size_t sent = 0; sent < data.size();)
   {
      const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0)
         return false;
      sent += n;
   }
   return true;
}

bool SendHttp(const int fd, const int code, const char* const reason, const string& body, const string& extraHeaders = string())
{
   return SendAll(fd, "HTTP/1.1 " + std::to_string(code) + ' ' + reason + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extraHeaders + "\r\n" + body);
}

bool SameToken(const string& a, const string& b)
{
   if (a.size() != b.size())
      return false;
   unsigned char diff = 0;
   for (size_t i = 0; i < a.size(); i++)
      diff |= static_cast<unsigned char>(a[i] ^ b[i]);
   return diff == 0;
}

// A wrong type is refused before anything is touched: Load has already reset the part when a reader hits it
// exact: same array sizes and same keys in objects, at any depth (an edit). Otherwise, only what both have is compared (a file, which may leave out defaults)
bool SameShape(const nlohmann::ordered_json& have, const nlohmann::ordered_json& got, const bool exact)
{
   if (have.is_number() && got.is_number())
      return true;
   if (have.type() != got.type())
      return false;
   if (got.is_object())
   {
      if (exact && have.size() != got.size())
         return false;
      for (const auto& [key, value] : got.items())
      {
         if (have.contains(key) ? !SameShape(have[key], value, exact) : exact)
            return false;
      }
   }
   else if (got.is_array())
   {
      if (exact && have.size() != got.size())
         return false;
      if (!have.empty())
         for (size_t i = 0; i < got.size(); i++)
            if (!SameShape(have[std::min(i, have.size() - 1)], got[i], exact))
               return false;
   }
   return true;
}

string Hostname(const string& hostHeader)
{
   return hostHeader.substr(0, hostHeader.rfind(':'));
}
}

EngineMcp* EngineMcp::Get()
{
   static const std::unique_ptr<EngineMcp> instance = []() -> std::unique_ptr<EngineMcp>
   {
      const char* const env = std::getenv("VPX_MCP_PORT");
      if (env == nullptr || env[0] == '\0')
         return nullptr;
      const char* const token = std::getenv("VPX_MCP_TOKEN");
      if (token == nullptr || token[0] == '\0')
      {
         PLOGE << "VPX_MCP_PORT is set without VPX_MCP_TOKEN: MCP server not started";
         return nullptr;
      }
      const int port = atoi(env);
      const int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (port <= 0 || port > 65535 || fd < 0)
      {
         PLOGE << "VPX_MCP_PORT: bad port " << env;
         return nullptr;
      }
      const int yes = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(port));
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // Local only, never any other interface
      if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 8) != 0)
      {
         PLOGE << "VPX_MCP_PORT: can not listen on 127.0.0.1:" << port;
         close(fd);
         return nullptr;
      }
      return std::unique_ptr<EngineMcp>(new EngineMcp(port, fd, token));
   }();
   return instance.get();
}

EngineMcp::EngineMcp(const int port, const int listenSocket, string token)
   : m_port(port)
   , m_listenSocket(listenSocket)
   , m_token(std::move(token))
{
   string workDir = (std::filesystem::temp_directory_path() / "vpx-mcp-XXXXXX").string();
   if (mkdtemp(workDir.data()) == nullptr)
      PLOGE << "MCP: can not create a temporary folder";
   m_workDir = workDir;
   PLOGI << "MCP server listening on http://127.0.0.1:" << port << "/mcp";
   m_thread = std::thread(&EngineMcp::ListenLoop, this);
}

EngineMcp::~EngineMcp()
{
   m_stop = true;
   if (m_thread.joinable())
      m_thread.join();
   while (m_openConnections > 0) // They see m_stop within a poll
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
   close(m_listenSocket);
   std::error_code ec;
   std::filesystem::remove_all(m_workDir, ec);
}

void EngineMcp::ListenLoop()
{
   SetThreadName("EngineMcp"s);
   while (!m_stop)
   {
      pollfd pfd { m_listenSocket, POLLIN, 0 };
      if (poll(&pfd, 1, 200) <= 0)
         continue;
      const int client = accept(m_listenSocket, nullptr, nullptr);
      if (client < 0)
         continue;
      #ifdef __APPLE__
      const int yes = 1;
      setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
      #endif
      if (m_openConnections >= 16)
      {
         close(client);
         continue;
      }
      m_openConnections++;
      std::thread([this, client]()
         {
            try
            {
               ServeConnection(client);
            }
            catch (const std::exception& e)
            {
               PLOGE << "MCP connection: " << e.what();
            }
            close(client);
            m_openConnections--;
         }).detach();
   }
}

void EngineMcp::ServeConnection(const int client)
{
   string buffer;
   char chunk[16384];
   constexpr size_t kMaxRequest = 16 * 1024 * 1024;
   int idleMs = 0;
   while (!m_stop)
   {
      size_t headerEnd = buffer.find("\r\n\r\n");
      if (headerEnd != string::npos)
      {
         std::istringstream head(buffer.substr(0, headerEnd));
         string method, target, line;
         head >> method >> target;
         std::getline(head, line);
         std::map<string, string> headers;
         while (std::getline(head, line))
         {
            const size_t colon = line.find(':');
            if (colon == string::npos)
               continue;
            string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r") + 1);
            headers[lowerCase(line.substr(0, colon))] = value;
         }
         if (headers.contains("transfer-encoding"))
         {
            SendHttp(client, 411, "Length Required", "{\"error\":\"Content-Length required\"}", "Connection: close\r\n");
            return;
         }
         const size_t length = headers.contains("content-length") ? strtoull(headers["content-length"].c_str(), nullptr, 10) : 0;
         if (length > kMaxRequest)
         {
            SendHttp(client, 413, "Payload Too Large", "{\"error\":\"request too large\"}", "Connection: close\r\n");
            return;
         }
         if (buffer.size() >= headerEnd + 4 + length)
         {
            const string body = buffer.substr(headerEnd + 4, length);
            buffer.erase(0, headerEnd + 4 + length);
            if (!HandleHttp(client, method, target.substr(0, target.find('?')), headers, body))
               return;
            idleMs = 0;
            continue;
         }
      }
      else if (buffer.size() > 64 * 1024)
         return;
      pollfd pfd { client, POLLIN, 0 };
      if (poll(&pfd, 1, 200) <= 0)
      {
         idleMs += 200;
         if (idleMs > 10 * 60 * 1000)
            return;
         continue;
      }
      const ssize_t got = recv(client, chunk, sizeof(chunk), 0);
      if (got <= 0)
         return;
      buffer.append(chunk, got);
      idleMs = 0;
   }
}

bool EngineMcp::HandleHttp(const int client, const string& method, const string& path, const std::map<string, string>& headers, const string& body)
{
   if (path != "/mcp" && path != "/health")
      return SendHttp(client, 404, "Not Found", "{}");
   // A web page reaching localhost: the Host of a rebound name, or a foreign Origin
   const auto header = [&headers](const char* const name) { const auto it = headers.find(name); return it == headers.end() ? string() : it->second; };
   const string host = Hostname(header("host")), origin = header("origin");
   const size_t originStart = origin.find("://");
   const string originHost = originStart == string::npos ? origin : Hostname(origin.substr(originStart + 3));
   if ((host != "127.0.0.1" && host != "localhost") || (!origin.empty() && originHost != "127.0.0.1" && originHost != "localhost"))
      return SendHttp(client, 403, "Forbidden", "{\"error\":\"local clients only\"}");
   if (!SameToken(header("authorization"), "Bearer " + m_token))
   {
      PLOGW << "MCP: rejected a request with " << (headers.contains("authorization") ? "a wrong" : "no") << " access token";
      return SendHttp(client, 401, "Unauthorized", "{\"error\":\"unauthorized\",\"hint\":\"Send Authorization: Bearer <VPX_MCP_TOKEN>\"}", "WWW-Authenticate: Bearer\r\n");
   }
   if (path == "/health")
      return SendHttp(client, 200, "OK", "{\"ok\":true,\"name\":\"vpx-engine\"}");
   if (method == "DELETE") // End of session: there is no session state
      return SendHttp(client, 200, "OK", "{}");
   if (method != "POST")
      return SendHttp(client, 405, "Method Not Allowed", "{}", "Allow: POST, DELETE\r\n"); // No server initiated stream
   json rpc;
   try
   {
      rpc = json::parse(body);
   }
   catch (const std::exception&)
   {
      return SendHttp(client, 400, "Bad Request", "{\"error\":\"invalid JSON\"}");
   }
   if (!rpc.is_object())
      return SendHttp(client, 400, "Bad Request", "{\"error\":\"one JSON-RPC message per request\"}");
   bool isNotification = false;
   string sessionId;
   json reply = HandleRpc(rpc, isNotification, sessionId);
   if (isNotification)
      return SendAll(client, "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n");
   return SendHttp(client, 200, "OK", reply.dump(-1, ' ', false, json::error_handler_t::replace), sessionId.empty() ? string() : "Mcp-Session-Id: " + sessionId + "\r\n");
}

EngineMcp::json EngineMcp::HandleRpc(const json& rpc, bool& isNotification, string& sessionId)
{
   isNotification = !rpc.contains("id");
   if (isNotification)
      return nullptr;
   json reply = { { "jsonrpc", "2.0" }, { "id", rpc["id"] } };
   try
   {
      if (!rpc.contains("method") || !rpc["method"].is_string())
         throw std::runtime_error("method must be a string");
      const string method = rpc["method"].get<string>();
      if (method == "initialize")
      {
         const string asked = rpc.contains("params") ? rpc["params"].value("protocolVersion", ""s) : ""s;
         std::mt19937_64 random { std::random_device {}() };
         sessionId = std::to_string(random());
         reply["result"] = { { "protocolVersion", asked == "2025-03-26" || asked == "2025-06-18" ? asked : string(kProtocolVersion) }, { "capabilities", { { "tools", json::object() } } },
            { "serverInfo", { { "name", "vpx-engine" }, { "version", "0.1.0" } } } };
      }
      else if (method == "ping")
         reply["result"] = json::object();
      else if (method == "tools/list")
      {
         static const json tools = json::parse(kTools);
         reply["result"] = { { "tools", tools } };
      }
      else if (method == "tools/call")
         reply["result"] = CallTool(rpc.at("params").at("name").get<string>(), rpc["params"].contains("arguments") ? rpc["params"]["arguments"] : json::object());
      else
         reply["error"] = { { "code", -32601 }, { "message", "Method not found: " + method } };
   }
   catch (const std::exception& e)
   {
      reply["error"] = { { "code", -32602 }, { "message", e.what() } };
   }
   return reply;
}

EngineMcp::json EngineMcp::CallTool(const string& name, const json& args)
{
   auto call = std::make_shared<Call>();
   call->tool = name;
   call->args = args;
   std::future<ToolReply> future = call->reply.get_future();
   {
      std::lock_guard lock(m_queueMutex);
      m_queue.push_back(call);
   }
   // A long wait is a table being loaded or a stalled frame, not an error of the call
   while (future.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready)
      if (m_stop)
         return { { "content", json::array({ { { "type", "text" }, { "text", "The player closed" } } }) }, { "isError", true } };
   ToolReply reply = future.get();
   if (!reply.error.empty())
      return { { "content", json::array({ { { "type", "text" }, { "text", reply.error } } }) }, { "isError", true } };
   json content = json::array();
   if (!reply.imageFile.empty())
      return FinishView(reply);
   content.push_back({ { "type", "text" }, { "text", reply.result.dump(1, ' ', false, json::error_handler_t::replace) } });
   return { { "content", content }, { "structuredContent", reply.result } };
}

void EngineMcp::Update(Player* player)
{
   const uint32_t newFrame = player->m_renderer->m_renderDevice->GetFrameIndex();
   const bool newSession = player != m_player || newFrame < m_lastSeenFrame; // A new Player may land at the address of the previous one
   m_lastSeenFrame = newFrame;
   if (newSession)
   {
      // A new session (another table): what waits on the frames of the previous one will not happen, what was held belongs to it
      const auto fail = [](const std::shared_ptr<Call>& call)
      {
         ToolReply reply;
         reply.error = "The table changed";
         call->reply.set_value(std::move(reply));
      };
      for (const FrameWait& wait : m_frameWaits)
         fail(wait.call);
      for (const SettlingView& view : m_settling)
         fail(view.call);
      m_frameWaits.clear();
      m_settling.clear();
      m_held.clear();
      m_lastEditFrame = 0;
      m_player = player;
   }
   vector<std::shared_ptr<Call>> queue;
   {
      std::lock_guard lock(m_queueMutex);
      queue.swap(m_queue);
   }
   for (const auto& call : queue)
   {
      try
      {
         Handle(player, call);
      }
      catch (const std::exception& e)
      {
         ToolReply reply;
         reply.error = e.what();
         call->reply.set_value(std::move(reply));
      }
   }
   const uint32_t frame = player->m_renderer->m_renderDevice->GetFrameIndex();
   std::erase_if(m_frameWaits, [frame](const FrameWait& wait)
      {
         if (frame < wait.frame)
            return false;
         ToolReply reply;
         reply.result = { { "frame", frame } };
         wait.call->reply.set_value(std::move(reply));
         return true;
      });
   UpdateSettling(player);
}

void EngineMcp::Handle(Player* player, const std::shared_ptr<Call>& call)
{
   const string& name = call->tool;
   const json& args = call->args;
   if (name == "vpx_view")
      return View(player, call);
   bool deferred = false;
   ToolReply reply;
   if (name == "vpx_guide") reply.result = Guide(player);
   else if (name == "vpx_table") reply.result = Table(player, args);
   else if (name == "vpx_part") reply.result = Part(player, args);
   else if (name == "vpx_material") reply.result = MaterialTool(player, args);
   else if (name == "vpx_history") reply.result = History(player, args);
   else if (name == "vpx_engine_status") reply.result = Status(player);
   else if (name == "vpx_engine_camera") reply.result = Camera(player, args);
   else if (name == "vpx_engine_lights") reply.result = Lights(player, args);
   else if (name == "vpx_engine_run") reply.result = Run(player, call, deferred);
   else if (name == "vpx_engine_timing") reply.result = Timing(args);
   else if (name == "vpx_engine_reload") reply.result = Reload(player, args);
   else if (name == "vpx_engine_shaders")
   {
      ShaderHotReload* const hotReload = ShaderHotReload::Get();
      if (hotReload == nullptr)
         throw std::runtime_error("Shader hot reload is off: start the player with VPX_SHADER_HOTRELOAD=<shader folder>");
      if (args.at("action").get<string>() == "reload")
         hotReload->RequestReload(args.at("file").get<string>());
      reply.result = hotReload->Status();
   }
   else if (name == "vpx_engine_quit")
      player->SetCloseState(Player::CS_CLOSE_APP);
   else
      throw std::runtime_error("Unknown tool: " + name);
   if (!deferred)
      call->reply.set_value(std::move(reply));
}

EngineMcp::json EngineMcp::Guide(Player* player) const
{
   return {
      { "whatThisIs", "The running VPX engine: edits land on the live table and show in the next frame, nothing is written to disk. Nothing here saves: the pack on disk stays as it was." },
      { "youAreHere", Status(player) },
      { "recommendedNext", "vpx_table(action:\"overview\"), then vpx_view() to look." },
      { "fieldNames", "Part and material fields are the keys of the pack files (parts/*.json, materials/*.json). vpx_table(action:\"part\") shows them." },
      { "undo", "In an edit session (-LiveEdit) each edit is one step of the editor's undo history (vpx_history). In play or inspection edits go to the live copy of the table, without undo." },
      { "engineOnly", "vpx_engine_camera, vpx_engine_lights, vpx_engine_run, vpx_engine_timing, vpx_engine_shaders, vpx_engine_reload, vpx_engine_quit." },
      { "workflow", "Change, vpx_view(path:...), compare. Shader files in the watched folder reload by themselves; vpx_engine_shaders(action:\"status\") tells the last compile error." } };
}

EngineMcp::json EngineMcp::Status(Player* player) const
{
   const auto& editor = player->m_liveUI->m_editorUI;
   const char* const mode = player->m_playMode == Player::PlayMode::CaptureAttract ? "capture" : !editor.IsOpened() ? "play" : editor.IsInspectMode() ? "inspect" : "edit";
   json status = { { "table", player->m_ptable->m_tableName }, { "path", player->m_ptable->m_filename.string() }, { "mode", mode },
      { "frame", player->m_renderer->m_renderDevice->GetFrameIndex() }, { "playing", player->IsPlaying(false) }, { "undo_available", editor.IsOpened() && !editor.IsInspectMode() && editor.m_undo.HasUndo() },
      { "held_lights", m_held.size() } };
   if (const ShaderHotReload* const hotReload = ShaderHotReload::Get())
      status["shaders"] = hotReload->Status();
   return status;
}

EngineMcp::json EngineMcp::Table(Player* player, const json& args) const
{
   const string action = args.at("action").get<string>();
   PinTable* const table = player->m_ptable;
   if (action == "overview")
   {
      std::map<string, int> counts;
      for (IEditable* const part : table->GetParts())
         if (const char* const type = JSONSerializer::GetPartTypeName(part->GetItemType()))
            counts[type]++;
      return { { "name", table->m_tableName }, { "path", table->m_filename.string() }, { "dimensions", { { "left", table->m_left }, { "top", table->m_top }, { "right", table->m_right }, { "bottom", table->m_bottom } } },
         { "counts", counts }, { "materials", table->m_materials.size() } };
   }
   if (action == "part")
   {
      const Target target = FindPart(player, args.at("name").get<string>());
      return { { "name", target.part->GetName() }, { "type", JSONSerializer::GetPartTypeName(target.part->GetItemType()) }, { "data", SaveDoc(player, target) } };
   }
   if (action != "parts")
      throw std::runtime_error("Unknown action: " + action);
   const string type = lowerCase(args.value("type", ""s));
   const string regexText = args.value("nameRegex", ""s);
   const std::regex regex(regexText, std::regex::icase | std::regex::ECMAScript);
   const size_t limit = args.value("limit", 200u);
   json parts = json::array();
   size_t total = 0;
   for (IEditable* const part : table->GetParts())
   {
      const char* const typeName = JSONSerializer::GetPartTypeName(part->GetItemType());
      if (typeName == nullptr || part->GetName().empty() || (!type.empty() && type != lowerCase(typeName)) || (!regexText.empty() && !std::regex_search(part->GetName(), regex)))
         continue;
      if (++total <= limit)
         parts.push_back({ { "name", part->GetName() }, { "type", typeName } });
   }
   return { { "count", total }, { "parts", parts } };
}

EngineMcp::Target EngineMcp::FindMaterial(const Player* player, const string& name)
{
   Target target;
   for (Material* const material : player->m_ptable->m_materials)
      if (StrCompareNoCase(material->m_name, name))
      {
         target.material = material;
         target.name = material->m_name;
      }
   if (target.material == nullptr)
      throw std::runtime_error("Material not found: " + name);
   return target;
}

EngineMcp::Target EngineMcp::FindPart(const Player* player, const string& name)
{
   Target target;
   for (IEditable* const part : player->m_ptable->GetParts())
      if (StrCompareNoCase(part->GetName(), name))
      {
         target.part = part;
         target.name = part->GetName();
      }
   if (target.part == nullptr)
      throw std::runtime_error("Part not found: " + name);
   return target;
}

// As saved by the undo records for a part (no mesh: Load leaves it alone), as in the pack for a material
EngineMcp::json EngineMcp::SaveDoc(const Player* player, const Target& target)
{
   if (target.material != nullptr)
   {
      JSONObjectWriter writer(eItemTable, nullptr);
      target.material->Save(writer, false);
      if (writer.HasError())
         throw std::runtime_error("could not read the material");
      const json& fields = writer.Json();
      return fields.contains("materials") ? fields["materials"][0] : fields;
   }
   JSONObjectWriter writer(target.part->GetItemType(), nullptr);
   player->m_liveUI->m_editorUI.m_undo.SyncPartBeforeSave(target.part);
   target.part->Save(writer, true);
   for (const auto& uiPart : player->m_liveUI->m_editorUI.m_editables) // Back to what the editor displays, the save took the authored visibility
      if (uiPart->GetEditable() == target.part)
         uiPart->ApplyDisplayVisibility();
   if (writer.HasError())
      throw std::runtime_error("could not read the part");
   return writer.Json();
}

// Editor session: an undo step like an edit in a pane. Otherwise the change is on the live copy of the table, there is no undo
bool EngineMcp::ApplyDoc(Player* player, const Target& target, const json& doc, const string& label, const JSONSerializer::Deserializer* pack)
{
   auto& editor = player->m_liveUI->m_editorUI;
   const bool withUndo = editor.IsOpened() && !editor.IsInspectMode();
   if (editor.IsOpened() && (editor.IsDragInProgress() || editor.m_fileOp != nullptr))
      throw std::runtime_error("busy: a drag or a file operation is running in the editor");
   if (target.material != nullptr)
   {
      Material edited(*target.material);
      JSONObjectReader reader(doc, JSONSerializer::kMaterialNode, pack, CURRENT_FILE_FORMAT_VERSION);
      edited.Load(reader);
      if (reader.HasError())
         throw std::runtime_error("the material could not be read back, nothing changed");
      if (withUndo)
         VPX::EditorUI::MaterialEditUndo::Record(editor.m_undo, player->m_ptable, target.material, std::make_shared<Material>(*target.material), label);
      *target.material = edited;
   }
   else
   {
      if (withUndo)
      {
         editor.m_undo.BeginUndo();
         editor.m_undo.MarkForUndo(target.part);
         editor.m_undo.SetFieldLabel(label);
         editor.m_undo.EndUndo();
      }
      const json before = withUndo ? json() : SaveDoc(player, target);
      JSONObjectReader reader(doc, target.part->GetItemType(), pack, CURRENT_FILE_FORMAT_VERSION);
      try
      {
         editor.m_undo.OverwritePart(target.part, reader);
         if (reader.HasError())
            throw std::runtime_error("the part could not be read back");
      }
      catch (const std::exception&)
      {
         // It was already reset by Load
         if (withUndo)
            editor.m_undo.Undo(false);
         else
         {
            JSONObjectReader restore(before, target.part->GetItemType(), nullptr, CURRENT_FILE_FORMAT_VERSION);
            editor.m_undo.OverwritePart(target.part, restore);
         }
         throw;
      }
   }
   if (withUndo)
   {
      editor.m_lastUndoPart = nullptr; // The next pane edit is a step of its own
      editor.m_lastUndoId = 0;
   }
   player->m_renderer->MarkStaticPrepassDirty();
   if (target.part != nullptr)
      player->m_renderer->InvalidateShadowContent(); // The undo record marks the table dirty, which does the same in an edit session
   m_lastEditFrame = player->m_renderer->m_renderDevice->GetFrameIndex();
   return withUndo;
}

// The answer of an edit, as the VPX Editor's: applied, description, preview, nextStep
EngineMcp::json EngineMcp::Patch(Player* player, const Target& target, const json& patch, const bool apply, const string& description)
{
   if (!patch.is_object() || patch.empty())
      throw std::runtime_error("no fields to change");
   json doc = SaveDoc(player, target);
   json changes = json::object();
   string label;
   for (const auto& [key, value] : patch.items())
   {
      if (key == "name" || key[0] == '$')
         throw std::runtime_error("'" + key + "' can not be changed here");
      if (!doc.contains(key))
         throw std::runtime_error("No field '" + key + "' on '" + target.name + "'. Field names are the pack file keys, see vpx_table action=part or vpx_material action=list");
      json merged = doc[key];
      if (value.is_object() && merged.is_object())
         merged.update(value); // Only the given fields of a vector
      else
         merged = value;
      if (!SameShape(doc[key], merged, true))
         throw std::runtime_error("'" + key + "' is " + doc[key].dump() + ", got " + value.dump());
      changes[key] = { { "from", doc[key] }, { "to", merged } };
      doc[key] = std::move(merged);
      label += (label.empty() ? "" : ", ") + key;
   }
   json reply = { { "applied", apply }, { "description", description }, { "preview", apply ? json(nullptr) : changes } };
   if (apply)
   {
      reply["changes"] = changes;
      reply["undo"] = ApplyDoc(player, target, doc, label, nullptr);
      reply["playing"] = player->IsPlaying(false); // Physics pauses the game for a changed part
   }
   reply["nextStep"] = apply ? "Applied." : "Call again to apply (confirm:true, or without preview).";
   return reply;
}

EngineMcp::json EngineMcp::Part(Player* player, const json& args)
{
   if (args.at("action").get<string>() != "modify")
      throw std::runtime_error("Only action=modify in this server: the running engine edits existing parts");
   json fields = args.at("part");
   const Target target = FindPart(player, fields.at("partName").get<string>());
   if (fields.contains("type") && !StrCompareNoCase(fields["type"].get<string>(), JSONSerializer::GetPartTypeName(target.part->GetItemType())))
      throw std::runtime_error("'" + target.name + "' is a " + JSONSerializer::GetPartTypeName(target.part->GetItemType()));
   fields.erase("partName");
   fields.erase("type");
   if (fields.contains("more"))
   {
      const json more = std::move(fields["more"]);
      fields.erase("more");
      fields.update(more);
   }
   return Patch(player, target, fields, !args.value("preview", false), "Modify part \"" + target.name + '"');
}

EngineMcp::json EngineMcp::MaterialTool(Player* player, const json& args)
{
   if (args.at("action").get<string>() == "list")
   {
      json materials = json::array();
      for (Material* const material : player->m_ptable->m_materials)
      {
         json doc = { { "name", material->m_name } };
         Target target;
         target.material = material;
         doc.update(SaveDoc(player, target));
         materials.push_back(doc);
      }
      return { { "count", materials.size() }, { "materials", materials } };
   }
   const Target target = FindMaterial(player, args.at("name").get<string>());
   return Patch(player, target, args.at("patch"), args.value("confirm", false), "Modify material \"" + target.name + '"');
}

EngineMcp::json EngineMcp::History(Player* player, const json& args)
{
   const bool undo = args.at("action").get<string>() == "undo";
   auto& editor = player->m_liveUI->m_editorUI;
   if (!editor.IsOpened() || editor.IsInspectMode())
      throw std::runtime_error("Undo exists only in an edit session (-LiveEdit)");
   if (undo ? !editor.CanUndo() : !editor.CanRedo())
      throw std::runtime_error(undo ? "Nothing to undo" : "Nothing to redo");
   const string label = undo ? editor.m_undo.UndoLabel(0) : editor.m_undo.RedoLabel(0);
   if (undo)
      editor.UndoSteps(1);
   else
      editor.RedoSteps(1);
   player->m_renderer->MarkStaticPrepassDirty();
   player->m_renderer->InvalidateShadowContent();
   m_lastEditFrame = player->m_renderer->m_renderDevice->GetFrameIndex();
   return { { "applied", true }, { "description", (undo ? "Undo: " : "Redo: ") + label } };
}

EngineMcp::json EngineMcp::Camera(Player* player, const json& args)
{
   auto& editor = player->m_liveUI->m_editorUI;
   if (args.value("release", false))
   {
      if (editor.IsOpened())
         editor.SetLookAtCamera(vec3(0.f, 0.f, 0.f), vec3(0.f, 0.f, 0.f), 0.f, Renderer::ShadeMode::Default);
      else
         player->m_renderer->InitLayout();
      return { { "released", true } };
   }
   const auto eye = args.at("eye").get<std::array<float, 3>>();
   const auto target = args.at("target").get<std::array<float, 3>>();
   const float fov = args.value("fov", 39.6f);
   if (fov < 1.f || fov > 170.f)
      throw std::runtime_error("fov out of range");
   if (player->m_renderer->m_stereo3D == STEREO_VR)
      throw std::runtime_error("No scripted camera in VR");
   if (editor.IsOpened())
   {
      const string shading = args.value("shading", "pbr"s);
      if (shading != "pbr" && shading != "solid" && shading != "wireframe")
         throw std::runtime_error("shading is one of pbr, solid, wireframe");
      editor.SetLookAtCamera(vec3(eye[0], eye[1], eye[2]), vec3(target[0], target[1], target[2]), fov,
         shading == "wireframe" ? Renderer::ShadeMode::Wireframe : shading == "solid" ? Renderer::ShadeMode::Solid : Renderer::ShadeMode::Default);
   }
   else
      player->m_renderer->SetLookAtCamera(Vertex3Ds(eye[0], eye[1], eye[2]), Vertex3Ds(target[0], target[1], target[2]), fov);
   return { { "eye", eye }, { "target", target }, { "fov", fov } };
}

EngineMcp::json EngineMcp::Lights(Player* player, const json& args)
{
   const Target target = FindPart(player, args.at("name").get<string>());
   const ItemTypeEnum type = target.part->GetItemType();
   if (type != eItemLight && type != eItemFlasher)
      throw std::runtime_error("'" + target.name + "' is not a light or a flasher");
   const string key = target.part->GetName();
   const auto it = m_held.find(key);
   if (!args.contains("level") || args["level"].is_null()) // Released: back to what it was, script and ROM take over at their next change
   {
      if (it != m_held.end())
      {
         if (type == eItemLight)
            static_cast<Light*>(target.part)->m_currentIntensity = it->second.restore;
         else
            static_cast<Flasher*>(target.part)->m_d.m_intensity_scale = it->second.restore;
         m_held.erase(it);
      }
      return { { "held", false } };
   }
   const float level = args["level"].get<float>();
   if (level < 0.f)
      throw std::runtime_error("level must be >= 0");
   const float restore = it != m_held.end() ? it->second.restore
      : type == eItemLight ? static_cast<Light*>(target.part)->m_currentIntensity : static_cast<Flasher*>(target.part)->m_d.m_intensity_scale;
   m_held[key] = { level, restore };
   return { { "held", true }, { "level", level } };
}

void EngineMcp::ApplyHeldLights(PinTable* table)
{
   if (m_held.empty())
      return;
   for (IEditable* const part : table->GetParts())
   {
      const auto it = m_held.find(part->GetName());
      if (it == m_held.end())
         continue;
      if (part->GetItemType() == eItemLight)
      {
         Light* const light = static_cast<Light*>(part);
         light->m_currentIntensity = it->second.level * light->m_d.m_intensity * light->m_d.m_intensity_scale;
      }
      else if (part->GetItemType() == eItemFlasher)
         static_cast<Flasher*>(part)->m_d.m_intensity_scale = it->second.level;
   }
}

EngineMcp::json EngineMcp::Run(Player* player, const std::shared_ptr<Call>& call, bool& deferred)
{
   const string action = call->args.at("action").get<string>();
   if (action == "wait_frames")
   {
      m_frameWaits.push_back({ call, player->m_renderer->m_renderDevice->GetFrameIndex() + call->args.value("n", 1u) });
      deferred = true;
      return json::object();
   }
   if (player->IsEditorMode())
      throw std::runtime_error("No game time in an edit session: Play first, or start with -Play");
   if (action == "pause" || action == "resume")
      player->SetPlayState(action == "resume");
   else if (action == "step")
   {
      if (const int ms = call->args.value("ms", 0); ms > 0)
         player->SetPlayState(true, ms);
      else
         player->m_step = true;
   }
   else
      throw std::runtime_error("Unknown action: " + action);
   return { { "playing", player->IsPlaying(false) } };
}

void EngineMcp::View(Player* player, const std::shared_ptr<Call>& call)
{
   const json& args = call->args;
   if (args.value("view", "3d"s) != "3d")
      throw std::runtime_error("Only view=3d in the running engine");
   string target;
   if (args.contains("path"))
   {
      target = args["path"].get<string>();
      const std::filesystem::path path = PathFromUTF8(target);
      const string extension = lowerCase(path.extension().string());
      if (!path.is_absolute() || (extension != ".png" && extension != ".jpg"))
         throw std::runtime_error("path must be absolute and end in .png or .jpg");
      std::filesystem::create_directories(path.parent_path());
   }
   static std::atomic<int> counter { 0 };
   const std::filesystem::path capture = m_workDir / ("view-" + std::to_string(counter++) + ".png");
   ToolReply base;
   base.imageFile = PathToUTF8(capture);
   base.imageTarget = target;
   base.imageMaxWidth = args.value("max_width", 1024);
   base.imageCaption = player->m_ptable->m_tableName + ", 3D view";
   const auto& editor = player->m_liveUI->m_editorUI;
   base.imageCropped = editor.IsOpened() && args.value("area", "view"s) == "view" && editor.GetViewportFraction(base.imageCrop);
   base.imageSettled = true;
   if (args.value("settle", editor.IsOpened())) // In play, moving parts render shadows all the time
      m_settling.push_back({ call, base, usec(), g_shadowWork.load(), player->m_renderer->m_renderDevice->GetFrameIndex(), 0 });
   else
      StartCapture(player, call, base);
}

// Shadow pages and cached shadows are rebuilt over several frames after an edit, with wrong light on the way: an image must not catch that
void EngineMcp::UpdateSettling(Player* player)
{
   const uint64_t work = g_shadowWork.load(), now = usec();
   const int pending = g_shadowBakesPending.load();
   const uint32_t frame = player->m_renderer->m_renderDevice->GetFrameIndex();
   const ShaderHotReload* const hotReload = ShaderHotReload::Get();
   const uint32_t swapFrame = hotReload ? hotReload->GetShadowRestartFrame() : 0u;
   const uint32_t restart = max(m_lastEditFrame, swapFrame <= frame ? swapFrame : 0u); // Later than now: from a previous session
   std::erase_if(m_settling, [&](SettlingView& view)
      {
         if (frame != view.lastFrame)
         {
            view.quietFrames = work == view.lastWork && pending == 0 ? view.quietFrames + 1 : 0;
            view.lastWork = work;
            view.lastFrame = frame;
         }
         const bool timedOut = now - view.start > 5000000;
         // The shadows of the table restart from the frame after an edit, and take about 40 frames on Police Force (measured)
         if ((view.quietFrames < 5 || frame < restart + 60) && !timedOut)
            return false;
         view.reply.imageSettled = !timedOut;
         StartCapture(player, view.call, view.reply);
         return true;
      });
}

void EngineMcp::StartCapture(Player* player, const std::shared_ptr<Call>& call, const ToolReply& base)
{
   player->m_renderer->m_renderDevice->CaptureScreenshot({ player->m_playfieldWnd }, { PathFromUTF8(base.imageFile) },
      [call, base](const bool success)
      {
         ToolReply reply = base;
         if (!success)
         {
            reply.imageFile.clear();
            reply.error = "The capture failed, or another one is in progress";
         }
         call->reply.set_value(std::move(reply));
      }, 2); // Two frames from now: what was edited before this call is in the picture
}

// On a connection's thread and not on the logic or render thread: decoding and resizing take a while
EngineMcp::json EngineMcp::FinishView(const ToolReply& reply)
{
   const auto failed = [](const string& text) { return json { { "content", json::array({ { { "type", "text" }, { "text", text } } }) }, { "isError", true } }; };
   const std::filesystem::path capture = PathFromUTF8(reply.imageFile), scaled = capture.parent_path() / (capture.stem().string() + "_scaled.png");
   const auto cleanup = [&]()
   {
      std::error_code ec;
      std::filesystem::remove(capture, ec);
      std::filesystem::remove(scaled, ec);
   };
   std::shared_ptr<BaseTexture> image = BaseTexture::CreateFromFile(capture);
   if (image == nullptr)
   {
      cleanup();
      return failed("Could not read back the capture");
   }
   if (reply.imageCropped)
   {
      const unsigned int imageW = image->width(), imageH = image->height();
      const unsigned int x = std::min(static_cast<unsigned int>(reply.imageCrop[0] * static_cast<float>(imageW)), imageW - 1), y = std::min(static_cast<unsigned int>(reply.imageCrop[1] * static_cast<float>(imageH)), imageH - 1);
      const unsigned int w = std::clamp(static_cast<unsigned int>(reply.imageCrop[2] * static_cast<float>(imageW)), 1u, imageW - x), h = std::clamp(static_cast<unsigned int>(reply.imageCrop[3] * static_cast<float>(imageH)), 1u, imageH - y);
      const std::shared_ptr<BaseTexture> cropped = BaseTexture::Create(w, h, image->m_format);
      const unsigned int bytes = image->pitch() / imageW;
      for (unsigned int row = 0; row < h; row++)
         memcpy(static_cast<uint8_t*>(cropped->data()) + row * cropped->pitch(), static_cast<const uint8_t*>(image->data()) + (y + row) * image->pitch() + x * bytes, static_cast<size_t>(w) * bytes);
      image = cropped;
   }
   if (!reply.imageTarget.empty())
   {
      const bool saved = image->Save(PathFromUTF8(reply.imageTarget));
      const unsigned int w = image->width(), h = image->height();
      cleanup();
      if (!saved)
         return failed("Could not save " + reply.imageTarget);
      const json result = { { "path", reply.imageTarget }, { "width", w }, { "height", h }, { "settled", reply.imageSettled } };
      return { { "content", json::array({ { { "type", "text" }, { "text", result.dump() } } }) }, { "structuredContent", result } };
   }
   json content = json::array();
   if (image->Save(scaled))
   {
      // The argument limits the larger side
      const unsigned int largestSide = image->height() > image->width() ? reply.imageMaxWidth * image->height() / image->width() : reply.imageMaxWidth;
      if (const auto resized = BaseTexture::CreateFromFile(scaled, largestSide); resized != nullptr && resized->Save(scaled))
      {
         content.push_back({ { "type", "text" }, { "text", reply.imageCaption + ", " + std::to_string(resized->width()) + "x" + std::to_string(resized->height()) + "px" } });
         content.push_back({ { "type", "image" }, { "data", Base64(ReadFile(scaled)) }, { "mimeType", "image/png" } });
      }
   }
   cleanup();
   return content.empty() ? failed("Could not read back the capture") : json { { "content", content } };
}

EngineMcp::json EngineMcp::Reload(Player* player, const json& args)
{
   const Target target = args.contains("material") ? FindMaterial(player, args["material"].get<string>()) : FindPart(player, args.at("part").get<string>());
   const std::filesystem::path packPath = player->m_ptable->m_liveBaseTable ? player->m_ptable->m_liveBaseTable->m_filename : player->m_ptable->m_filename;
   if (!JSONSerializer::IsPack(packPath))
      throw std::runtime_error("The table is not a pack: " + packPath.string());
   const auto pack = JSONSerializer::CreateReader(packPath);
   if (pack == nullptr)
      throw std::runtime_error("Can not open the pack " + packPath.string());
   // Files are named after the part, the 'name' field (if any) wins over it for a name that was not file safe
   const string folder = target.material ? "materials" : "parts";
   std::filesystem::path file = std::filesystem::path(folder) / (JSONSerializer::SanitizeFileName(target.name) + ".json");
   json doc;
   string content;
   if (!pack->ReadTextFile(file, content))
   {
      for (const std::filesystem::path& candidate : pack->ListFiles())
         if (candidate.parent_path() == folder && candidate.extension() == ".json" && pack->ReadTextFile(candidate, content))
         {
            const json parsed = json::parse(content);
            if (StrCompareNoCase(parsed.value("name", ""s), target.name))
            {
               file = candidate;
               doc = parsed;
               break;
            }
         }
      if (doc.is_null())
         throw std::runtime_error("No file for '" + target.name + "' in " + folder);
   }
   else
      doc = json::parse(content);
   doc["name"] = doc.value("name", target.name);
   // Reader errors of a field do not reach the part's reader: a wrong type is refused here, as for an edit
   const json current = SaveDoc(player, target);
   for (const auto& [key, value] : doc.items())
      if (current.contains(key) && !SameShape(current[key], value, false))
         throw std::runtime_error("'" + key + "' in " + file.string() + " is " + value.dump() + ", expected like " + current[key].dump());
   if (target.part != nullptr && doc.value("$type", ""s) != JSONSerializer::GetPartTypeName(target.part->GetItemType()))
      throw std::runtime_error("The file is of another part type: " + doc.value("$type", ""s));
   const bool undo = ApplyDoc(player, target, doc, "Reload", pack.get());
   json reply = { { "applied", true }, { "name", target.name }, { "file", file.string() }, { "undo", undo }, { "playing", player->IsPlaying(false) } };
   // Meshes are not part of undo records: on request only, and not undoable
   if (args.value("mesh", false) && target.part != nullptr && target.part->GetItemType() == eItemPrimitive && static_cast<Primitive*>(target.part)->m_d.m_use3DMesh)
   {
      Primitive* const prim = static_cast<Primitive*>(target.part);
      std::filesystem::path meshFile = PathFromUTF8(doc.value("mesh", ""s));
      if (meshFile.empty())
         meshFile = std::filesystem::path("meshes") / (JSONSerializer::SanitizeFileName(target.name) + ".glb");
      vector<uint8_t> data;
      if (!pack->ReadBinaryFile(meshFile, data) || !prim->m_mesh.LoadGLB(data.data(), data.size()))
         throw std::runtime_error("Could not load the mesh " + meshFile.string());
      player->m_renderer->ReinitRenderable(prim);
      player->m_physics->Update(prim);
      reply["mesh"] = meshFile.string();
      reply["mesh_undo"] = false;
   }
   return reply;
}

void EngineMcp::OnFramePresented(const uint64_t timestampUs, const uint64_t gpuUs)
{
   std::lock_guard lock(m_framesMutex);
   if (m_lastPresent != 0)
   {
      const std::pair<uint32_t, uint32_t> sample { static_cast<uint32_t>(timestampUs - m_lastPresent), static_cast<uint32_t>(gpuUs) };
      if (m_frames.size() < kFrames)
         m_frames.push_back(sample);
      else
         m_frames[m_framesNext] = sample;
      m_framesNext = (m_framesNext + 1) % kFrames;
   }
   m_lastPresent = timestampUs;
}

EngineMcp::json EngineMcp::Timing(const json& args)
{
   vector<uint32_t> frame, gpu;
   {
      std::lock_guard lock(m_framesMutex);
      const size_t count = m_frames.size(), last = std::min<size_t>(args.value("last", kFrames), count);
      for (size_t i = 0; i < last; i++)
      {
         const auto& sample = m_frames[(m_framesNext + count - 1 - i) % count];
         frame.push_back(sample.first);
         gpu.push_back(sample.second);
      }
      if (args.value("reset", false))
      {
         m_frames.clear();
         m_framesNext = 0;
      }
   }
   if (frame.empty())
      return { { "frames", 0 } };
   const auto summary = [](vector<uint32_t>& values)
   {
      std::ranges::sort(values);
      const auto at = [&values](const double p) { return values[std::min(values.size() - 1, static_cast<size_t>(p * static_cast<double>(values.size())))] / 1000.0; };
      uint64_t sum = 0;
      for (const uint32_t value : values)
         sum += value;
      return json { { "avg", static_cast<double>(sum) / static_cast<double>(values.size()) / 1000.0 }, { "p50", at(0.5) }, { "p95", at(0.95) }, { "p99", at(0.99) }, { "max", values.back() / 1000.0 } };
   };
   uint64_t total = 0;
   for (const uint32_t value : frame)
      total += value;
   const size_t nFrames = frame.size();
   const json frameMs = summary(frame);
   const double p50 = frameMs["p50"].get<double>();
   return { { "frames", nFrames }, { "fps", static_cast<double>(nFrames) * 1000000.0 / static_cast<double>(total) }, { "frame_ms", frameMs }, { "gpu_ms", summary(gpu) },
      { "over_2x_median", std::ranges::count_if(frame, [p50](const uint32_t v) { return v / 1000.0 > 2.0 * p50; }) } };
}

#endif
