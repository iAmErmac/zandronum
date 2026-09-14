#ifndef ZANDRONUM_ANDROID_INPUT_H
#define ZANDRONUM_ANDROID_INPUT_H

#ifdef __ANDROID__
void Zandronum_AndroidInput_Key(int keycode, bool pressed);
void Zandronum_AndroidInput_GuiKey(int keycode, bool pressed);
void Zandronum_AndroidInput_Text(int codepoint);
void Zandronum_AndroidInput_Pointer(int pointerId, int action, float x, float y);
void Zandronum_AndroidInput_Axis(int axis, float value);
void Zandronum_AndroidInput_ControllerAxis(int axis, float value);
float Zandronum_AndroidInput_GetControllerAxis(int axis);
void Zandronum_AndroidInput_ControllerKey(int keycode, bool pressed);
void Zandronum_AndroidInput_Device(int deviceId, const char *name, bool connected);
void Zandronum_AndroidInput_Look(int deltaX, int deltaY);
void Zandronum_AndroidInput_Reset();
bool Zandronum_AndroidInput_FlyActive();
bool Zandronum_AndroidInput_TouchJumpDown();
bool Zandronum_AndroidInput_TouchCrouchDown();
void Zandronum_AndroidInput_Action(int action, bool pressed);
void Zandronum_AndroidInput_MenuAction(int direction, bool pressed);
void Zandronum_AndroidInput_ApplyAction(int action, bool pressed);
void Zandronum_AndroidInput_ApplyAxes(float axes[]);
#endif

#endif
