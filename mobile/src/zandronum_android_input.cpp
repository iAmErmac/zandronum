#include "zandronum_android_input.h"

#ifdef __ANDROID__

#include <android/input.h>
#include <android/keycodes.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>

#include "c_console.h"
#include "c_dispatch.h"
#include "chat.h"
#include "d_event.h"
#include "d_gui.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_joy.h"
#include "menu/menu.h"
#include "scoreboard.h"
#include "sdl/dikeys.h"

static std::atomic<float> AndroidTouchAxes[2] = {};
static std::atomic<float> AndroidControllerAxes[4] = {};
static std::atomic<int> AndroidLookX = 0;
static std::atomic<int> AndroidLookY = 0;
static std::atomic<int> AndroidOverlayMode = 0;
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
	ANDROID_ACTION_USE_ITEM = 39,
	ANDROID_ACTION_SCORE_SCROLL_UP = 40,
	ANDROID_ACTION_SCORE_SCROLL_DOWN,
};

static constexpr int AndroidTouchKeyBase = 0x7000;
static constexpr int AndroidTouchScoreboardKeyBase = AndroidTouchKeyBase + 0x40;

static void PostKey(int keycode, bool pressed, int character = 0)
{
	event_t event = {};
	event.type = pressed ? EV_KeyDown : EV_KeyUp;
	event.data1 = static_cast<SWORD>(keycode);
	event.data2 = static_cast<SWORD>(character);
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

static bool AndroidInput_TitleMenuKey()
{
	return gameaction == ga_nothing &&
		(demoplayback || gamestate == GS_DEMOSCREEN || gamestate == GS_TITLELEVEL);
}

EXTERN_CVAR(Float, m_pitch)
EXTERN_CVAR(Float, m_yaw)
EXTERN_CVAR(Bool, invertmouse)

static event_t TranslateKey(int keycode, bool pressed)
{
	int character = 0;
	int translated = 0;
	if (keycode >= AKEYCODE_A && keycode <= AKEYCODE_Z)
	{
		static constexpr int letterKeys[] = {
			DIK_A, DIK_B, DIK_C, DIK_D, DIK_E, DIK_F, DIK_G, DIK_H, DIK_I,
			DIK_J, DIK_K, DIK_L, DIK_M, DIK_N, DIK_O, DIK_P, DIK_Q, DIK_R,
			DIK_S, DIK_T, DIK_U, DIK_V, DIK_W, DIK_X, DIK_Y, DIK_Z
		};
		const int letter = keycode - AKEYCODE_A;
		translated = letterKeys[letter];
		character = 'a' + letter;
	}
	else if (keycode >= AKEYCODE_0 && keycode <= AKEYCODE_9)
	{
		const int digit = keycode - AKEYCODE_0;
		translated = digit == 0 ? DIK_0 : DIK_1 + digit - 1;
		character = '0' + digit;
	}
	else if (keycode >= AKEYCODE_NUMPAD_0 && keycode <= AKEYCODE_NUMPAD_9)
	{
		static constexpr int numpadKeys[] = {
			DIK_NUMPAD0, DIK_NUMPAD1, DIK_NUMPAD2, DIK_NUMPAD3, DIK_NUMPAD4,
			DIK_NUMPAD5, DIK_NUMPAD6, DIK_NUMPAD7, DIK_NUMPAD8, DIK_NUMPAD9
		};
		translated = numpadKeys[keycode - AKEYCODE_NUMPAD_0];
	}
	else if (keycode >= AKEYCODE_F1 && keycode <= AKEYCODE_F10)
	{
		translated = DIK_F1 + keycode - AKEYCODE_F1;
	}
	else
	{
		switch (keycode)
		{
		case AKEYCODE_BACK:
		case AKEYCODE_ESCAPE: translated = DIK_ESCAPE; break;
		case AKEYCODE_DPAD_UP: translated = DIK_UP; break;
		case AKEYCODE_DPAD_DOWN: translated = DIK_DOWN; break;
		case AKEYCODE_DPAD_LEFT: translated = DIK_LEFT; break;
		case AKEYCODE_DPAD_RIGHT: translated = DIK_RIGHT; break;
		case AKEYCODE_TAB: translated = DIK_TAB; break;
		case AKEYCODE_SPACE: translated = DIK_SPACE; character = ' '; break;
		case AKEYCODE_ENTER: translated = DIK_RETURN; character = '\r'; break;
		case AKEYCODE_DEL: translated = DIK_BACK; break;
		case AKEYCODE_FORWARD_DEL: translated = DIK_DELETE; break;
		case AKEYCODE_PAGE_UP: translated = DIK_PRIOR; break;
		case AKEYCODE_PAGE_DOWN: translated = DIK_NEXT; break;
		case AKEYCODE_CTRL_LEFT: translated = DIK_LCONTROL; break;
		case AKEYCODE_CTRL_RIGHT: translated = DIK_RCONTROL; break;
		case AKEYCODE_ALT_LEFT: translated = DIK_LMENU; break;
		case AKEYCODE_ALT_RIGHT: translated = DIK_RMENU; break;
		case AKEYCODE_SHIFT_LEFT: translated = DIK_LSHIFT; break;
		case AKEYCODE_SHIFT_RIGHT: translated = DIK_RSHIFT; break;
		case AKEYCODE_META_LEFT: translated = DIK_LWIN; break;
		case AKEYCODE_META_RIGHT: translated = DIK_RWIN; break;
		case AKEYCODE_CAPS_LOCK: translated = DIK_CAPITAL; break;
		case AKEYCODE_NUM_LOCK: translated = DIK_NUMLOCK; break;
		case AKEYCODE_SCROLL_LOCK: translated = DIK_SCROLL; break;
		case AKEYCODE_SYSRQ: translated = DIK_SYSRQ; break;
		case AKEYCODE_BREAK: translated = DIK_PAUSE; break;
		case AKEYCODE_MOVE_HOME: translated = DIK_HOME; break;
		case AKEYCODE_MOVE_END: translated = DIK_END; break;
		case AKEYCODE_INSERT: translated = DIK_INSERT; break;
		case AKEYCODE_MENU: translated = DIK_APPS; break;
		case AKEYCODE_F11: translated = DIK_F11; break;
		case AKEYCODE_F12: translated = DIK_F12; break;
		case AKEYCODE_GRAVE: translated = DIK_GRAVE; character = '`'; break;
		case AKEYCODE_MINUS: translated = DIK_MINUS; character = '-'; break;
		case AKEYCODE_EQUALS: translated = DIK_EQUALS; character = '='; break;
		case AKEYCODE_LEFT_BRACKET: translated = DIK_LBRACKET; character = '['; break;
		case AKEYCODE_RIGHT_BRACKET: translated = DIK_RBRACKET; character = ']'; break;
		case AKEYCODE_BACKSLASH: translated = DIK_BACKSLASH; character = '\\'; break;
		case AKEYCODE_SEMICOLON: translated = DIK_SEMICOLON; character = ';'; break;
		case AKEYCODE_APOSTROPHE: translated = DIK_APOSTROPHE; character = '\''; break;
		case AKEYCODE_COMMA: translated = DIK_COMMA; character = ','; break;
		case AKEYCODE_PERIOD: translated = DIK_PERIOD; character = '.'; break;
		case AKEYCODE_SLASH: translated = DIK_SLASH; character = '/'; break;
		case AKEYCODE_NUMPAD_DIVIDE: translated = DIK_DIVIDE; break;
		case AKEYCODE_NUMPAD_MULTIPLY: translated = DIK_MULTIPLY; break;
		case AKEYCODE_NUMPAD_SUBTRACT: translated = DIK_SUBTRACT; break;
		case AKEYCODE_NUMPAD_ADD: translated = DIK_ADD; break;
		case AKEYCODE_NUMPAD_DOT: translated = DIK_DECIMAL; break;
		case AKEYCODE_NUMPAD_COMMA: translated = DIK_NUMPADCOMMA; break;
		case AKEYCODE_NUMPAD_ENTER: translated = DIK_NUMPADENTER; character = '\r'; break;
		case AKEYCODE_NUMPAD_EQUALS: translated = DIK_NUMPAD_EQUALS; break;
		default: break;
		}
	}
	event_t event = {};
	event.type = pressed ? EV_KeyDown : EV_KeyUp;
	event.data1 = static_cast<SWORD>(translated);
	event.data2 = static_cast<SWORD>(character);
	return event;
}

void Zandronum_AndroidInput_Key(int keycode, bool pressed)
{
	const event_t event = TranslateKey(keycode, pressed);
	if (event.data1 != 0) D_PostEvent(&event);
}

void Zandronum_AndroidInput_KeyboardKey(int keycode, bool pressed, int codepoint, int metaState, bool repeat)
{
	event_t event = TranslateKey(keycode, pressed);
	event.type = EV_AndroidKeyboard;
	event.subtype = pressed ? (repeat ? EV_GUI_KeyRepeat : EV_GUI_KeyDown) : EV_GUI_KeyUp;
	event.data3 = static_cast<SWORD>((metaState & AMETA_SHIFT_ON ? GKM_SHIFT : 0) |
		(metaState & AMETA_CTRL_ON ? GKM_CTRL : 0) | (metaState & AMETA_ALT_ON ? GKM_ALT : 0));
	event.x = codepoint;
	event.y = keycode;
	D_PostEvent(&event);
}

bool Zandronum_AndroidInput_ConsumeKeyboardEvent(event_t &event)
{
	// Key releases follow their original press even if chat opened or closed in between.
	static std::set<int> chatKeys;
	if (event.subtype == EV_GUI_KeyUp)
	{
		if (chatKeys.erase(event.y) != 0 || event.data1 == 0) return true;
		event.type = EV_KeyUp;
		return false;
	}

	const bool chatActive = (gamestate == GS_LEVEL || gamestate == GS_INTERMISSION) && CHAT_GetChatMode();
	if (event.subtype == EV_GUI_KeyRepeat)
	{
		if (!chatActive || chatKeys.find(event.y) == chatKeys.end()) return true;
	}
	else if (chatActive)
	{
		chatKeys.insert(event.y);
	}
	else
	{
		if (event.data1 == 0) return true;
		event.type = EV_KeyDown;
		return false;
	}

	int guiKey = event.data2;
	switch (event.y)
	{
	case AKEYCODE_BACK:
	case AKEYCODE_ESCAPE: guiKey = GK_ESCAPE; break;
	case AKEYCODE_ENTER:
	case AKEYCODE_NUMPAD_ENTER: guiKey = GK_RETURN; break;
	case AKEYCODE_DEL: guiKey = GK_BACKSPACE; break;
	case AKEYCODE_FORWARD_DEL: guiKey = GK_DEL; break;
	case AKEYCODE_TAB: guiKey = GK_TAB; break;
	case AKEYCODE_DPAD_LEFT: guiKey = GK_LEFT; break;
	case AKEYCODE_DPAD_RIGHT: guiKey = GK_RIGHT; break;
	case AKEYCODE_DPAD_UP: guiKey = GK_UP; break;
	case AKEYCODE_DPAD_DOWN: guiKey = GK_DOWN; break;
	case AKEYCODE_MOVE_HOME: guiKey = GK_HOME; break;
	case AKEYCODE_MOVE_END: guiKey = GK_END; break;
	default:
		if (guiKey >= 'a' && guiKey <= 'z') guiKey += 'A' - 'a';
		break;
	}

	event_t gui = {};
	gui.type = EV_GUI_Event;
	gui.subtype = event.subtype;
	gui.data1 = static_cast<SWORD>(guiKey);
	gui.data3 = event.data3;
	if (guiKey != 0 && CHAT_Input(&gui)) return true;
	if (event.x >= ' ' && event.x <= 255 && event.x != 127)
	{
		gui.subtype = EV_GUI_Char;
		gui.data1 = static_cast<SWORD>(event.x);
		gui.data2 = (event.data3 & GKM_ALT) != 0;
		CHAT_Input(&gui);
	}
	return true;
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
	event.data1 = static_cast<SWORD>(x);
	event.data2 = static_cast<SWORD>(y);
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

void Zandronum_AndroidInput_MouseMotion(int x, int y, int deltaX, int deltaY)
{
	if (menuactive != MENU_Off || ConsoleState != c_up)
	{
		event_t event = {};
		event.type = EV_GUI_Event;
		event.subtype = EV_GUI_MouseMove;
		event.data1 = static_cast<SWORD>(x);
		event.data2 = static_cast<SWORD>(y);
		D_PostEvent(&event);
		return;
	}
	Zandronum_AndroidInput_Look(deltaX, deltaY);
}

void Zandronum_AndroidInput_MouseButton(int button, bool pressed, int x, int y)
{
	if (menuactive != MENU_Off || ConsoleState != c_up)
	{
		if (button < 1 || button > 3)
			return;
		static const int guiButtons[3] = { EV_GUI_LButtonDown, EV_GUI_MButtonDown, EV_GUI_RButtonDown };
		event_t event = {};
		event.type = EV_GUI_Event;
		event.subtype = static_cast<BYTE>(guiButtons[button - 1] + (pressed ? 0 : 1));
		event.data1 = static_cast<SWORD>(x);
		event.data2 = static_cast<SWORD>(y);
		D_PostEvent(&event);
		return;
	}
	int key = 0;
	switch (button)
	{
	case 1: key = KEY_MOUSE1; break;
	case 2: key = KEY_MOUSE3; break;
	case 3: key = KEY_MOUSE2; break;
	case 4: key = KEY_MOUSE4; break;
	case 5: key = KEY_MOUSE5; break;
	case 6: key = KEY_MOUSE6; break;
	case 7: key = KEY_MOUSE7; break;
	case 8: key = KEY_MOUSE8; break;
	default: break;
	}
	if (key != 0)
		PostKey(key, pressed);
}

void Zandronum_AndroidInput_MouseWheel(float horizontal, float vertical)
{
	const bool gui = menuactive != MENU_Off || ConsoleState != c_up;
	const float amounts[2] = { vertical, horizontal };
	const int guiKeys[2][2] = {
		{ EV_GUI_WheelUp, EV_GUI_WheelDown },
		{ EV_GUI_WheelRight, EV_GUI_WheelLeft }
	};
	const int gameKeys[2][2] = {
		{ KEY_MWHEELUP, KEY_MWHEELDOWN },
		{ KEY_MWHEELRIGHT, KEY_MWHEELLEFT }
	};
	for (int axis = 0; axis < 2; ++axis)
	{
		const float amount = amounts[axis];
		if (amount == 0.0f)
			continue;
		int steps = static_cast<int>(std::floor(std::fabs(amount) + 0.5f));
		if (steps == 0) steps = 1;
		steps = std::min(steps, 16);
		const int direction = amount > 0.0f ? 0 : 1;
		for (int step = 0; step < steps; ++step)
		{
			if (gui)
			{
				event_t event = {};
				event.type = EV_GUI_Event;
				event.subtype = static_cast<BYTE>(guiKeys[axis][direction]);
				D_PostEvent(&event);
			}
			else
			{
				PostKey(gameKeys[axis][direction], true);
				PostKey(gameKeys[axis][direction], false);
			}
		}
	}
}

void Zandronum_AndroidInput_Axis(int axis, float value)
{
	if (axis >= 0 && axis < 2)
	{
		AndroidTouchAxes[axis].store(std::max(-1.0f, std::min(1.0f, value)));
	}
}

void Zandronum_AndroidInput_ControllerAxis(int axis, float value)
{
	if (axis >= 0 && axis < 4)
		AndroidControllerAxes[axis].store(std::max(-1.0f, std::min(1.0f, value)));
}

float Zandronum_AndroidInput_GetControllerAxis(int axis)
{
	return axis >= 0 && axis < 4 ? AndroidControllerAxes[axis].load() : 0.0f;
}

void Zandronum_AndroidInput_ControllerKey(int keycode, bool pressed)
{
	int key = -1;
	switch (keycode)
	{
	case 96: key = KEY_JOY1; break;
	case 97: key = KEY_JOY2; break;
	case 98: key = KEY_JOY3; break;
	case 99: key = KEY_JOY4; break;
	case 100: key = KEY_JOY5; break;
	case 102: key = KEY_JOY6; break;
	case 103: key = KEY_JOY7; break;
	case 104: key = KEY_JOY8; break;
	case 105: key = KEY_FIRSTJOYBUTTON + 8; break;
	case 106: key = KEY_FIRSTJOYBUTTON + 9; break;
	case 107: key = KEY_FIRSTJOYBUTTON + 10; break;
	case 108: key = KEY_FIRSTJOYBUTTON + 11; break;
	case 109: key = KEY_FIRSTJOYBUTTON + 12; break;
	case 110: key = KEY_FIRSTJOYBUTTON + 13; break;
	case 19: key = KEY_JOYPOV1_UP; break;
	case 20: key = KEY_JOYPOV1_DOWN; break;
	case 21: key = KEY_JOYPOV1_LEFT; break;
	case 22: key = KEY_JOYPOV1_RIGHT; break;
	default: break;
	}
	if (key >= 0)
		PostKey(key, pressed);
}

void Zandronum_AndroidInput_Look(int deltaX, int deltaY)
{
	AndroidLookX.fetch_add(deltaX);
	AndroidLookY.fetch_add(deltaY);
}

void Zandronum_AndroidInput_Reset()
{
	AndroidTouchAxes[0].store(0.0f);
	AndroidTouchAxes[1].store(0.0f);
	for (std::atomic<float> &axis : AndroidControllerAxes)
		axis.store(0.0f);
	AndroidLookX.store(0);
	AndroidLookY.store(0);
	AndroidTouchJump.Reset();
	AndroidTouchCrouch.Reset();
	Button_SB_ScrollUp.ReleaseKey(AndroidTouchScoreboardKeyBase);
	Button_SB_ScrollDn.ReleaseKey(AndroidTouchScoreboardKeyBase + 1);
}

void Zandronum_AndroidInput_UpdateOverlayMode()
{
	const int mode = menuactive != MENU_Off ? 1 : (ConsoleState != c_up ? 2 : 0);
	AndroidOverlayMode.store(mode, std::memory_order_relaxed);
}

int Zandronum_AndroidInput_GetOverlayMode()
{
	return AndroidOverlayMode.load(std::memory_order_relaxed);
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
	axes[JOYAXIS_Forward] += -AndroidTouchAxes[1].load();
	axes[JOYAXIS_Side] += -AndroidTouchAxes[0].load();
}

void Zandronum_AndroidInput_Action(int action, bool pressed)
{
	event_t event = {};
	event.type = EV_AndroidAction;
	event.data1 = static_cast<SWORD>(action);
	event.data2 = pressed ? 1 : 0;
	D_PostEvent(&event);
}

void Zandronum_AndroidInput_MenuClear()
{
	if (menuactive == MENU_Off || DMenu::CurrentMenu == nullptr)
		return;

	if (menuactive == MENU_WaitKey && DMenu::CurrentMenu->mParentMenu != nullptr)
	{
		DMenu *inputMenu = DMenu::CurrentMenu;
		DMenu *parentMenu = inputMenu->mParentMenu;
		menuactive = MENU_On;
		parentMenu->MenuEvent(MKEY_Clear, true);
		inputMenu->MenuEvent(MKEY_Clear, true);
		return;
	}

	DMenu::CurrentMenu->MenuEvent(MKEY_Clear, true);
}

void Zandronum_AndroidInput_MenuAction(int direction, bool pressed)
{
	if (direction < 0 || direction > 3)
		return;
	if (menuactive == MENU_WaitKey)
		return;
	static const int androidDpadKeys[] = {
		19, 20, 21, 22
	};
	Zandronum_AndroidInput_Key(androidDpadKeys[direction], pressed);
}

void Zandronum_AndroidInput_ScoreboardScroll(int direction, bool pressed)
{
	if (direction < 0 || direction > 1)
		return;
	Zandronum_AndroidInput_Action(
		direction == 0 ? ANDROID_ACTION_SCORE_SCROLL_UP : ANDROID_ACTION_SCORE_SCROLL_DOWN,
		pressed);
}

bool Zandronum_AndroidInput_ScoreboardActive()
{
	return Button_ShowScores.bDown && SCOREBOARD_ShouldDrawBoard();
}

void Zandronum_AndroidInput_ApplyAction(int action, bool pressed)
{
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

	if (action == ANDROID_ACTION_SCORE_SCROLL_UP || action == ANDROID_ACTION_SCORE_SCROLL_DOWN)
	{
		if (menuactive == MENU_Off)
		{
			FButtonStatus *button = action == ANDROID_ACTION_SCORE_SCROLL_UP
				? &Button_SB_ScrollUp : &Button_SB_ScrollDn;
			const int key = AndroidTouchScoreboardKeyBase +
				(action == ANDROID_ACTION_SCORE_SCROLL_UP ? 0 : 1);
			if (pressed)
				button->PressKey(key);
			else
				button->ReleaseKey(key);
		}
		return;
	}

	if (action == ANDROID_ACTION_MENU)
	{
		if (pressed && menuactive == MENU_Off)
			PostKey(KEY_ESCAPE, true);
		else if (pressed && DMenu::CurrentMenu != nullptr)
			DMenu::CurrentMenu->MenuEvent(MKEY_Back, true);
		return;
	}

	if (action == ANDROID_ACTION_FIRE || action == ANDROID_ACTION_USE)
	{
		if (pressed && menuactive == MENU_Off && AndroidInput_TitleMenuKey())
		{
			PostKey(KEY_ESCAPE, true);
			return;
		}
		if (menuactive != MENU_Off)
		{
			if (pressed && DMenu::CurrentMenu != nullptr)
				DMenu::CurrentMenu->MenuEvent(MKEY_Enter, true);
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
	case ANDROID_ACTION_USE_ITEM: C_DoCommand("invuse"); break;
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
	case ANDROID_ACTION_VOTE_YES:
		if (menuactive != MENU_Off)
			PostGuiKey('y', true);
		else
			C_DoCommand("vote_yes");
		break;
	case ANDROID_ACTION_VOTE_NO:
		if (menuactive != MENU_Off)
			PostGuiKey('n', true);
		else
			C_DoCommand("vote_no");
		break;
	case ANDROID_ACTION_TAUNT: C_DoCommand("taunt"); break;
	case ANDROID_ACTION_CHASECAM: C_DoCommand("chase"); break;
	default: break;
	}
}

#endif
