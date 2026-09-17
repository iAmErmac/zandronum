#include "m_joy.h"

#ifdef __ANDROID__

#include <algorithm>
#include <mutex>
#include <vector>

#include "d_event.h"
#include "cmdlib.h"
#include "doomdef.h"
#include "gameconfigfile.h"
#include "templates.h"
#include "zandronum_android_input.h"
#include "zandronum_android_host.h"
#include "zstring.h"

namespace
{
	constexpr float DefaultDeadZone = 0.05f;
	constexpr int ControllerAxes = 4;
	const EJoyAxis DefaultAxisMap[ControllerAxes] = {
		JOYAXIS_Side, JOYAXIS_Forward, JOYAXIS_Yaw, JOYAXIS_Pitch
	};
	const char *DefaultAxisNames[ControllerAxes] = {
		"Left stick X", "Left stick Y", "Right stick X", "Right stick Y"
	};

	class AndroidJoystickConfig final : public IJoystickConfig
	{
	public:
		AndroidJoystickConfig(int deviceId, const char *name)
			: DeviceId(deviceId), Name(name != nullptr && *name != '\0' ? name : "Android Controller"), Connected(true), Sensitivity(1.0f)
		{
			SetDefaultConfig();
		}

		FString GetName() override
		{
			return Name;
		}

		float GetSensitivity() override
		{
			return Sensitivity;
		}

		void SetSensitivity(float scale) override
		{
			Sensitivity = scale;
		}

		int GetNumAxes() override
		{
			return ControllerAxes;
		}

		float GetAxisDeadZone(int axis) override
		{
			return IsValidAxis(axis) ? DeadZones[axis] : DefaultDeadZone;
		}

		EJoyAxis GetAxisMap(int axis) override
		{
			return IsValidAxis(axis) ? AxisMaps[axis] : JOYAXIS_None;
		}

		const char *GetAxisName(int axis) override
		{
			return IsValidAxis(axis) ? AxisNames[axis].GetChars() : "";
		}

		float GetAxisScale(int axis) override
		{
			return IsValidAxis(axis) ? AxisScales[axis] : 1.0f;
		}

		void SetAxisDeadZone(int axis, float zone) override
		{
			if (IsValidAxis(axis))
				DeadZones[axis] = clamp(zone, 0.0f, 0.9f);
		}

		void SetAxisMap(int axis, EJoyAxis gameaxis) override
		{
			if (IsValidAxis(axis))
				AxisMaps[axis] = gameaxis;
		}

		void SetAxisScale(int axis, float scale) override
		{
			if (IsValidAxis(axis))
				AxisScales[axis] = scale;
		}

		bool IsSensitivityDefault() override
		{
			return Sensitivity == 1.0f;
		}

		bool IsAxisDeadZoneDefault(int axis) override
		{
			return IsValidAxis(axis) && DeadZones[axis] == DefaultDeadZone;
		}

		bool IsAxisMapDefault(int axis) override
		{
			return IsValidAxis(axis) && AxisMaps[axis] == DefaultAxisMap[axis];
		}

		bool IsAxisScaleDefault(int axis) override
		{
			return IsValidAxis(axis) && AxisScales[axis] == 1.0f;
		}

		void SetDefaultConfig() override
		{
			Sensitivity = 1.0f;
			for (int axis = 0; axis < ControllerAxes; ++axis)
			{
				AxisNames[axis] = DefaultAxisNames[axis];
				DeadZones[axis] = DefaultDeadZone;
				AxisMaps[axis] = DefaultAxisMap[axis];
				AxisScales[axis] = 1.0f;
			}
		}

		FString GetIdentifier() override
		{
			char identifier[32];
			mysnprintf(identifier, countof(identifier), "Android:%d", DeviceId);
			return FString(identifier);
		}

		void SetConnected(bool connected)
		{
			Connected = connected;
		}

		bool IsConnected() const
		{
			return Connected;
		}

		int GetDeviceId() const
		{
			return DeviceId;
		}

		void AddAxes(float axes[NUM_JOYAXIS]) const
		{
			for (int axis = 0; axis < ControllerAxes; ++axis)
			{
				const EJoyAxis gameAxis = AxisMaps[axis];
				if (gameAxis == JOYAXIS_None)
					continue;
				const float value = static_cast<float>(Joy_RemoveDeadZone(
					Zandronum_AndroidInput_GetControllerAxis(axis), DeadZones[axis], nullptr));
				axes[gameAxis] -= value * Sensitivity * AxisScales[axis];
			}
		}

	private:
		bool IsValidAxis(int axis) const
		{
			return axis >= 0 && axis < ControllerAxes;
		}

		int DeviceId;
		FString Name;
		FString AxisNames[ControllerAxes];
		float DeadZones[ControllerAxes] = {};
		EJoyAxis AxisMaps[ControllerAxes] = {};
		float AxisScales[ControllerAxes] = {};
		bool Connected;
		float Sensitivity;
	};

	std::mutex DeviceMutex;
	std::vector<AndroidJoystickConfig *> Devices;
	AndroidJoystickConfig *ActiveDevice = nullptr;

	AndroidJoystickConfig *FindDevice(int deviceId)
	{
		for (AndroidJoystickConfig *device : Devices)
			if (device->GetDeviceId() == deviceId)
				return device;
		return nullptr;
	}

	void SelectActiveDevice()
	{
		if (ActiveDevice != nullptr && ActiveDevice->IsConnected())
			return;
		ActiveDevice = nullptr;
		for (AndroidJoystickConfig *device : Devices)
		{
			if (device->IsConnected())
			{
				ActiveDevice = device;
				break;
			}
		}
	}
}

void I_SetMouseCapture()
{
	Zandronum_AndroidHost_SetPointerCapture(true);
}

void I_ReleaseMouseCapture()
{
	Zandronum_AndroidHost_SetPointerCapture(false);
}

// Android delivers input and controller changes through JNI; the tick hook only publishes UI state.
void I_StartTic()
{
	Zandronum_AndroidInput_UpdateOverlayMode();
}

void I_StartFrame()
{
}

void I_ShutdownJoysticks()
{
	std::lock_guard<std::mutex> lock(DeviceMutex);
	for (AndroidJoystickConfig *device : Devices)
	{
		if (device->IsConnected() && GameConfig != nullptr)
			M_SaveJoystickConfig(device);
		delete device;
	}
	Devices.clear();
	ActiveDevice = nullptr;
	Zandronum_AndroidInput_Reset();
}

void Zandronum_AndroidInput_Device(int deviceId, const char *name, bool connected)
{
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(DeviceMutex);
		AndroidJoystickConfig *device = FindDevice(deviceId);
		if (device == nullptr && connected)
		{
			device = new AndroidJoystickConfig(deviceId, name);
			Devices.push_back(device);
			changed = true;
		}
		if (device != nullptr)
		{
			if (device->IsConnected() != connected)
				changed = true;
			device->SetConnected(connected);
			if (connected)
				ActiveDevice = device;
			else
				SelectActiveDevice();
		}
	}
	if (changed)
	{
		event_t event = {};
		event.type = EV_DeviceChange;
		D_PostEvent(&event);
	}
}

void I_GetJoysticks(TArray<IJoystickConfig *> &sticks)
{
	sticks.Clear();
	std::lock_guard<std::mutex> lock(DeviceMutex);
	for (AndroidJoystickConfig *device : Devices)
	{
		if (device->IsConnected())
		{
			M_LoadJoystickConfig(device);
			sticks.Push(device);
		}
	}
}

void I_GetAxes(float axes[NUM_JOYAXIS])
{
	for (int axis = 0; axis < NUM_JOYAXIS; ++axis)
		axes[axis] = 0.0f;
	if (!use_joystick)
		return;

	std::lock_guard<std::mutex> lock(DeviceMutex);
	SelectActiveDevice();
	if (ActiveDevice != nullptr)
		ActiveDevice->AddAxes(axes);
}

IJoystickConfig *I_UpdateDeviceList()
{
	std::lock_guard<std::mutex> lock(DeviceMutex);
	SelectActiveDevice();
	return ActiveDevice;
}

#endif
