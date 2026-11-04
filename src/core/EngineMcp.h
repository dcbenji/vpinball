// license:GPLv3+

// Development only: a small MCP server inside the running player, HTTP on 127.0.0.1 with a bearer token. Off unless VPX_MCP_PORT and VPX_MCP_TOKEN are set. See docs/FastIteration.md

#pragma once

#include "renderer/ShaderHotReload.h" // VPX_DEV_TOOLS

#ifdef VPX_DEV_TOOLS

#include "utils/JSONSerializer.h"
#include <future>

class Player;
class PinTable;
class Material;

class EngineMcp final
{
public:
   static EngineMcp* Get(); // nullptr when disabled, which is the normal case. Thread safe

   void Update(Player* player); // Logic thread, once per frame, between frames
   void ApplyHeldLights(PinTable* table); // Logic thread, where the frame reads the light levels
   void OnFramePresented(uint64_t timestampUs, uint64_t gpuUs); // Render thread

   ~EngineMcp();

private:
   using json = nlohmann::ordered_json;
   struct ToolReply
   {
      json result = json::object(); // Shown as text and structured content
      string error;
      string imageFile, imageTarget, imageCaption; // A capture to finish: crop, then save to the target or return inline
      int imageMaxWidth = 0;
      bool imageCropped = false, imageSettled = true;
      float imageCrop[4] = { 0.f, 0.f, 1.f, 1.f };
   };
   struct Call
   {
      string tool;
      json args;
      std::promise<ToolReply> reply;
   };
   struct FrameWait { std::shared_ptr<Call> call; uint32_t frame; };
   struct SettlingView { std::shared_ptr<Call> call; ToolReply reply; uint64_t start, lastWork; uint32_t lastFrame; int quietFrames; };
   struct HeldLight { float level, restore; };
   struct Target { IEditable* part = nullptr; Material* material = nullptr; string name; };

   EngineMcp(int port, int listenSocket, string token);
   void ListenLoop();
   void ServeConnection(int client);
   bool HandleHttp(int client, const string& method, const string& path, const std::map<string, string>& headers, const string& body);
   json HandleRpc(const json& rpc, bool& isNotification, string& sessionId); // On the connection's thread
   json CallTool(const string& name, const json& args); // Queues to the logic thread and waits

   void Handle(Player* player, const std::shared_ptr<Call>& call);
   json Guide(Player* player) const;
   json Table(Player* player, const json& args) const;
   json Part(Player* player, const json& args);
   json MaterialTool(Player* player, const json& args);
   json History(Player* player, const json& args);
   json Status(Player* player) const;
   json Camera(Player* player, const json& args);
   json Lights(Player* player, const json& args);
   json Run(Player* player, const std::shared_ptr<Call>& call, bool& deferred);
   json Timing(const json& args);
   json Reload(Player* player, const json& args);
   void View(Player* player, const std::shared_ptr<Call>& call);
   void UpdateSettling(Player* player);
   static void StartCapture(Player* player, const std::shared_ptr<Call>& call, const ToolReply& base);
   static json FinishView(const ToolReply& reply);

   static Target FindPart(const Player* player, const string& name);
   static Target FindMaterial(const Player* player, const string& name);
   static json SaveDoc(const Player* player, const Target& target);
   json Patch(Player* player, const Target& target, const json& patch, bool apply, const string& description);
   // Applies a full document to the part or material, recording an undo step in an edit session. Returns whether a step was recorded
   bool ApplyDoc(Player* player, const Target& target, const json& doc, const string& label, const JSONSerializer::Deserializer* pack);

   const int m_port, m_listenSocket;
   const string m_token;
   std::thread m_thread;
   std::atomic<bool> m_stop { false };

   std::atomic<int> m_openConnections { 0 };
   std::filesystem::path m_workDir; // Private: captures on their way

   std::mutex m_queueMutex;
   vector<std::shared_ptr<Call>> m_queue;
   vector<FrameWait> m_frameWaits; // Logic thread
   vector<SettlingView> m_settling; // Logic thread
   uint32_t m_lastEditFrame = 0; // Logic thread

   std::map<string, HeldLight> m_held; // Logic thread
   Player* m_player = nullptr; // Logic thread: the session all this belongs to
   uint32_t m_lastSeenFrame = 0;

   static constexpr size_t kFrames = 1024;
   std::mutex m_framesMutex;
   vector<std::pair<uint32_t, uint32_t>> m_frames; // Frame interval, GPU time in us; a ring
   size_t m_framesNext = 0;
   uint64_t m_lastPresent = 0;
};

#endif
