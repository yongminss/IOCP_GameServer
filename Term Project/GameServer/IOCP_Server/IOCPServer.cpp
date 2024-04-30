#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h> // AcceptEx를 사용하기 위해 선언
#pragma comment (lib, "WS2_32.lib")
#pragma comment (lib, "mswsock.lib")
#pragma comment (lib, "lua53.lib")

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <vector>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <queue>

#define UNICODE
#include <Windows.h>
#include <sqlext.h>

void show_error() {
	printf("error\n");
}

using namespace std;
using namespace chrono;

#include "protocol.h"

constexpr auto MAX_PACKET_SIZE = 255;
constexpr auto MAX_BUF_SIZE = 1024;
constexpr auto MAX_USER = 10000;
constexpr auto VIEW_RADIUS = 8;
constexpr auto MAX_HP = 100;

// 무엇이 완료되었는가를 알기 위해 사용하는 enum type
enum ENUMOP { OP_RECV, OP_SEND, OP_ACCEPT, OP_RANDOM_MOVE, OP_NPC_RESET, OP_PLAYER_RESET, OP_AUTO_HEAL };
enum C_STATUS { ST_FREE, ST_ALLOC, ST_ACTIVE, ST_SLEEP };

struct event_type {
	int obj_id; // 무슨 오브젝트가 동작을 해야하는가
	ENUMOP event_id; // 무슨 이벤트인가 (Move, heal ...)
	high_resolution_clock::time_point wakeup_time; // 언제 이벤트가 실행되야 하는가
	int target_id; // 추가 정보

	// 우선순위를 정하기 위한 연산자 오버로딩
	constexpr bool operator < (const event_type& left) const {
		return (wakeup_time > left.wakeup_time);
	}
};

priority_queue<event_type> timer_queue;
mutex timer_lock;

// OVERLAPPED 구조체의 정보가 빈약해서 사용하는 확장 구조체
struct EXOVER {
	WSAOVERLAPPED	over;
	ENUMOP			op;
	char			io_buf[MAX_BUF_SIZE]; // 실제 버퍼 정보는 여기에 저장
	union {
		WSABUF			wsabuf; // 버퍼들을 관리하는 버퍼로, 실제 데이터와 길이를 저장
		SOCKET			c_socket;
		int				p_id; // 어떤 플레이어가 움직였는지 LUA에 알리기 위해 사용
	};
};

struct CLIENT {
	mutex m_cl;
	SOCKET m_s;
	int m_id;
	int m_hp;
	int m_level;
	int m_exp;
	int m_rq_exp;
	EXOVER m_recv_over;
	int m_prev_size;
	char m_packe_buf[MAX_PACKET_SIZE];
	atomic<C_STATUS> m_status;
	int db_id;
	int is_dead;

	short x, y;
	char m_name[MAX_ID_LEN + 1];
	unsigned m_move_time;
	high_resolution_clock::time_point m_last_move_time; // 일정 시간마다 움직이도록 하기 위해 사용 - NPC

	// 시야 처리를 위한 view-list - 순서 중요하지 않기 때문에 unordered-set 사용
	// view-list는 여러 thread 에서 읽고 쓰기 때문에 data race 발생 가능 - lock 필요
	unordered_set<int> m_view_list;

	// LUA Script를 사용하기 위한 가상 머신을 생성
	lua_State* L;
	mutex lua_l;
};

CLIENT g_clients[NPC_ID_START + NUM_NPC];
HANDLE g_iocp; // IOCP의 HANDLE
SOCKET l_socket;
bool is_auto_heal = true;

void disconnect(int user_id);
void load_database();
void insert_database(int id, int x, int y);
void update_database(int user_id, int db_id, int x, int y);

struct DB_INFO {
	int id;
	int hp;
	int level;
	int exp;
	int x;
	int y;
};

DB_INFO g_db_data[MAX_USER]; // db에 저장된 id를 저장
int db_user_size = 0;

void add_timer(int obj_id, ENUMOP op_type, int duration)
{
	timer_lock.lock();
	event_type ev{ obj_id, op_type, high_resolution_clock::now() + milliseconds(duration), 0 };
	timer_queue.push(ev);
	timer_lock.unlock();
}

bool is_player(int id)
{
	return id < NPC_ID_START;
}

bool is_near(int user_id, int o_id)
{
	// 시야 처리를 할 플레이언지 확인
	if (abs(g_clients[user_id].x - g_clients[o_id].x) < VIEW_RADIUS && abs(g_clients[user_id].y - g_clients[o_id].y) < VIEW_RADIUS)
		return true;
	return false;
}

void send_packet(int user_id, void *p)
{
	char *buf = reinterpret_cast<char*>(p);

	CLIENT& u = g_clients[user_id];

	EXOVER *exover = new EXOVER;
	exover->op = OP_SEND;
	ZeroMemory(&exover->over, sizeof(exover->over));
	exover->wsabuf.buf = exover->io_buf;
	exover->wsabuf.len = buf[0]; // 패킷의 길이
	memcpy(exover->io_buf, buf, buf[0]); // 패킷의 내용을 exover buffer에 저장

	WSASend(u.m_s, &exover->wsabuf, 1, NULL, 0, &exover->over, NULL);
}

void send_login_ok_packet(int user_id)
{
	sc_packet_login_ok p;
	p.exp = 0;
	p.hp = 0;
	p.id = user_id;
	p.level = 0;
	p.size = sizeof(p);
	p.type = S2C_LOGIN_OK;
	p.x = g_clients[user_id].x;
	p.y = g_clients[user_id].y;

	send_packet(user_id, &p);
}

void send_enter_packet(int user_id, int o_id)
{
	sc_packet_enter p;
	p.id = o_id;
	p.size = sizeof(p);
	p.type = S2C_ENTER;
	p.x = g_clients[o_id].x;
	p.y = g_clients[o_id].y;
	strcpy_s(p.name, g_clients[o_id].m_name);
	p.o_type = O_HUMAN;

	// view-list에 시야 처리할 다른 플레이어들을 추가
	g_clients[user_id].m_cl.lock();
	g_clients[user_id].m_view_list.insert(o_id);
	g_clients[user_id].m_cl.unlock();

	send_packet(user_id, &p);
}

void send_leave_packet(int user_id, int o_id)
{
	sc_packet_leave p;
	p.id = o_id;
	p.size = sizeof(p);
	p.type = S2C_LEAVE;

	// 시야 처리되어 떠난 플레이어들 제거
	g_clients[user_id].m_cl.lock();
	g_clients[user_id].m_view_list.erase(o_id);
	g_clients[user_id].m_cl.unlock();

	send_packet(user_id, &p);
}

void send_move_packet(int user_id, int mover)
{
	sc_packet_move p;
	p.id = mover;
	p.size = sizeof(p);
	p.type = S2C_MOVE;
	p.x = g_clients[mover].x;
	p.y = g_clients[mover].y;
	p.move_time = g_clients[mover].m_move_time;

	send_packet(user_id, &p);
}

void send_state_packet(int user_id, int hp, int level, int exp)
{
	sc_packet_stat_change p;
	p.size = sizeof(p);
	p.type = S2C_STAT_CHANGE;
	p.hp = hp;
	p.level = level;
	p.exp = exp;

	send_packet(user_id, &p);
}

void send_chat_packet(int user_id, int chatter, char mess[])
{
	sc_packet_chat p;
	p.id = chatter;
	p.size = sizeof(p);
	p.type = S2C_CHAT;
	strcpy(p.mess, mess);

	send_packet(user_id, &p);
}

void send_dead_packet(int user_id)
{
	sc_dead_packet p;
	p.size = sizeof(p);
	p.type = S2C_DEAD;

	send_packet(user_id, &p);
}

void send_recall_packet(int user_id)
{
	sc_recall_packet p;
	p.size = sizeof(p);
	p.type = S2C_RECALL;

	send_packet(user_id, &p);
}

void activate_npc(int npc_id)
{
	C_STATUS old_state = ST_SLEEP;
	// Multi-Thread 이기 때문에 add_timer가 2번 실행될 수 있음
	// State를 Active로 바꾸는 것에 성공한 thread만 add_timer를 호출하게 하자
	if (true == atomic_compare_exchange_strong(&g_clients[npc_id].m_status, &old_state, ST_ACTIVE))
		add_timer(npc_id, OP_RANDOM_MOVE, 1000);
}

void do_move(int user_id, int direction)
{
	CLIENT &u = g_clients[user_id];

	int x = u.x;
	int y = u.y;

	switch (direction) {
	case D_UP: if (y > 0) --y; break;

	case D_DOWN: if (y < (WORLD_HEIGHT - 1)) ++y; break;

	case D_LEFT: if (x > 0) --x; break;

	case D_RIGHT: if (x < (WORLD_WIDTH - 1)) ++x; break;

	default:
		cout << "Unknown Direction from Client move packet!" << endl;
		DebugBreak();
		exit(-1);
	}
	u.x = x;
	u.y = y;

	//update_database(g_clients[user_id].db_id, u.x, u.y);

	// 이동 전의 view-list - 움직이기 전에는 이 view-list를 보고, 이동 후에는 new를 봄
	g_clients[user_id].m_cl.lock();
	unordered_set<int> old_vl = g_clients[user_id].m_view_list;
	g_clients[user_id].m_cl.unlock();

	// 이동한 플레이어들의 view-list를 새로 생성 - 어디서 들어오는지 모르기 때문 (비교 필요)
	unordered_set<int> new_vl;
	for (auto& cl : g_clients) {
		if (is_near(cl.m_id, user_id) == false) continue;
		if (cl.m_status == ST_SLEEP) activate_npc(cl.m_id);
		if (cl.m_status != ST_ACTIVE) continue;
		if (cl.m_id == user_id) continue;
		//if (false == is_player(cl.m_id)) { // 플레이어가 움직였을 때, NPC Script를 위해 정보 전달
		//	EXOVER *over = new EXOVER;
		//	over->op = OP_PLAYER_MOVE;
		//	over->p_id = user_id;
		//	PostQueuedCompletionStatus(g_iocp, 1, cl.m_id, &over->over);
		//}
		new_vl.insert(cl.m_id);
	}
	// 내가 이동한 것을 나 자신에게 알려줌
	send_move_packet(user_id, user_id);

	// 시야에 새로 들어온 플레이어
	for (auto np : new_vl) {
		// old view-list에 없다가, new view-list에 들어온 경우 - 새로 들어온 경우
		if (0 == old_vl.count(np)) {
			// 서로 시야가 들어오면 둘 모두에게 보내줘야 함
			send_enter_packet(user_id, np);
			// 플레이어가 아니면 pass
			if (is_player(np) == false) continue;
			// 단, multi-thread 이기에, 이미 내가 갱신됐을 수도 있음 - 확인 필요
			g_clients[np].m_cl.lock();
			if (0 == g_clients[np].m_view_list.count(user_id)) {
				g_clients[np].m_cl.unlock();
				send_enter_packet(np, user_id);
			}
			// 이미 상대방의 view-list에 내가 있으면, move packet을 보내줌
			else {
				g_clients[np].m_cl.unlock();
				send_move_packet(np, user_id);
			}
		}
		// 새로 들어오지 않고, 이전에도 view-list에 있던 것이면
		else {
			if (is_player(np) == false) continue;
			g_clients[np].m_cl.lock();
			// 상대방이 이동하면서 내가 시야에서 벗어났을 수도 있음
			if (g_clients[np].m_view_list.count(user_id) != 0) {
				g_clients[np].m_cl.unlock();
				send_move_packet(np, user_id);
			}
			else {
				g_clients[np].m_cl.unlock();
				send_enter_packet(np, user_id);
			}
		}
	}

	// 시야에서 벗어난 플레이어
	for (auto op : old_vl) {
		// old view-list에는 있으나, new view-list에는 없는 경우
		if (new_vl.count(op) == 0) {
			send_leave_packet(user_id, op);
			if (is_player(op) == false) continue; // NPC는 leave packet을 할 필요가 없음
			// 상대방의 view-list에 내가 있을 때 leave packet을 하면 안됨
			g_clients[op].m_cl.lock();
			if (g_clients[op].m_view_list.count(g_clients[user_id].m_id) != 0) {
				g_clients[op].m_cl.unlock();
				send_leave_packet(op, user_id);
			}
			else
				g_clients[op].m_cl.unlock();
		}
	}
}

void do_attack(int user_id)
{
	g_clients[user_id].m_rq_exp = g_clients[user_id].m_level * 50;
	for (auto other : g_clients[user_id].m_view_list) {
		if (!is_player(other) && false == g_clients[other].is_dead) {
			if (abs(g_clients[user_id].x - g_clients[other].x) == 1 && g_clients[user_id].y == g_clients[other].y) {
				char text[32];
				sprintf_s(text, "Monster Damage - %d", g_clients[user_id].m_level * 5);
				g_clients[other].m_hp -= g_clients[user_id].m_level * 5;
				send_chat_packet(user_id, other, text);
			}
			if (abs(g_clients[user_id].y - g_clients[other].y) == 1 && g_clients[user_id].x == g_clients[other].x) {
				char text[32];
				sprintf_s(text, "Monster Damage - %d", g_clients[user_id].m_level * 5);
				g_clients[other].m_hp -= g_clients[user_id].m_level * 5;
				send_chat_packet(user_id, other, text);
			}
			// 몬스터의 HP가 0이하로 떨어졌을 경우
			if (g_clients[other].m_hp <= 0) {
				g_clients[other].is_dead = true;
				// 몬스터 처치 이벤트 발생 (몬스터 사망 대사, 플레이어 경험치 증가, 30초 후 몬스터 재생성)
				char text[16];
				sprintf_s(text, "EXP + %d!", g_clients[other].m_level * 10);
				g_clients[user_id].m_cl.lock();
				g_clients[user_id].m_exp += (g_clients[other].m_level * 10);
				g_clients[user_id].m_cl.unlock();
				if (g_clients[user_id].m_exp > g_clients[user_id].m_rq_exp) {
					g_clients[user_id].m_cl.lock();
					++g_clients[user_id].m_level; // 요구 경험치보다 경험치량 많아지면 레벨 업
					g_clients[user_id].m_cl.unlock();
					g_clients[user_id].m_exp = 0;
				}
				send_chat_packet(user_id, other, text);
				send_state_packet(user_id, g_clients[user_id].m_hp, g_clients[user_id].m_level, g_clients[user_id].m_exp);
				g_clients[other].x = WORLD_WIDTH * 10;
				// NPC를 랜덤한 위치에 30초 후에 다시 생성
				add_timer(other, OP_NPC_RESET, 30000);
			}
		}
	}
}

void random_move_npc(int npc_id)
{
	int x = g_clients[npc_id].x;
	int y = g_clients[npc_id].y;

	if (npc_id < 24000) {

		switch (rand() % 4) {
		case 0: if (x < WORLD_WIDTH - 1) ++x; break;
		case 1: if (x > 0) --x; break;
		case 2: if (y < WORLD_HEIGHT - 1) ++y; break;
		case 3: if (y > 0) --y; break;
		}
		g_clients[npc_id].x = x;
		g_clients[npc_id].y = y;
	}
	else {
		for (int i = 0; i < MAX_USER; ++i) {
			if (is_near(i, npc_id)) {
				if (g_clients[i].m_hp <= 0) break;
				int targetX = g_clients[i].x;
				int targetY = g_clients[i].y;
				// ID가 24000 이상인 NPC들은 플레이어를 쫓아서 다음 위치 결정
				if (x < targetX) ++x;
				else if (x > targetX) --x;
				else {
					if (y < targetY) ++y;
					else if (y > targetY) --y;
					else {
						switch (rand() % 4) {
						case 0: if (x < WORLD_WIDTH - 1) ++x; break;
						case 1: if (x > 0) --x; break;
						case 2: if (y < WORLD_HEIGHT - 1) ++y; break;
						case 3: if (y > 0) --y; break;
						}
					}
				}
				g_clients[npc_id].x = x;
				g_clients[npc_id].y = y;
				break;
			}
		}
	}

	// 이동을 했으니 주위의 플레이어들에게 알려주자
	for (int i = 0; i < MAX_USER; ++i) {
		if (g_clients[i].m_status != ST_ACTIVE) continue;
		// 시야에 보이는 플레이어에게만 알려주자
		if (is_near(i, npc_id) == true) {
			g_clients[i].m_cl.lock();
			if (g_clients[i].m_view_list.count(npc_id) != 0) {
				g_clients[i].m_cl.unlock();
				send_move_packet(i, npc_id);

				// 함부로 호출하면 안되기 때문에 locking
				g_clients[i].lua_l.lock();
				// 주위 플레이어가 움직였으므로 LUA에 알려주어야 함
				lua_State* L = g_clients[npc_id].L;
				lua_getglobal(L, "event_player_move");
				lua_pushnumber(L, i);
				int error = lua_pcall(L, 1, 0, 0);
				if (error) cout << "Lua error : " << lua_tostring(L, -1) << endl;
				g_clients[i].lua_l.unlock();

				// user와 npc가 충돌했을 경우, hp를 감소시킴
				if (g_clients[i].x == g_clients[npc_id].x && g_clients[i].y == g_clients[npc_id].y) {
					g_clients[i].m_hp -= g_clients[npc_id].m_level * 2;
					// 플레이어의 HP가 0이하로 떨어져서 사망했을 경우
					if (g_clients[i].m_hp <= 0) {
						g_clients[i].m_cl.lock();
						g_clients[i].m_hp = 0;
						g_clients[i].m_exp /= 2;
						g_clients[i].m_cl.unlock();
						// 죽었다는 사실을 클라이언트에게 전달
						send_dead_packet(i);
						add_timer(i, OP_PLAYER_RESET, 1000); // 5초 후에 부활
					}
					send_state_packet(i, g_clients[i].m_hp, g_clients[i].m_level, g_clients[i].m_exp);
				}
			}
			else {
				g_clients[i].m_cl.unlock();
				send_enter_packet(i, npc_id);
			}
		}
		// NPC가 시야에 벗어난 경우엔 view-list에서 삭제
		else {
			g_clients[i].m_cl.lock();
			if (g_clients[i].m_view_list.count(npc_id) != 0) {
				g_clients[i].m_cl.unlock();
				send_leave_packet(i, npc_id);
			}
			else
				g_clients[i].m_cl.unlock();
		}
	}
}

void enter_game(int user_id, char name[])
{
	// enter_game 일 때만, true로 만들기에 굳이 lock 할 필요 없음
	// sum += 2 완 다른 case, but 굉장히 위험한 행동 (이왕 lock을 걸었으니 전부 다 걸자)
	g_clients[user_id].m_cl.lock();

	// lock을 걸어주기 위해 가져옴
	strcpy_s(g_clients[user_id].m_name, name);
	g_clients[user_id].m_name[MAX_ID_LEN] = NULL;

	// DB에 저장 되어 있던 x, y값을 넣어줌
	for (int i = 0; i < db_user_size; ++i)
		if (g_clients[user_id].db_id == g_db_data[i].id) {
			g_clients[user_id].m_hp = g_db_data[i].hp;
			g_clients[user_id].m_level = g_db_data[i].level;
			g_clients[user_id].m_exp = g_db_data[i].exp;
			g_clients[user_id].x = g_db_data[i].x;
			g_clients[user_id].y = g_db_data[i].y;
			send_state_packet(user_id, g_clients[user_id].m_hp, g_clients[user_id].m_level, g_clients[user_id].m_exp);
			break;
		}

	// lock을 걸고 login-ok packet을 보내는건 문제 있음 - 다른 모든 패킷보다 먼저 와야 함
	// 위로 빼는 것이 좋으나, name setting 등의 문제 때문에 일단은 이렇게 사용
	send_login_ok_packet(user_id);

	// 자신의 존재를 다른 클라이언트에 전부 알려 준 후에 Active 상태로
	// -> dead-lock 문제 때문에 위로 빠짐 -> login-ok packet만을 보내고 unlock을 수행
	g_clients[user_id].m_status = ST_ACTIVE; // 서버와 연결된 클라이언트를 구분하기 위해 사용
	g_clients[user_id].m_cl.unlock();
	// unlock은 원래 데이터를 모두 보낸 후, 하는게 좋으나 lock-unlock 사이의 크기 너무 커짐 (병렬성 저하)

	// 이미 접속한 플레이어를 나에게 보내고, 나의 정보를 이미 접속한 플레이어들에게 전송
	for (auto& cl : g_clients) {
		int i = cl.m_id;
		if (user_id == i) continue; // 이중 lock을 방지하기 위해 user_id와 i가 같을 땐 패스

	// 시야 처리를 하기 전에, 모든 플레이어와 자신의 packet 정보를 주고 받음
		if (true == is_near(user_id, i)) {
			if (ST_SLEEP == g_clients[i].m_status) // 플레이어 근처에 있는 NPC를 활성화
				activate_npc(i);
			// i번째 client도 Data Race가 일어나면 안되기 때문에 lock
			//g_clinets[i].m_cl.lock();
			// -> 실행 중에 ACTIVE 아닌 것이 되었을 때, 문제 발생 가능
			// 상태를 바꾸는 thread 에서, 상태에 따른 변화가 없을 때만 상태를 바꿔야 함
			// 완전히 해결할 순 없으나, atomic으로 선언해주어 메모리 오더 문제는 해결해주자
			if (g_clients[i].m_status == ST_ACTIVE) {
				send_enter_packet(user_id, i);
				if (true == is_player(i)) send_enter_packet(i, user_id);
			}
		}
		//g_clinets[i].m_cl.unlock();
	}
}

void process_packet(int user_id, char *buf)
{
	switch (buf[1]) {
	case C2S_LOGIN:
	{
		load_database();
		cs_packet_login *packet = reinterpret_cast<cs_packet_login *>(buf);
		// 이미 있는 ID일 경우 연결을 못하게 함
		for (int i = 0; i < MAX_USER; ++i) {
			if (g_clients[i].db_id != packet->id) {
				g_clients[user_id].db_id = packet->id;
				break;
			}
			disconnect(user_id);
			return;
		}
		
		// DB에 저장된 ID일 때만 enter game
		for (int i = 0; i < db_user_size; ++i) {
			if (g_db_data[i].id == g_clients[user_id].db_id) {
				enter_game(user_id, packet->name);
				return;
			}
		}
		// ID가 없을 경우
		// DB에 ID를 생성하는 함수를 호출하자
		g_clients[user_id].db_id = packet->id;
		if (g_clients[user_id].db_id < MAX_USER) insert_database(g_clients[user_id].db_id, g_clients[user_id].x, g_clients[user_id].y);
		enter_game(user_id, packet->name);
	}
	break;

	case C2S_MOVE:
	{
		cs_packet_move *packet = reinterpret_cast<cs_packet_move*>(buf);
		g_clients[user_id].m_move_time = packet->move_time;
		do_move(user_id, packet->direction);
	}
	break;

	case C2S_ATTACK:
	{
		cs_packet_attack *packet = reinterpret_cast<cs_packet_attack*>(buf);
		do_attack(user_id);
	}
	break;

	case C2S_CHAT:
	{
		cs_packet_chat *packet = reinterpret_cast<cs_packet_chat*>(buf);
		// 받아온 메세지를 view-list에 있는 다른 player에게만 전달
		for (auto cl : g_clients[user_id].m_view_list)
			if (is_player) send_chat_packet(cl, user_id, packet->mess);
	}
	break;

	default:
		cout << "Unknown Packet Type Error!" << endl;
		DebugBreak(); // 여기서 멈추라는 뜻
		exit(-1);
		break;
	}
}

void initialize_clients()
{
	// thread를 쓰지 않는 싱글쓰레드로 돌아가는 함수
	// lock을 해줄 필요 없음
	for (int i = 0; i < MAX_USER; ++i) {
		g_clients[i].m_id = i;
		g_clients[i].m_hp = MAX_HP;
		g_clients[i].m_level = 1;
		g_clients[i].m_exp = 0;
		g_clients[i].m_status = ST_FREE;
	}
}

void disconnect(int user_id)
{
	// 연결이 끊길 때 좌표를 DB에 저장
	update_database(user_id, g_clients[user_id].db_id, g_clients[user_id].x, g_clients[user_id].y);

	// for문에서 내 자신은 send_leave_packet() 함수를 호출해주지 않기 때문에 미리 해줌
	send_leave_packet(user_id, user_id);

	g_clients[user_id].m_cl.lock();
	// 여기서 Free로 바꿀 경우, 연결 종료 데이터를 처리하지 않았는데 연결될 수도 있기에 Allocated
	g_clients[user_id].m_status = ST_ALLOC;

	closesocket(g_clients[user_id].m_s);

	// m_connected 를 false로 바꾸면 데이터를 보내는 상황은 없지만 남아있을 순 있음
	for (int i = 0; i < NPC_ID_START; ++i) {
		CLIENT& cl = g_clients[i];
		if (g_clients[user_id].m_id == cl.m_id) continue; // 나와 다른 id가 같을 땐, lock을 걸면 안됨
		//cl.m_cl.lock();
		if (cl.m_status == ST_ACTIVE)
			send_leave_packet(cl.m_id, g_clients[user_id].m_id);
		//cl.m_cl.unlock();
	}
	g_clients[user_id].m_status = ST_FREE;
	g_clients[user_id].m_cl.unlock();
}

void recv_packet_construct(int user_id, int io_byte)
{
	CLIENT &cu = g_clients[user_id];
	EXOVER &r_o = cu.m_recv_over;

	int rest_byte = io_byte;
	char *p = r_o.io_buf; // 우리가 처리 해야할 데이터의 포인터 - 시작 부분이기에 io_buf의 처음
	int packet_size = 0;
	if (cu.m_prev_size != 0) packet_size = cu.m_packe_buf[0]; // 미리 받아둔 데이터가 있을 시 넣어줌

	// 처리할 데이터가 남아 있을 경우 처리
	while (rest_byte > 0) {
		if (packet_size == 0) packet_size = *p;

		// 남아 있는 데이터로 패킷을 만들 수 있는가?
		if (packet_size <= rest_byte + cu.m_prev_size) {
			memcpy(cu.m_packe_buf + cu.m_prev_size, p, packet_size - cu.m_prev_size);
			p += packet_size - cu.m_prev_size;
			rest_byte -= packet_size - cu.m_prev_size;
			packet_size = 0;
			process_packet(user_id, cu.m_packe_buf);
			// 패킷 버퍼에 있는 것을 모두 소모 했으므로 초기화
			cu.m_prev_size = 0;
		}
		else { // 패킷을 완성할 수 없는 경우엔 나머지를 다음 번에 보내줄 수 있도록 저장
			memcpy(cu.m_packe_buf + cu.m_prev_size, p, rest_byte);
			cu.m_prev_size += rest_byte;
			rest_byte = 0;
			p += rest_byte;
		}
	}
}

void worker_thread()
{
	while (true) {
		// 일반 accept는 data를 받기 전까지 멈춰 있기 때문에 AcceptEx 사용
		//SOCKET c_socket = accept(l_socket, reinterpret_cast<sockaddr*>(&c_address), &c_addr_size);
		DWORD io_byte;
		ULONG_PTR key;
		WSAOVERLAPPED *over;
		// Recv를 했으니 Callback을 받아야 함 (AcceptEx로 바꾸기 전)
		// Blocking - Client에 Data를 보내기 전까지는 계속 정지 상태 (AcceptEx로 바꾸기 전)
		GetQueuedCompletionStatus(g_iocp, &io_byte, &key, &over, INFINITE);

		EXOVER *exover = reinterpret_cast<EXOVER*>(over);
		int user_id = static_cast<int>(key);
		CLIENT &cl = g_clients[user_id];

		// 자동 회복
		g_clients[user_id].m_cl.lock();
		if (is_auto_heal && g_clients[user_id].m_hp != MAX_HP && is_player(user_id)) {
			is_auto_heal = false;
			add_timer(user_id, OP_AUTO_HEAL, 5000);
		}
		g_clients[user_id].m_cl.unlock();

		// 오퍼레이션의 타입에 따라 해야할 일이 다름
		switch (exover->op) {
		case OP_RECV:
		{
			if (io_byte == 0) {
				disconnect(user_id);
				g_clients[user_id].db_id = -1;
			}
			else {
				// 패킷 재조립
				recv_packet_construct(user_id, io_byte);
				//process_packet(user_id, exover->io_buf);
				ZeroMemory(&cl.m_recv_over.over, sizeof(cl.m_recv_over.over));
				DWORD flag = 0;
				WSARecv(cl.m_s, &cl.m_recv_over.wsabuf, 1, NULL, &flag, &cl.m_recv_over.over, NULL);
			}
		}
		break;

		case OP_SEND:
			if (io_byte == 0) {
				disconnect(user_id);
				g_clients[user_id].db_id = -1;
		}
			delete exover;
			break;

		case OP_ACCEPT:
		{
			int user_id = -1;
			for (int i = 0; i < MAX_USER; ++i) {
				// lock guard 가 속한 지역({ })에서 빠져나갈 때, unlock을 해줌
				lock_guard<mutex> gl{ g_clients[i].m_cl };
				if (ST_FREE == g_clients[i].m_status) {
					g_clients[i].m_status = ST_ALLOC;
					user_id = i;
					break;
				}
			}
			//int user_id = g_curr_user_id++;
			//g_curr_user_id = g_curr_user_id % MAX_USER; // ID 재사용을 위해 넣은 코드
			SOCKET c_socket = exover->c_socket;
			// 서버 인원이 최대여서 user_id를 할당받지 못한 경우
			if (user_id == -1) closesocket(c_socket);
			else {
				// 소켓을 받았으니 이것을 IOCP에 등록
				CreateIoCompletionPort(reinterpret_cast<HANDLE>(c_socket), g_iocp, user_id, 0); // key 값은 우리가 id를 사용할 것이므로 id
				CLIENT &nc = g_clients[user_id];
				nc.m_id = user_id;
				nc.m_prev_size = 0;
				nc.m_recv_over.op = OP_RECV;
				ZeroMemory(&nc.m_recv_over.over, sizeof(nc.m_recv_over.over));
				nc.m_recv_over.wsabuf.buf = nc.m_recv_over.io_buf;
				nc.m_recv_over.wsabuf.len = MAX_BUF_SIZE;
				nc.m_s = c_socket;
				nc.m_view_list.clear();
				nc.x = rand() % WORLD_WIDTH;
				nc.y = rand() % WORLD_HEIGHT;
				DWORD flag = 0;
				WSARecv(c_socket, &nc.m_recv_over.wsabuf, 1, NULL, &flag, &nc.m_recv_over.over, NULL);
			}
			// 재사용을 위해 소켓과 오버랩드 구조체를 초기화
			c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			exover->c_socket = c_socket;
			ZeroMemory(&exover->over, sizeof(exover->over));

			// 다시 Accept 해주어야 함 - 다중 접속을 받기 위해 (accept를 받고 계속해서 유지)
			AcceptEx(l_socket, c_socket, exover->io_buf, NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &exover->over);
		}
		break;

		case OP_RANDOM_MOVE:
		{
			random_move_npc(user_id);
			bool keep_alive = false;
			// 주위에 플레이어가 있는지 검사 - 있으면 timer 실행하고 없으면 Sleep 상태로
			for (int i = 0; i < NPC_ID_START; ++i)
				if (true == is_near(user_id, i))
					if (ST_ACTIVE == g_clients[i].m_status) {
						keep_alive = true;
						break;
					}
			if (true == keep_alive) add_timer(user_id, OP_RANDOM_MOVE, 1000);
			else g_clients[user_id].m_status = ST_SLEEP;
		}
		break;

		//case OP_PLAYER_MOVE:
		//{
		//	// 함부로 호출하면 안되기 때문에 locking
		//	g_clients[user_id].lua_l.lock();
		//	// 주위 플레이어가 움직였으므로 LUA에 알려주어야 함
		//	lua_State* L = g_clients[user_id].L;
		//	lua_getglobal(L, "event_player_move");
		//	lua_pushnumber(L, exover->p_id);
		//	int error = lua_pcall(L, 1, 0, 0);
		//	if (error) cout << "Lua error : " << lua_tostring(L, -1) << endl;
		//	g_clients[user_id].lua_l.unlock();
		//	delete exover;
		//}
		//break;

		case OP_NPC_RESET:
		{
			g_clients[user_id].m_hp = 50;
			g_clients[user_id].m_level = 1 + rand() % 10;
			g_clients[user_id].x = rand() % WORLD_WIDTH;
			g_clients[user_id].y = rand() % WORLD_HEIGHT;
			g_clients[user_id].is_dead = false;
			delete exover;
		}
		break;

		case OP_PLAYER_RESET:
		{
			g_clients[user_id].m_hp = MAX_HP;
			g_clients[user_id].x = rand() % WORLD_WIDTH;
			g_clients[user_id].y = rand() % WORLD_HEIGHT;
			// 부활 하라는 신호를 클라이언트에게 전달
			send_recall_packet(user_id);
			send_state_packet(user_id, g_clients[user_id].m_hp, g_clients[user_id].m_level, g_clients[user_id].m_exp);
			delete exover;
		}
		break;

		case OP_AUTO_HEAL:
		{
			g_clients[user_id].m_cl.lock();
			if (g_clients[user_id].m_hp <= 0) {
				delete exover;
				return;
			}
			g_clients[user_id].m_hp += (MAX_HP / 20);
			if (g_clients[user_id].m_hp >= MAX_HP) g_clients[user_id].m_hp = MAX_HP;
			is_auto_heal = true;
			g_clients[user_id].m_cl.unlock();
			send_state_packet(user_id, g_clients[user_id].m_hp, g_clients[user_id].m_level, g_clients[user_id].m_exp);
			delete exover;
		}
		break;

		default:
			cout << "Unknown Operation in worker_thread!\n";
			while (true);
		}
	}
}

int API_send_message(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char *mess = (char *)lua_tostring(L, -1);

	send_chat_packet(user_id, my_id, mess);
	lua_pop(L, 3);

	return 0;
}

int API_get_x(lua_State* L)
{
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int x = g_clients[obj_id].x;
	lua_pushnumber(L, x);

	return 1; // 파라미터를 하나 보내주기에 return 1
}

int API_get_y(lua_State* L)
{
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int y = g_clients[obj_id].y;
	lua_pushnumber(L, y);

	return 1; // 파라미터를 하나 보내주기에 return 1
}

void init_npc()
{
	for (int i = NPC_ID_START; i < NPC_ID_START + NUM_NPC; ++i) {
		g_clients[i].m_s = 0; // socket은 사용하지 않기 때문에 0
		g_clients[i].m_id = i;
		g_clients[i].m_hp = 50;
		g_clients[i].m_level = 1 + rand() % 10;
		sprintf_s(g_clients[i].m_name, "Monster-%d (Level %d)", i, g_clients[i].m_level);
		g_clients[i].m_status = ST_SLEEP; // NPC가 Player 근처에 오면 Active 상태로 변경
		g_clients[i].x = rand() % WORLD_WIDTH;
		g_clients[i].y = rand() % WORLD_HEIGHT;
		g_clients[i].is_dead = false;
		//g_clinets[i].m_last_move_time = high_resolution_clock::now();
		//add_timer(i, OP_RANDOM_MOVE, 1000);

		// 가상 머신 초기화
		lua_State *L = g_clients[i].L = luaL_newstate();
		luaL_openlibs(L);
		luaL_loadfile(L, "NPC.LUA");
		lua_pcall(L, 0, 0, 0);
		lua_getglobal(L, "set_uid"); // set_uid 함수를 stack에 올림 - 사용하기 위해
		lua_pushnumber(L, i); // NPC의 ID
		lua_pcall(L, 1, 0, 0);
		lua_pop(L, 1); // getglobal 한 것을 없앰

		// API 등록
		lua_register(L, "API_send_message", API_send_message);
		lua_register(L, "API_get_x", API_get_x);
		lua_register(L, "API_get_y", API_get_y);
	}
}

void do_ai()
{
	// 20만마리가 동시에 움직이는 것은 오버헤드가 너무 큼
	// 시야 주변에 NPC만 움직이게 해야 함 -> 여러 개의 이벤트에 나눠서 해야 최적화 가능 (Timer)
	while (true) {
		for (int i = NPC_ID_START; i < NPC_ID_START + NUM_NPC; ++i) {
			// 현재 시간과 NPC가 가지고 있는 마지막 이동 시간이 1초보다 클 때 NPC를 이동하자
			if (high_resolution_clock::now() - g_clients[i].m_last_move_time > 1s) {
				random_move_npc(i);
				g_clients[i].m_last_move_time = high_resolution_clock::now();
			}
		}
	}
}

void do_timer()
{
	while (true) {
		// 계속 loop를 돌면 busy waiting이기 때문에 1ms 만큼 thread를 쉬자
		this_thread::sleep_for(1ms);
		// 1ms 마다 실행되지 않고 계속해서 실행될 수 있게 while문을 한번 더 걸어줌
		while (true) {
			timer_lock.lock();
			if (true == timer_queue.empty()) {
				timer_lock.unlock();
				break;
			}
			// 꺼낼 준비가 되지 않았으면 loop 재시작
			if (timer_queue.top().wakeup_time > high_resolution_clock::now()) {
				timer_lock.unlock();
				break;
			}
			event_type ev = timer_queue.top();
			// 꺼낼 때 메모리 복사가 일어날 수 있음 - 포인터를 넣고 free-list를 통해 재사용 등의 방법 존재
			timer_queue.pop();
			timer_lock.unlock();
			switch (ev.event_id) {
			case OP_RANDOM_MOVE:
			{
				EXOVER *over = new EXOVER;
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
				//random_move_npc(ev.obj_id);
				//add_timer(ev.obj_id, ev.event_id, 1000);
			}
			break;

			case OP_NPC_RESET:
			{
				EXOVER *over = new EXOVER;
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
			}
			break;

			case OP_PLAYER_RESET:
			{
				EXOVER *over = new EXOVER;
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
			}
			break;

			case OP_AUTO_HEAL:
			{
				EXOVER *over = new EXOVER;
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
			}
			break;
			}
		}
	}
}

/************************************************************************
/* HandleDiagnosticRecord : display error/warning information
/*
/* Parameters:
/* hHandle ODBC handle
/* hType Type of handle (SQL_HANDLE_STMT, SQL_HANDLE_ENV, SQL_HANDLE_DBC)
/* RetCode Return code of failing command
/************************************************************************/
void HandleDiagnosticRecord(SQLHANDLE hHandle, SQLSMALLINT hType, RETCODE RetCode)
{
	SQLSMALLINT iRec = 0;
	SQLINTEGER iError;
	WCHAR wszMessage[1000];
	WCHAR wszState[SQL_SQLSTATE_SIZE + 1];
	if (RetCode == SQL_INVALID_HANDLE) {
		fwprintf(stderr, L"Invalid handle!\n");
		return;
	}
	while (SQLGetDiagRec(hType, hHandle, ++iRec, wszState, &iError, wszMessage,
		(SQLSMALLINT)(sizeof(wszMessage) / sizeof(WCHAR)), (SQLSMALLINT *)NULL) == SQL_SUCCESS) {
		// Hide data truncated..
		if (wcsncmp(wszState, L"01004", 5)) {
			fwprintf(stderr, L"[%5.5s] %s (%d)\n", wszState, wszMessage, iError);
		}
	}
}

void load_database()
{
	// Read Data for DB - DB에 저장된 데이터를 읽자 (ODBC를 통해서) - SQLBindCol()
	// SQLBindCol() : DB에 들어있는 값과 C의 변수를 연결해줌
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	// Data를 읽을 변수
	SQLINTEGER dUser_id, dUser_hp, dUser_level, dUser_exp, dUser_x, dUser_y;
	// 실제 읽었을 때, 몇 byte를 읽었는지를 나타냄
	SQLLEN cbID = 0, cbHp = 0, cbLevel = 0, cbExp = 0, cbX = 0, cbY = 0;

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	// retcode의 값이 SQL_SUCCESS 이 0이 아니면 Error
	// HandleDiagnosticRecord 함수로 오류 발생 시 대처 (어떤 오류인지 파악)

	// Set the ODBC version environment attribute  
	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"gs_odbc_2015180006", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					//printf("ODBC connect OK!\n");
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

					// 필드, 테이블의 이름 맞춰주기
					// ORDER BY 1 - user_id의 순서대로 sort (1번 항목이 user_id 이기 때문)
					retcode = SQLExecDirect(hstmt, (SQLWCHAR *)L"SELECT user_id, user_hp, user_level, user_exp, user_x, user_y FROM user_data", SQL_NTS);
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						//printf("Select OK!\n");
						// Bind columns 1, 2, and 3  
						// SQLBindCol - 필드를 어떤 변수를 통해서 읽은 것인가
						// C 변수에 연결할 것이므로 SQL_"C"_LONG 을 넣어줌
						retcode = SQLBindCol(hstmt, 1, SQL_C_LONG, &dUser_id, 100, &cbID);
						retcode = SQLBindCol(hstmt, 2, SQL_C_LONG, &dUser_hp, 100, &cbHp);
						retcode = SQLBindCol(hstmt, 3, SQL_C_LONG, &dUser_level, 100, &cbLevel);
						retcode = SQLBindCol(hstmt, 4, SQL_C_LONG, &dUser_exp, 100, &cbExp);
						retcode = SQLBindCol(hstmt, 5, SQL_C_LONG, &dUser_x, 100, &cbX);
						retcode = SQLBindCol(hstmt, 6, SQL_C_LONG, &dUser_y, 100, &cbY);

						// Fetch and print each row of data. On an error, display a message and exit.  
						cout << "----------------------------------------------------------------\n";
						for (int i = 0; ; i++) {
							retcode = SQLFetch(hstmt);
							if (retcode == SQL_ERROR || retcode == SQL_SUCCESS_WITH_INFO)
								show_error();
							if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO)
							{
								wprintf(L"[%d] HP - %d, Level - %d Exp - %d  Pos - (x: %d, y: %d)\n", dUser_id, dUser_hp, dUser_level, dUser_exp, dUser_x, dUser_y);
								g_db_data[i].id = dUser_id;
								g_db_data[i].hp = dUser_hp;
								g_db_data[i].level = dUser_level;
								g_db_data[i].exp = dUser_exp;
								g_db_data[i].x = dUser_x;
								g_db_data[i].y = dUser_y;
								++db_user_size;
							}
							else
								break;
						}
					}
					// Process data
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt);
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}
				// Error로 인해 실패 했을 경우를 처리 - 어디서 오류인지에 따라 handle 값 설정
				else HandleDiagnosticRecord(hdbc, SQL_HANDLE_DBC, retcode);

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}
}

void insert_database(int id, int x, int y)
{
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	// Data를 읽을 변수
	SQLINTEGER dUser_id, dUser_x, dUser_y;
	// 실제 읽었을 때, 몇 byte를 읽었는지를 나타냄
	SQLLEN cbID = 0, cbX = 0, cbY = 0;

	// SQL 명령어
	SQLWCHAR sql[256];
	wsprintf(sql, L"INSERT user_data VALUES (%d, 100, 1, 0, %d, %d)", id, x, y);

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	// Set the ODBC version environment attribute  
	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"gs_odbc_2015180006", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					//printf("ODBC connect OK!\n");
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

					retcode = SQLExecDirect(hstmt, (SQLWCHAR *)sql, SQL_NTS);
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						//printf("Insert OK!\n");
					}
					else HandleDiagnosticRecord(hstmt, SQL_HANDLE_STMT, retcode);

					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt);
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}
				// Error로 인해 실패 했을 경우를 처리 - 어디서 오류인지에 따라 handle 값 설정
				else HandleDiagnosticRecord(hdbc, SQL_HANDLE_DBC, retcode);

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}
}

void update_database(int user_id, int db_id, int x, int y)
{
	// Read Data for DB - DB에 저장된 데이터를 읽자 (ODBC를 통해서) - SQLBindCol()
	// SQLBindCol() : DB에 들어있는 값과 C의 변수를 연결해줌
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	// Data를 읽을 변수
	SQLINTEGER dUser_id, dUser_x, dUser_y;
	// 실제 읽었을 때, 몇 byte를 읽었는지를 나타냄
	SQLLEN cbID = 0, cbX = 0, cbY = 0;

	// SQL 명령어
	SQLWCHAR sql[256];
	wsprintf(sql, L"UPDATE user_data SET user_hp = %d, user_level = %d, user_exp = %d, user_x = %d, user_y = %d WHERE user_id = %d",
					g_clients[user_id].m_hp, g_clients[user_id].m_level, g_clients[user_id].m_exp, x, y, db_id);

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	// Set the ODBC version environment attribute  
	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"gs_odbc_2015180006", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					//printf("ODBC connect OK!\n");
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

					retcode = SQLExecDirect(hstmt, (SQLWCHAR *)sql, SQL_NTS);
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						//printf("Update OK!\n");
					}
					else HandleDiagnosticRecord(hstmt, SQL_HANDLE_STMT, retcode);

					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt);
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}
				// Error로 인해 실패 했을 경우를 처리 - 어디서 오류인지에 따라 handle 값 설정
				else HandleDiagnosticRecord(hdbc, SQL_HANDLE_DBC, retcode);

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);

	// NPC Initialization
	init_npc();
	// Load DB
	load_database();

	l_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

	SOCKADDR_IN s_address;
	memset(&s_address, 0, sizeof(s_address));
	s_address.sin_family = AF_INET;
	s_address.sin_port = htons(SERVER_PORT);
	::bind(l_socket, reinterpret_cast<sockaddr*>(&s_address), sizeof(s_address));

	listen(l_socket, SOMAXCONN);

	/*SOCKADDR_IN c_address; - 기존 accept는 사용하지 않기 때문에 더 이상 필요 없음
	memset(&c_address, 0, sizeof(c_address));
	int c_addr_size = sizeof(c_address);*/

	// IOCP를 만들자
	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	
	initialize_clients();

	// Listen Socket을 IOCP로 받기 위해 생성
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(l_socket), g_iocp, 999, 0);

	SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	EXOVER accept_over;
	// 초기화
	ZeroMemory(&accept_over.over, sizeof(accept_over.over));
	accept_over.op = OP_ACCEPT;
	accept_over.c_socket = c_socket;

	// 기존에 사용하던 accept와 달리 클라이언트의 소켓을 미리 만들어 두어야 함 - CallBack에서 받아서 사용
	AcceptEx(l_socket, c_socket, accept_over.io_buf, NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &accept_over.over);

	vector<thread> worker_threads;
	for (int i = 0; i < 4; ++i) worker_threads.emplace_back(worker_thread);
	thread timer_thread{ do_timer };

	for (auto& th : worker_threads) th.join();

	//thread ai_thread{ do_ai };
	//ai_thread.join();
}