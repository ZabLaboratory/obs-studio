#include <obs-module.h>
#include <strsafe.h>
#include <strmif.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-dshow", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Windows DirectShow source/encoder";
}

extern void RegisterDShowSource();
extern void RegisterDShowEncoders();

#ifdef VIRTUALCAM_AVAILABLE
extern "C" struct obs_output_info virtualcam_info;
extern "C" struct obs_output_info program_return_info;
#endif

bool obs_module_load(void)
{
	RegisterDShowSource();
	RegisterDShowEncoders();
#ifdef VIRTUALCAM_AVAILABLE
	// The OBS outputs are producers, not COM-device discovery. Register them
	// whenever the native virtual-output implementation is compiled in. The
	// DirectShow module may be installed later (or on another consumer machine),
	// while libobs must still be able to create the program-return output and
	// publish its named shared-memory queue.
	obs_register_output(&virtualcam_info);
	obs_register_output(&program_return_info);
#endif

	return true;
}
