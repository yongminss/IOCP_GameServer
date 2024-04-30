#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h> // AcceptEx�� ����ϱ� ���� ����
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

// ������ �Ϸ�Ǿ��°��� �˱� ���� ����ϴ� enum type
enum ENUMOP { OP_RECV, OP_SEND, OP_ACCEPT, OP_RANDOM_MOVE, OP_NPC_RESET, OP_PLAYER_RESET, OP_AUTO_HEAL };
enum C_STATUS { ST_FREE, ST_ALLOC, ST_ACTIVE, ST_SLEEP };

struct event_type {
	int obj_id; // ���� ������Ʈ�� ������ �ؾ��ϴ°�
	ENUMOP event_id; // ���� �̺�Ʈ�ΰ� (Move, heal ...)
	high_resolution_clock::time_point wakeup_time; // ���� �̺�Ʈ�� ����Ǿ� �ϴ°�
	int target_id; // �߰� ����

	// �켱������ ���ϱ� ���� ������ �����ε�
	constexpr bool operator < (const event_type& left) const {
		return (wakeup_time > left.wakeup_time);
	}
};

priority_queue<event_type> timer_queue;
mutex timer_lock;

// OVERLAPPED ����ü�� ������ ����ؼ� ����ϴ� Ȯ�� ����ü
struct EXOVER {
	WSAOVERLAPPED	over;
	ENUMOP			op;
	char			io_buf[MAX_BUF_SIZE]; // ���� ���� ������ ���⿡ ����
	union {
		WSABUF			wsabuf; // ���۵��� �����ϴ� ���۷�, ���� �����Ϳ� ���̸� ����
		SOCKET			c_socket;
		int				p_id; // � �÷��̾ ���������� LUA�� �˸��� ���� ���
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
	high_resolution_clock::time_point m_last_move_time; // ���� �ð����� �����̵��� �ϱ� ���� ��� - NPC

	// �þ� ó���� ���� view-list - ���� �߿����� �ʱ� ������ unordered-set ���
	// view-list�� ���� thread ���� �а� ���� ������ data race �߻� ���� - lock �ʿ�
	unordered_set<int> m_view_list;

	// LUA Script�� ����ϱ� ���� ���� �ӽ��� ����
	lua_State* L;
	mutex lua_l;
};

CLIENT g_clients[NPC_ID_START + NUM_NPC];
HANDLE g_iocp; // IOCP�� HANDLE
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

DB_INFO g_db_data[MAX_USER]; // db�� ����� id�� ����
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
	// �þ� ó���� �� �÷��̾��� Ȯ��
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
	exover->wsabuf.len = buf[0]; // ��Ŷ�� ����
	memcpy(exover->io_buf, buf, buf[0]); // ��Ŷ�� ������ exover buffer�� ����

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

	// view-list�� �þ� ó���� �ٸ� �÷��̾���� �߰�
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

	// �þ� ó���Ǿ� ���� �÷��̾�� ����
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
	// Multi-Thread �̱� ������ add_timer�� 2�� ����� �� ����
	// State�� Active�� �ٲٴ� �Ϳ� ������ thread�� add_timer�� ȣ���ϰ� ����
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

	// �̵� ���� view-list - �����̱� ������ �� view-list�� ����, �̵� �Ŀ��� new�� ��
	g_clients[user_id].m_cl.lock();
	unordered_set<int> old_vl = g_clients[user_id].m_view_list;
	g_clients[user_id].m_cl.unlock();

	// �̵��� �÷��̾���� view-list�� ���� ���� - ��� �������� �𸣱� ���� (�� �ʿ�)
	unordered_set<int> new_vl;
	for (auto& cl : g_clients) {
		if (is_near(cl.m_id, user_id) == false) continue;
		if (cl.m_status == ST_SLEEP) activate_npc(cl.m_id);
		if (cl.m_status != ST_ACTIVE) continue;
		if (cl.m_id == user_id) continue;
		//if (false == is_player(cl.m_id)) { // �÷��̾ �������� ��, NPC Script�� ���� ���� ����
		//	EXOVER *over = new EXOVER;
		//	over->op = OP_PLAYER_MOVE;
		//	over->p_id = user_id;
		//	PostQueuedCompletionStatus(g_iocp, 1, cl.m_id, &over->over);
		//}
		new_vl.insert(cl.m_id);
	}
	// ���� �̵��� ���� �� �ڽſ��� �˷���
	send_move_packet(user_id, user_id);

	// �þ߿� ���� ���� �÷��̾�
	for (auto np : new_vl) {
		// old view-list�� ���ٰ�, new view-list�� ���� ��� - ���� ���� ���
		if (0 == old_vl.count(np)) {
			// ���� �þ߰� ������ �� ��ο��� ������� ��
			send_enter_packet(user_id, np);
			// �÷��̾ �ƴϸ� pass
			if (is_player(np) == false) continue;
			// ��, multi-thread �̱⿡, �̹� ���� ���ŵ��� ���� ���� - Ȯ�� �ʿ�
			g_clients[np].m_cl.lock();
			if (0 == g_clients[np].m_view_list.count(user_id)) {
				g_clients[np].m_cl.unlock();
				send_enter_packet(np, user_id);
			}
			// �̹� ������ view-list�� ���� ������, move packet�� ������
			else {
				g_clients[np].m_cl.unlock();
				send_move_packet(np, user_id);
			}
		}
		// ���� ������ �ʰ�, �������� view-list�� �ִ� ���̸�
		else {
			if (is_player(np) == false) continue;
			g_clients[np].m_cl.lock();
			// ������ �̵��ϸ鼭 ���� �þ߿��� ����� ���� ����
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

	// �þ߿��� ��� �÷��̾�
	for (auto op : old_vl) {
		// old view-list���� ������, new view-list���� ���� ���
		if (new_vl.count(op) == 0) {
			send_leave_packet(user_id, op);
			if (is_player(op) == false) continue; // NPC�� leave packet�� �� �ʿ䰡 ����
			// ������ view-list�� ���� ���� �� leave packet�� �ϸ� �ȵ�
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
			// ������ HP�� 0���Ϸ� �������� ���
			if (g_clients[other].m_hp <= 0) {
				g_clients[other].is_dead = true;
				// ���� óġ �̺�Ʈ �߻� (���� ��� ���, �÷��̾� ����ġ ����, 30�� �� ���� �����)
				char text[16];
				sprintf_s(text, "EXP + %d!", g_clients[other].m_level * 10);
				g_clients[user_id].m_cl.lock();
				g_clients[user_id].m_exp += (g_clients[other].m_level * 10);
				g_clients[user_id].m_cl.unlock();
				if (g_clients[user_id].m_exp > g_clients[user_id].m_rq_exp) {
					g_clients[user_id].m_cl.lock();
					++g_clients[user_id].m_level; // �䱸 ����ġ���� ����ġ�� �������� ���� ��
					g_clients[user_id].m_cl.unlock();
					g_clients[user_id].m_exp = 0;
				}
				send_chat_packet(user_id, other, text);
				send_state_packet(user_id, g_clients[user_id].m_hp, g_clients[user_id].m_level, g_clients[user_id].m_exp);
				g_clients[other].x = WORLD_WIDTH * 10;
				// NPC�� ������ ��ġ�� 30�� �Ŀ� �ٽ� ����
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
				// ID�� 24000 �̻��� NPC���� �÷��̾ �ѾƼ� ���� ��ġ ����
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

	// �̵��� ������ ������ �÷��̾�鿡�� �˷�����
	for (int i = 0; i < MAX_USER; ++i) {
		if (g_clients[i].m_status != ST_ACTIVE) continue;
		// �þ߿� ���̴� �÷��̾�Ը� �˷�����
		if (is_near(i, npc_id) == true) {
			g_clients[i].m_cl.lock();
			if (g_clients[i].m_view_list.count(npc_id) != 0) {
				g_clients[i].m_cl.unlock();
				send_move_packet(i, npc_id);

				// �Ժη� ȣ���ϸ� �ȵǱ� ������ locking
				g_clients[i].lua_l.lock();
				// ���� �÷��̾ ���������Ƿ� LUA�� �˷��־�� ��
				lua_State* L = g_clients[npc_id].L;
				lua_getglobal(L, "event_player_move");
				lua_pushnumber(L, i);
				int error = lua_pcall(L, 1, 0, 0);
				if (error) cout << "Lua error : " << lua_tostring(L, -1) << endl;
				g_clients[i].lua_l.unlock();

				// user�� npc�� �浹���� ���, hp�� ���ҽ�Ŵ
				if (g_clients[i].x == g_clients[npc_id].x && g_clients[i].y == g_clients[npc_id].y) {
					g_clients[i].m_hp -= g_clients[npc_id].m_level * 2;
					// �÷��̾��� HP�� 0���Ϸ� �������� ������� ���
					if (g_clients[i].m_hp <= 0) {
						g_clients[i].m_cl.lock();
						g_clients[i].m_hp = 0;
						g_clients[i].m_exp /= 2;
						g_clients[i].m_cl.unlock();
						// �׾��ٴ� ����� Ŭ���̾�Ʈ���� ����
						send_dead_packet(i);
						add_timer(i, OP_PLAYER_RESET, 1000); // 5�� �Ŀ� ��Ȱ
					}
					send_state_packet(i, g_clients[i].m_hp, g_clients[i].m_level, g_clients[i].m_exp);
				}
			}
			else {
				g_clients[i].m_cl.unlock();
				send_enter_packet(i, npc_id);
			}
		}
		// NPC�� �þ߿� ��� ��쿣 view-list���� ����
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
	// enter_game �� ����, true�� ����⿡ ���� lock �� �ʿ� ����
	// sum += 2 �� �ٸ� case, but ������ ������ �ൿ (�̿� lock�� �ɾ����� ���� �� ����)
	g_clients[user_id].m_cl.lock();

	// lock�� �ɾ��ֱ� ���� ������
	strcpy_s(g_clients[user_id].m_name, name);
	g_clients[user_id].m_name[MAX_ID_LEN] = NULL;

	// DB�� ���� �Ǿ� �ִ� x, y���� �־���
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

	// lock�� �ɰ� login-ok packet�� �����°� ���� ���� - �ٸ� ��� ��Ŷ���� ���� �;� ��
	// ���� ���� ���� ������, name setting ���� ���� ������ �ϴ��� �̷��� ���
	send_login_ok_packet(user_id);

	// �ڽ��� ���縦 �ٸ� Ŭ���̾�Ʈ�� ���� �˷� �� �Ŀ� Active ���·�
	// -> dead-lock ���� ������ ���� ���� -> login-ok packet���� ������ unlock�� ����
	g_clients[user_id].m_status = ST_ACTIVE; // ������ ����� Ŭ���̾�Ʈ�� �����ϱ� ���� ���
	g_clients[user_id].m_cl.unlock();
	// unlock�� ���� �����͸� ��� ���� ��, �ϴ°� ������ lock-unlock ������ ũ�� �ʹ� Ŀ�� (���ļ� ����)

	// �̹� ������ �÷��̾ ������ ������, ���� ������ �̹� ������ �÷��̾�鿡�� ����
	for (auto& cl : g_clients) {
		int i = cl.m_id;
		if (user_id == i) continue; // ���� lock�� �����ϱ� ���� user_id�� i�� ���� �� �н�

	// �þ� ó���� �ϱ� ����, ��� �÷��̾�� �ڽ��� packet ������ �ְ� ����
		if (true == is_near(user_id, i)) {
			if (ST_SLEEP == g_clients[i].m_status) // �÷��̾� ��ó�� �ִ� NPC�� Ȱ��ȭ
				activate_npc(i);
			// i��° client�� Data Race�� �Ͼ�� �ȵǱ� ������ lock
			//g_clinets[i].m_cl.lock();
			// -> ���� �߿� ACTIVE �ƴ� ���� �Ǿ��� ��, ���� �߻� ����
			// ���¸� �ٲٴ� thread ����, ���¿� ���� ��ȭ�� ���� ���� ���¸� �ٲ�� ��
			// ������ �ذ��� �� ������, atomic���� �������־� �޸� ���� ������ �ذ�������
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
		// �̹� �ִ� ID�� ��� ������ ���ϰ� ��
		for (int i = 0; i < MAX_USER; ++i) {
			if (g_clients[i].db_id != packet->id) {
				g_clients[user_id].db_id = packet->id;
				break;
			}
			disconnect(user_id);
			return;
		}
		
		// DB�� ����� ID�� ���� enter game
		for (int i = 0; i < db_user_size; ++i) {
			if (g_db_data[i].id == g_clients[user_id].db_id) {
				enter_game(user_id, packet->name);
				return;
			}
		}
		// ID�� ���� ���
		// DB�� ID�� �����ϴ� �Լ��� ȣ������
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
		// �޾ƿ� �޼����� view-list�� �ִ� �ٸ� player���Ը� ����
		for (auto cl : g_clients[user_id].m_view_list)
			if (is_player) send_chat_packet(cl, user_id, packet->mess);
	}
	break;

	default:
		cout << "Unknown Packet Type Error!" << endl;
		DebugBreak(); // ���⼭ ���߶�� ��
		exit(-1);
		break;
	}
}

void initialize_clients()
{
	// thread�� ���� �ʴ� �̱۾������ ���ư��� �Լ�
	// lock�� ���� �ʿ� ����
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
	// ������ ���� �� ��ǥ�� DB�� ����
	update_database(user_id, g_clients[user_id].db_id, g_clients[user_id].x, g_clients[user_id].y);

	// for������ �� �ڽ��� send_leave_packet() �Լ��� ȣ�������� �ʱ� ������ �̸� ����
	send_leave_packet(user_id, user_id);

	g_clients[user_id].m_cl.lock();
	// ���⼭ Free�� �ٲ� ���, ���� ���� �����͸� ó������ �ʾҴµ� ����� ���� �ֱ⿡ Allocated
	g_clients[user_id].m_status = ST_ALLOC;

	closesocket(g_clients[user_id].m_s);

	// m_connected �� false�� �ٲٸ� �����͸� ������ ��Ȳ�� ������ �������� �� ����
	for (int i = 0; i < NPC_ID_START; ++i) {
		CLIENT& cl = g_clients[i];
		if (g_clients[user_id].m_id == cl.m_id) continue; // ���� �ٸ� id�� ���� ��, lock�� �ɸ� �ȵ�
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
	char *p = r_o.io_buf; // �츮�� ó�� �ؾ��� �������� ������ - ���� �κ��̱⿡ io_buf�� ó��
	int packet_size = 0;
	if (cu.m_prev_size != 0) packet_size = cu.m_packe_buf[0]; // �̸� �޾Ƶ� �����Ͱ� ���� �� �־���

	// ó���� �����Ͱ� ���� ���� ��� ó��
	while (rest_byte > 0) {
		if (packet_size == 0) packet_size = *p;

		// ���� �ִ� �����ͷ� ��Ŷ�� ���� �� �ִ°�?
		if (packet_size <= rest_byte + cu.m_prev_size) {
			memcpy(cu.m_packe_buf + cu.m_prev_size, p, packet_size - cu.m_prev_size);
			p += packet_size - cu.m_prev_size;
			rest_byte -= packet_size - cu.m_prev_size;
			packet_size = 0;
			process_packet(user_id, cu.m_packe_buf);
			// ��Ŷ ���ۿ� �ִ� ���� ��� �Ҹ� �����Ƿ� �ʱ�ȭ
			cu.m_prev_size = 0;
		}
		else { // ��Ŷ�� �ϼ��� �� ���� ��쿣 �������� ���� ���� ������ �� �ֵ��� ����
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
		// �Ϲ� accept�� data�� �ޱ� ������ ���� �ֱ� ������ AcceptEx ���
		//SOCKET c_socket = accept(l_socket, reinterpret_cast<sockaddr*>(&c_address), &c_addr_size);
		DWORD io_byte;
		ULONG_PTR key;
		WSAOVERLAPPED *over;
		// Recv�� ������ Callback�� �޾ƾ� �� (AcceptEx�� �ٲٱ� ��)
		// Blocking - Client�� Data�� ������ �������� ��� ���� ���� (AcceptEx�� �ٲٱ� ��)
		GetQueuedCompletionStatus(g_iocp, &io_byte, &key, &over, INFINITE);

		EXOVER *exover = reinterpret_cast<EXOVER*>(over);
		int user_id = static_cast<int>(key);
		CLIENT &cl = g_clients[user_id];

		// �ڵ� ȸ��
		g_clients[user_id].m_cl.lock();
		if (is_auto_heal && g_clients[user_id].m_hp != MAX_HP && is_player(user_id)) {
			is_auto_heal = false;
			add_timer(user_id, OP_AUTO_HEAL, 5000);
		}
		g_clients[user_id].m_cl.unlock();

		// ���۷��̼��� Ÿ�Կ� ���� �ؾ��� ���� �ٸ�
		switch (exover->op) {
		case OP_RECV:
		{
			if (io_byte == 0) {
				disconnect(user_id);
				g_clients[user_id].db_id = -1;
			}
			else {
				// ��Ŷ ������
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
				// lock guard �� ���� ����({ })���� �������� ��, unlock�� ����
				lock_guard<mutex> gl{ g_clients[i].m_cl };
				if (ST_FREE == g_clients[i].m_status) {
					g_clients[i].m_status = ST_ALLOC;
					user_id = i;
					break;
				}
			}
			//int user_id = g_curr_user_id++;
			//g_curr_user_id = g_curr_user_id % MAX_USER; // ID ������ ���� ���� �ڵ�
			SOCKET c_socket = exover->c_socket;
			// ���� �ο��� �ִ뿩�� user_id�� �Ҵ���� ���� ���
			if (user_id == -1) closesocket(c_socket);
			else {
				// ������ �޾����� �̰��� IOCP�� ���
				CreateIoCompletionPort(reinterpret_cast<HANDLE>(c_socket), g_iocp, user_id, 0); // key ���� �츮�� id�� ����� ���̹Ƿ� id
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
			// ������ ���� ���ϰ� �������� ����ü�� �ʱ�ȭ
			c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			exover->c_socket = c_socket;
			ZeroMemory(&exover->over, sizeof(exover->over));

			// �ٽ� Accept ���־�� �� - ���� ������ �ޱ� ���� (accept�� �ް� ����ؼ� ����)
			AcceptEx(l_socket, c_socket, exover->io_buf, NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &exover->over);
		}
		break;

		case OP_RANDOM_MOVE:
		{
			random_move_npc(user_id);
			bool keep_alive = false;
			// ������ �÷��̾ �ִ��� �˻� - ������ timer �����ϰ� ������ Sleep ���·�
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
		//	// �Ժη� ȣ���ϸ� �ȵǱ� ������ locking
		//	g_clients[user_id].lua_l.lock();
		//	// ���� �÷��̾ ���������Ƿ� LUA�� �˷��־�� ��
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
			// ��Ȱ �϶�� ��ȣ�� Ŭ���̾�Ʈ���� ����
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

	return 1; // �Ķ���͸� �ϳ� �����ֱ⿡ return 1
}

int API_get_y(lua_State* L)
{
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int y = g_clients[obj_id].y;
	lua_pushnumber(L, y);

	return 1; // �Ķ���͸� �ϳ� �����ֱ⿡ return 1
}

void init_npc()
{
	for (int i = NPC_ID_START; i < NPC_ID_START + NUM_NPC; ++i) {
		g_clients[i].m_s = 0; // socket�� ������� �ʱ� ������ 0
		g_clients[i].m_id = i;
		g_clients[i].m_hp = 50;
		g_clients[i].m_level = 1 + rand() % 10;
		sprintf_s(g_clients[i].m_name, "Monster-%d (Level %d)", i, g_clients[i].m_level);
		g_clients[i].m_status = ST_SLEEP; // NPC�� Player ��ó�� ���� Active ���·� ����
		g_clients[i].x = rand() % WORLD_WIDTH;
		g_clients[i].y = rand() % WORLD_HEIGHT;
		g_clients[i].is_dead = false;
		//g_clinets[i].m_last_move_time = high_resolution_clock::now();
		//add_timer(i, OP_RANDOM_MOVE, 1000);

		// ���� �ӽ� �ʱ�ȭ
		lua_State *L = g_clients[i].L = luaL_newstate();
		luaL_openlibs(L);
		luaL_loadfile(L, "NPC.LUA");
		lua_pcall(L, 0, 0, 0);
		lua_getglobal(L, "set_uid"); // set_uid �Լ��� stack�� �ø� - ����ϱ� ����
		lua_pushnumber(L, i); // NPC�� ID
		lua_pcall(L, 1, 0, 0);
		lua_pop(L, 1); // getglobal �� ���� ����

		// API ���
		lua_register(L, "API_send_message", API_send_message);
		lua_register(L, "API_get_x", API_get_x);
		lua_register(L, "API_get_y", API_get_y);
	}
}

void do_ai()
{
	// 20�������� ���ÿ� �����̴� ���� ������尡 �ʹ� ŭ
	// �þ� �ֺ��� NPC�� �����̰� �ؾ� �� -> ���� ���� �̺�Ʈ�� ������ �ؾ� ����ȭ ���� (Timer)
	while (true) {
		for (int i = NPC_ID_START; i < NPC_ID_START + NUM_NPC; ++i) {
			// ���� �ð��� NPC�� ������ �ִ� ������ �̵� �ð��� 1�ʺ��� Ŭ �� NPC�� �̵�����
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
		// ��� loop�� ���� busy waiting�̱� ������ 1ms ��ŭ thread�� ����
		this_thread::sleep_for(1ms);
		// 1ms ���� ������� �ʰ� ����ؼ� ����� �� �ְ� while���� �ѹ� �� �ɾ���
		while (true) {
			timer_lock.lock();
			if (true == timer_queue.empty()) {
				timer_lock.unlock();
				break;
			}
			// ���� �غ� ���� �ʾ����� loop �����
			if (timer_queue.top().wakeup_time > high_resolution_clock::now()) {
				timer_lock.unlock();
				break;
			}
			event_type ev = timer_queue.top();
			// ���� �� �޸� ���簡 �Ͼ �� ���� - �����͸� �ְ� free-list�� ���� ���� ���� ��� ����
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
	// Read Data for DB - DB�� ����� �����͸� ���� (ODBC�� ���ؼ�) - SQLBindCol()
	// SQLBindCol() : DB�� ����ִ� ���� C�� ������ ��������
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	// Data�� ���� ����
	SQLINTEGER dUser_id, dUser_hp, dUser_level, dUser_exp, dUser_x, dUser_y;
	// ���� �о��� ��, �� byte�� �о������� ��Ÿ��
	SQLLEN cbID = 0, cbHp = 0, cbLevel = 0, cbExp = 0, cbX = 0, cbY = 0;

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	// retcode�� ���� SQL_SUCCESS �� 0�� �ƴϸ� Error
	// HandleDiagnosticRecord �Լ��� ���� �߻� �� ��ó (� �������� �ľ�)

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

					// �ʵ�, ���̺��� �̸� �����ֱ�
					// ORDER BY 1 - user_id�� ������� sort (1�� �׸��� user_id �̱� ����)
					retcode = SQLExecDirect(hstmt, (SQLWCHAR *)L"SELECT user_id, user_hp, user_level, user_exp, user_x, user_y FROM user_data", SQL_NTS);
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						//printf("Select OK!\n");
						// Bind columns 1, 2, and 3  
						// SQLBindCol - �ʵ带 � ������ ���ؼ� ���� ���ΰ�
						// C ������ ������ ���̹Ƿ� SQL_"C"_LONG �� �־���
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
				// Error�� ���� ���� ���� ��츦 ó�� - ��� ���������� ���� handle �� ����
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
	// Data�� ���� ����
	SQLINTEGER dUser_id, dUser_x, dUser_y;
	// ���� �о��� ��, �� byte�� �о������� ��Ÿ��
	SQLLEN cbID = 0, cbX = 0, cbY = 0;

	// SQL ��ɾ�
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
				// Error�� ���� ���� ���� ��츦 ó�� - ��� ���������� ���� handle �� ����
				else HandleDiagnosticRecord(hdbc, SQL_HANDLE_DBC, retcode);

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}
}

void update_database(int user_id, int db_id, int x, int y)
{
	// Read Data for DB - DB�� ����� �����͸� ���� (ODBC�� ���ؼ�) - SQLBindCol()
	// SQLBindCol() : DB�� ����ִ� ���� C�� ������ ��������
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	// Data�� ���� ����
	SQLINTEGER dUser_id, dUser_x, dUser_y;
	// ���� �о��� ��, �� byte�� �о������� ��Ÿ��
	SQLLEN cbID = 0, cbX = 0, cbY = 0;

	// SQL ��ɾ�
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
				// Error�� ���� ���� ���� ��츦 ó�� - ��� ���������� ���� handle �� ����
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

	/*SOCKADDR_IN c_address; - ���� accept�� ������� �ʱ� ������ �� �̻� �ʿ� ����
	memset(&c_address, 0, sizeof(c_address));
	int c_addr_size = sizeof(c_address);*/

	// IOCP�� ������
	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	
	initialize_clients();

	// Listen Socket�� IOCP�� �ޱ� ���� ����
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(l_socket), g_iocp, 999, 0);

	SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	EXOVER accept_over;
	// �ʱ�ȭ
	ZeroMemory(&accept_over.over, sizeof(accept_over.over));
	accept_over.op = OP_ACCEPT;
	accept_over.c_socket = c_socket;

	// ������ ����ϴ� accept�� �޸� Ŭ���̾�Ʈ�� ������ �̸� ����� �ξ�� �� - CallBack���� �޾Ƽ� ���
	AcceptEx(l_socket, c_socket, accept_over.io_buf, NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &accept_over.over);

	vector<thread> worker_threads;
	for (int i = 0; i < 4; ++i) worker_threads.emplace_back(worker_thread);
	thread timer_thread{ do_timer };

	for (auto& th : worker_threads) th.join();

	//thread ai_thread{ do_ai };
	//ai_thread.join();
}