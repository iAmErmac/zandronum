#include "zandronum_android_input.h"

#ifdef __ANDROID__

#include <algorithm>

#include "d_event.h"
#include "d_gui.h"
#include "doomdef.h"
#include "m_joy.h"

static volatile float AndroidAxes[4] = {};
static bool AndroidArrowState[4] = {};

static void PostKey(int keycode, bool pressed)
{
	event_t event = {};
	event.type = pressed ? EV_KeyDown : EV_KeyUp;
	event.data1 = static_cast<SWORD>(keycode);
	D_PostEvent(&event);
}

static void SetArrowState(int index, int keycode, bool pressed)
{
	if (AndroidArrowState[index] == pressed) return;
	AndroidArrowState[index] = pressed;
	PostKey(keycode, pressed);
}

void Zandronum_AndroidInput_Key(int keycode, bool pressed)
{
	PostKey(keycode, pressed);
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
	if (axis >= 0 && axis < 4)
	{
		AndroidAxes[axis] = std::max(-1.0f, std::min(1.0f, value));
		if (axis == 0)
		{
			SetArrowState(0, KEY_LEFTARROW, AndroidAxes[axis] < -0.35f);
			SetArrowState(1, KEY_RIGHTARROW, AndroidAxes[axis] > 0.35f);
		}
		else if (axis == 1)
		{
			SetArrowState(2, KEY_UPARROW, AndroidAxes[axis] < -0.35f);
			SetArrowState(3, KEY_DOWNARROW, AndroidAxes[axis] > 0.35f);
		}
	}
}

void Zandronum_AndroidInput_ApplyAxes(float axes[])
{
	axes[JOYAXIS_Yaw] = AndroidAxes[2];
	axes[JOYAXIS_Pitch] = AndroidAxes[3];
	axes[JOYAXIS_Forward] = -AndroidAxes[1];
	axes[JOYAXIS_Side] = AndroidAxes[0];
}

void Zandronum_AndroidInput_Action(int action, bool pressed)
{
	static const int actionKeys[] = {
		KEY_MOUSE1, 0x12, KEY_SPACE, KEY_LCTRL, KEY_LSHIFT,
		KEY_MWHEELUP, KEY_MWHEELDOWN, KEY_ESCAPE, KEY_GRAVE,
		KEY_TAB, KEY_EQUALS, KEY_MINUS,
	};
	if (action >= 0 && action < static_cast<int>(sizeof(actionKeys) / sizeof(actionKeys[0])))
		PostKey(actionKeys[action], pressed);
}

#endif
