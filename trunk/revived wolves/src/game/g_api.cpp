/*
===========================================================================

Daemon GPL Source Code
Copyright (C) 2011 Dusan Jocic <dusanjocic@msn.com>

Daemon is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Daemon is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include "g_local.h"
#include "g_cm_world.h"
#include "../shared/VMMain.h"
#include "../shared/CommonProxies.h"

// This really should go in the common code
static IPC::Channel GetRootChannel(int argc, char** argv)
{
	const char* channel;
	if (argc == 1) {
		channel = getenv("ROOT_SOCKET");
		if (!channel) {
			fprintf(stderr, "Environment variable ROOT_SOCKET not found\n");
			VM::Exit();
		}
	} else {
		channel = argv[1];
	}

	char* end;
	IPC::OSHandleType h = (IPC::OSHandleType)strtol(channel, &end, 10);
	if (channel == end || *end != '\0') {
		fprintf(stderr, "Environment variable ROOT_SOCKET does not contain a valid handle\n");
		VM::Exit();
	}

	return IPC::Channel(IPC::Socket::FromHandle(h));
}

class ExitException{};

void VM::Exit() {
#ifndef VM_IN_PROCESS
	// Give the engine time to kill our process
	std::this_thread::sleep_for(std::chrono::seconds(2));
#endif
	throw ExitException();
}

IPC::Channel VM::rootChannel;

static IPC::SharedMemory shmRegion;

DLLEXPORT int main(int argc, char** argv)
{
	try {
		VM::rootChannel = GetRootChannel(argc, argv);

		// Send syscall ABI version, also acts as a sign that the module loaded
		IPC::Writer writer;
		writer.Write<uint32_t>(GAME_API_VERSION);
		VM::rootChannel.SendMsg(writer);

		// Allocate entities and clients shared memory region
		shmRegion = IPC::SharedMemory::Create(sizeof(gentity_t) * MAX_GENTITIES + sizeof(gclient_t) * MAX_CLIENTS);
		char* shmBase = reinterpret_cast<char*>(shmRegion.GetBase());
		g_entities = reinterpret_cast<gentity_t*>(shmBase);
		g_clients = reinterpret_cast<gclient_t*>(shmBase + sizeof(gentity_t) * MAX_GENTITIES);

		// Start main loop
		while (true) {
			IPC::Reader reader = VM::rootChannel.RecvMsg();
			uint32_t id = reader.Read<uint32_t>();
			VM::VMMain(id, std::move(reader));
		}
	} catch (ExitException e) {
		return 0;
	}
}

//HACK: NaCl doesn't support messages bigger than 128k so for now
//we send it by small chunks

std::vector<char> mapData;

void G_LoadMapChunk(std::vector<char> data)
{
	mapData.insert(mapData.end(), data.begin(), data.end());
}

void G_FinishMapLoad(std::string name)
{
	CM_LoadMap(name.c_str(), mapData.data(), false);
	mapData.clear();
	G_CM_ClearWorld();
}

void VM::VMMain(uint32_t id, IPC::Reader reader)
{
	int major = id >> 16;
	int minor = id & 0xffff;
	if (major == VM::QVM) {
		switch (minor) {
		case GAME_STATIC_INIT:
			IPC::HandleMsg<GameStaticInitMsg>(VM::rootChannel, std::move(reader), [](){
				// Initialize VM proxies
				VM::InitializeProxies();
			});
			break;

		case GAME_INIT:
			IPC::HandleMsg<GameInitMsg>(VM::rootChannel, std::move(reader), [](int levelTime, int randomSeed, bool restart) {
				FS::Initialize();
				G_InitGame(levelTime, randomSeed, restart);
			});
			break;

		case GAME_SHUTDOWN:
			IPC::HandleMsg<GameShutdownMsg>(VM::rootChannel, std::move(reader), [](bool restart) {
				G_ShutdownGame(restart);
			});
			break;

		case GAME_LOADMAP:
			IPC::HandleMsg<GameLoadMapMsg>(VM::rootChannel, std::move(reader), [](std::string name) {
				G_FinishMapLoad(std::move(name));
			});
			break;

		case GAME_LOADMAPCHUNK:
			IPC::HandleMsg<GameLoadMapChunkMsg>(VM::rootChannel, std::move(reader), [](std::vector<char> data) {
				G_LoadMapChunk(std::move(data));
			});
			break;

		case GAME_CLIENT_CONNECT:
			IPC::HandleMsg<GameClientConnectMsg>(VM::rootChannel, std::move(reader), [](int clientNum, bool firstTime, int isBot, bool& denied, std::string& reason) {
				const char* deniedStr = isBot ? ClientBotConnect(clientNum, firstTime, TEAM_NONE) : ClientConnect(clientNum, firstTime);
				denied = deniedStr != nullptr;
				if (denied)
					reason = deniedStr;
			});
			break;

		case GAME_CLIENT_THINK:
			IPC::HandleMsg<GameClientThinkMsg>(VM::rootChannel, std::move(reader), [](int clientNum) {
				ClientThink(clientNum);
			});
			break;

		case GAME_CLIENT_USERINFO_CHANGED:
			IPC::HandleMsg<GameClientUserinfoChangedMsg>(VM::rootChannel, std::move(reader), [](int clientNum) {
				ClientUserinfoChanged(clientNum, qfalse);
			});
			break;

		case GAME_CLIENT_DISCONNECT:
			IPC::HandleMsg<GameClientDisconnectMsg>(VM::rootChannel, std::move(reader), [](int clientNum) {
				ClientDisconnect(clientNum);
			});
			break;

		case GAME_CLIENT_BEGIN:
			IPC::HandleMsg<GameClientBeginMsg>(VM::rootChannel, std::move(reader), [](int clientNum) {
				ClientBegin(clientNum);
			});
			break;

		case GAME_CLIENT_COMMAND:
			IPC::HandleMsg<GameClientCommandMsg>(VM::rootChannel, std::move(reader), [](int clientNum, std::string command) {
				Cmd::PushArgs(command);
				ClientCommand(clientNum);
				Cmd::PopArgs();
			});
			break;

		case GAME_RUN_FRAME:
			IPC::HandleMsg<GameRunFrameMsg>(VM::rootChannel, std::move(reader), [](int levelTime) {
				G_RunFrame(levelTime);
			});
			break;

		case GAME_SNAPSHOT_CALLBACK:
			G_Error("GAME_SNAPSHOT_CALLBACK not implemented");
			break;

		case BOTAI_START_FRAME:
			G_Error("BOTAI_START_FRAME not implemented");
			break;

		case GAME_MESSAGERECEIVED:
			G_Error("GAME_MESSAGERECEIVED not implemented");
			break;

		default:
			G_Error("VMMain(): unknown game command %i", minor);
		}
	} else if (major < VM::LAST_COMMON_SYSCALL) {
		VM::HandleCommonSyscall(major, minor, std::move(reader), VM::rootChannel);
	} else {
		G_Error("unhandled VM major syscall number %i", major);
	}
}

void trap_Print(const char *string)
{
	VM::SendMsg<PrintMsg>(string);
}

void NORETURN trap_Error(const char *string)
{
	static bool recursiveError = false;
	if (recursiveError)
		VM::Exit();
	recursiveError = true;
	VM::SendMsg<ErrorMsg>(string);
	VM::Exit(); // Amanieu: Need to implement proper error handling
}

void trap_Log(log_event_t *event)
{
	G_Error("trap_Log not implemented");
}

int trap_Milliseconds(void)
{
#ifdef LIBSTDCXX_BROKEN_CXX11
	auto duration = std::chrono::monotonic_clock::now().time_since_epoch();
#else
	auto duration = std::chrono::steady_clock::now().time_since_epoch();
#endif
	return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

void trap_SendConsoleCommand(int exec_when, const char *text)
{
	VM::SendMsg<SendConsoleCommandMsg>(exec_when, text);
}

int trap_FS_FOpenFile(const char *qpath, fileHandle_t *f, fsMode_t mode)
{
	int ret, handle;
	VM::SendMsg<FSFOpenFileMsg>(qpath, f != NULL, mode, ret, handle);
	if (f)
		*f = handle;
	return ret;
}

void trap_FS_Read(void *buffer, int len, fileHandle_t f)
{
	std::string res;
	VM::SendMsg<FSReadMsg>(f, len, res);
	memcpy(buffer, res.c_str(), std::min((int)res.size() + 1, len));
}

int trap_FS_Write(const void *buffer, int len, fileHandle_t f)
{
	int res;
	std::string text((const char*)buffer, len);
	VM::SendMsg<FSWriteMsg>(f, text, res);
	return res;
}

void trap_FS_Rename(const char *from, const char *to)
{
	VM::SendMsg<FSRenameMsg>(from, to);
}

void trap_FS_FCloseFile(fileHandle_t f)
{
	VM::SendMsg<FSFCloseFileMsg>(f);
}

int trap_FS_GetFileList(const char *path, const char *extension, char *listbuf, int bufsize)
{
	int res;
	std::string text;
	VM::SendMsg<FSGetFileListMsg>(path, extension, bufsize, res, text);
	memcpy(listbuf, text.c_str(), std::min((int)text.size() + 1, bufsize));
	return res;
}

qboolean trap_FindPak(const char *name)
{
	bool res;
	VM::SendMsg<FSFindPakMsg>(name, res);
	return res;
}

// The actual shared memory region is handled in this file, and is pretty much invisible to the rest of the code
void trap_LocateGameData(gentity_t *, int numGEntities, int sizeofGEntity_t, playerState_t *clients, int sizeofGClient)
{
	static bool firstTime = true;
	if (firstTime) {
		VM::SendMsg<LocateGameDataMsg1>(shmRegion, numGEntities, sizeofGEntity_t, sizeofGClient);
		firstTime = false;
	} else
		VM::SendMsg<LocateGameDataMsg2>(numGEntities, sizeofGEntity_t, sizeofGClient);
}

void trap_LinkEntity(gentity_t *ent)
{
	G_CM_LinkEntity(ent);
}

void trap_UnlinkEntity(gentity_t *ent)
{
	G_CM_UnlinkEntity(ent);
}

int trap_EntitiesInBox(const vec3_t mins, const vec3_t maxs, int *list, int maxcount)
{
	return G_CM_AreaEntities(mins, maxs, list, maxcount);
}

qboolean trap_EntityContact(const vec3_t mins, const vec3_t maxs, const gentity_t *ent)
{
	return G_CM_EntityContact( mins, maxs, ent, TT_AABB );
}

void trap_Trace(trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentmask)
{
	vec3_t origin = {0.0f, 0.0f, 0.0f};
	if (!mins) {
		mins = origin;
	}
	if (!maxs) {
		mins = origin;
	}
	G_CM_Trace(results, start, mins, maxs, end, passEntityNum, contentmask, TT_AABB);
}

void trap_TraceNoEnts(trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentmask)
{
	trap_Trace(results, start, mins, maxs, end, -2, contentmask);
}

int trap_PointContents(const vec3_t point, int passEntityNum)
{
	return G_CM_PointContents( point, passEntityNum );
}

void trap_SetBrushModel(gentity_t *ent, const char *name)
{
	G_CM_SetBrushModel( ent, name );
}

qboolean trap_InPVS(const vec3_t p1, const vec3_t p2)
{
	return G_CM_inPVS( p1, p2 );
}

qboolean trap_InPVSIgnorePortals(const vec3_t p1, const vec3_t p2)
{
	return G_CM_inPVSIgnorePortals( p1, p2 );
}

void trap_AdjustAreaPortalState(gentity_t *ent, qboolean open)
{
    VM::SendMsg<AdjustAreaPortalStateMsg>(ent - g_entities, open);
	G_CM_AdjustAreaPortalState( ent, open );
}

qboolean CM_AreasConnected(int area1, int area2);
qboolean trap_AreasConnected(int area1, int area2)
{
	return CM_AreasConnected(area1, area2);
}

void trap_DropClient(int clientNum, const char *reason)
{
	VM::SendMsg<DropClientMsg>(clientNum, reason);
}

void trap_SendServerCommand(int clientNum, const char *text)
{
	if (strlen(text) > 1022) {
		G_LogPrintf("%s: trap_SendServerCommand( %d, ... ) length exceeds 1022.\n", GAME_VERSION, clientNum);
		G_LogPrintf("%s: text [%.950s]... truncated\n", GAME_VERSION, text);
		return;
	}

	VM::SendMsg<SendServerCommandMsg>(clientNum, text);
}

void trap_SetConfigstring(int num, const char *string)
{
	VM::SendMsg<SetConfigStringMsg>(num, string);
}

void trap_GetConfigstring(int num, char *buffer, int bufferSize)
{
	std::string res;
	VM::SendMsg<GetConfigStringMsg>(num, bufferSize, res);
	Q_strncpyz(buffer, res.c_str(), bufferSize);
}

void trap_SetConfigstringRestrictions(int num, const clientList_t *clientList)
{
	VM::SendMsg<SetConfigStringRestrictionsMsg>(); // not implemented
}

void trap_SetUserinfo(int num, const char *buffer)
{
	VM::SendMsg<SetUserinfoMsg>(num, buffer);
}

void trap_GetUserinfo(int num, char *buffer, int bufferSize)
{
	std::string res;
	VM::SendMsg<GetUserinfoMsg>(num, bufferSize, res);
	Q_strncpyz(buffer, res.c_str(), bufferSize);
}

void trap_GetServerinfo(char *buffer, int bufferSize)
{
	std::string res;
	VM::SendMsg<GetServerinfoMsg>(bufferSize, res);
	Q_strncpyz(buffer, res.c_str(), bufferSize);
}

void trap_GetUsercmd(int clientNum, usercmd_t *cmd)
{
	VM::SendMsg<GetUsercmdMsg>(clientNum, *cmd);
}

qboolean trap_GetEntityToken(char *buffer, int bufferSize)
{
	std::string text;
	bool res;
	VM::SendMsg<GetEntityTokenMsg>(res, text);
	Q_strncpyz(buffer, text.c_str(), bufferSize);
	return res;
}

void trap_SendGameStat(const char *data)
{
	VM::SendMsg<SendGameStatMsg>(data);
}

qboolean trap_GetTag(int clientNum, int tagFileNumber, const char *tagName, orientation_t *ori)
{
	int res;
	VM::SendMsg<GetTagMsg>(clientNum, tagFileNumber, tagName, res, *ori);
	return res;
}

qboolean trap_LoadTag(const char *filename)
{
	int res;
	VM::SendMsg<RegisterTagMsg>(filename, res);
	return res;
}

void trap_SendMessage(int clientNum, char *buf, int buflen)
{
	std::vector<char> buffer(buflen, 0);
	memcpy(buffer.data(), buf, buflen);
	VM::SendMsg<SendMessageMsg>(clientNum, buflen, buffer);
}

messageStatus_t trap_MessageStatus(int clientNum)
{
	int res;
	VM::SendMsg<MessageStatusMsg>(clientNum, res);
	return static_cast<messageStatus_t>(res);
}

int trap_RSA_GenerateMessage(const char *public_key, char *cleartext, char *encrypted)
{
	std::string cleartext2, encrypted2;
	int res;
	VM::SendMsg<RSAGenMsgMsg>(public_key, res, cleartext2, encrypted2);
	Q_strncpyz(cleartext, cleartext2.c_str(), RSA_STRING_LENGTH);
	Q_strncpyz(encrypted, encrypted2.c_str(), RSA_STRING_LENGTH);
	return res;
}

void trap_GenFingerprint(const char *pubkey, int size, char *buffer, int bufsize)
{
	std::vector<char> pubkey2(size, 0);
	memcpy(pubkey2.data(), pubkey, size);
	std::string fingerprint;
	VM::SendMsg<GenFingerprintMsg>(size, pubkey2, bufsize, fingerprint);
	Q_strncpyz(buffer, fingerprint.c_str(), bufsize);
}

void trap_GetPlayerPubkey(int clientNum, char *pubkey, int size)
{
	std::string pubkey2;
	VM::SendMsg<GetPlayerPubkeyMsg>(clientNum, size, pubkey2);
	Q_strncpyz(pubkey, pubkey2.c_str(), size);
}

int trap_GMTime(qtime_t *qtime)
{
	int res;
	if (qtime) {
		VM::SendMsg<GMTimeMsg>(res, *qtime);
	} else {
		qtime_t t;
		VM::SendMsg<GMTimeMsg>(res, t);
	}
	return res;
}

void trap_GetTimeString(char *buffer, int size, const char *format, const qtime_t *tm)
{
	std::string text;
	VM::SendMsg<GetTimeStringMsg>(size, format, *tm, text);
	Q_strncpyz(buffer, text.c_str(), size);
}

int trap_Parse_AddGlobalDefine(const char *define)
{
	int res;
	VM::SendMsg<ParseAddGlobalDefineMsg>(define, res);
	return res;
}

int trap_Parse_LoadSource(const char *filename)
{
	int res;
	VM::SendMsg<ParseLoadSourceMsg>(filename, res);
	return res;
}

int trap_Parse_FreeSource(int handle)
{
	int res;
	VM::SendMsg<ParseFreeSourceMsg>(handle, res);
	return res;
}

int trap_Parse_ReadToken(int handle, pc_token_t *pc_token)
{
	int res;
	VM::SendMsg<ParseReadTokenMsg>(handle, res, *pc_token);
	return res;
}

int trap_Parse_SourceFileAndLine(int handle, char *filename, int *line)
{
	int res;
	std::string filename2;
	VM::SendMsg<ParseSourceFileAndLineMsg>(handle, res, filename2, *line);
	Q_strncpyz(filename, filename2.c_str(), 128);
	return res;
}

void trap_QuoteString(const char *str, char *buffer, int size)
{
	Q_strncpyz(buffer, Cmd::Escape(str).c_str(), size);
}

int trap_BotAllocateClient(int clientNum)
{
	int res;
	VM::SendMsg<BotAllocateClientMsg>(clientNum, res);
	return res;
}

void trap_BotFreeClient(int clientNum)
{
	VM::SendMsg<BotFreeClientMsg>(clientNum);
}

int trap_BotGetServerCommand(int clientNum, char *message, int size)
{
	int res;
	std::string message2;
	VM::SendMsg<BotGetConsoleMessageMsg>(clientNum, size, res, message2);
	Q_strncpyz(message, message2.c_str(), size);
	return res;
}

qboolean trap_BotSetupNav(const botClass_t *botClass, qhandle_t *navHandle)
{
	int res;
	VM::SendMsg<BotNavSetupMsg>(*botClass, res, *navHandle);
	return res;
}

void trap_BotShutdownNav(void)
{
	VM::SendMsg<BotNavShutdownMsg>();
}

void trap_BotSetNavMesh(int botClientNum, qhandle_t navHandle)
{
	VM::SendMsg<BotSetNavmeshMsg>(botClientNum, navHandle);
}

qboolean trap_BotFindRoute(int botClientNum, const botRouteTarget_t *target, qboolean allowPartial)
{
	int res;
	VM::SendMsg<BotFindRouteMsg>(botClientNum, *target, allowPartial, res);
	return res;
}

qboolean trap_BotUpdatePath(int botClientNum, const botRouteTarget_t *target, botNavCmd_t *cmd)
{
	VM::SendMsg<BotUpdatePathMsg>(botClientNum, *target, *cmd);
	return 0; // Amanieu: This always returns 0, but the value isn't used
}

qboolean trap_BotNavTrace(int botClientNum, botTrace_t *botTrace, const vec3_t start, const vec3_t end)
{
	std::array<float, 3> start2, end2;
	VectorCopy(start, start2.data());
	VectorCopy(end, end2.data());
	int res;
	VM::SendMsg<BotNavRaycastMsg>(botClientNum, start2, end2, res, *botTrace);
	return res;
}

void trap_BotFindRandomPoint(int botClientNum, vec3_t point)
{
	std::array<float, 3> point2;
	VM::SendMsg<BotNavRandomPointMsg>(botClientNum, point2);
	VectorCopy(point2.data(), point);
}

qboolean trap_BotFindRandomPointInRadius(int botClientNum, const vec3_t origin, vec3_t point, float radius)
{
	std::array<float, 3> point2, origin2;
	VectorCopy(origin, origin2.data());
	int res;
	VM::SendMsg<BotNavRandomPointRadiusMsg>(botClientNum, origin2, radius, res, point2);
	VectorCopy(point2.data(), point);
	return res;
}

void trap_BotEnableArea(const vec3_t origin, const vec3_t mins, const vec3_t maxs)
{
	std::array<float, 3> origin2, mins2, maxs2;
	VectorCopy(origin, origin2.data());
	VectorCopy(mins, mins2.data());
	VectorCopy(maxs, maxs2.data());
	VM::SendMsg<BotEnableAreaMsg>(origin2, mins2, maxs2);
}

void trap_BotDisableArea(const vec3_t origin, const vec3_t mins, const vec3_t maxs)
{
	std::array<float, 3> origin2, mins2, maxs2;
	VectorCopy(origin, origin2.data());
	VectorCopy(mins, mins2.data());
	VectorCopy(maxs, maxs2.data());
	VM::SendMsg<BotDisableAreaMsg>(origin2, mins2, maxs2);
}

void trap_BotAddObstacle(const vec3_t mins, const vec3_t maxs, qhandle_t *handle)
{
	std::array<float, 3> mins2, maxs2;
	VectorCopy(mins, mins2.data());
	VectorCopy(maxs, maxs2.data());
	VM::SendMsg<BotAddObstacleMsg>(mins2, maxs2, *handle);
}

void trap_BotRemoveObstacle(qhandle_t handle)
{
	VM::SendMsg<BotRemoveObstacleMsg>(handle);
}

void trap_BotUpdateObstacles(void)
{
	VM::SendMsg<BotUpdateObstaclesMsg>();
}
