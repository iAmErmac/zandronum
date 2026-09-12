#include "zandronum_android_input.h"

#ifdef __ANDROID__

#include <algorithm>
#include <atomic>

#include "c_console.h"
#include "c_dispatch.h"
#include "chat.h"
#include "d_event.h"
#include "d_gui.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_joy.h"
#include "menu/menu.h"
#include "sdl/dikeys.h"

static std::atomic<float> AndroidAxes[2] = {};
static std::atomic<int> AndroidLookX = 0;
static std::atomic<int> AndroidLookY = 0;
static FButtonStatus AndroidTouchJump;
static FButtonStatus AndroidTouchCrouch;

enum AndroidAction
{
	ANDROID_ACTION_FIRE = 0,
	ANDROID_ACTION_USE,
	ANDROID_ACTION_JUMP,
	ANDROID_ACTION_CROUCH,
	ANDROID_ACTION_RUN,
	ANDROID_ACTION_NEXT,
	ANDROID_ACTION_PREV,
	ANDROID_ACTION_MENU,
	ANDROID_ACTION_CONSOLE,
	ANDROID_ACTION_AUTOMAP,
	ANDROID_ACTION_CHAT,
	ANDROID_ACTION_TEAM,
	ANDROID_ACTION_SCORE,
	ANDROID_ACTION_VOTE_YES,
	ANDROID_ACTION_VOTE_NO,
	ANDROID_ACTION_TAUNT,
	ANDROID_ACTION_CHASECAM,
	ANDROID_ACTION_ZOOM,
	ANDROID_ACTION_RELOAD,
	ANDROID_ACTION_ITEM_PREV = 23,
	ANDROID_ACTION_ITEM_NEXT,
	ANDROID_ACTION_DROP_ITEM,
	ANDROID_ACTION_DROP_WEAPON,
	ANDROID_ACTION_MENU_UP,
	ANDROID_ACTION_MENU_DOWN,
	ANDROID_ACTION_MENU_LEFT,
	ANDROID_ACTION_MENU_RIGHT,
};

static constexpr int AndroidTouchKeyBase = 0x7000;

static void PostKey(int keycode, bool pressed)
{
	event_t event = {};
	event.type = pressed ? EV_KeyDown : EV_KeyUp;
	event.data1 = static_cast<SWORD>(keycode);
	D_PostEvent(&event);
}

static void PostGuiKey(int keycode, bool pressed)
{
	event_t event = {};
	event.type = EV_GUI_Event;
	event.subtype = pressed ? EV_GUI_KeyDown : EV_GUI_KeyUp;
	event.data1 = static_cast<SWORD>(keycode);
	D_PostEvent(&event);
}

EXTERN_CVAR(Float, m_pitch)
EXTERN_CVAR(Float, m_yaw)
EXTERN_CVAR(Bool, invertmouse)

void Zandronum_AndroidInput_Key(int keycode, bool pressed)
{
	PostKey(keycode, pressed);
}

void Zandronum_AndroidInput_GuiKey(int keycode, bool pressed)
{
	PostGuiKey(keycode, pressed);
}

void Zandronum_AndroidInput_Text(int codepoint)
{
	event_t event = {};
	event.type = EV_GUI_Event;
	event.subtype = EV_GUI_Char;
	event.data1 = static_cast<SWORD>(std::max(-32768, std::min(32767, codepoint)));
	D_PostEvent(&event);
}

void Zandronum_AndroidInput_Pointer(int, int action, float x, float y)
{
	// Gameplay touch controls use key actions; pointer events are for engine menus.
	if (menuactive == MENU_Off && ConsoleState == c_up)
		return;
	event_t event = {};
	event.type = EV_GUI_Event;
	event.x = static_cast<int>(x);
	event.y = static_cast<int>(y);
	switch (action)
	{
	case 0:
	case 5: event.subtype = EV_GUI_LButtonDown; break;
	case 1:
	case 6: event.subtype = EV_GUI_LButtonUp; break;
	default: event.subtype = EV_GUI_MouseMove; break;
	}
	D_PostEvent(&event);
}

void Zandronum_AndroidInput_Axis(int axis, float value)
{
	if (axis >= 0 && axis < 2)
	{
		AndroidAxes[axis].store(std::max(-1.0f, std::min(1.0f, value)));
	}
}

void Zandronum_AndroidInput_Look(int deltaX, int deltaY)
{
	AndroidLookX.fetch_add(deltaX);
	AndroidLookY.fetch_add(deltaY);
}

void Zandronum_AndroidInput_Reset()
{
	AndroidAxes[0].store(0.0f);
	AndroidAxes[1].store(0.0f);
	AndroidLookX.store(0);
	AndroidLookY.store(0);
	AndroidTouchJump.Reset();
	AndroidTouchCrouch.Reset();
}

bool Zandronum_AndroidInput_FlyActive()
{
	const player_t &player = players[consoleplayer];
	return player.mo != nullptr &&
		(((player.mo->flags2 & MF2_FLY) != 0) || ((player.cheats & CF_FLY) != 0));
}

bool Zandronum_AndroidInput_TouchJumpDown()
{
	return AndroidTouchJump.bDown != 0;
}

bool Zandronum_AndroidInput_TouchCrouchDown()
{
	return AndroidTouchCrouch.bDown != 0;
}

void Zandronum_AndroidInput_ApplyAxes(float axes[])
{
	if (menuactive != MENU_Off)
	{
		AndroidLookX.exchange(0);
		AndroidLookY.exchange(0);
		return;
	}

	const float lookX = static_cast<float>(AndroidLookX.exchange(0));
	float lookY = -static_cast<float>(AndroidLookY.exchange(0));
	if (invertmouse)
		lookY = -lookY;
	axes[JOYAXIS_Yaw] += -lookX * m_yaw * mouse_sensitivity * 8.0f / 1280.0f;
	axes[JOYAXIS_Pitch] += lookY * m_pitch * mouse_sensitivity * 16.0f / 2048.0f;
	axes[JOYAXIS_Forward] += -AndroidAxes[1].load();
	axes[JOYAXIS_Side] += -AndroidAxes[0].load();
}

void Zandronum_AndroidInput_Action(int action, bool pressed)
{
	event_t event = {};
	event.type = EV_AndroidAction;
	event.data1 = static_cast<SWORD>(action);
	event.data2 = pressed ? 1 : 0;
	D_PostEvent(&event);
}

void Zandronum_AndroidInput_MenuAction(int direction, bool pressed)
{
	if (direction < 0 || direction > 3)
		return;
	Zandronum_AndroidInput_Action(ANDROID_ACTION_MENU_UP + direction, pressed);
}

void Zandronum_AndroidInput_ApplyAction(int action, bool pressed)
{
	static bool menuFireDown = false;

	if (action >= ANDROID_ACTION_MENU_UP && action <= ANDROID_ACTION_MENU_RIGHT)
	{
		if (menuactive != MENU_Off)
		{
			static constexpr int menuKeys[] = {
				KEY_PAD_DPAD_UP, KEY_PAD_DPAD_DOWN, KEY_PAD_DPAD_LEFT, KEY_PAD_DPAD_RIGHT
			};
			PostKey(menuKeys[action - ANDROID_ACTION_MENU_UP], pressed);
		}
		return;
	}

	if (action == ANDROID_ACTION_MENU)
	{
		if (menuactive == MENU_Off)
		{
			if (pressed)
			{
				M_StartControlPanel(true);
				M_SetMenu(NAME_Mainmenu, -1);
			}
		}
		else if (pressed && DMenu::CurrentMenu != nullptr)
			DMenu::CurrentMenu->MenuEvent(MKEY_Back, true);
		return;
	}

	if (action == ANDROID_ACTION_FIRE)
	{
		if (pressed && menuactive != MENU_Off)
		{
			menuFireDown = true;
			PostGuiKey(GK_RETURN, true);
			return;
		}
		if (!pressed && menuFireDown)
		{
			menuFireDown = false;
			PostGuiKey(GK_RETURN, false);
			return;
		}
	}

	FButtonStatus *button = nullptr;

	switch (action)
	{
	case ANDROID_ACTION_FIRE: button = &Button_Attack; break;
	case ANDROID_ACTION_USE: button = &Button_Use; break;
	case ANDROID_ACTION_JUMP: button = &AndroidTouchJump; break;
	case ANDROID_ACTION_CROUCH: button = &AndroidTouchCrouch; break;
	case ANDROID_ACTION_RUN: button = &Button_Speed; break;
	case ANDROID_ACTION_SCORE: button = &Button_ShowScores; break;
	case ANDROID_ACTION_ZOOM: button = &Button_Zoom; break;
	case ANDROID_ACTION_RELOAD: button = &Button_Reload; break;
	default: break;
	}

	if (button != nullptr)
	{
		const int key = AndroidTouchKeyBase + action;
		if (pressed)
			button->PressKey(key);
		else
			button->ReleaseKey(key);
		return;
	}

	if (!pressed)
		return;

	switch (action)
	{
	case ANDROID_ACTION_NEXT: C_DoCommand("weapnext"); break;
	case ANDROID_ACTION_PREV: C_DoCommand("weapprev"); break;
	case ANDROID_ACTION_ITEM_PREV: C_DoCommand("invprev"); break;
	case ANDROID_ACTION_ITEM_NEXT: C_DoCommand("invnext"); break;
	case ANDROID_ACTION_DROP_ITEM: C_DoCommand("invdrop"); break;
	case ANDROID_ACTION_DROP_WEAPON: C_DoCommand("weapdrop"); break;
	case ANDROID_ACTION_CONSOLE: C_ToggleConsole(); break;
	case ANDROID_ACTION_AUTOMAP: C_DoCommand("togglemap"); break;
	case ANDROID_ACTION_CHAT:
		if (CHAT_GetChatMode() != CHATMODE_NONE)
			CHAT_SetChatMode(CHATMODE_NONE);
		else
			C_DoCommand("say");
		break;
	case ANDROID_ACTION_TEAM:
		if (CHAT_GetChatMode() != CHATMODE_NONE)
			CHAT_SetChatMode(CHATMODE_NONE);
		else
			C_DoCommand("say_team");
		break;
	case ANDROID_ACTION_VOTE_YES: C_DoCommand("vote_yes"); break;
	case ANDROID_ACTION_VOTE_NO: C_DoCommand("vote_no"); break;
	case ANDROID_ACTION_TAUNT: C_DoCommand("taunt"); break;
	case ANDROID_ACTION_CHASECAM: C_DoCommand("chase"); break;
	default: break;
	}
}

#endif
